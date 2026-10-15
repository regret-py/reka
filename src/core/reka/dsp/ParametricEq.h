#pragma once

#include "reka/common/Types.h"
#include "reka/dsp/Biquad.h"

#include <array>
#include <atomic>
#include <cstdint>

namespace reka {

struct EqBand {
    bool enabled = false;
    FilterType type = FilterType::Bell;
    float frequency = 1000.0f;
    float gainDb = 0.0f;
    float q = 0.707f;

    bool operator==(const EqBand&) const = default;
};

using EqBandArray = std::array<EqBand, kNumEqBands>;

// Musically useful starting points: low cut, low shelf, four bells, presence bell, high shelf.
EqBandArray defaultEqBands() noexcept;

// True when the band changes the signal (an enabled bell at 0 dB is skipped by the processor).
bool isBandAudible(const EqBand& band) noexcept;

// Response of the whole band set at `frequency` (UI curve).
double eqMagnitudeDb(const EqBandArray& bands, double frequency, double sampleRate) noexcept;

// Shared parameter block: written by the UI / hardware / profile loader, read by the audio thread.
// Writers bump `version` after the fields; the audio thread redesigns coefficients only on change.
class EqParameters {
public:
    EqParameters() noexcept;

    void setBand(int index, const EqBand& band) noexcept;
    EqBand band(int index) const noexcept;
    EqBandArray bands() const noexcept;
    void setBands(const EqBandArray& bands) noexcept;

    void setEnabled(bool enabled) noexcept;
    bool enabled() const noexcept { return enabled_.load(std::memory_order_relaxed); }

    std::uint32_t version() const noexcept { return version_.load(std::memory_order_acquire); }

private:
    struct AtomicBand {
        std::atomic<bool> enabled{false};
        std::atomic<std::uint8_t> type{0};
        std::atomic<float> frequency{1000.0f};
        std::atomic<float> gainDb{0.0f};
        std::atomic<float> q{0.707f};
    };
    void store(int index, const EqBand& band) noexcept;

    std::array<AtomicBand, kNumEqBands> bands_;
    std::atomic<bool> enabled_{true};
    std::atomic<std::uint32_t> version_{1};
};

// Real-time state for one stereo EQ instance.
class EqProcessor {
public:
    void prepare(double sampleRate) noexcept;
    void reset() noexcept;

    // Re-designs coefficients if the parameters changed. Returns true if any band is audible.
    bool update(const EqParameters& params) noexcept;
    bool isActive() const noexcept { return numActive_ > 0; }

    void process(float* const* channels, int numChannels, int numFrames) noexcept;

private:
    double sampleRate_ = 48000.0;
    std::uint32_t seenVersion_ = 0;
    int numActive_ = 0;
    std::array<int, kNumEqBands> activeBands_{};
    std::array<bool, kNumEqBands> wasActive_{};
    std::array<BiquadCoefficients, kNumEqBands> coefficients_{};
    std::array<std::array<BiquadState, kNumEqBands>, kStripChannels> state_{};
};

}  // namespace reka
