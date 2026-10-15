#include "reka/dsp/MeterTap.h"

#include <algorithm>
#include <cmath>

namespace reka {

void MeterTap::prepare(double sampleRate, double rmsWindowMs) noexcept {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    windowSamples_ = std::max(1.0, rmsWindowMs * 0.001 * sampleRate_);
    reset();
}

void MeterTap::reset() noexcept {
    for (auto& reader : peak_)
        for (auto& p : reader) p.store(0.0f, std::memory_order_relaxed);
    for (auto& c : clip_) c.store(false, std::memory_order_relaxed);
    for (auto& r : rms_) r.store(0.0f, std::memory_order_relaxed);
    meanSquare_.fill(0.0);
}

void MeterTap::process(const float* const* channels, int numChannels, int numFrames) noexcept {
    if (numFrames <= 0) return;
    const int chCount = std::min(numChannels, kStripChannels);
    // Block-rate exponential window: exact for constant signals, cheap for everything else.
    const double blend = 1.0 - std::exp(-static_cast<double>(numFrames) / windowSamples_);
    bool clipped = false;
    for (int ch = 0; ch < chCount; ++ch) {
        const float* data = channels[ch];
        float peak = 0.0f;
        double sumSquares = 0.0;
        for (int i = 0; i < numFrames; ++i) {
            const float s = data[i];
            const float a = std::fabs(s);
            peak = a > peak ? a : peak;
            sumSquares += static_cast<double>(s) * static_cast<double>(s);
        }
        clipped = clipped || peak >= 1.0f;
        const auto c = static_cast<std::size_t>(ch);
        meanSquare_[c] += blend * (sumSquares / numFrames - meanSquare_[c]);
        rms_[c].store(static_cast<float>(std::sqrt(meanSquare_[c])), std::memory_order_relaxed);
        for (auto& reader : peak_) {
            // Benign race with read(): a peak may be reported twice, never lost.
            if (peak > reader[c].load(std::memory_order_relaxed)) reader[c].store(peak, std::memory_order_relaxed);
        }
    }
    if (clipped)
        for (auto& c : clip_) c.store(true, std::memory_order_relaxed);
}

MeterReading MeterTap::read(MeterReader reader) noexcept {
    MeterReading out;
    const auto r = static_cast<std::size_t>(reader);
    for (std::size_t ch = 0; ch < static_cast<std::size_t>(kStripChannels); ++ch) {
        out.peak[ch] = peak_[r][ch].exchange(0.0f, std::memory_order_relaxed);
        out.rms[ch] = rms_[ch].load(std::memory_order_relaxed);
    }
    out.clipped = clip_[r].exchange(false, std::memory_order_relaxed);
    return out;
}

}  // namespace reka
