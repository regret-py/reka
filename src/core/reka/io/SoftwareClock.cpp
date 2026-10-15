#include "reka/io/SoftwareClock.h"

#include "reka/platform/ThreadPriority.h"

#include <chrono>

namespace reka {

void SoftwareClock::start(double sampleRate, int blockSize, Callback callback) {
    stop();
    callback_ = std::move(callback);
    running_.store(true);
    thread_ = std::thread([this, sampleRate, blockSize] { run(sampleRate, blockSize); });
}

void SoftwareClock::stop() {
    running_.store(false);
    if (thread_.joinable()) thread_.join();
}

void SoftwareClock::run(double sampleRate, int blockSize) {
    ScopedAudioThreadPriority priority;
    using Clock = std::chrono::steady_clock;
    const auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(blockSize / sampleRate));
    auto next = Clock::now() + period;
    while (running_.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_until(next);
        callback_(blockSize);
        next += period;
        // After a long stall (debugger, suspend) resynchronise instead of bursting.
        const auto now = Clock::now();
        if (now - next > period * 8) next = now + period;
    }
}

}  // namespace reka
