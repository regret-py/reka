#include <doctest/doctest.h>

#include "reka/platform/SharedAudioChannel.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#endif

using namespace reka;

namespace {
std::string uniqueName(const char* tag) {
#if defined(_WIN32)
    return std::string("reka-test-") + tag;
#else
    return std::string("reka-test-") + tag + "-" + std::to_string(getpid());
#endif
}
}  // namespace

TEST_CASE("SharedAudioChannel round trip with a plugin-host thread") {
    SharedAudioChannel host;
    std::string error;
    const std::string name = uniqueName("rt");
    REQUIRE_MESSAGE(host.create(name, 512, error), error);

    std::atomic<bool> stop{false};
    std::atomic<bool> ready{false};
    std::atomic<int> stall{0};
    std::thread pluginHost([&] {
        SharedAudioChannel client;
        std::string e;
        if (!client.open(name, e)) return;
        client.setLatency(64);
        ready.store(true);
        while (!stop.load()) {
            const int frames = client.waitForRequest(20);
            if (frames < 0) break;
            if (frames == 0) continue;
            if (stall.load() > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(stall.load()));
            }
            for (int ch = 0; ch < client.requestChannels(); ++ch)
                for (int i = 0; i < frames; ++i) client.channel(ch)[i] *= 2.0f;  // the "plugin"
            client.completeRequest();
        }
    });

    // Always stop and join the plugin-host thread, even when an assertion aborts the test case.
    struct Joiner {
        std::thread& thread;
        std::atomic<bool>& stop;
        SharedAudioChannel& channel;
        ~Joiner() {
            stop.store(true);
            channel.requestShutdown();
            if (thread.joinable()) thread.join();
        }
    } joiner{pluginHost, stop, host};

    std::vector<float> l(256, 0.25f), r(256, -0.5f);
    float* ch[2] = {l.data(), r.data()};
    for (int i = 0; i < 500 && !ready.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    REQUIRE(ready.load());
    for (int block = 0; block < 200; ++block) {
        std::fill(l.begin(), l.end(), 0.25f);
        std::fill(r.begin(), r.end(), -0.5f);
        REQUIRE(host.process(ch, 2, 256, 200000) == SharedAudioChannel::Result::Ok);
        CHECK(l[100] == doctest::Approx(0.5f));
        CHECK(r[255] == doctest::Approx(-1.0f));
    }
    CHECK(host.latency() == 64);

    // A stalled plugin: the host times out, leaves audio dry and recovers once the plugin catches up.
    stall.store(30);
    std::fill(l.begin(), l.end(), 0.25f);
    CHECK(host.process(ch, 2, 256, 2000) == SharedAudioChannel::Result::Timeout);
    CHECK(l[0] == doctest::Approx(0.25f));
    CHECK(host.process(ch, 2, 256, 2000) == SharedAudioChannel::Result::Timeout);  // still busy: buffer untouched
    stall.store(0);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    std::fill(l.begin(), l.end(), 0.25f);
    CHECK(host.process(ch, 2, 256, 200000) == SharedAudioChannel::Result::Ok);
    CHECK(l[0] == doctest::Approx(0.5f));

    host.requestShutdown();
    stop.store(true);
    pluginHost.join();
    CHECK(host.process(ch, 2, 256, 1000) == SharedAudioChannel::Result::Disconnected);
}
