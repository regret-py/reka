#include <doctest/doctest.h>

#include "TestHelpers.h"
#include "reka/engine/MixerModel.h"
#include "reka/engine/RekaAudioEngine.h"

#include <atomic>
#include <cmath>
#include <thread>

using namespace reka;
using reka::test::GainDelayInsert;
using reka::test::VectorPort;

namespace {
constexpr int kBlock = 256;

struct Rig {
    RekaAudioEngine engine;
    MixerModel model{engine};
    std::shared_ptr<VectorPort> device = std::make_shared<VectorPort>(4, 4, kBlock);

    Rig() {
        model.prepare(48000.0, kBlock);
        PortInfo info;
        info.key = "device:test";
        info.displayName = "Test device";
        info.type = PortType::MasterDevice;
        model.registerPort(info, device);
    }

    StripId input(int left, int right) {
        const StripId id = model.addStrip(StripKind::Input, "in");
        model.setBinding(id, {"device:test", left, right});
        return id;
    }
    StripId bus(int left, int right) {
        const StripId id = model.addStrip(StripKind::Bus, "bus");
        model.setBinding(id, {"device:test", left, right});
        return id;
    }
    void run(int blocks = 60) {
        for (int i = 0; i < blocks; ++i) engine.processBlock(kBlock);
        model.housekeeping();
    }
    float out(int channel, int frame = kBlock - 1) const {
        return device->outputs[static_cast<std::size_t>(channel)][static_cast<std::size_t>(frame)];
    }
};
}  // namespace

TEST_CASE("Input routed to a bus reaches the device at unity gain") {
    Rig rig;
    const StripId in = rig.input(0, 1);
    const StripId bus = rig.bus(2, 3);
    rig.device->fill(0, 0.5f);
    rig.device->fill(1, -0.25f);
    rig.model.setRouteEnabled(in, bus, true);
    rig.run();
    CHECK(rig.out(2) == doctest::Approx(0.5f).epsilon(1e-3));
    CHECK(rig.out(3) == doctest::Approx(-0.25f).epsilon(1e-3));
    CHECK(rig.out(0) == 0.0f);  // nothing routed to channels 0/1
    CHECK(rig.engine.graphSwaps() >= 1);
}

TEST_CASE("Gain, mute, solo-in-place and polarity") {
    Rig rig;
    const StripId a = rig.input(0, 1);
    const StripId b = rig.input(2, 3);
    const StripId bus = rig.bus(0, 1);
    rig.device->fill(0, 0.1f);
    rig.device->fill(1, 0.1f);
    rig.device->fill(2, 0.2f);
    rig.device->fill(3, 0.2f);
    rig.model.setRouteEnabled(a, bus, true);
    rig.model.setRouteEnabled(b, bus, true);
    rig.run();
    CHECK(rig.out(0) == doctest::Approx(0.3f).epsilon(1e-3));

    rig.model.parameters(a)->setGainDb(-6.0206f);
    rig.run();
    CHECK(rig.out(0) == doctest::Approx(0.25f).epsilon(1e-3));

    rig.model.parameters(b)->setSolo(true);
    rig.run();
    CHECK(rig.out(0) == doctest::Approx(0.2f).epsilon(1e-3));

    rig.model.parameters(b)->setSolo(false);
    rig.model.parameters(b)->setMute(true);
    rig.run();
    CHECK(rig.out(0) == doctest::Approx(0.05f).epsilon(1e-3));

    rig.model.parameters(a)->setPhaseInvertLeft(true);
    rig.run();
    CHECK(rig.out(0) == doctest::Approx(-0.05f).epsilon(1e-3));
    CHECK(rig.out(1) == doctest::Approx(0.05f).epsilon(1e-3));
}

TEST_CASE("Mute is de-clicked: no step larger than the ramp allows") {
    Rig rig;
    const StripId in = rig.input(0, 1);
    const StripId bus = rig.bus(0, 1);
    rig.device->fill(0, 1.0f);
    rig.device->fill(1, 1.0f);
    rig.model.setRouteEnabled(in, bus, true);
    rig.run();
    rig.model.parameters(in)->setMute(true);
    rig.engine.processBlock(kBlock);
    const auto& out = rig.device->outputs[0];
    for (int i = 1; i < kBlock; ++i) CHECK(std::fabs(out[static_cast<std::size_t>(i)] - out[static_cast<std::size_t>(i - 1)]) < 0.01f);
    CHECK(out[kBlock - 1] > 0.0f);  // still fading
}

TEST_CASE("Stereo/mono summing and mono endpoints") {
    Rig rig;
    const StripId in = rig.input(0, 1);
    const StripId bus = rig.bus(2, 3);
    rig.device->fill(0, 1.0f);
    rig.device->fill(1, 0.0f);
    rig.model.setRouteEnabled(in, bus, true);
    rig.model.parameters(in)->setChannelMode(ChannelMode::Mono);
    rig.run();
    // (1 + 0) / 2 = 0.5 per side, then -3 dB centre pan for a mono strip.
    CHECK(rig.out(2) == doctest::Approx(0.5f * std::sqrt(0.5f)).epsilon(1e-3));
    CHECK(rig.out(3) == doctest::Approx(0.5f * std::sqrt(0.5f)).epsilon(1e-3));

    rig.model.parameters(in)->setChannelMode(ChannelMode::SwapLR);
    rig.run();
    CHECK(rig.out(2) == doctest::Approx(0.0f).epsilon(1e-3));
    CHECK(rig.out(3) == doctest::Approx(1.0f).epsilon(1e-3));

    // Mono sink: bus writes (L + R) / 2 into a single channel.
    rig.model.setBinding(bus, {"device:test", 2, -1});
    rig.run();
    CHECK(rig.out(2) == doctest::Approx(0.5f).epsilon(1e-3));
}

TEST_CASE("Pre-fader sends survive a muted strip") {
    Rig rig;
    const StripId in = rig.input(0, 1);
    const StripId monitor = rig.bus(2, 3);
    rig.device->fill(0, 0.5f);
    rig.device->fill(1, 0.5f);
    auto send = rig.model.route(in, monitor);
    send->setPreFader(true);
    send->setGainDb(-6.0206f);
    rig.model.setRouteEnabled(in, monitor, true);
    rig.model.parameters(in)->setMute(true);
    rig.run();
    CHECK(rig.out(2) == doctest::Approx(0.25f).epsilon(1e-3));
}

TEST_CASE("Plugin delay compensation aligns parallel paths") {
    Rig rig;
    const StripId slow = rig.input(0, 1);
    const StripId fast = rig.input(2, 3);
    const StripId bus = rig.bus(0, 1);
    auto plugin = std::make_shared<GainDelayInsert>(1.0f, 100);
    plugin->prepare(48000.0, kBlock);
    rig.model.addInsert(slow, std::make_shared<InsertSlot>(plugin));
    rig.model.setRouteEnabled(slow, bus, true);
    rig.model.setRouteEnabled(fast, bus, true);
    rig.run(80);  // ramps settle on silence

    // Same impulse on both inputs at frame 10 of the next block.
    for (int ch = 0; ch < 4; ++ch) rig.device->fill(ch, 0.0f);
    for (int ch = 0; ch < 4; ++ch) rig.device->inputs[static_cast<std::size_t>(ch)][10] = 0.5f;
    rig.engine.processBlock(kBlock);
    for (int ch = 0; ch < 4; ++ch) rig.device->fill(ch, 0.0f);
    CHECK(rig.out(0, 10) == doctest::Approx(0.0f));  // fast path is held back
    CHECK(rig.out(0, 110) == doctest::Approx(1.0f).epsilon(1e-3));  // both arrive together

    // Bypassing the plugin keeps the timing (latency-matched bypass).
    rig.model.strip(slow)->inserts[0]->bypassed.store(true);
    for (int ch = 0; ch < 4; ++ch) rig.device->inputs[static_cast<std::size_t>(ch)][10] = 0.5f;
    rig.engine.processBlock(kBlock);
    CHECK(rig.out(0, 10) == doctest::Approx(0.0f));
    CHECK(rig.out(0, 110) == doctest::Approx(1.0f).epsilon(1e-3));
}

TEST_CASE("A failed insert is treated as bypassed") {
    Rig rig;
    const StripId in = rig.input(0, 1);
    const StripId bus = rig.bus(0, 1);
    rig.device->fill(0, 0.5f);
    rig.device->fill(1, 0.5f);
    auto plugin = std::make_shared<GainDelayInsert>(0.0f, 0);  // mutes everything
    plugin->prepare(48000.0, kBlock);
    rig.model.addInsert(in, std::make_shared<InsertSlot>(plugin));
    rig.model.setRouteEnabled(in, bus, true);
    rig.run();
    CHECK(rig.out(0) == doctest::Approx(0.0f));
    plugin->fail();
    rig.run();
    CHECK(rig.out(0) == doctest::Approx(0.5f).epsilon(1e-3));
}

TEST_CASE("Topology edits while the audio thread runs (RCU graph swap stress)") {
    Rig rig;
    rig.device->fill(0, 0.1f);
    rig.device->fill(1, 0.1f);
    const StripId bus = rig.bus(0, 1);

    std::atomic<bool> running{true};
    std::atomic<long> blocks{0};
    std::thread audio([&] {
        while (running.load()) {
            rig.engine.processBlock(kBlock);
            ++blocks;
        }
    });

    std::vector<StripId> created;
    for (int i = 0; i < 300; ++i) {
        const StripId in = rig.input(0, 1);
        rig.model.setRouteEnabled(in, bus, true);
        auto plugin = std::make_shared<GainDelayInsert>(1.0f, i % 7 * 16);
        plugin->prepare(48000.0, kBlock);
        rig.model.addInsert(in, std::make_shared<InsertSlot>(plugin));
        created.push_back(in);
        if (created.size() > 8) {
            rig.model.removeStrip(created.front());
            created.erase(created.begin());
        }
        if (i % 3 == 0) rig.model.moveStrip(created.back(), 0);
        rig.model.housekeeping();
    }
    while (blocks.load() < 1000) std::this_thread::yield();
    running.store(false);
    audio.join();
    rig.model.housekeeping();
    CHECK(rig.engine.graphSwaps() > 1);
    CHECK(rig.model.strips(StripKind::Input).size() == 8);
}

TEST_CASE("Unbound and invalid bindings render silence instead of crashing") {
    Rig rig;
    const StripId in = rig.model.addStrip(StripKind::Input, "unbound");
    const StripId bus = rig.bus(0, 1);
    rig.model.setRouteEnabled(in, bus, true);
    rig.model.setBinding(in, {"device:test", 0, 9});  // channel 9 does not exist
    rig.device->fill(0, 1.0f);
    rig.run();
    CHECK(rig.out(0) == 0.0f);
    rig.model.unregisterPort("device:test");  // device unplugged
    rig.run();
    SUBCASE("the port comes back under the same key") {
        PortInfo info;
        info.key = "device:test";
        rig.model.registerPort(info, rig.device);
        rig.model.setBinding(in, {"device:test", 0, 1});
        rig.run();
        CHECK(rig.out(0) == doctest::Approx(1.0f).epsilon(1e-3));
    }
}
