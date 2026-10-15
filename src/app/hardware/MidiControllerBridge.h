#pragma once

#include "reka/engine/MixerModel.h"
#include "reka/hardware/PickupFilter.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace reka::app {

// Generic USB-MIDI control surfaces (and REKA.ino built with REKA_USB_MIDI=1): CC / note mappings with
// MIDI learn, soft takeover for non-motorised faders and value feedback (LED rings, button LEDs,
// motorised faders) on the matching MIDI output.
class MidiControllerBridge final : private juce::MidiInputCallback, private juce::Timer {
public:
    enum class Target { Gain, Pan, Mute, Solo, FxBypass };

    struct Mapping {
        int channel = 1;   // 1..16
        bool isNote = false;
        int number = 0;    // CC or note number
        StripId strip = kInvalidStrip;
        Target target = Target::Gain;
    };

    explicit MidiControllerBridge(MixerModel& mixer);
    ~MidiControllerBridge() override;

    juce::Array<juce::MidiDeviceInfo> availableInputs() const { return juce::MidiInput::getAvailableDevices(); }
    juce::Array<juce::MidiDeviceInfo> availableOutputs() const { return juce::MidiOutput::getAvailableDevices(); }
    bool openInput(const juce::String& identifier);
    bool openOutput(const juce::String& identifier);
    void closeAll();
    juce::String inputIdentifier() const { return inputId_; }
    juce::String outputIdentifier() const { return outputId_; }

    void setMappings(std::vector<Mapping> mappings);
    std::vector<Mapping> mappings() const;
    void startLearn(StripId strip, Target target);
    void cancelLearn();
    bool isLearning() const noexcept { return learning_.load(); }

    // Message thread: re-resolve strips after topology changes.
    void refreshBindings();

    nlohmann::json saveState() const;
    void restoreState(const nlohmann::json& midiSection);

    static const char* toString(Target target) noexcept;
    static Target targetFromString(const std::string& text) noexcept;

private:
    struct Binding {
        Mapping mapping;
        std::shared_ptr<StripParameters> params;
        PickupFilter pickup;
        int lastSent = -1;
    };
    using BindingList = std::vector<Binding>;

    void handleIncomingMidiMessage(juce::MidiInput* source, const juce::MidiMessage& message) override;
    void timerCallback() override;
    void rebuild();

    MixerModel& mixer_;
    std::unique_ptr<juce::MidiInput> input_;
    std::unique_ptr<juce::MidiOutput> output_;
    juce::String inputId_, outputId_;

    mutable std::mutex mutex_;  // MIDI thread <-> message thread (never the audio thread)
    std::vector<Mapping> mappings_;
    std::shared_ptr<BindingList> bindings_;

    std::atomic<bool> learning_{false};
    StripId learnStrip_ = kInvalidStrip;
    Target learnTarget_ = Target::Gain;
    std::atomic<int> learnedChannel_{0}, learnedNumber_{-1};
    std::atomic<bool> learnedIsNote_{false};
};

}  // namespace reka::app
