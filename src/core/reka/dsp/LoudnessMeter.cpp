#include "reka/dsp/LoudnessMeter.h"

#include <algorithm>
#include <cmath>

namespace reka {

namespace {
constexpr double kPi = 3.14159265358979323846;

float energyToLufs(double meanSquare) noexcept {
    if (meanSquare <= 1.0e-12) return kLoudnessSilence;
    return static_cast<float>(-0.691 + 10.0 * std::log10(meanSquare));
}
}  // namespace

// Coefficients re-derived for any sample rate (same analogue prototypes as BS.1770's 48 kHz tables).
BiquadCoefficients LoudnessMeter::kWeightingStage1(double sampleRate) noexcept {
    const double f0 = 1681.974450955533;
    const double G = 3.999843853973347;
    const double Q = 0.7071752369554196;
    const double K = std::tan(kPi * f0 / sampleRate);
    const double Vh = std::pow(10.0, G / 20.0);
    const double Vb = std::pow(Vh, 0.4996667741545416);
    const double a0 = 1.0 + K / Q + K * K;
    BiquadCoefficients c;
    c.b0 = (Vh + Vb * K / Q + K * K) / a0;
    c.b1 = 2.0 * (K * K - Vh) / a0;
    c.b2 = (Vh - Vb * K / Q + K * K) / a0;
    c.a1 = 2.0 * (K * K - 1.0) / a0;
    c.a2 = (1.0 - K / Q + K * K) / a0;
    return c;
}

BiquadCoefficients LoudnessMeter::kWeightingStage2(double sampleRate) noexcept {
    const double f0 = 38.13547087602444;
    const double Q = 0.5003270373238773;
    const double K = std::tan(kPi * f0 / sampleRate);
    const double a0 = 1.0 + K / Q + K * K;
    BiquadCoefficients c;
    c.b0 = 1.0;
    c.b1 = -2.0;
    c.b2 = 1.0;
    c.a1 = 2.0 * (K * K - 1.0) / a0;
    c.a2 = (1.0 - K / Q + K * K) / a0;
    return c;
}

void LoudnessMeter::prepare(double sampleRate) noexcept {
    sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    stage1_ = kWeightingStage1(sampleRate);
    stage2_ = kWeightingStage2(sampleRate);
    stepLength_ = std::max(1, static_cast<int>(std::lround(sampleRate * 0.1)));
    reset();
}

void LoudnessMeter::reset() noexcept {
    for (auto& s : state1_) s.reset();
    for (auto& s : state2_) s.reset();
    sumSquares_.fill(0.0);
    steps_.fill(0.0);
    stepPosition_ = 0;
    stepWrite_ = 0;
    stepsFilled_ = 0;
    momentary_.store(kLoudnessSilence, std::memory_order_relaxed);
    shortTerm_.store(kLoudnessSilence, std::memory_order_relaxed);
}

void LoudnessMeter::process(const float* const* channels, int numChannels, int numFrames) noexcept {
    const int chCount = std::min(numChannels, kStripChannels);
    int offset = 0;
    while (offset < numFrames) {
        const int run = std::min(numFrames - offset, stepLength_ - stepPosition_);
        for (int ch = 0; ch < chCount; ++ch) {
            const auto c = static_cast<std::size_t>(ch);
            const float* data = channels[ch] + offset;
            double acc = 0.0;
            for (int i = 0; i < run; ++i) {
                const double y = state2_[c].process(stage2_, state1_[c].process(stage1_, data[i]));
                acc += y * y;
            }
            sumSquares_[c] += acc;
        }
        stepPosition_ += run;
        offset += run;
        if (stepPosition_ >= stepLength_) completeStep();
    }
}

void LoudnessMeter::completeStep() noexcept {
    double energy = 0.0;  // channel weights: L = R = 1.0
    for (auto& s : sumSquares_) {
        energy += s / stepLength_;
        s = 0.0;
    }
    stepPosition_ = 0;
    steps_[static_cast<std::size_t>(stepWrite_)] = energy;
    stepWrite_ = (stepWrite_ + 1) % kShortTermSteps;
    stepsFilled_ = std::min(stepsFilled_ + 1, kShortTermSteps);

    auto meanOfLast = [this](int count) {
        double sum = 0.0;
        for (int i = 1; i <= count; ++i)
            sum += steps_[static_cast<std::size_t>((stepWrite_ - i + kShortTermSteps) % kShortTermSteps)];
        return sum / count;
    };

    if (stepsFilled_ >= kMomentarySteps) {
        const double momentaryEnergy = meanOfLast(kMomentarySteps);
        momentary_.store(energyToLufs(momentaryEnergy), std::memory_order_relaxed);
        gatingBlocks_.push(static_cast<float>(momentaryEnergy));  // dropped if nobody drains: harmless
    }
    shortTerm_.store(energyToLufs(meanOfLast(stepsFilled_)), std::memory_order_relaxed);
}

// ---- LoudnessIntegrator -------------------------------------------------------------------------------

LoudnessIntegrator::LoudnessIntegrator() : counts_(kBins, 0), energies_(kBins, 0.0) {}

void LoudnessIntegrator::reset() noexcept {
    std::fill(counts_.begin(), counts_.end(), 0u);
    std::fill(energies_.begin(), energies_.end(), 0.0);
    totalBlocks_ = 0;
}

void LoudnessIntegrator::addBlock(double meanSquare) noexcept {
    if (meanSquare <= 0.0) return;
    const double lufs = -0.691 + 10.0 * std::log10(meanSquare);
    if (lufs < kMinLufs) return;  // absolute gate
    const int bin = std::clamp(static_cast<int>((lufs - kMinLufs) * 10.0), 0, kBins - 1);
    counts_[static_cast<std::size_t>(bin)] += 1;
    energies_[static_cast<std::size_t>(bin)] += meanSquare;
    ++totalBlocks_;
}

void LoudnessIntegrator::drain(LoudnessMeter& meter) noexcept {
    float energy = 0.0f;
    while (meter.gatingBlocks().pop(energy)) addBlock(static_cast<double>(energy));
}

float LoudnessIntegrator::integratedLufs() const noexcept {
    if (totalBlocks_ == 0) return kLoudnessSilence;
    double sum = 0.0;
    std::uint64_t count = 0;
    for (int i = 0; i < kBins; ++i) {
        sum += energies_[static_cast<std::size_t>(i)];
        count += counts_[static_cast<std::size_t>(i)];
    }
    const double relativeGate = -0.691 + 10.0 * std::log10(sum / static_cast<double>(count)) - 10.0;
    const int firstBin = std::clamp(static_cast<int>(std::ceil((relativeGate - kMinLufs) * 10.0)), 0, kBins);
    sum = 0.0;
    count = 0;
    for (int i = firstBin; i < kBins; ++i) {
        sum += energies_[static_cast<std::size_t>(i)];
        count += counts_[static_cast<std::size_t>(i)];
    }
    if (count == 0) return kLoudnessSilence;
    return static_cast<float>(-0.691 + 10.0 * std::log10(sum / static_cast<double>(count)));
}

}  // namespace reka
