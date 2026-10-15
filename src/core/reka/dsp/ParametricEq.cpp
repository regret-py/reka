#include "reka/dsp/ParametricEq.h"

#include <cmath>

namespace reka {

EqBandArray defaultEqBands() noexcept {
    EqBandArray bands;
    bands[0] = {false, FilterType::LowCut, 40.0f, 0.0f, 0.707f};
    bands[1] = {false, FilterType::LowShelf, 100.0f, 0.0f, 0.707f};
    bands[2] = {false, FilterType::Bell, 250.0f, 0.0f, 1.0f};
    bands[3] = {false, FilterType::Bell, 600.0f, 0.0f, 1.0f};
    bands[4] = {false, FilterType::Bell, 1500.0f, 0.0f, 1.0f};
    bands[5] = {false, FilterType::Bell, 3500.0f, 0.0f, 1.0f};
    bands[6] = {false, FilterType::Bell, 7000.0f, 0.0f, 1.0f};
    bands[7] = {false, FilterType::HighShelf, 12000.0f, 0.0f, 0.707f};
    return bands;
}

bool isBandAudible(const EqBand& band) noexcept {
    if (!band.enabled) return false;
    switch (band.type) {
        case FilterType::Bell:
        case FilterType::LowShelf:
        case FilterType::HighShelf: return std::fabs(band.gainDb) >= 0.01f;
        default: return true;
    }
}

double eqMagnitudeDb(const EqBandArray& bands, double frequency, double sampleRate) noexcept {
    double total = 0.0;
    for (const EqBand& band : bands) {
        if (!isBandAudible(band)) continue;
        total += magnitudeDb(designBiquad(band.type, sampleRate, band.frequency, band.q, band.gainDb),
                             frequency, sampleRate);
    }
    return total;
}

// ---- EqParameters -------------------------------------------------------------------------------------

EqParameters::EqParameters() noexcept { setBands(defaultEqBands()); }

void EqParameters::store(int index, const EqBand& band) noexcept {
    AtomicBand& b = bands_[static_cast<std::size_t>(index)];
    b.enabled.store(band.enabled, std::memory_order_relaxed);
    b.type.store(static_cast<std::uint8_t>(band.type), std::memory_order_relaxed);
    b.frequency.store(band.frequency, std::memory_order_relaxed);
    b.gainDb.store(band.gainDb, std::memory_order_relaxed);
    b.q.store(band.q, std::memory_order_relaxed);
}

void EqParameters::setBand(int index, const EqBand& band) noexcept {
    if (index < 0 || index >= kNumEqBands) return;
    store(index, band);
    version_.fetch_add(1, std::memory_order_release);
}

void EqParameters::setBands(const EqBandArray& bands) noexcept {
    for (int i = 0; i < kNumEqBands; ++i) store(i, bands[static_cast<std::size_t>(i)]);
    version_.fetch_add(1, std::memory_order_release);
}

EqBand EqParameters::band(int index) const noexcept {
    EqBand out;
    if (index < 0 || index >= kNumEqBands) return out;
    const AtomicBand& b = bands_[static_cast<std::size_t>(index)];
    out.enabled = b.enabled.load(std::memory_order_relaxed);
    out.type = static_cast<FilterType>(b.type.load(std::memory_order_relaxed));
    out.frequency = b.frequency.load(std::memory_order_relaxed);
    out.gainDb = b.gainDb.load(std::memory_order_relaxed);
    out.q = b.q.load(std::memory_order_relaxed);
    return out;
}

EqBandArray EqParameters::bands() const noexcept {
    EqBandArray out;
    for (int i = 0; i < kNumEqBands; ++i) out[static_cast<std::size_t>(i)] = band(i);
    return out;
}

void EqParameters::setEnabled(bool enabled) noexcept {
    enabled_.store(enabled, std::memory_order_relaxed);
    version_.fetch_add(1, std::memory_order_release);
}

// ---- EqProcessor --------------------------------------------------------------------------------------

void EqProcessor::prepare(double sampleRate) noexcept {
    sampleRate_ = sampleRate;
    seenVersion_ = 0;  // force a redesign at the new rate
    reset();
}

void EqProcessor::reset() noexcept {
    for (auto& channel : state_)
        for (auto& s : channel) s.reset();
}

bool EqProcessor::update(const EqParameters& params) noexcept {
    const std::uint32_t version = params.version();
    if (version == seenVersion_) return numActive_ > 0;
    seenVersion_ = version;

    numActive_ = 0;
    const bool enabled = params.enabled();
    for (int i = 0; i < kNumEqBands; ++i) {
        const EqBand band = params.band(i);
        const bool active = enabled && isBandAudible(band);
        const auto index = static_cast<std::size_t>(i);
        if (active) {
            coefficients_[index] = designBiquad(band.type, sampleRate_, band.frequency, band.q, band.gainDb);
            if (!wasActive_[index])
                for (auto& channel : state_) channel[index].reset();  // drop stale state
            activeBands_[static_cast<std::size_t>(numActive_++)] = i;
        }
        wasActive_[index] = active;
    }
    return numActive_ > 0;
}

void EqProcessor::process(float* const* channels, int numChannels, int numFrames) noexcept {
    if (numActive_ == 0) return;
    const int chCount = numChannels < kStripChannels ? numChannels : kStripChannels;
    for (int ch = 0; ch < chCount; ++ch) {
        float* data = channels[ch];
        auto& states = state_[static_cast<std::size_t>(ch)];
        for (int n = 0; n < numActive_; ++n) {
            const auto band = static_cast<std::size_t>(activeBands_[static_cast<std::size_t>(n)]);
            const BiquadCoefficients& c = coefficients_[band];
            BiquadState& s = states[band];
            for (int i = 0; i < numFrames; ++i) data[i] = static_cast<float>(s.process(c, data[i]));
        }
    }
}

}  // namespace reka
