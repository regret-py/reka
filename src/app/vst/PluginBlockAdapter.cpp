#include "vst/PluginBlockAdapter.h"

namespace reka::app {

void PluginBlockAdapter::configureLayout(juce::AudioPluginInstance& plugin) {
    plugin.disableNonMainBuses();
    auto layout = plugin.getBusesLayout();
    const bool hasInput = !layout.inputBuses.isEmpty();
    const bool hasOutput = !layout.outputBuses.isEmpty();
    if (!hasOutput) return;

    auto tryLayout = [&](const juce::AudioChannelSet& in, const juce::AudioChannelSet& out) {
        auto candidate = layout;
        if (hasInput) candidate.inputBuses.getReference(0) = in;
        candidate.outputBuses.getReference(0) = out;
        return plugin.checkBusesLayoutSupported(candidate) && plugin.setBusesLayout(candidate);
    };
    if (tryLayout(juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo())) return;
    if (tryLayout(juce::AudioChannelSet::mono(), juce::AudioChannelSet::stereo())) return;
    tryLayout(juce::AudioChannelSet::mono(), juce::AudioChannelSet::mono());
    // Otherwise keep the plugin's preferred layout; process() maps channels generically.
}

void PluginBlockAdapter::prepare(juce::AudioPluginInstance& plugin, double sampleRate, int maxBlockSize) {
    plugin.setRateAndBufferSizeDetails(sampleRate, maxBlockSize);
    plugin.prepareToPlay(sampleRate, maxBlockSize);
    numInputs_ = plugin.getTotalNumInputChannels();
    numOutputs_ = plugin.getTotalNumOutputChannels();
    maxBlock_ = maxBlockSize;
    scratch_.setSize(juce::jmax(2, numInputs_, numOutputs_), maxBlockSize, false, true, false);
    midi_.ensureSize(512);
}

void PluginBlockAdapter::process(juce::AudioPluginInstance& plugin, float* const* channels, int numChannels,
                                 int numFrames) noexcept {
    if (numFrames > maxBlock_ || numChannels < 2) return;
    const juce::ScopedTryLock lock(plugin.getCallbackLock());
    if (!lock.isLocked() || plugin.isSuspended()) return;

    if (numInputs_ == 2 && numOutputs_ == 2) {
        // Fast path: run the plugin directly on REKA's buffers.
        juce::AudioBuffer<float> buffer(channels, 2, numFrames);
        plugin.processBlock(buffer, midi_);
        midi_.clear();
        return;
    }

    const int scratchChannels = scratch_.getNumChannels();
    for (int ch = 0; ch < scratchChannels; ++ch) scratch_.clear(ch, 0, numFrames);
    if (numInputs_ == 1) {
        float* mono = scratch_.getWritePointer(0);
        for (int i = 0; i < numFrames; ++i) mono[i] = 0.5f * (channels[0][i] + channels[1][i]);
    } else {
        for (int ch = 0; ch < juce::jmin(2, numInputs_); ++ch) scratch_.copyFrom(ch, 0, channels[ch], numFrames);
    }

    juce::AudioBuffer<float> view(scratch_.getArrayOfWritePointers(), scratchChannels, numFrames);
    plugin.processBlock(view, midi_);
    midi_.clear();

    if (numOutputs_ >= 2) {
        juce::FloatVectorOperations::copy(channels[0], scratch_.getReadPointer(0), numFrames);
        juce::FloatVectorOperations::copy(channels[1], scratch_.getReadPointer(1), numFrames);
    } else if (numOutputs_ == 1) {
        juce::FloatVectorOperations::copy(channels[0], scratch_.getReadPointer(0), numFrames);
        juce::FloatVectorOperations::copy(channels[1], scratch_.getReadPointer(0), numFrames);
    }
}

}  // namespace reka::app
