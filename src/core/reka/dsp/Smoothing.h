#pragma once

#include <cmath>
#include <cstring>

namespace reka {

// Linear ramp of a gain across one block.
struct GainSpan {
    float start = 0.0f;
    float end = 0.0f;
    bool isConstant() const noexcept { return start == end; }
    bool isSilent() const noexcept { return start == 0.0f && end == 0.0f; }
    bool isUnity() const noexcept { return start == 1.0f && end == 1.0f; }
};

// Block-rate one-pole smoother with intra-block linear interpolation.
//
// Every block the control value moves toward its target with a fixed time constant (independent of the
// block size), and the audio is multiplied by a linear ramp from the previous to the new value. This
// de-clicks gain, pan, mute, solo, phase flips and route toggles at the cost of one exp() per block.
class SmoothedValue {
public:
    void prepare(double sampleRate, double timeConstantMs = 15.0) noexcept {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        timeConstantSamples_ = timeConstantMs * 0.001 * sampleRate_;
    }

    void reset(float value) noexcept { current_ = value; }
    float current() const noexcept { return current_; }

    GainSpan advance(float target, int numFrames) noexcept {
        const float start = current_;
        if (current_ != target) {
            const double a = std::exp(-static_cast<double>(numFrames) / timeConstantSamples_);
            current_ = static_cast<float>(static_cast<double>(target) + static_cast<double>(current_ - target) * a);
            if (std::fabs(current_ - target) < 1.0e-5f) current_ = target;
        }
        return {start, current_};
    }

private:
    double sampleRate_ = 48000.0;
    double timeConstantSamples_ = 720.0;
    float current_ = 0.0f;
};

// ---- Ramp kernels ------------------------------------------------------------------------------------

inline void applyGain(float* buffer, int numFrames, GainSpan span) noexcept {
    if (span.isUnity()) return;
    if (span.isSilent()) {
        std::memset(buffer, 0, static_cast<std::size_t>(numFrames) * sizeof(float));
        return;
    }
    if (span.isConstant()) {
        for (int i = 0; i < numFrames; ++i) buffer[i] *= span.start;
        return;
    }
    const float step = (span.end - span.start) / static_cast<float>(numFrames);
    float g = span.start;
    for (int i = 0; i < numFrames; ++i) {
        g += step;
        buffer[i] *= g;
    }
}

inline void copyWithGain(float* dest, const float* source, int numFrames, GainSpan span) noexcept {
    if (span.isSilent()) {
        std::memset(dest, 0, static_cast<std::size_t>(numFrames) * sizeof(float));
        return;
    }
    if (span.isConstant()) {
        for (int i = 0; i < numFrames; ++i) dest[i] = source[i] * span.start;
        return;
    }
    const float step = (span.end - span.start) / static_cast<float>(numFrames);
    float g = span.start;
    for (int i = 0; i < numFrames; ++i) {
        g += step;
        dest[i] = source[i] * g;
    }
}

inline void addWithGain(float* dest, const float* source, int numFrames, GainSpan span) noexcept {
    if (span.isSilent()) return;
    if (span.isConstant()) {
        for (int i = 0; i < numFrames; ++i) dest[i] += source[i] * span.start;
        return;
    }
    const float step = (span.end - span.start) / static_cast<float>(numFrames);
    float g = span.start;
    for (int i = 0; i < numFrames; ++i) {
        g += step;
        dest[i] += source[i] * g;
    }
}

}  // namespace reka
