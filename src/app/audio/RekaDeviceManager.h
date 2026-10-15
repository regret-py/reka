#pragma once

#include "reka/engine/MixerModel.h"
#include "reka/io/AsyncEndpointPort.h"
#include "reka/io/DirectDevicePort.h"
#include "reka/io/SoftwareClock.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <memory>
#include <optional>
#include <vector>

namespace reka::app {

// The device whose callback clocks the engine. ASIO / WASAPI exclusive / WASAPI low-latency are the
// natural choices; an empty type selects the software clock (virtual-only setups).
struct MasterSetup {
    juce::String type;
    juce::String input;
    juce::String output;
    double sampleRate = 48000.0;
    int bufferSize = 256;

    bool operator==(const MasterSetup& other) const {
        return type == other.type && input == other.input && output == other.output &&
               juce::approximatelyEqual(sampleRate, other.sampleRate) && bufferSize == other.bufferSize;
    }
    nlohmann::json toJson() const;
    static MasterSetup fromJson(const nlohmann::json& json);
};

// Any additional device direction (a USB headset, a second interface, a VB-CABLE end...). It runs on its
// own clock and is locked to the engine through an AsyncEndpointPort (drift-corrected resampling).
struct SecondarySetup {
    juce::String type = "Windows Audio";
    juce::String device;
    bool isInput = false;
    int bufferSize = 0;  // 0 = device default

    PortKey key() const { return (isInput ? "in:" : "out:") + device.toStdString(); }
    nlohmann::json toJson() const;
    static SecondarySetup fromJson(const nlohmann::json& json);
};

class RekaDeviceManager final : private juce::AudioIODeviceCallback,
                                private juce::AudioIODeviceType::Listener,
                                private juce::Timer {
public:
    struct Listener {
        virtual ~Listener() = default;
        virtual void engineFormatWillChange() {}
        virtual void engineFormatChanged(double sampleRate, int blockSize) { juce::ignoreUnused(sampleRate, blockSize); }
        virtual void deviceStateChanged() {}
    };

    static constexpr const char* kMasterPortKey = "master";

    explicit RekaDeviceManager(MixerModel& mixer);
    ~RekaDeviceManager() override;

    void addListener(Listener* listener) { listeners_.add(listener); }
    void removeListener(Listener* listener) { listeners_.remove(listener); }

    // ---- discovery ----
    juce::StringArray typeNames() const;
    juce::StringArray deviceNames(const juce::String& type, bool inputs) const;
    juce::Array<double> sampleRates(const MasterSetup& setup) const;
    juce::Array<int> bufferSizes(const MasterSetup& setup) const;

    // ---- master ----
    juce::String setMaster(const MasterSetup& setup);  // returns an error message, empty on success
    const MasterSetup& master() const noexcept { return masterSetup_; }
    bool isMasterRunning() const;
    juce::String masterStatus() const;
    juce::StringArray masterChannelNames(bool inputs) const;

    // ---- secondary devices ----
    juce::String addSecondary(const SecondarySetup& setup);
    void removeSecondary(const PortKey& key);
    std::vector<SecondarySetup> secondaries() const;
    juce::String secondaryStatus(const PortKey& key) const;
    std::optional<AsyncEndpointPort::Stats> secondaryStats(const PortKey& key) const;

    // ---- engine format ----
    double sampleRate() const noexcept { return sampleRate_; }
    int blockSize() const noexcept { return blockSize_; }
    double outputLatencyMs() const;
    int xruns() const;

    // ---- persistence ----
    nlohmann::json saveState() const;
    juce::String restoreState(const nlohmann::json& audioSection);  // returns warnings

    void shutdown();

private:
    struct Secondary;

    void audioDeviceIOCallbackWithContext(const float* const* inputs, int numInputs, float* const* outputs, int numOutputs,
                                          int numSamples, const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart(juce::AudioIODevice*) override {}
    void audioDeviceStopped() override {}
    void audioDeviceError(const juce::String& message) override;
    void audioDeviceListChanged() override;
    void timerCallback() override;

    juce::AudioIODeviceType* findType(const juce::String& name) const;
    juce::String openSecondary(Secondary& secondary);
    void stopAll();
    void startAll();

    MixerModel& mixer_;
    juce::OwnedArray<juce::AudioIODeviceType> types_;
    MasterSetup masterSetup_;
    std::unique_ptr<juce::AudioIODevice> masterDevice_;
    std::shared_ptr<DirectDevicePort> masterPort_;
    SoftwareClock softwareClock_;
    juce::String masterError_;
    std::atomic<bool> masterErrored_{false};
    std::vector<std::unique_ptr<Secondary>> secondaries_;
    juce::ListenerList<Listener> listeners_;
    double sampleRate_ = 48000.0;
    int blockSize_ = 256;
};

}  // namespace reka::app
