#include "vst/RekaVstHost.h"

#include "vst/JuceInsertProcessor.h"
#include "vst/SandboxProtocol.h"
#include "vst/SandboxedInsertProcessor.h"

namespace reka::app {

// ---- out-of-process scanner ----------------------------------------------------------------------------

class RekaVstHost::OutOfProcessScanner final : public juce::KnownPluginList::CustomScanner {
public:
    OutOfProcessScanner(juce::File hostExecutable, juce::KnownPluginList& list)
        : hostExecutable_(std::move(hostExecutable)), list_(list) {}

    bool findPluginTypesFor(juce::AudioPluginFormat& format, juce::OwnedArray<juce::PluginDescription>& result,
                            const juce::String& fileOrIdentifier) override {
        if (shouldExit()) return false;
        if (!hostExecutable_.existsAsFile()) {
            // Development fallback: scan in-process.
            format.findAllTypesForFile(result, fileOrIdentifier);
            return true;
        }

        const juce::File output = juce::File::createTempFile(".xml");
        juce::ChildProcess child;
        const juce::StringArray args{hostExecutable_.getFullPathName(), sandbox::kScanArgument, format.getName(),
                                     fileOrIdentifier, output.getFullPathName()};
        if (!child.start(args, 0)) return false;

        // Generous timeout: some plugins run licence checks on first load.
        if (!child.waitForProcessToFinish(60000)) {
            child.kill();
            list_.addToBlacklist(fileOrIdentifier);
            output.deleteFile();
            return false;
        }
        const auto exitCode = child.getExitCode();
        std::unique_ptr<juce::XmlElement> xml = juce::parseXML(output);
        output.deleteFile();
        if (exitCode != 0 || xml == nullptr) {
            list_.addToBlacklist(fileOrIdentifier);  // crashed or failed to load: never probe it again
            return false;
        }
        for (const auto* element : xml->getChildIterator()) {
            juce::PluginDescription description;
            if (description.loadFromXml(*element)) result.add(new juce::PluginDescription(description));
        }
        return true;
    }

private:
    juce::File hostExecutable_;
    juce::KnownPluginList& list_;
};

// ---- scan thread ----------------------------------------------------------------------------------------

class RekaVstHost::ScanThread final : public juce::Thread {
public:
    ScanThread(RekaVstHost& host, bool rescanEverything)
        : juce::Thread("REKA plugin scanner"), host_(host), rescanEverything_(rescanEverything) {}

    void run() override {
        const juce::File pedal = host_.pluginListFile_.getSiblingFile("scan-in-progress.txt");
        for (auto* format : host_.formats_.getFormats()) {
            if (threadShouldExit()) break;
            juce::FileSearchPath path = host_.searchPath_;
            if (path.getNumPaths() == 0) path = format->getDefaultLocationsToSearch();
            juce::PluginDirectoryScanner scanner(host_.knownPlugins_, *format, path, true, pedal, false);
            juce::String scanning;
            while (!threadShouldExit()) {
                {
                    const juce::ScopedLock lock(host_.scanNameLock_);
                    host_.scanName_ = scanner.getNextPluginFileThatWillBeScanned();
                }
                if (!scanner.scanNextFile(!rescanEverything_, scanning)) break;
                host_.progress_.store(scanner.getProgress());
            }
        }
        host_.scanning_.store(false);
        host_.triggerAsyncUpdate();
    }

private:
    RekaVstHost& host_;
    bool rescanEverything_;
};

// ---- editor window ---------------------------------------------------------------------------------------

class RekaVstHost::PluginWindow final : public juce::DocumentWindow {
public:
    PluginWindow(RekaVstHost& host, IInsertProcessor& insert, juce::AudioProcessorEditor* editor, const juce::String& title)
        : juce::DocumentWindow(title, juce::Colour(0xFF141924), juce::DocumentWindow::closeButton | juce::DocumentWindow::minimiseButton),
          host_(host), insert_(insert) {
        setUsingNativeTitleBar(true);
        setContentOwned(editor, true);
        setResizable(editor->isResizable(), false);
        centreWithSize(getWidth(), getHeight());
        setVisible(true);
    }

    void closeButtonPressed() override {
        // Defer: the window cannot delete itself from inside its own callback.
        juce::MessageManager::callAsync([safe = juce::Component::SafePointer<PluginWindow>(this)] {
            if (safe != nullptr) safe->host_.closeEditor(safe->insert_);
        });
    }

private:
    RekaVstHost& host_;
    IInsertProcessor& insert_;
};

// ---- RekaVstHost ---------------------------------------------------------------------------------------

RekaVstHost::RekaVstHost(juce::File pluginListFile, juce::File hostExecutable)
    : pluginListFile_(std::move(pluginListFile)), hostExecutable_(std::move(hostExecutable)) {
    juce::addDefaultFormatsToManager(formats_);
    knownPlugins_.setCustomScanner(std::make_unique<OutOfProcessScanner>(hostExecutable_, knownPlugins_));
    if (auto xml = juce::parseXML(pluginListFile_)) knownPlugins_.recreateFromXml(*xml);
}

RekaVstHost::~RekaVstHost() {
    cancelScan();
    closeAllEditors();
    cancelPendingUpdate();
}

juce::Array<juce::PluginDescription> RekaVstHost::effectPlugins() const {
    juce::Array<juce::PluginDescription> effects;
    for (const auto& description : knownPlugins_.getTypes())
        if (!description.isInstrument) effects.add(description);
    return effects;
}

std::optional<juce::PluginDescription> RekaVstHost::findDescription(const juce::String& identifier) const {
    if (auto description = knownPlugins_.getTypeForIdentifierString(identifier)) return *description;
    return std::nullopt;
}

juce::FileSearchPath RekaVstHost::searchPath() const { return searchPath_; }
void RekaVstHost::setSearchPath(const juce::FileSearchPath& path) { searchPath_ = path; }

void RekaVstHost::startScan(bool rescanEverything) {
    if (scanning_.load()) return;
    scanThread_.reset();
    scanning_.store(true);
    progress_.store(0.0f);
    scanThread_ = std::make_unique<ScanThread>(*this, rescanEverything);
    scanThread_->startThread(juce::Thread::Priority::low);
    sendChangeMessage();
}

void RekaVstHost::cancelScan() {
    if (scanThread_ != nullptr) scanThread_->stopThread(65000);
    scanThread_.reset();
    scanning_.store(false);
}

juce::String RekaVstHost::currentlyScanning() const {
    const juce::ScopedLock lock(scanNameLock_);
    return scanName_;
}

void RekaVstHost::clearBlacklist() {
    knownPlugins_.clearBlacklistedFiles();
    savePluginList();
}

void RekaVstHost::handleAsyncUpdate() { scanFinished(); }

void RekaVstHost::scanFinished() {
    savePluginList();
    sendChangeMessage();
}

void RekaVstHost::savePluginList() {
    if (auto xml = knownPlugins_.createXml()) {
        pluginListFile_.getParentDirectory().createDirectory();
        xml->writeTo(pluginListFile_);
    }
}

InsertDescriptor RekaVstHost::describe(const juce::PluginDescription& description, bool sandboxed) const {
    InsertDescriptor out;
    out.pluginId = description.createIdentifierString().toStdString();
    out.name = description.name.toStdString();
    out.format = description.pluginFormatName.toStdString();
    out.sandboxed = sandboxed;
    return out;
}

std::shared_ptr<IInsertProcessor> RekaVstHost::createInsert(const InsertDescriptor& descriptor, double sampleRate,
                                                            int maxBlockSize, std::string& error) {
    const auto description = findDescription(descriptor.pluginId);
    if (!description) {
        error = "plugin is not installed (or not scanned yet)";
        return nullptr;
    }
    auto onLatency = [this] { latencyChanged_.store(true); };

    if (descriptor.sandboxed) {
        SandboxedInsertProcessor::Options options;
        options.hostExecutable = hostExecutable_;
        options.description = *description;
        options.descriptor = descriptor;
        options.sampleRate = sampleRate;
        options.maxBlockSize = maxBlockSize;
        options.onLatencyChanged = onLatency;
        juce::String sandboxError;
        auto insert = SandboxedInsertProcessor::launch(options, sandboxError);
        if (insert == nullptr) error = sandboxError.toStdString();
        return insert;
    }

    juce::String loadError;
    auto instance = formats_.createPluginInstance(*description, sampleRate, maxBlockSize, loadError);
    if (instance == nullptr) {
        error = loadError.toStdString();
        return nullptr;
    }
    auto insert = std::make_shared<JuceInsertProcessor>(std::move(instance), descriptor, onLatency);
    insert->onDestroyed = [this](JuceInsertProcessor* dying) { windows_.erase(dying); };
    insert->prepare(sampleRate, maxBlockSize);
    return insert;
}

void RekaVstHost::showEditor(IInsertProcessor& insert) {
    if (auto* sandboxed = dynamic_cast<SandboxedInsertProcessor*>(&insert)) {
        sandboxed->showEditor();
        return;
    }
    auto* local = dynamic_cast<JuceInsertProcessor*>(&insert);
    if (local == nullptr) return;
    if (auto it = windows_.find(&insert); it != windows_.end()) {
        it->second->toFront(true);
        return;
    }
    auto& plugin = local->plugin();
    juce::AudioProcessorEditor* editor = plugin.hasEditor() ? plugin.createEditorIfNeeded() : nullptr;
    if (editor == nullptr) editor = new juce::GenericAudioProcessorEditor(plugin);
    windows_[&insert] = std::make_unique<PluginWindow>(*this, insert, editor, plugin.getName() + " - REKA");
}

void RekaVstHost::closeEditor(IInsertProcessor& insert) {
    if (auto* sandboxed = dynamic_cast<SandboxedInsertProcessor*>(&insert)) sandboxed->hideEditor();
    windows_.erase(&insert);
}

void RekaVstHost::closeAllEditors() { windows_.clear(); }

}  // namespace reka::app
