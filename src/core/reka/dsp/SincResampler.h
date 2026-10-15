#pragma once

#include <memory>
#include <vector>

namespace reka {

// Variable-ratio polyphase windowed-sinc resampler (64 taps, 512 phases, Kaiser beta 8.6, linear
// interpolation between phases). One instance converts one stream between two clock domains; the ratio
// can be nudged every block by a DriftController, which is how REKA locks secondary devices, virtual
// endpoints and per-app captures to the master device clock and converts their sample rates in one pass.
//
// ratio = input frames consumed per output frame (e.g. 44100 -> 48000 gives 0.91875).
class SincResampler {
public:
    static constexpr int kTaps = 64;
    static constexpr int kHalfTaps = kTaps / 2;
    static constexpr int kPhases = 512;

    // Non-real-time. The filter cutoff is designed for nominalRatio; setRatio() may move within ~1 %.
    void prepare(int numChannels, double nominalRatio, int maxInputChunk);
    void reset() noexcept;

    void setRatio(double ratio) noexcept;
    double ratio() const noexcept { return ratio_; }
    double nominalRatio() const noexcept { return nominalRatio_; }
    int numChannels() const noexcept { return numChannels_; }

    // ---- input ----
    int inputSpace() const noexcept { return capacity_ - fill_; }
    float* inputWritePointer(int channel) noexcept;  // valid for inputSpace() frames
    void commitInput(int numFrames) noexcept;
    int pushInput(const float* const* source, int numFrames) noexcept;  // returns frames accepted

    // ---- output ----
    int outputsAvailable() const noexcept;
    // Input frames that must still be pushed before `numOutputs` outputs can be produced.
    int inputNeededFor(int numOutputs) const noexcept;
    int process(float* const* destination, int maxOutputs) noexcept;

    // Input frames buffered but not yet consumed (latency/fill accounting for drift control).
    double bufferedInputFrames() const noexcept;

private:
    void compact() noexcept;

    int numChannels_ = 0;
    int capacity_ = 0;
    int fill_ = 0;
    double position_ = 0.0;  // fractional read position (input frames) inside history_
    double ratio_ = 1.0;
    double nominalRatio_ = 1.0;
    std::shared_ptr<const std::vector<float>> table_;  // (kPhases + 1) x kTaps, shared between instances
    std::vector<std::vector<float>> history_;
    std::vector<float> interpolated_;  // per-output kernel, reused across channels
};

// PI controller that converts a buffer fill error into a resampling ratio. The fill level is
// low-pass filtered first because device callbacks make it saw-tooth by a whole period.
class DriftController {
public:
    // targetFill is in frames of the measured buffer. During the first `calibrationSeconds` the
    // controller only observes: the average fill it sees becomes the set point, so it holds the latency
    // the stream naturally settled at instead of pulling the pitch toward an arbitrary threshold.
    void reset(double nominalRatio, double targetFillFrames, double framesPerSecond,
               double calibrationSeconds = 0.5) noexcept;
    // Call once per engine block with the measured fill and the block duration in seconds.
    double update(double measuredFillFrames, double blockSeconds) noexcept;

    double ratio() const noexcept { return ratio_; }
    double correctionPpm() const noexcept { return (ratio_ / nominalRatio_ - 1.0) * 1.0e6; }
    double smoothedFill() const noexcept { return smoothedFill_; }
    double targetFill() const noexcept { return target_; }

private:
    double nominalRatio_ = 1.0;
    double ratio_ = 1.0;
    double target_ = 0.0;
    double framesPerSecond_ = 48000.0;
    double smoothedFill_ = 0.0;
    double integral_ = 0.0;
    double calibrationSeconds_ = 0.5;
    double calibrationTime_ = 0.0;
    double calibrationSum_ = 0.0;
    bool calibrating_ = true;
};

}  // namespace reka
