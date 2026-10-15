#pragma once

#include "reka/common/Types.h"
#include "reka/engine/Parameters.h"
#include "reka/engine/StripProcessor.h"
#include "reka/hardware/PickupFilter.h"
#include "reka/hardware/SerialPort.h"

#include <nlohmann/json.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace reka {

// Which strip each physical channel controls. Built on the message thread from the MixerModel and the
// profile's hardware.map, then handed to the I/O thread as an immutable snapshot.
struct HardwareChannelBinding {
    StripId strip = kInvalidStrip;
    std::string name;
    std::shared_ptr<StripParameters> params;
    std::shared_ptr<StripProcessor> processor;  // meters
};

struct HardwareBindings {
    std::string profileName;
    std::vector<HardwareChannelBinding> channels;  // index == physical channel
    bool anySolo = false;  // unused by the I/O thread; solo state is read live from params
};

struct HardwareSettings {
    bool enabled = true;
    std::string port = "auto";  // "auto" or an explicit port ("COM7", "/dev/ttyACM0")
    int baud = 115200;
    std::vector<StripId> map;   // physical channel -> strip; empty = first N strips in mixer order
    int ledBrightness = 64;     // 0..255
    int oledContrast = 200;     // 0..255

    static HardwareSettings fromJson(const nlohmann::json& hardwareSection);
    void writeTo(nlohmann::json& hardwareSection) const;
};

enum class HardwareStatus : std::uint8_t { Stopped, Searching, Connected, Error };

struct HardwareDeviceInfo {
    std::string port;
    int protocolVersion = 0;
    int firmwareMajor = 0;
    int firmwareMinor = 0;
    int numChannels = 0;
    std::uint8_t capabilities = 0;
};

// Hardware Controller Bridge: owns a dedicated I/O thread that talks the RekaProtocol (COBS frames over
// USB serial) to a REKA.ino control surface.
//
//   device -> host : fader moves (with soft-takeover), mute/solo/fx/select buttons
//   host -> device : channel names, mute/solo/fx/selected flags, gain + pickup hint, 30 Hz meters,
//                    profile name, LED/OLED settings
//
// The I/O thread reads and writes StripParameters atomics directly (no locks shared with the audio
// thread) and reads meters through the MeterReader::Hardware slot. Any change made on screen is detected
// through StripParameters::revision() and mirrored to the device within one feedback tick (33 ms).
class RekaHardwareController {
public:
    using PortFactory = std::function<std::unique_ptr<ISerialPort>()>;
    using PortEnumerator = std::function<std::vector<SerialPortInfo>()>;

    explicit RekaHardwareController(PortFactory factory = createSerialPort,
                                    PortEnumerator enumerator = enumerateSerialPorts);
    ~RekaHardwareController();
    RekaHardwareController(const RekaHardwareController&) = delete;
    RekaHardwareController& operator=(const RekaHardwareController&) = delete;

    void start(const HardwareSettings& settings);
    void stop();

    // Any thread. Triggers a full resync of names/states on the device.
    void setBindings(std::shared_ptr<const HardwareBindings> bindings);
    void setSelectedChannel(int channel);

    HardwareStatus status() const noexcept { return status_.load(); }
    std::string statusText() const;
    HardwareDeviceInfo deviceInfo() const;

    // Last physical channel touched (for UI "follow hardware" selection). Counter increments per touch.
    int lastTouchedChannel() const noexcept { return lastTouched_.load(); }
    std::uint32_t touchCounter() const noexcept { return touchCounter_.load(); }
    std::uint32_t framesReceived() const noexcept { return framesReceived_.load(); }
    std::uint32_t frameErrors() const noexcept { return frameErrors_.load(); }

private:
    using Clock = std::chrono::steady_clock;

    struct ChannelState {
        PickupFilter pickup;
        float lastAppliedGainDb = 0.0f;
        bool gainKnown = false;
        bool clipLatched = false;
        Clock::time_point clipUntil{};
        // last values sent to the device
        int sentFlags = -1;
        int sentGain = 0x7FFFFFFF;
        int sentPickup = 0x7F;
        std::string sentName;
        bool nameSent = false;
    };

    void threadMain();
    std::unique_ptr<ISerialPort> connect();
    bool handshake(ISerialPort& port, const std::string& name);
    void runSession(ISerialPort& port);
    void handleFrame(std::uint8_t type, const std::uint8_t* payload, std::uint8_t size, ISerialPort& port);
    void handleFader(int channel, float position);
    void handleButton(int channel, int button, bool pressed);
    void sendFullState(ISerialPort& port);
    void sendFeedback(ISerialPort& port, bool forceAll);
    void sendMeters(ISerialPort& port);
    bool sendFrame(ISerialPort& port);
    std::shared_ptr<const HardwareBindings> currentBindings() const;
    void setStatus(HardwareStatus status, std::string text);

    PortFactory factory_;
    PortEnumerator enumerator_;
    HardwareSettings settings_;

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<HardwareStatus> status_{HardwareStatus::Stopped};

    mutable std::mutex mutex_;  // guards the members below (never touched by the audio thread)
    std::shared_ptr<const HardwareBindings> bindings_;
    std::string statusText_ = "Stopped";
    HardwareDeviceInfo device_;

    std::atomic<std::uint64_t> bindingsGeneration_{0};
    std::atomic<int> selected_{0};
    std::atomic<bool> selectedDirty_{true};
    std::atomic<int> lastTouched_{-1};
    std::atomic<std::uint32_t> touchCounter_{0};
    std::atomic<std::uint32_t> framesReceived_{0};
    std::atomic<std::uint32_t> frameErrors_{0};

    // I/O-thread state
    std::array<ChannelState, 16> channels_{};
    std::shared_ptr<const HardwareBindings> sessionBindings_;
    std::uint64_t sessionGeneration_ = 0;
    bool resyncRequested_ = false;
    int deviceChannels_ = 0;
    Clock::time_point lastHeard_{};
    std::uint8_t txBuffer_[64]{};
    std::uint8_t txLength_ = 0;
};

}  // namespace reka
