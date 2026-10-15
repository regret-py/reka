#pragma once

#include "reka/common/Types.h"
#include "reka/dsp/Decibels.h"
#include "reka/dsp/ParametricEq.h"

#include <algorithm>
#include <atomic>
#include <cstdint>

namespace reka {

// Lock-free parameter block of one strip (input or bus).
//
// Writers: UI thread, hardware I/O thread, MIDI thread, profile loader. Reader: the audio thread, plus
// observers (UI, hardware feedback) that poll revision() to find out cheaply whether anything changed.
// Parameter changes never rebuild the render graph and never allocate.
class StripParameters {
public:
    float gainDb() const noexcept { return gainDb_.load(std::memory_order_relaxed); }
    float pan() const noexcept { return pan_.load(std::memory_order_relaxed); }
    bool mute() const noexcept { return mute_.load(std::memory_order_relaxed); }
    bool solo() const noexcept { return solo_.load(std::memory_order_relaxed); }
    bool fxBypass() const noexcept { return fxBypass_.load(std::memory_order_relaxed); }
    bool phaseInvertLeft() const noexcept { return phaseL_.load(std::memory_order_relaxed); }
    bool phaseInvertRight() const noexcept { return phaseR_.load(std::memory_order_relaxed); }
    ChannelMode channelMode() const noexcept { return mode_.load(std::memory_order_relaxed); }

    void setGainDb(float db) noexcept { store(gainDb_, clampGainDb(db)); }
    void setPan(float pan) noexcept { store(pan_, std::clamp(pan, -1.0f, 1.0f)); }
    void setMute(bool v) noexcept { store(mute_, v); }
    void setSolo(bool v) noexcept { store(solo_, v); }
    void setFxBypass(bool v) noexcept { store(fxBypass_, v); }
    void setPhaseInvertLeft(bool v) noexcept { store(phaseL_, v); }
    void setPhaseInvertRight(bool v) noexcept { store(phaseR_, v); }
    void setChannelMode(ChannelMode m) noexcept { store(mode_, m); }

    void toggleMute() noexcept { setMute(!mute()); }
    void toggleSolo() noexcept { setSolo(!solo()); }
    void toggleFxBypass() noexcept { setFxBypass(!fxBypass()); }

    EqParameters& eq() noexcept { return eq_; }
    const EqParameters& eq() const noexcept { return eq_; }

    // Monotonic counter bumped by every setter (EQ has its own version()).
    std::uint32_t revision() const noexcept { return revision_.load(std::memory_order_acquire); }

private:
    template <typename T, typename V>
    void store(std::atomic<T>& target, V value) noexcept {
        target.store(static_cast<T>(value), std::memory_order_relaxed);
        revision_.fetch_add(1, std::memory_order_release);
    }

    std::atomic<float> gainDb_{0.0f};
    std::atomic<float> pan_{0.0f};
    std::atomic<bool> mute_{false};
    std::atomic<bool> solo_{false};
    std::atomic<bool> fxBypass_{false};
    std::atomic<bool> phaseL_{false};
    std::atomic<bool> phaseR_{false};
    std::atomic<ChannelMode> mode_{ChannelMode::Stereo};
    std::atomic<std::uint32_t> revision_{0};
    EqParameters eq_;

    static_assert(std::atomic<float>::is_always_lock_free, "audio parameters must be lock-free");
    static_assert(std::atomic<ChannelMode>::is_always_lock_free, "audio parameters must be lock-free");
};

// Parameters of one input -> bus crosspoint of the routing matrix.
class CrosspointParameters {
public:
    bool enabled() const noexcept { return enabled_.load(std::memory_order_relaxed); }
    float gainDb() const noexcept { return gainDb_.load(std::memory_order_relaxed); }
    float pan() const noexcept { return pan_.load(std::memory_order_relaxed); }
    bool phaseInvert() const noexcept { return phase_.load(std::memory_order_relaxed); }
    bool preFader() const noexcept { return preFader_.load(std::memory_order_relaxed); }

    void setEnabled(bool v) noexcept { store(enabled_, v); }
    void setGainDb(float db) noexcept { store(gainDb_, clampGainDb(db)); }
    void setPan(float pan) noexcept { store(pan_, std::clamp(pan, -1.0f, 1.0f)); }
    void setPhaseInvert(bool v) noexcept { store(phase_, v); }
    void setPreFader(bool v) noexcept { store(preFader_, v); }

    EqParameters& eq() noexcept { return eq_; }
    const EqParameters& eq() const noexcept { return eq_; }
    std::uint32_t revision() const noexcept { return revision_.load(std::memory_order_acquire); }

private:
    template <typename T, typename V>
    void store(std::atomic<T>& target, V value) noexcept {
        target.store(static_cast<T>(value), std::memory_order_relaxed);
        revision_.fetch_add(1, std::memory_order_release);
    }

    std::atomic<bool> enabled_{false};
    std::atomic<float> gainDb_{0.0f};
    std::atomic<float> pan_{0.0f};
    std::atomic<bool> phase_{false};
    std::atomic<bool> preFader_{false};
    std::atomic<std::uint32_t> revision_{0};
    EqParameters eq_;
};

}  // namespace reka
