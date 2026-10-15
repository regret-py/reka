#pragma once

#include "reka/lockfree/SpscQueue.h"

#include <atomic>
#include <cstdint>
#include <vector>

namespace reka {

// Planar multi-channel float FIFO, single producer / single consumer, lock-free.
// Producer and consumer run on different clocks (device threads, virtual-endpoint pumps, the engine).
class AudioRingBuffer {
public:
    // Non-real-time. Not safe while either side is active.
    void allocate(int numChannels, int minCapacityFrames);

    int numChannels() const noexcept { return numChannels_; }
    int capacity() const noexcept { return static_cast<int>(capacity_); }

    int availableToRead() const noexcept;
    int availableToWrite() const noexcept;

    // ---- producer ----
    // Missing source channels are zero-filled (or duplicated from channel 0 when the source is mono).
    int write(const float* const* source, int sourceChannels, int numFrames) noexcept;
    int writeInterleaved(const float* source, int sourceChannels, int numFrames) noexcept;
    int writeSilence(int numFrames) noexcept;

    // ---- consumer ----
    // Returns the number of frames actually read; the caller decides how to handle a short read.
    int read(float* const* destination, int destChannels, int numFrames) noexcept;
    int readInterleaved(float* destination, int destChannels, int numFrames) noexcept;
    int discard(int numFrames) noexcept;

private:
    float* channel(int ch) noexcept { return data_.data() + static_cast<std::size_t>(ch) * capacity_; }
    const float* channel(int ch) const noexcept {
        return data_.data() + static_cast<std::size_t>(ch) * capacity_;
    }

    std::vector<float> data_;
    std::uint64_t capacity_ = 0;
    std::uint64_t mask_ = 0;
    int numChannels_ = 0;

    alignas(kCacheLineSize) std::atomic<std::uint64_t> writePos_{0};
    alignas(kCacheLineSize) std::atomic<std::uint64_t> readPos_{0};
};

}  // namespace reka
