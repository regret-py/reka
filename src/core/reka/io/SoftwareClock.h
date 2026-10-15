#pragma once

#include <atomic>
#include <functional>
#include <thread>

namespace reka {

// Drives the engine when no hardware master device is selected (e.g. a machine that only mixes
// virtual endpoints). Every endpoint is then an AsyncEndpointPort, so jitter here is absorbed by their
// rings and drift controllers.
class SoftwareClock {
public:
    using Callback = std::function<void(int numFrames)>;

    ~SoftwareClock() { stop(); }

    void start(double sampleRate, int blockSize, Callback callback);
    void stop();
    bool isRunning() const noexcept { return running_.load(); }

private:
    void run(double sampleRate, int blockSize);

    Callback callback_;
    std::thread thread_;
    std::atomic<bool> running_{false};
};

}  // namespace reka
