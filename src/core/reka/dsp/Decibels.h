#pragma once

#include <algorithm>
#include <cmath>

namespace reka {

// Anything at or below this is treated as "-inf" (gain 0).
inline constexpr float kSilenceDb = -100.0f;
inline constexpr float kMaxGainDb = 12.0f;

inline float dbToGain(float db) noexcept { return db <= kSilenceDb ? 0.0f : std::pow(10.0f, db * 0.05f); }

inline float gainToDb(float gain) noexcept {
    return gain <= 1.0e-5f ? kSilenceDb : 20.0f * std::log10(gain);
}

inline double powerToDb(double meanSquare) noexcept {
    return meanSquare <= 1.0e-10 ? static_cast<double>(kSilenceDb) : 10.0 * std::log10(meanSquare);
}

inline float clampGainDb(float db) noexcept { return std::clamp(db, kSilenceDb, kMaxGainDb); }

}  // namespace reka
