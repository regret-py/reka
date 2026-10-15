#include "vst/SandboxedInsertProcessor.h"

#include "vst/SandboxProtocol.h"

#include <chrono>

namespace reka::app {

class SandboxedInsertProcessor::Coordinator final : public juce::ChildProcessCoordinator {
public:
    explicit Coordinator(SandboxedInsertProcessor& owner) : owner_(owner) {}
    // Kill the worker while this subclass (and its owner) are still fully alive.
    ~Coordinator() override { killWorkerProcess(); }

    void handleMessageFromWorker(const juce::MemoryBlock& block) override { owner_.onMessage(sandbox::decode(block)); }
    void handleConnectionLost() override { owner_.onConnectionLost(); }

private:
    SandboxedInsertProcessor& owner_;
};

std::shared_ptr<SandboxedInsertProcessor> SandboxedInsertProcessor::launch(const Options& options, juce::String& error) {
    std::shared_ptr<SandboxedInsertProcessor> insert(new SandboxedInsertProcessor(options));
    if (!insert->start(error)) return nullptr;
    return insert;
}

SandboxedInsertProcessor::SandboxedInsertProcessor(Options options) : options_(std::move(options)) {}

SandboxedInsertProcessor::~SandboxedInsertProcessor() {
    coordinator_.reset();
    channel_.close();
}

bool SandboxedInsertProcessor::start(juce::String& error) {
    if (!options_.hostExecutable.existsAsFile()) {
        error = "REKA-PluginHost executable not found next to REKA";
        return false;
    }
    channelName_ = "reka-sbx-" + juce::Uuid().toString().substring(0, 20);
    std::string channelError;
    if (!channel_.create(channelName_.toStdString(), juce::jmax(8192, options_.maxBlockSize), channelError)) {
        error = "shared memory: " + juce::String(channelError);
        return false;
    }

    coordinator_ = std::make_unique<Coordinator>(*this);
    if (!coordinator_->launchWorkerProcess(options_.hostExecutable, sandbox::kCommandLineUid, sandbox::kPingTimeoutMs, 0)) {
        error = "could not start the plugin sandbox process";
        return false;
    }

    std::unique_ptr<juce::XmlElement> xml = options_.description.createXml();
    const juce::var reply = request(sandbox::makeMessage({{"cmd", "load"},
                                                          {"desc", xml->toString()},
                                                          {"sampleRate", options_.sampleRate},
                                                          {"blockSize", options_.maxBlockSize},
                                                          {"shm", channelName_}}),
                                    sandbox::kLaunchTimeoutMs, error);
    if (!static_cast<bool>(reply.getProperty("ok", false))) {
        if (error.isEmpty()) error = "the plugin could not be loaded in the sandbox";
        return false;
    }
    latency_.store(static_cast<int>(reply.getProperty("latency", 0)));
    timeoutMicros_ = juce::jmax(1500, static_cast<int>(0.6 * options_.maxBlockSize / options_.sampleRate * 1.0e6));
    return true;
}

void SandboxedInsertProcessor::prepare(double sampleRate, int maxBlockSize) {
    options_.sampleRate = sampleRate;
    options_.maxBlockSize = maxBlockSize;
    timeoutMicros_ = juce::jmax(1500, static_cast<int>(0.6 * maxBlockSize / sampleRate * 1.0e6));
    if (hasFailed()) return;
    if (maxBlockSize > channel_.maxFrames()) {
        markFailed();  // cannot resize a live mapping; the watchdog relaunches with a larger one
        return;
    }
    juce::String error;
    const juce::var reply =
        request(sandbox::makeMessage({{"cmd", "prepare"}, {"sampleRate", sampleRate}, {"blockSize", maxBlockSize}}), 10000, error);
    if (static_cast<bool>(reply.getProperty("ok", false))) latency_.store(static_cast<int>(reply.getProperty("latency", 0)));
}

void SandboxedInsertProcessor::process(float* const* channels, int numChannels, int numFrames) noexcept {
    switch (channel_.process(channels, numChannels, numFrames, timeoutMicros_)) {
        case SharedAudioChannel::Result::Ok: break;
        case SharedAudioChannel::Result::Timeout: missed_.fetch_add(1, std::memory_order_relaxed); break;
        case SharedAudioChannel::Result::Disconnected: markFailed(); break;
    }
}

std::vector<std::uint8_t> SandboxedInsertProcessor::saveState() {
    if (hasFailed()) return lastState_;
    juce::String error;
    const juce::var reply = request(sandbox::makeMessage({{"cmd", "getState"}}), 5000, error);
    if (static_cast<bool>(reply.getProperty("ok", false))) {
        juce::MemoryBlock block;
        if (block.fromBase64Encoding(reply.getProperty("state", "").toString())) {
            const auto* bytes = static_cast<const std::uint8_t*>(block.getData());
            lastState_.assign(bytes, bytes + block.getSize());
        }
    }
    return lastState_;
}

bool SandboxedInsertProcessor::restoreState(const std::vector<std::uint8_t>& state) {
    lastState_ = state;
    if (state.empty()) return true;
    const juce::MemoryBlock block(state.data(), state.size());
    juce::String error;
    const juce::var reply =
        request(sandbox::makeMessage({{"cmd", "setState"}, {"state", block.toBase64Encoding()}}), 5000, error);
    return static_cast<bool>(reply.getProperty("ok", false));
}

void SandboxedInsertProcessor::showEditor() {
    if (coordinator_ != nullptr && !hasFailed()) coordinator_->sendMessageToWorker(sandbox::encode(sandbox::makeMessage({{"cmd", "showEditor"}})));
}

void SandboxedInsertProcessor::hideEditor() {
    if (coordinator_ != nullptr && !hasFailed()) coordinator_->sendMessageToWorker(sandbox::encode(sandbox::makeMessage({{"cmd", "hideEditor"}})));
}

juce::var SandboxedInsertProcessor::request(const juce::var& message, int timeoutMs, juce::String& error) {
    if (coordinator_ == nullptr) {
        error = "sandbox not running";
        return {};
    }
    int id = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (connectionLost_) {
            error = "the plugin process has exited";
            return {};
        }
        id = nextRequestId_++;
    }
    if (auto* object = message.getDynamicObject()) object->setProperty("id", id);
    if (!coordinator_->sendMessageToWorker(sandbox::encode(message))) {
        error = "the plugin process is not responding";
        return {};
    }

    std::unique_lock<std::mutex> lock(mutex_);
    const bool done = replied_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                                        [&] { return replies_.count(id) > 0 || connectionLost_; });
    if (!done) {
        error = "the plugin process timed out";
        return {};
    }
    auto it = replies_.find(id);
    if (it == replies_.end()) {
        error = "the plugin process has exited";
        return {};
    }
    juce::var reply = it->second;
    replies_.erase(it);
    if (!static_cast<bool>(reply.getProperty("ok", false))) error = reply.getProperty("error", "unknown error").toString();
    return reply;
}

void SandboxedInsertProcessor::onMessage(const juce::var& message) {
    // IPC thread.
    if (message.hasProperty("id")) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            replies_[static_cast<int>(message.getProperty("id", 0))] = message;
        }
        replied_.notify_all();
        return;
    }
    if (message.getProperty("event", "").toString() == "latency") {
        latency_.store(static_cast<int>(message.getProperty("latency", 0)));
        if (options_.onLatencyChanged) options_.onLatencyChanged();
    }
}

void SandboxedInsertProcessor::onConnectionLost() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        connectionLost_ = true;
    }
    replied_.notify_all();
    markFailed();
}

}  // namespace reka::app
