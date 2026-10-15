#include "reka/dsp/Biquad.h"

#include <algorithm>
#include <cmath>
#include <complex>

namespace reka {

const char* toString(FilterType type) noexcept {
    switch (type) {
        case FilterType::Bell: return "bell";
        case FilterType::LowShelf: return "lowShelf";
        case FilterType::HighShelf: return "highShelf";
        case FilterType::LowCut: return "lowCut";
        case FilterType::HighCut: return "highCut";
        case FilterType::Notch: return "notch";
        case FilterType::BandPass: return "bandPass";
        case FilterType::AllPass: return "allPass";
    }
    return "bell";
}

bool fromString(const std::string& text, FilterType& type) noexcept {
    static constexpr FilterType kAll[] = {FilterType::Bell,    FilterType::LowShelf, FilterType::HighShelf,
                                          FilterType::LowCut,  FilterType::HighCut,  FilterType::Notch,
                                          FilterType::BandPass, FilterType::AllPass};
    for (const FilterType candidate : kAll) {
        if (text == toString(candidate)) {
            type = candidate;
            return true;
        }
    }
    return false;
}

BiquadCoefficients designBiquad(FilterType type, double sampleRate, double frequency, double q,
                                double gainDb) noexcept {
    constexpr double kPi = 3.14159265358979323846;
    sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    frequency = std::clamp(frequency, 10.0, sampleRate * 0.49);
    q = std::clamp(q, 0.05, 40.0);

    const double A = std::pow(10.0, gainDb / 40.0);
    const double w0 = 2.0 * kPi * frequency / sampleRate;
    const double cosW = std::cos(w0);
    const double sinW = std::sin(w0);
    const double alpha = sinW / (2.0 * q);

    double b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;
    switch (type) {
        case FilterType::Bell:
            b0 = 1.0 + alpha * A;
            b1 = -2.0 * cosW;
            b2 = 1.0 - alpha * A;
            a0 = 1.0 + alpha / A;
            a1 = -2.0 * cosW;
            a2 = 1.0 - alpha / A;
            break;
        case FilterType::LowShelf: {
            const double s = 2.0 * std::sqrt(A) * alpha;
            b0 = A * ((A + 1.0) - (A - 1.0) * cosW + s);
            b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cosW);
            b2 = A * ((A + 1.0) - (A - 1.0) * cosW - s);
            a0 = (A + 1.0) + (A - 1.0) * cosW + s;
            a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cosW);
            a2 = (A + 1.0) + (A - 1.0) * cosW - s;
            break;
        }
        case FilterType::HighShelf: {
            const double s = 2.0 * std::sqrt(A) * alpha;
            b0 = A * ((A + 1.0) + (A - 1.0) * cosW + s);
            b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cosW);
            b2 = A * ((A + 1.0) + (A - 1.0) * cosW - s);
            a0 = (A + 1.0) - (A - 1.0) * cosW + s;
            a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cosW);
            a2 = (A + 1.0) - (A - 1.0) * cosW - s;
            break;
        }
        case FilterType::LowCut:
            b0 = (1.0 + cosW) * 0.5;
            b1 = -(1.0 + cosW);
            b2 = (1.0 + cosW) * 0.5;
            a0 = 1.0 + alpha;
            a1 = -2.0 * cosW;
            a2 = 1.0 - alpha;
            break;
        case FilterType::HighCut:
            b0 = (1.0 - cosW) * 0.5;
            b1 = 1.0 - cosW;
            b2 = (1.0 - cosW) * 0.5;
            a0 = 1.0 + alpha;
            a1 = -2.0 * cosW;
            a2 = 1.0 - alpha;
            break;
        case FilterType::Notch:
            b0 = 1.0;
            b1 = -2.0 * cosW;
            b2 = 1.0;
            a0 = 1.0 + alpha;
            a1 = -2.0 * cosW;
            a2 = 1.0 - alpha;
            break;
        case FilterType::BandPass:  // constant 0 dB peak gain
            b0 = alpha;
            b1 = 0.0;
            b2 = -alpha;
            a0 = 1.0 + alpha;
            a1 = -2.0 * cosW;
            a2 = 1.0 - alpha;
            break;
        case FilterType::AllPass:
            b0 = 1.0 - alpha;
            b1 = -2.0 * cosW;
            b2 = 1.0 + alpha;
            a0 = 1.0 + alpha;
            a1 = -2.0 * cosW;
            a2 = 1.0 - alpha;
            break;
    }

    BiquadCoefficients c;
    c.b0 = b0 / a0;
    c.b1 = b1 / a0;
    c.b2 = b2 / a0;
    c.a1 = a1 / a0;
    c.a2 = a2 / a0;
    return c;
}

double magnitudeDb(const BiquadCoefficients& c, double frequency, double sampleRate) noexcept {
    constexpr double kPi = 3.14159265358979323846;
    const double w = 2.0 * kPi * frequency / sampleRate;
    const std::complex<double> z1 = std::polar(1.0, -w);
    const std::complex<double> z2 = std::polar(1.0, -2.0 * w);
    const std::complex<double> num = c.b0 + c.b1 * z1 + c.b2 * z2;
    const std::complex<double> den = 1.0 + c.a1 * z1 + c.a2 * z2;
    const double mag = std::abs(num / den);
    return 20.0 * std::log10(std::max(mag, 1.0e-12));
}

}  // namespace reka
