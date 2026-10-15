#pragma once

#include "reka/dsp/Decibels.h"

#include <array>

namespace reka {

// The one fader taper shared by on-screen faders/knobs, physical faders and MIDI controllers, so a
// physical fader and its on-screen twin always sit at the same position for the same dB value.
//
// Piecewise linear in dB (the classic console scale):
//   pos 0.00 -> -inf   pos 0.05 -> -60 dB   pos 0.25 -> -30 dB
//   pos 0.50 -> -10 dB pos 0.75 ->   0 dB   pos 1.00 -> +12 dB
class FaderLaw {
public:
    static float positionToDb(float position) noexcept {
        if (position <= 0.0f) return kSilenceDb;
        if (position >= 1.0f) return kMaxGainDb;
        for (std::size_t i = 1; i < kPoints.size(); ++i) {
            if (position <= kPoints[i].position) {
                const auto& a = kPoints[i - 1];
                const auto& b = kPoints[i];
                const float t = (position - a.position) / (b.position - a.position);
                return a.db + t * (b.db - a.db);
            }
        }
        return kMaxGainDb;
    }

    static float dbToPosition(float db) noexcept {
        if (db <= kSilenceDb) return 0.0f;
        if (db >= kMaxGainDb) return 1.0f;
        for (std::size_t i = 1; i < kPoints.size(); ++i) {
            if (db <= kPoints[i].db) {
                const auto& a = kPoints[i - 1];
                const auto& b = kPoints[i];
                const float t = (db - a.db) / (b.db - a.db);
                return a.position + t * (b.position - a.position);
            }
        }
        return 1.0f;
    }

private:
    struct Point {
        float position;
        float db;
    };
    static constexpr std::array<Point, 6> kPoints{{
        {0.00f, kSilenceDb},
        {0.05f, -60.0f},
        {0.25f, -30.0f},
        {0.50f, -10.0f},
        {0.75f, 0.0f},
        {1.00f, kMaxGainDb},
    }};
};

}  // namespace reka
