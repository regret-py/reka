// "REKA Test Gain": a deterministic VST3 used by the plugin-hosting integration test.
//   * multiplies by its gain parameter (default 0.5)
//   * delays by exactly kLatency samples and reports that latency (exercises PDC reporting)
//   * parameters: gain, and "crash" - when crash is on, processBlock aborts, which is how the test proves
//     a crashing plugin is contained by the sandbox.
#include <juce_audio_processors/juce_audio_processors.h>

#include <cstdlib>

class TestGainProcessor final : public juce::AudioProcessor {
public:
    static constexpr int kLatency = 64;

    TestGainProcessor()
        : AudioProcessor(BusesProperties()
                             .withInput("Input", juce::AudioChannelSet::stereo(), true)
                             .withOutput("Output", juce::AudioChannelSet::stereo(), true)) {
        addParameter(gain_ = new juce::AudioParameterFloat(juce::ParameterID{"gain", 1}, "Gain", 0.0f, 1.0f, 0.5f));
        addParameter(crash_ = new juce::AudioParameterBool(juce::ParameterID{"crash", 1}, "Crash", false));
        setLatencySamples(kLatency);
    }

    const juce::String getName() const override { return "REKA Test Gain"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override { return new juce::GenericAudioProcessorEditor(*this); }

    bool isBusesLayoutSupported(const BusesLayout& layouts) const override {
        return layouts.getMainInputChannelSet() == juce::AudioChannelSet::stereo() &&
               layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
    }

    void prepareToPlay(double, int) override {
        delay_.setSize(2, kLatency);
        delay_.clear();
        position_ = 0;
    }
    void releaseResources() override {}

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override {
        if (crash_->get()) std::abort();
        const float g = gain_->get();
        for (int i = 0; i < buffer.getNumSamples(); ++i) {
            for (int ch = 0; ch < 2; ++ch) {
                const float in = buffer.getSample(ch, i) * g;
                buffer.setSample(ch, i, delay_.getSample(ch, position_));
                delay_.setSample(ch, position_, in);
            }
            position_ = (position_ + 1) % kLatency;
        }
    }

    void getStateInformation(juce::MemoryBlock& data) override {
        juce::MemoryOutputStream stream(data, false);
        stream.writeFloat(gain_->get());
        stream.writeBool(crash_->get());
    }
    void setStateInformation(const void* data, int size) override {
        juce::MemoryInputStream stream(data, static_cast<size_t>(size), false);
        *gain_ = stream.readFloat();
        *crash_ = stream.readBool();
    }

private:
    juce::AudioParameterFloat* gain_ = nullptr;
    juce::AudioBuffer<float> delay_;
    int position_ = 0;
    juce::AudioParameterBool* crash_ = nullptr;
};

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new TestGainProcessor(); }
