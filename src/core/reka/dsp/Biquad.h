#pragma once

#include <cstdint>
#include <string>

namespace reka {

enum class FilterType : std::uint8_t {
    Bell = 0,
    LowShelf = 1,
    HighShelf = 2,
    LowCut = 3,   // 12 dB/oct high-pass
    HighCut = 4,  // 12 dB/oct low-pass
    Notch = 5,
    BandPass = 6,
    AllPass = 7,
};

const char* toString(FilterType type) noexcept;
bool fromString(const std::string& text, FilterType& type) noexcept;

// Normalised (a0 == 1) biquad coefficients. Double precision: low-frequency filters at 192 kHz are
// numerically fragile in float.
struct BiquadCoefficients {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
};

// RBJ "Audio EQ Cookbook" designs. Frequency and Q are clamped to safe ranges.
BiquadCoefficients designBiquad(FilterType type, double sampleRate, double frequency, double q,
                                double gainDb) noexcept;

// Magnitude response in dB at `frequency` (for EQ curve drawing and tests).
double magnitudeDb(const BiquadCoefficients& c, double frequency, double sampleRate) noexcept;

// Transposed direct form II state.
struct BiquadState {
    double z1 = 0.0;
    double z2 = 0.0;

    double process(const BiquadCoefficients& c, double x) noexcept {
        const double y = c.b0 * x + z1;
        z1 = c.b1 * x - c.a1 * y + z2;
        z2 = c.b2 * x - c.a2 * y;
        return y;
    }
    void reset() noexcept { z1 = z2 = 0.0; }
};

}  // namespace reka
