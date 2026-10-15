// REKA-PluginHost.exe - out-of-process plugin scanner and plugin sandbox (see vst/SandboxProtocol.h).
#include "reka/platform/SharedAudioChannel.h"
#include "reka/platform/ThreadPriority.h"
#include "vst/PluginBlockAdapter.h"
#include "vst/SandboxProtocol.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <atomic>

namespace reka::app {

// ---- scan mode -------------------------------------------------------------------------------------------------

static int runScan(const juce::StringArray& args) {
    // REKA-PluginHost --scan <format> <file-or-identifier> <output.xml>
    const int index = args.indexOf(sandbox::kScanArgument);
    if (index < 0 || args.size() < index + 4) return 2;
    const juce::String formatName = args[index + 1].unquoted();
    const juce::String identifier = args[index + 2].unquoted();
    const juce::File output(args[index + 3].unquoted());

    juce::AudioPluginFormatManager formats;
    juce::addDefaultFormatsToManager(formats);
    for (auto* format : formats.getFormats()) {
        if (format->getName() != formatName) continue;
        juce::OwnedArray<juce::PluginDescription> found;
        format->findAllTypesForFile(found, identifier);  // a crash here only kills this process
        juce::XmlElement root("PLUGINS");
        for (auto* description : found) root.addChildElement(description->createXml().release());
        return root.writeTo(output) ? 0 : 3;
    }
    return 4;
}

// ---- sandbox mode ----------------------------------------------------------------------------------------------

class SandboxWorker final : public juce::ChildProcessWorker, private juce::AudioProcessorListener, private juce::Timer {
public:
    SandboxWorker() {
        juce::addDefaultFormatsToManager(formats_);
        startTimerHz(10);
    }

    ~SandboxWorker() override {
        stopTimer();
        stopAudio();
        editorWindow_.reset();
        if (plugin_ != nullptr) {
            plugin_->removeListener(this);
            plugin_->releaseResources();
        }
        plugin_.reset();
    }

    void handleMessageFromCoordinator(const juce::MemoryBlock& block) override {
        // IPC thread: hop to the message thread (plugins expect it for loading, state and editors).
        const juce::var message = sandbox::decode(block);
        juce::MessageManager::callAsync([weak = juce::WeakReference<SandboxWorker>(this), message] {
            if (auto* self = weak.get()) self->handle(message);
        });
    }

    void handleConnectionLost() override {
        // The coordinator (REKA.exe) went away: never outlive it.
        juce::MessageManager::callAsync([] { juce::JUCEApplicationBase::quit(); });
    }

private:
    class AudioThread final : public juce::Thread {
    public:
        explicit AudioThread(SandboxWorker& owner) : juce::Thread("REKA sandbox audio"), owner_(owner) {}
        void run() override {
            ScopedAudioThreadPriority audioPriority;
            auto& channel = owner_.channel_;
            while (!threadShouldExit()) {
                const int frames = channel.waitForRequest(50);
                if (frames < 0) break;
                if (frames == 0) continue;
                float* channels[2] = {channel.channel(0), channel.channel(1)};
                {
                    const juce::ScopedLock lock(owner_.processLock_);
                    if (owner_.plugin_ != nullptr && channel.requestChannels() >= 2)
                        owner_.adapter_.process(*owner_.plugin_, channels, 2, frames);
                }
                channel.completeRequest();
            }
        }

    private:
        SandboxWorker& owner_;
    };

    class EditorWindow final : public juce::DocumentWindow {
    public:
        EditorWindow(juce::AudioProcessorEditor* editor, const juce::String& title, std::function<void()> onClose)
            : juce::DocumentWindow(title, juce::Colours::black, juce::DocumentWindow::closeButton | juce::DocumentWindow::minimiseButton),
              onClose_(std::move(onClose)) {
            setUsingNativeTitleBar(true);
            setContentOwned(editor, true);
            setResizable(editor->isResizable(), false);
            setAlwaysOnTop(true);
            centreWithSize(getWidth(), getHeight());
            setVisible(true);
        }
        void closeButtonPressed() override { juce::MessageManager::callAsync(onClose_); }

    private:
        std::function<void()> onClose_;
    };

    void handle(const juce::var& message) {
        const juce::String command = message.getProperty("cmd", "").toString();
        auto* reply = new juce::DynamicObject();
        juce::var replyVar(reply);
        reply->setProperty("ok", true);

        if (command == "load") {
            juce::String error;
            if (!load(message, error)) {
                reply->setProperty("ok", false);
                reply->setProperty("error", error);
            } else {
                reply->setProperty("latency", plugin_->getLatencySamples());
                reply->setProperty("name", plugin_->getName());
                reply->setProperty("inputs", adapter_.pluginInputs());
                reply->setProperty("outputs", adapter_.pluginOutputs());
            }
        } else if (plugin_ == nullptr) {
            reply->setProperty("ok", false);
            reply->setProperty("error", "no plugin loaded");
        } else if (command == "prepare") {
            {
                const juce::ScopedLock lock(processLock_);
                plugin_->releaseResources();
                adapter_.prepare(*plugin_, static_cast<double>(message.getProperty("sampleRate", 48000.0)),
                                 static_cast<int>(message.getProperty("blockSize", 512)));
            }
            channel_.setLatency(plugin_->getLatencySamples());
            reply->setProperty("latency", plugin_->getLatencySamples());
        } else if (command == "getState") {
            juce::MemoryBlock state;
            plugin_->getStateInformation(state);
            reply->setProperty("state", state.toBase64Encoding());
        } else if (command == "setState") {
            juce::MemoryBlock state;
            if (state.fromBase64Encoding(message.getProperty("state", "").toString()) && state.getSize() > 0)
                plugin_->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        } else if (command == "showEditor") {
            showEditor();
        } else if (command == "hideEditor") {
            editorWindow_.reset();
        }

        if (message.hasProperty("id")) {
            reply->setProperty("id", message.getProperty("id", 0));
            sendMessageToCoordinator(sandbox::encode(replyVar));
        }
    }

    bool load(const juce::var& message, juce::String& error) {
        if (plugin_ != nullptr) {
            error = "a plugin is already loaded";
            return false;
        }
        std::unique_ptr<juce::XmlElement> xml = juce::parseXML(message.getProperty("desc", "").toString());
        juce::PluginDescription description;
        if (xml == nullptr || !description.loadFromXml(*xml)) {
            error = "invalid plugin description";
            return false;
        }
        const double sampleRate = message.getProperty("sampleRate", 48000.0);
        const int blockSize = message.getProperty("blockSize", 512);
        auto instance = formats_.createPluginInstance(description, sampleRate, blockSize, error);
        if (instance == nullptr) return false;

        std::string channelError;
        if (!channel_.open(message.getProperty("shm", "").toString().toStdString(), channelError)) {
            error = "shared memory: " + juce::String(channelError);
            return false;
        }
        PluginBlockAdapter::configureLayout(*instance);
        adapter_.prepare(*instance, sampleRate, blockSize);
        plugin_ = std::move(instance);
        plugin_->addListener(this);
        channel_.setLatency(plugin_->getLatencySamples());
        audioThread_ = std::make_unique<AudioThread>(*this);
        audioThread_->startThread(juce::Thread::Priority::highest);
        return true;
    }

    void stopAudio() {
        if (audioThread_ == nullptr) return;
        audioThread_->signalThreadShouldExit();
        audioThread_->stopThread(2000);
        audioThread_.reset();
    }

    void showEditor() {
        if (editorWindow_ != nullptr) {
            editorWindow_->toFront(true);
            return;
        }
        juce::AudioProcessorEditor* editor = plugin_->hasEditor() ? plugin_->createEditorIfNeeded() : nullptr;
        if (editor == nullptr) editor = new juce::GenericAudioProcessorEditor(*plugin_);
        editorWindow_ = std::make_unique<EditorWindow>(editor, plugin_->getName() + " - REKA (sandboxed)", [this] {
            editorWindow_.reset();
            sendMessageToCoordinator(sandbox::encode(sandbox::makeMessage({{"event", "editorClosed"}})));
        });
    }

    void audioProcessorParameterChanged(juce::AudioProcessor*, int, float) override {}
    void audioProcessorChanged(juce::AudioProcessor* processor, const ChangeDetails& details) override {
        // Possibly on the audio thread: just flag it, the timer reports on the message thread.
        if (details.latencyChanged) {
            channel_.setLatency(processor->getLatencySamples());
            latencyChanged_.store(true);
        }
    }

    void timerCallback() override {
        if (!latencyChanged_.exchange(false) || plugin_ == nullptr) return;
        sendMessageToCoordinator(sandbox::encode(sandbox::makeMessage({{"event", "latency"}, {"latency", plugin_->getLatencySamples()}})));
    }

    juce::AudioPluginFormatManager formats_;
    std::unique_ptr<juce::AudioPluginInstance> plugin_;
    PluginBlockAdapter adapter_;
    SharedAudioChannel channel_;
    juce::CriticalSection processLock_;
    std::unique_ptr<AudioThread> audioThread_;
    std::unique_ptr<EditorWindow> editorWindow_;
    std::atomic<bool> latencyChanged_{false};

    JUCE_DECLARE_WEAK_REFERENCEABLE(SandboxWorker)
};

class PluginHostApplication final : public juce::JUCEApplication {
public:
    const juce::String getApplicationName() override { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override { return true; }  // one process per sandboxed plugin

    void initialise(const juce::String& commandLine) override {
        const auto args = juce::StringArray::fromTokens(commandLine, true);
        if (args.contains(sandbox::kScanArgument)) {
            setApplicationReturnValue(runScan(args));
            quit();
            return;
        }
        worker_ = std::make_unique<SandboxWorker>();
        if (!worker_->initialiseFromCommandLine(commandLine, sandbox::kCommandLineUid)) {
            // Launched by hand: nothing to do.
            worker_.reset();
            setApplicationReturnValue(1);
            quit();
        }
    }

    void shutdown() override { worker_.reset(); }
    void systemRequestedQuit() override { quit(); }

private:
    std::unique_ptr<SandboxWorker> worker_;
};

}  // namespace reka::app

START_JUCE_APPLICATION(reka::app::PluginHostApplication)
