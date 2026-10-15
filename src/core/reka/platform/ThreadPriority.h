#pragma once

namespace reka {

// Raises the calling thread to real-time audio priority for its lifetime:
//   Windows : MMCSS "Pro Audio" task (AvSetMmThreadCharacteristicsW) + THREAD_PRIORITY_TIME_CRITICAL
//   POSIX   : SCHED_FIFO when permitted, otherwise best effort
// Used by every REKA-owned audio thread (software clock, virtual-endpoint pumps, process-loopback
// capture, plugin sandbox audio thread). Device threads owned by JUCE register themselves on first callback.
class ScopedAudioThreadPriority {
public:
    ScopedAudioThreadPriority() noexcept;
    ~ScopedAudioThreadPriority() noexcept;
    ScopedAudioThreadPriority(const ScopedAudioThreadPriority&) = delete;
    ScopedAudioThreadPriority& operator=(const ScopedAudioThreadPriority&) = delete;

    bool elevated() const noexcept { return elevated_; }

private:
    void* handle_ = nullptr;
    int previousPriority_ = 0;
    bool elevated_ = false;
};

}  // namespace reka
