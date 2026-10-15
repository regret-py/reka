#pragma once

#include "AppSettings.h"
#include "audio/RekaDeviceManager.h"
#include "audio/VirtualIoManager.h"
#include "hardware/MidiControllerBridge.h"
#include "reka/engine/MixerModel.h"
#include "reka/engine/RekaAudioEngine.h"
#include "reka/hardware/RekaHardwareController.h"
#include "reka/virtualaudio/RekaVirtualAudio.h"
#include "vst/RekaVstHost.h"

#include <juce_events/juce_events.h>

#include <functional>
#include <map>
#include <memory>

namespace reka::app {

class ProfileService;

// Composition root. Owns every subsystem, wires them together and runs the message-thread housekeeping
// timer. Threads in play:
//   audio thread     : RekaAudioEngine::processBlock (master device callback or software clock)
//   device threads   : secondary devices, virtual endpoint pumps, process-loopback captures
//   hardware thread  : RekaHardwareController serial I/O
//   MIDI thread      : MidiControllerBridge
//   sandbox IPC      : one per sandboxed plugin
//   message thread   : UI, MixerModel, profiles, this timer
class AppServices final : public juce::ChangeBroadcaster, private juce::Timer, private RekaDeviceManager::Listener {
public:
    AppServices();
    ~AppServices() override;

    void initialise(const juce::String& commandLine);
    void shutdown();

    AppSettings& settings() noexcept { return settings_; }
    RekaAudioEngine& engine() noexcept { return engine_; }
    MixerModel& mixer() noexcept { return mixer_; }
    RekaDeviceManager& devices() noexcept { return *devices_; }
    VirtualIoManager& virtualIo() noexcept { return *virtualIo_; }
    RekaVstHost& plugins() noexcept { return *plugins_; }
    RekaHardwareController& hardware() noexcept { return hardware_; }
    MidiControllerBridge& midi() noexcept { return *midi_; }
    ProfileService& profiles() noexcept { return *profiles_; }

    // Control surface
    const HardwareSettings& hardwareSettings() const noexcept { return hardwareSettings_; }
    void applyHardwareSettings(const HardwareSettings& settings);
    void refreshHardwareBindings();

    // Inserts (message thread)
    juce::String addPlugin(StripId strip, const juce::PluginDescription& description, bool sandboxed, int index = -1);

    // UI hooks installed by the GUI engine (layout & theme live in the profile's "ui" section).
    std::function<nlohmann::json()> captureUiState;
    std::function<void(const nlohmann::json&)> applyUiState;

    // Event log for the status bar.
    void log(const juce::String& message);
    juce::StringArray recentLog() const { return log_; }

private:
    void timerCallback() override;
    void engineFormatChanged(double sampleRate, int blockSize) override;
    void restartFailedInserts();

    AppSettings settings_;
    RekaAudioEngine engine_;
    MixerModel mixer_{engine_};
    RekaVirtualAudio virtualAudio_;
    std::unique_ptr<RekaDeviceManager> devices_;
    std::unique_ptr<VirtualIoManager> virtualIo_;
    std::unique_ptr<RekaVstHost> plugins_;
    RekaHardwareController hardware_;
    HardwareSettings hardwareSettings_;
    std::unique_ptr<MidiControllerBridge> midi_;
    std::unique_ptr<ProfileService> profiles_;

    std::uint64_t lastTopology_ = 0;
    std::map<std::string, std::pair<int, juce::uint32>> restartHistory_;  // key -> (count, first restart ms)
    juce::StringArray log_;
    bool shutDown_ = false;
};

}  // namespace reka::app
