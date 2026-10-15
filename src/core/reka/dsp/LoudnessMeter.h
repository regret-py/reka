#pragma once

#include "reka/common/Types.h"
#include "reka/dsp/Biquad.h"
#include "reka/lockfree/SpscQueue.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

namespace reka {

inline constexpr float kLoudnessSilence = -100.0f;

// ITU-R BS.1770-4 / EBU R128 loudness.
//
// Audio thread : K-weighting + 100 ms energy steps -> momentary (400 ms) and short-term (3 s) LUFS,
//                published through atomics; every 400 ms gating block (75 % overlap) is pushed to a
//                lock-free queue.
// Other thread : LoudnessIntegrator drains the queue and computes gated integrated loudness.
class LoudnessMeter {
public:
    static constexpr int kShortTermSteps = 30;  // 30 x 100 ms
    static constexpr int kMomentarySteps = 4;   //  4 x 100 ms

    void prepare(double sampleRate) noexcept;
    void reset() noexcept;

    void process(const float* const* channels, int numChannels, int numFrames) noexcept;

    float momentaryLufs() const noexcept { return momentary_.load(std::memory_order_relaxed); }
    float shortTermLufs() const noexcept { return shortTerm_.load(std::memory_order_relaxed); }

    // Mean-square energies of completed 400 ms gating blocks (consumer: LoudnessIntegrator).
    SpscQueue<float, 256>& gatingBlocks() noexcept { return gatingBlocks_; }

    static BiquadCoefficients kWeightingStage1(double sampleRate) noexcept;
    static BiquadCoefficients kWeightingStage2(double sampleRate) noexcept;

private:
    void completeStep() noexcept;

    BiquadCoefficients stage1_{}, stage2_{};
    std::array<BiquadState, kStripChannels> state1_{}, state2_{};
    std::array<double, kStripChannels> sumSquares_{};
    int stepLength_ = 4800;
    int stepPosition_ = 0;
    std::array<double, kShortTermSteps> steps_{};
    int stepWrite_ = 0;
    int stepsFilled_ = 0;
    std::atomic<float> momentary_{kLoudnessSilence};
    std::atomic<float> shortTerm_{kLoudnessSilence};
    SpscQueue<float, 256> gatingBlocks_;
};

// Gated integrated loudness (absolute gate -70 LUFS, relative gate -10 LU) with a 0.1 LU histogram so
// memory stays bounded however long the program runs. Not real-time: call from a timer.
class LoudnessIntegrator {
public:
    LoudnessIntegrator();
    void reset() noexcept;
    void addBlock(double meanSquare) noexcept;
    void drain(LoudnessMeter& meter) noexcept;
    float integratedLufs() const noexcept;
    std::uint64_t blockCount() const noexcept { return totalBlocks_; }

private:
    static constexpr double kMinLufs = -70.0;
    static constexpr double kMaxLufs = 10.0;
    static constexpr int kBins = 800;  // 0.1 LU resolution
    std::vector<std::uint32_t> counts_;
    std::vector<double> energies_;
    std::uint64_t totalBlocks_ = 0;
};

}  // namespace reka
