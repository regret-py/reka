#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <type_traits>

namespace reka {

inline constexpr std::size_t kCacheLineSize = 64;

// Bounded single-producer / single-consumer queue. Wait-free on both sides, no allocation after
// construction. Used for every real-time <-> non-real-time hand-off (graph retirement, loudness blocks,
// hardware events).
template <typename T, std::size_t Capacity>
class SpscQueue {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");
    static_assert(std::is_nothrow_copy_assignable_v<T>, "T must be nothrow copy-assignable");

public:
    // Producer side.
    bool push(const T& value) noexcept {
        const std::size_t write = write_.load(std::memory_order_relaxed);
        if (write - cachedRead_ == Capacity) {
            cachedRead_ = read_.load(std::memory_order_acquire);
            if (write - cachedRead_ == Capacity) return false;
        }
        buffer_[write & kMask] = value;
        write_.store(write + 1, std::memory_order_release);
        return true;
    }

    // Producer side: true when a push() is guaranteed to succeed.
    bool hasSpace() noexcept {
        const std::size_t write = write_.load(std::memory_order_relaxed);
        if (write - cachedRead_ < Capacity) return true;
        cachedRead_ = read_.load(std::memory_order_acquire);
        return write - cachedRead_ < Capacity;
    }

    // Consumer side.
    bool pop(T& out) noexcept {
        const std::size_t read = read_.load(std::memory_order_relaxed);
        if (read == cachedWrite_) {
            cachedWrite_ = write_.load(std::memory_order_acquire);
            if (read == cachedWrite_) return false;
        }
        out = buffer_[read & kMask];
        read_.store(read + 1, std::memory_order_release);
        return true;
    }

    // Approximate when called from a third thread.
    std::size_t size() const noexcept {
        return write_.load(std::memory_order_acquire) - read_.load(std::memory_order_acquire);
    }
    bool empty() const noexcept { return size() == 0; }
    static constexpr std::size_t capacity() noexcept { return Capacity; }

private:
    static constexpr std::size_t kMask = Capacity - 1;

    alignas(kCacheLineSize) std::atomic<std::size_t> write_{0};
    std::size_t cachedRead_ = 0;  // producer-local
    alignas(kCacheLineSize) std::atomic<std::size_t> read_{0};
    std::size_t cachedWrite_ = 0;  // consumer-local
    alignas(kCacheLineSize) std::array<T, Capacity> buffer_{};
};

}  // namespace reka
