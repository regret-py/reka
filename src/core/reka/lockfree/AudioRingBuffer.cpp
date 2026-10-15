#include "reka/lockfree/AudioRingBuffer.h"

#include <algorithm>
#include <cstring>

namespace reka {

void AudioRingBuffer::allocate(int numChannels, int minCapacityFrames) {
    numChannels_ = std::max(1, numChannels);
    std::uint64_t capacity = 64;
    while (capacity < static_cast<std::uint64_t>(std::max(1, minCapacityFrames))) capacity <<= 1;
    capacity_ = capacity;
    mask_ = capacity - 1;
    data_.assign(static_cast<std::size_t>(capacity_) * static_cast<std::size_t>(numChannels_), 0.0f);
    writePos_.store(0, std::memory_order_relaxed);
    readPos_.store(0, std::memory_order_relaxed);
}

int AudioRingBuffer::availableToRead() const noexcept {
    const std::uint64_t w = writePos_.load(std::memory_order_acquire);
    const std::uint64_t r = readPos_.load(std::memory_order_acquire);
    return static_cast<int>(w - r);
}

int AudioRingBuffer::availableToWrite() const noexcept {
    return static_cast<int>(capacity_) - availableToRead();
}

int AudioRingBuffer::write(const float* const* source, int sourceChannels, int numFrames) noexcept {
    if (capacity_ == 0) return 0;
    const std::uint64_t w = writePos_.load(std::memory_order_relaxed);
    const std::uint64_t r = readPos_.load(std::memory_order_acquire);
    const int frames = std::min(numFrames, static_cast<int>(capacity_ - (w - r)));
    if (frames <= 0) return 0;

    const std::size_t start = static_cast<std::size_t>(w & mask_);
    const std::size_t first = std::min<std::size_t>(static_cast<std::size_t>(frames), capacity_ - start);
    const std::size_t second = static_cast<std::size_t>(frames) - first;

    for (int ch = 0; ch < numChannels_; ++ch) {
        const float* src = nullptr;
        if (source != nullptr && sourceChannels > 0)
            src = ch < sourceChannels ? source[ch] : (sourceChannels == 1 ? source[0] : nullptr);
        float* dst = channel(ch);
        if (src != nullptr) {
            std::memcpy(dst + start, src, first * sizeof(float));
            if (second > 0) std::memcpy(dst, src + first, second * sizeof(float));
        } else {
            std::memset(dst + start, 0, first * sizeof(float));
            if (second > 0) std::memset(dst, 0, second * sizeof(float));
        }
    }
    writePos_.store(w + static_cast<std::uint64_t>(frames), std::memory_order_release);
    return frames;
}

int AudioRingBuffer::writeInterleaved(const float* source, int sourceChannels, int numFrames) noexcept {
    if (capacity_ == 0 || sourceChannels <= 0) return 0;
    const std::uint64_t w = writePos_.load(std::memory_order_relaxed);
    const std::uint64_t r = readPos_.load(std::memory_order_acquire);
    const int frames = std::min(numFrames, static_cast<int>(capacity_ - (w - r)));
    if (frames <= 0) return 0;

    for (int ch = 0; ch < numChannels_; ++ch) {
        float* dst = channel(ch);
        const int srcCh = ch < sourceChannels ? ch : (sourceChannels == 1 ? 0 : -1);
        for (int i = 0; i < frames; ++i) {
            const std::size_t index = static_cast<std::size_t>((w + static_cast<std::uint64_t>(i)) & mask_);
            dst[index] = srcCh >= 0 ? source[static_cast<std::size_t>(i) * sourceChannels + srcCh] : 0.0f;
        }
    }
    writePos_.store(w + static_cast<std::uint64_t>(frames), std::memory_order_release);
    return frames;
}

int AudioRingBuffer::writeSilence(int numFrames) noexcept { return write(nullptr, 0, numFrames); }

int AudioRingBuffer::read(float* const* destination, int destChannels, int numFrames) noexcept {
    if (capacity_ == 0) return 0;
    const std::uint64_t r = readPos_.load(std::memory_order_relaxed);
    const std::uint64_t w = writePos_.load(std::memory_order_acquire);
    const int frames = std::min(numFrames, static_cast<int>(w - r));
    if (frames <= 0) return 0;

    const std::size_t start = static_cast<std::size_t>(r & mask_);
    const std::size_t first = std::min<std::size_t>(static_cast<std::size_t>(frames), capacity_ - start);
    const std::size_t second = static_cast<std::size_t>(frames) - first;

    for (int ch = 0; ch < destChannels; ++ch) {
        float* dst = destination[ch];
        if (dst == nullptr) continue;
        if (ch < numChannels_) {
            const float* src = channel(ch);
            std::memcpy(dst, src + start, first * sizeof(float));
            if (second > 0) std::memcpy(dst + first, src, second * sizeof(float));
        } else {
            std::memset(dst, 0, static_cast<std::size_t>(frames) * sizeof(float));
        }
    }
    readPos_.store(r + static_cast<std::uint64_t>(frames), std::memory_order_release);
    return frames;
}

int AudioRingBuffer::readInterleaved(float* destination, int destChannels, int numFrames) noexcept {
    if (capacity_ == 0 || destChannels <= 0) return 0;
    const std::uint64_t r = readPos_.load(std::memory_order_relaxed);
    const std::uint64_t w = writePos_.load(std::memory_order_acquire);
    const int frames = std::min(numFrames, static_cast<int>(w - r));
    if (frames <= 0) return 0;

    for (int ch = 0; ch < destChannels; ++ch) {
        // A stereo ring feeding a mono endpoint downmixes; extra endpoint channels get silence.
        if (ch >= numChannels_) {
            for (int i = 0; i < frames; ++i) destination[static_cast<std::size_t>(i) * destChannels + ch] = 0.0f;
            continue;
        }
        const float* src = channel(ch);
        for (int i = 0; i < frames; ++i) {
            const std::size_t index = static_cast<std::size_t>((r + static_cast<std::uint64_t>(i)) & mask_);
            destination[static_cast<std::size_t>(i) * destChannels + ch] = src[index];
        }
    }
    if (destChannels == 1 && numChannels_ >= 2) {
        const float* right = channel(1);
        for (int i = 0; i < frames; ++i) {
            const std::size_t index = static_cast<std::size_t>((r + static_cast<std::uint64_t>(i)) & mask_);
            destination[i] = 0.5f * (destination[i] + right[index]);
        }
    }
    readPos_.store(r + static_cast<std::uint64_t>(frames), std::memory_order_release);
    return frames;
}

int AudioRingBuffer::discard(int numFrames) noexcept {
    const std::uint64_t r = readPos_.load(std::memory_order_relaxed);
    const std::uint64_t w = writePos_.load(std::memory_order_acquire);
    const int frames = std::min(numFrames, static_cast<int>(w - r));
    if (frames <= 0) return 0;
    readPos_.store(r + static_cast<std::uint64_t>(frames), std::memory_order_release);
    return frames;
}

}  // namespace reka
