#pragma once

#include "reka/engine/InsertProcessor.h"
#include "vst/PluginBlockAdapter.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <functional>

namespace reka::app {

// A VST3 plugin hosted inside REKA.exe (lowest overhead; a crashing plugin takes REKA down with it -
// use SandboxedInsertProcessor for untrusted plugins).
class JuceInsertProcessor final : public IInsertProcessor, private juce::AudioProcessorListener {
public:
    JuceInsertProcessor(std::unique_ptr<juce::AudioPluginInstance> plugin, InsertDescriptor descriptor,
                        std::function<void()> onLatencyChanged);
    ~JuceInsertProcessor() override;

    void prepare(double sampleRate, int maxBlockSize) override;
    void release() override;
    void process(float* const* channels, int numChannels, int numFrames) noexcept override;
    int latencySamples() const noexcept override { return latency_.load(std::memory_order_relaxed); }
    std::vector<std::uint8_t> saveState() override;
    bool restoreState(const std::vector<std::uint8_t>& state) override;
    const InsertDescriptor& descriptor() const noexcept override { return descriptor_; }

    juce::AudioPluginInstance& plugin() noexcept { return *plugin_; }

    // Called from the destructor (message thread) so editor windows close before the plugin dies.
    std::function<void(JuceInsertProcessor*)> onDestroyed;

private:
    void audioProcessorParameterChanged(juce::AudioProcessor*, int, float) override {}
    void audioProcessorChanged(juce::AudioProcessor*, const ChangeDetails& details) override;

    std::unique_ptr<juce::AudioPluginInstance> plugin_;
    InsertDescriptor descriptor_;
    std::function<void()> onLatencyChanged_;
    PluginBlockAdapter adapter_;
    std::atomic<int> latency_{0};
    bool prepared_ = false;
};

}  // namespace reka::app
