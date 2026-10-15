#pragma once

#include "reka/common/Types.h"
#include "reka/dsp/LoudnessMeter.h"
#include "reka/dsp/MeterTap.h"
#include "reka/dsp/ParametricEq.h"
#include "reka/dsp/Smoothing.h"

#include <array>
#include <vector>

namespace reka {

// Real-time state of one strip/bus. Owned by MixerModel through shared_ptr, so it survives graph swaps:
// filter states, smoothers and meters are continuous across topology changes.
class StripProcessor {
public:
    // Non-real-time (audio stopped, or processor not yet in a published graph).
    void prepare(double sampleRate, int maxBlockSize);

    float* const* pre() noexcept { return pre_.data(); }    // after source/EQ/inserts, before fader
    float* const* post() noexcept { return post_.data(); }  // after fader/pan/mute

    std::array<SmoothedValue, kStripChannels> phase;
    std::array<SmoothedValue, kStripChannels> gain;
    EqProcessor eq;
    MeterTap meter;
    LoudnessMeter loudness;

private:
    std::vector<float> storage_;
    std::array<float*, kStripChannels> pre_{};
    std::array<float*, kStripChannels> post_{};
};

// Real-time state of one routing-matrix crosspoint.
class CrosspointProcessor {
public:
    void prepare(double sampleRate) noexcept {
        for (auto& g : gain) {
            g.prepare(sampleRate);
            g.reset(0.0f);  // a new route fades in
        }
        eq.prepare(sampleRate);
        wasSilent = true;
    }

    std::array<SmoothedValue, kStripChannels> gain;
    EqProcessor eq;
    bool wasSilent = true;
};

}  // namespace reka
