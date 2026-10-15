#pragma once

#include "reka/engine/InsertProcessor.h"
#include "reka/platform/SharedAudioChannel.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>

namespace reka::app {

// A plugin running inside its own REKA-PluginHost.exe process.
//
// Audio crosses the process boundary through SharedAudioChannel within the same engine block (no extra
// latency). If the plugin process stalls, the block passes through dry; if it dies, the insert reports
// hasFailed() (the engine then bypasses it) and AppServices respawns it from the last saved state.
class SandboxedInsertProcessor final : public IInsertProcessor {
public:
    struct Options {
        juce::File hostExecutable;
        juce::PluginDescription description;
        InsertDescriptor descriptor;
        double sampleRate = 48000.0;
        int maxBlockSize = 512;
        std::function<void()> onLatencyChanged;  // any thread
    };

    static std::shared_ptr<SandboxedInsertProcessor> launch(const Options& options, juce::String& error);
    ~SandboxedInsertProcessor() override;

    void prepare(double sampleRate, int maxBlockSize) override;
    void release() override {}
    void process(float* const* channels, int numChannels, int numFrames) noexcept override;
    int latencySamples() const noexcept override { return latency_.load(std::memory_order_relaxed); }
    std::vector<std::uint8_t> saveState() override;
    bool restoreState(const std::vector<std::uint8_t>& state) override;
    const InsertDescriptor& descriptor() const noexcept override { return options_.descriptor; }

    void showEditor();
    void hideEditor();
    std::uint32_t missedBlocks() const noexcept { return missed_.load(std::memory_order_relaxed); }
    const std::vector<std::uint8_t>& lastKnownState() const noexcept { return lastState_; }

private:
    class Coordinator;
    explicit SandboxedInsertProcessor(Options options);

    bool start(juce::String& error);
    juce::var request(const juce::var& message, int timeoutMs, juce::String& error);
    void onMessage(const juce::var& message);
    void onConnectionLost();

    Options options_;
    std::unique_ptr<Coordinator> coordinator_;
    SharedAudioChannel channel_;
    juce::String channelName_;
    int timeoutMicros_ = 2000;
    std::atomic<int> latency_{0};
    std::atomic<std::uint32_t> missed_{0};
    std::vector<std::uint8_t> lastState_;

    std::mutex mutex_;
    std::condition_variable replied_;
    std::map<int, juce::var> replies_;
    int nextRequestId_ = 1;
    bool connectionLost_ = false;
};

}  // namespace reka::app
