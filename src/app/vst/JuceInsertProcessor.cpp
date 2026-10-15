#include "vst/JuceInsertProcessor.h"

namespace reka::app {

JuceInsertProcessor::JuceInsertProcessor(std::unique_ptr<juce::AudioPluginInstance> plugin, InsertDescriptor descriptor,
                                         std::function<void()> onLatencyChanged)
    : plugin_(std::move(plugin)), descriptor_(std::move(descriptor)), onLatencyChanged_(std::move(onLatencyChanged)) {
    PluginBlockAdapter::configureLayout(*plugin_);
    plugin_->addListener(this);
}

JuceInsertProcessor::~JuceInsertProcessor() {
    if (onDestroyed) onDestroyed(this);
    plugin_->removeListener(this);
    release();
}

void JuceInsertProcessor::prepare(double sampleRate, int maxBlockSize) {
    adapter_.prepare(*plugin_, sampleRate, maxBlockSize);
    latency_.store(plugin_->getLatencySamples());
    prepared_ = true;
}

void JuceInsertProcessor::release() {
    if (prepared_) plugin_->releaseResources();
    prepared_ = false;
}

void JuceInsertProcessor::process(float* const* channels, int numChannels, int numFrames) noexcept {
    if (prepared_) adapter_.process(*plugin_, channels, numChannels, numFrames);
}

std::vector<std::uint8_t> JuceInsertProcessor::saveState() {
    juce::MemoryBlock block;
    plugin_->getStateInformation(block);
    const auto* bytes = static_cast<const std::uint8_t*>(block.getData());
    return {bytes, bytes + block.getSize()};
}

bool JuceInsertProcessor::restoreState(const std::vector<std::uint8_t>& state) {
    if (state.empty()) return true;
    plugin_->setStateInformation(state.data(), static_cast<int>(state.size()));
    return true;
}

void JuceInsertProcessor::audioProcessorChanged(juce::AudioProcessor* processor, const ChangeDetails& details) {
    // May arrive on any thread (often the audio thread): only touch atomics here.
    if (!details.latencyChanged) return;
    latency_.store(processor->getLatencySamples(), std::memory_order_relaxed);
    if (onLatencyChanged_) onLatencyChanged_();
}

}  // namespace reka::app
