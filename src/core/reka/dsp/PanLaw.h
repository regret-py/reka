#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace reka {

enum class PanLaw : std::uint8_t {
    ConstantPower = 0,           // sin/cos, centre -3 dB (DAW default)
    ConstantPowerUnityCentre = 1,// sin/cos scaled by sqrt(2): centre 0 dB, hard pan +3 dB
    Linear = 2,                  // linear, centre -6 dB
};

struct StereoGains {
    float left = 1.0f;
    float right = 1.0f;
};

// Panning of a (summed) mono source. pan in [-1, 1].
inline StereoGains panMono(float pan, PanLaw law) noexcept {
    pan = std::clamp(pan, -1.0f, 1.0f);
    constexpr float kQuarterPi = 0.78539816339f;
    switch (law) {
        case PanLaw::Linear: return {(1.0f - pan) * 0.5f, (1.0f + pan) * 0.5f};
        case PanLaw::ConstantPowerUnityCentre: {
            const float theta = (pan + 1.0f) * kQuarterPi;
            constexpr float kSqrt2 = 1.41421356237f;
            return {std::cos(theta) * kSqrt2, std::sin(theta) * kSqrt2};
        }
        case PanLaw::ConstantPower:
        default: {
            const float theta = (pan + 1.0f) * kQuarterPi;
            return {std::cos(theta), std::sin(theta)};
        }
    }
}

// Balance control for a stereo source: the centre is unity on both sides, the far side is attenuated
// along a quarter cosine.
inline StereoGains balance(float pan) noexcept {
    pan = std::clamp(pan, -1.0f, 1.0f);
    constexpr float kHalfPi = 1.57079632679f;
    if (pan < 0.0f) return {1.0f, std::cos(-pan * kHalfPi)};
    if (pan > 0.0f) return {std::cos(pan * kHalfPi), 1.0f};
    return {1.0f, 1.0f};
}

inline const char* toString(PanLaw law) noexcept {
    switch (law) {
        case PanLaw::ConstantPowerUnityCentre: return "constantPowerUnityCentre";
        case PanLaw::Linear: return "linear";
        case PanLaw::ConstantPower:
        default: return "constantPower";
    }
}

inline PanLaw panLawFromString(const std::string& text) noexcept {
    if (text == "constantPowerUnityCentre") return PanLaw::ConstantPowerUnityCentre;
    if (text == "linear") return PanLaw::Linear;
    return PanLaw::ConstantPower;
}

}  // namespace reka
