#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace reka::app {

// Shared by the in-process insert (REKA.exe) and the sandbox (REKA-PluginHost.exe): adapts REKA's
// fixed stereo strip buffers to whatever bus layout a plugin accepted (stereo, mono, 1-in/2-out, more
// channels), without allocating on the audio thread.
class PluginBlockAdapter {
public:
    // Message thread. Disables side-chains/aux buses, asks for stereo main buses (falls back to mono or
    // the plugin's own default), then calls prepareToPlay().
    static void configureLayout(juce::AudioPluginInstance& plugin);
    void prepare(juce::AudioPluginInstance& plugin, double sampleRate, int maxBlockSize);

    // Audio thread. Processes `channels` (stereo, in place). Never blocks: if the plugin's callback lock
    // is held by the message thread (state change in progress) the block passes through dry.
    void process(juce::AudioPluginInstance& plugin, float* const* channels, int numChannels, int numFrames) noexcept;

    int pluginInputs() const noexcept { return numInputs_; }
    int pluginOutputs() const noexcept { return numOutputs_; }

private:
    int numInputs_ = 2;
    int numOutputs_ = 2;
    int maxBlock_ = 0;
    juce::AudioBuffer<float> scratch_;
    juce::MidiBuffer midi_;
};

}  // namespace reka::app
