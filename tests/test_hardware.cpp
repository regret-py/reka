#include <doctest/doctest.h>

#include "RekaProtocol.h"
#include "reka/dsp/FaderLaw.h"
#include "reka/engine/MixerModel.h"
#include "reka/hardware/PickupFilter.h"
#include "reka/hardware/RekaHardwareController.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <thread>

using namespace reka;
using namespace std::chrono_literals;
namespace proto = reka_proto;

TEST_CASE("PickupFilter soft takeover") {
    PickupFilter pickup;
    pickup.setHardwarePosition(0.5f, 0.5f);
    CHECK(pickup.synced());
    CHECK(pickup.onHardwareMove(0.55f, 0.5f));

    pickup.onExternalChange(0.75f);  // UI moved the value up
    CHECK_FALSE(pickup.synced());
    CHECK(pickup.hint(0.75f) == 1);
    CHECK_FALSE(pickup.onHardwareMove(0.60f, 0.75f));
    CHECK_FALSE(pickup.onHardwareMove(0.70f, 0.75f));
    CHECK(pickup.onHardwareMove(0.80f, 0.75f));  // crossed
    CHECK(pickup.hint(0.80f) == 0);
}

namespace {
// Two byte queues standing in for a USB CDC link.
struct FakeLink {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::uint8_t> toDevice, toHost;
    bool open = false;
};

class FakeHostPort final : public ISerialPort {
public:
    explicit FakeHostPort(std::shared_ptr<FakeLink> link) : link_(std::move(link)) {}
    bool open(const std::string&, int, std::string&) override {
        std::lock_guard<std::mutex> lock(link_->mutex);
        link_->open = true;
        open_ = true;
        return true;
    }
    void close() override { open_ = false; }
    bool isOpen() const override { return open_; }
    int read(std::uint8_t* buffer, int maxBytes, int timeoutMs) override {
        std::unique_lock<std::mutex> lock(link_->mutex);
        link_->cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] { return !link_->toHost.empty(); });
        int n = 0;
        while (n < maxBytes && !link_->toHost.empty()) {
            buffer[n++] = link_->toHost.front();
            link_->toHost.pop_front();
        }
        return n;
    }
    bool write(const std::uint8_t* data, int size) override {
        std::lock_guard<std::mutex> lock(link_->mutex);
        link_->toDevice.insert(link_->toDevice.end(), data, data + size);
        link_->cv.notify_all();
        return true;
    }

private:
    std::shared_ptr<FakeLink> link_;
    bool open_ = false;
};

// Minimal REKA.ino behaviour model.
class DeviceSimulator {
public:
    explicit DeviceSimulator(std::shared_ptr<FakeLink> link) : link_(std::move(link)) {
        thread_ = std::thread([this] { run(); });
    }
    ~DeviceSimulator() {
        running_ = false;
        link_->cv.notify_all();
        thread_.join();
    }

    void sendFader(int channel, float position) {
        proto::FrameWriter w;
        w.begin(proto::MSG_FADER);
        w.u8(static_cast<std::uint8_t>(channel));
        w.u16(static_cast<std::uint16_t>(position * 65535.0f + 0.5f));
        send(w);
        positions_[channel] = position;
    }
    void sendButton(int channel, int button) {
        proto::FrameWriter w;
        w.begin(proto::MSG_BUTTON);
        w.u8(static_cast<std::uint8_t>(channel));
        w.u8(static_cast<std::uint8_t>(button));
        w.u8(1);
        send(w);
    }

    struct ChannelView {
        std::string name;
        int flags = -1;
        int gain = 0;
        int pickup = 0;
    };
    ChannelView channel(int index) {
        std::lock_guard<std::mutex> lock(stateMutex_);
        return channels_[index];
    }
    std::string profileName() {
        std::lock_guard<std::mutex> lock(stateMutex_);
        return profile_;
    }
    int meterFrames() const { return meterFrames_.load(); }

    std::map<int, float> positions_{{0, 0.75f}, {1, 0.75f}};

private:
    void send(proto::FrameWriter& w) {
        std::uint8_t frame[proto::kMaxEncodedFrame];
        const std::uint8_t n = w.finish(frame);
        std::lock_guard<std::mutex> lock(link_->mutex);
        link_->toHost.insert(link_->toHost.end(), frame, frame + n);
        link_->cv.notify_all();
    }

    void run() {
        proto::FrameParser parser;
        while (running_) {
            std::deque<std::uint8_t> bytes;
            {
                std::unique_lock<std::mutex> lock(link_->mutex);
                link_->cv.wait_for(lock, 5ms, [&] { return !link_->toDevice.empty() || !running_; });
                bytes.swap(link_->toDevice);
            }
            for (std::uint8_t b : bytes) {
                if (!parser.feed(b)) continue;
                handle(parser.type(), parser.payload(), parser.payloadSize());
            }
        }
    }

    void handle(std::uint8_t type, const std::uint8_t* p, std::uint8_t size) {
        switch (type) {
            case proto::MSG_HELLO_REQUEST: {
                proto::FrameWriter w;
                w.begin(proto::MSG_HELLO);
                w.u8(proto::kProtocolVersion);
                w.u8(1);
                w.u8(0);
                w.u8(2);  // two physical channels
                w.u8(proto::CAP_FADERS | proto::CAP_BUTTONS | proto::CAP_LEDS | proto::CAP_OLED);
                for (std::uint8_t c : proto::kMagic) w.u8(c);
                send(w);
                proto::FrameWriter s;
                s.begin(proto::MSG_FADER_SNAPSHOT);
                s.u8(0);
                s.u8(2);
                for (int ch = 0; ch < 2; ++ch) s.u16(static_cast<std::uint16_t>(positions_[ch] * 65535.0f + 0.5f));
                send(s);
                break;
            }
            case proto::MSG_PING: {
                proto::FrameWriter w;
                w.begin(proto::MSG_PONG);
                w.u8(p[0]);
                send(w);
                break;
            }
            case proto::MSG_CHANNEL_NAME: {
                std::lock_guard<std::mutex> lock(stateMutex_);
                channels_[p[0]].name.assign(reinterpret_cast<const char*>(p + 2), p[1]);
                break;
            }
            case proto::MSG_CHANNEL_STATE: {
                std::lock_guard<std::mutex> lock(stateMutex_);
                auto& c = channels_[p[0]];
                c.flags = p[1];
                c.gain = proto::readI16(p + 2);
                c.pickup = static_cast<std::int8_t>(p[4]);
                break;
            }
            case proto::MSG_PROFILE_NAME: {
                std::lock_guard<std::mutex> lock(stateMutex_);
                profile_.assign(reinterpret_cast<const char*>(p + 1), p[0]);
                break;
            }
            case proto::MSG_METERS: meterFrames_++; break;
            default: break;
        }
        (void)size;
    }

    std::shared_ptr<FakeLink> link_;
    std::thread thread_;
    std::atomic<bool> running_{true};
    std::mutex stateMutex_;
    std::map<int, ChannelView> channels_;
    std::string profile_;
    std::atomic<int> meterFrames_{0};
};

bool waitFor(const std::function<bool()>& predicate, std::chrono::milliseconds timeout = 3000ms) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return predicate();
}
}  // namespace

TEST_CASE("Hardware bridge: handshake, names, faders with pickup, buttons, meters") {
    RekaAudioEngine engine;
    MixerModel model(engine);
    model.prepare(48000.0, 256);
    const StripId mic = model.addStrip(StripKind::Input, "Microphone");
    const StripId game = model.addStrip(StripKind::Input, "Game");

    auto bindings = std::make_shared<HardwareBindings>();
    bindings->profileName = "Streaming";
    for (StripId id : {mic, game})
        bindings->channels.push_back({id, model.strip(id)->name, model.parameters(id), model.processor(id)});

    auto link = std::make_shared<FakeLink>();
    DeviceSimulator device(link);
    RekaHardwareController controller([link] { return std::make_unique<FakeHostPort>(link); },
                                      [] { return std::vector<SerialPortInfo>{{"FAKE0", "fake", "", true}}; });
    controller.setBindings(bindings);
    HardwareSettings settings;
    controller.start(settings);

    REQUIRE(waitFor([&] { return controller.status() == HardwareStatus::Connected; }));
    CHECK(controller.deviceInfo().numChannels == 2);
    REQUIRE(waitFor([&] { return device.channel(1).name == "Game"; }));
    CHECK(device.channel(0).name == "Micropho");  // truncated to the OLED's 8 characters
    CHECK(device.profileName() == "Streaming");
    CHECK((device.channel(0).flags & proto::FLAG_MAPPED) != 0);

    // Fader synced at 0 dB (position 0.75): a move applies immediately.
    REQUIRE(waitFor([&] { return device.channel(0).gain == 0; }));
    device.sendFader(0, 0.5f);
    REQUIRE(waitFor([&] { return std::fabs(model.parameters(mic)->gainDb() + 10.0f) < 0.05f; }));
    REQUIRE(waitFor([&] { return device.channel(0).gain == -100; }));

    // On-screen change: the fader is no longer in control and the device is told to move up.
    model.parameters(mic)->setGainDb(0.0f);
    REQUIRE(waitFor([&] { return device.channel(0).pickup == proto::PICKUP_MOVE_UP; }));
    device.sendFader(0, 0.6f);
    std::this_thread::sleep_for(100ms);
    CHECK(model.parameters(mic)->gainDb() == doctest::Approx(0.0f));  // ignored: not picked up yet
    device.sendFader(0, 0.8f);                                         // crosses 0.75 -> takes over
    REQUIRE(waitFor([&] { return std::fabs(model.parameters(mic)->gainDb() - FaderLaw::positionToDb(0.8f)) < 0.05f; }));
    REQUIRE(waitFor([&] { return device.channel(0).pickup == proto::PICKUP_SYNCED; }));

    // Buttons toggle and the LEDs follow the host state.
    device.sendButton(1, proto::BUTTON_MUTE);
    REQUIRE(waitFor([&] { return model.parameters(game)->mute(); }));
    REQUIRE(waitFor([&] { return (device.channel(1).flags & proto::FLAG_MUTE) != 0; }));
    model.parameters(game)->setSolo(true);  // solo on screen -> mic is implicitly muted
    REQUIRE(waitFor([&] { return (device.channel(0).flags & proto::FLAG_SOLO_IMPLIED_MUTE) != 0; }));
    CHECK(controller.lastTouchedChannel() == 1);

    REQUIRE(waitFor([&] { return device.meterFrames() > 5; }));
    controller.stop();
    CHECK(controller.status() == HardwareStatus::Stopped);
}
