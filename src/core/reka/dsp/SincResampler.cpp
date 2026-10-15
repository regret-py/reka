#include "reka/dsp/SincResampler.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>

namespace reka {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kKaiserBeta = 8.6;

double besselI0(double x) noexcept {
    double sum = 1.0, term = 1.0;
    const double halfX = x * 0.5;
    for (int k = 1; k < 50; ++k) {
        term *= (halfX / k) * (halfX / k);
        sum += term;
        if (term < sum * 1.0e-12) break;
    }
    return sum;
}

std::shared_ptr<const std::vector<float>> buildTable(double cutoff) {
    using Table = std::vector<float>;
    constexpr int N = SincResampler::kTaps;
    constexpr int P = SincResampler::kPhases;
    auto table = std::make_shared<Table>(static_cast<std::size_t>((P + 1) * N));
    const double i0Beta = besselI0(kKaiserBeta);
    const double halfSpan = static_cast<double>(SincResampler::kHalfTaps);
    for (int p = 0; p <= P; ++p) {
        const double frac = static_cast<double>(p) / P;
        double sum = 0.0;
        float* row = table->data() + static_cast<std::size_t>(p * N);
        for (int k = 0; k < N; ++k) {
            const double tau = (k - (SincResampler::kHalfTaps - 1)) - frac;
            const double x = 2.0 * cutoff * tau;
            const double sinc = std::fabs(x) < 1.0e-12 ? 1.0 : std::sin(kPi * x) / (kPi * x);
            const double r = tau / halfSpan;
            const double window = std::fabs(r) >= 1.0 ? 0.0 : besselI0(kKaiserBeta * std::sqrt(1.0 - r * r)) / i0Beta;
            const double h = 2.0 * cutoff * sinc * window;
            row[k] = static_cast<float>(h);
            sum += h;
        }
        for (int k = 0; k < N; ++k) row[k] = static_cast<float>(static_cast<double>(row[k]) / sum);  // unity DC gain per phase
    }
    return table;
}

// Tables only depend on the cutoff; share them between every resampler with the same ratio.
std::shared_ptr<const std::vector<float>> tableForCutoff(double cutoff) {
    static std::mutex mutex;
    static std::map<long, std::weak_ptr<const std::vector<float>>> cache;
    const long key = std::lround(cutoff * 1.0e6);
    std::lock_guard<std::mutex> lock(mutex);
    if (auto existing = cache[key].lock()) return existing;
    auto table = buildTable(cutoff);
    cache[key] = table;
    return table;
}
}  // namespace

void SincResampler::prepare(int numChannels, double nominalRatio, int maxInputChunk) {
    numChannels_ = std::max(1, numChannels);
    nominalRatio_ = nominalRatio > 0.0 ? nominalRatio : 1.0;
    ratio_ = nominalRatio_;
    // Stop band starts at the lower Nyquist; ~0.045 transition half-width for 64 taps.
    const double cutoff = 0.5 * std::min(1.0, 1.0 / nominalRatio_) - 0.045 * std::min(1.0, 1.0 / nominalRatio_);
    table_ = tableForCutoff(cutoff);
    capacity_ = std::max(64, maxInputChunk) + kTaps * 2 + 16;
    history_.assign(static_cast<std::size_t>(numChannels_), std::vector<float>(static_cast<std::size_t>(capacity_), 0.0f));
    interpolated_.assign(static_cast<std::size_t>(kTaps), 0.0f);
    reset();
}

void SincResampler::reset() noexcept {
    for (auto& ch : history_) std::fill(ch.begin(), ch.end(), 0.0f);
    // Pre-roll of zeros so the very first output can be centred on the first real input sample.
    fill_ = kHalfTaps - 1;
    position_ = static_cast<double>(kHalfTaps - 1);
}

void SincResampler::setRatio(double ratio) noexcept {
    // Guard against nonsense; the filter is only designed for small excursions around nominal.
    ratio_ = std::clamp(ratio, nominalRatio_ * 0.95, nominalRatio_ * 1.05);
}

float* SincResampler::inputWritePointer(int channel) noexcept {
    return history_[static_cast<std::size_t>(channel)].data() + fill_;
}

void SincResampler::commitInput(int numFrames) noexcept { fill_ = std::min(capacity_, fill_ + numFrames); }

int SincResampler::pushInput(const float* const* source, int numFrames) noexcept {
    const int frames = std::min(numFrames, inputSpace());
    if (frames <= 0) return 0;
    for (int ch = 0; ch < numChannels_; ++ch) {
        float* dst = inputWritePointer(ch);
        const float* src = source != nullptr ? source[ch] : nullptr;
        if (src != nullptr) std::memcpy(dst, src, static_cast<std::size_t>(frames) * sizeof(float));
        else std::memset(dst, 0, static_cast<std::size_t>(frames) * sizeof(float));
    }
    commitInput(frames);
    return frames;
}

int SincResampler::outputsAvailable() const noexcept {
    const double last = static_cast<double>(fill_ - 1 - kHalfTaps);  // highest usable centre index
    if (position_ >= last + 1.0) return 0;
    return static_cast<int>(std::ceil((last + 1.0 - position_) / ratio_ - 1.0e-9));
}

int SincResampler::inputNeededFor(int numOutputs) const noexcept {
    if (numOutputs <= 0) return 0;
    const double lastPosition = position_ + static_cast<double>(numOutputs - 1) * ratio_;
    const int required = static_cast<int>(std::floor(lastPosition)) + kHalfTaps + 1;
    return std::max(0, required - fill_);
}

int SincResampler::process(float* const* destination, int maxOutputs) noexcept {
    const float* table = table_->data();
    int produced = 0;
    while (produced < maxOutputs) {
        const int index = static_cast<int>(position_);
        if (index + kHalfTaps > fill_ - 1) break;
        const double phase = (position_ - index) * kPhases;
        const int p = std::min(static_cast<int>(phase), kPhases - 1);
        const float t = static_cast<float>(phase - p);
        const float* row0 = table + static_cast<std::size_t>(p * kTaps);
        const float* row1 = row0 + kTaps;
        for (int k = 0; k < kTaps; ++k) interpolated_[static_cast<std::size_t>(k)] = row0[k] + t * (row1[k] - row0[k]);

        const int base = index - kHalfTaps + 1;
        for (int ch = 0; ch < numChannels_; ++ch) {
            const float* x = history_[static_cast<std::size_t>(ch)].data() + base;
            float acc = 0.0f;
            for (int k = 0; k < kTaps; ++k) acc += x[k] * interpolated_[static_cast<std::size_t>(k)];
            destination[ch][produced] = acc;
        }
        position_ += ratio_;
        ++produced;
    }
    compact();
    return produced;
}

void SincResampler::compact() noexcept {
    const int firstNeeded = static_cast<int>(position_) - kHalfTaps + 1;
    const int shift = std::min(firstNeeded, fill_);
    if (shift <= 0) return;
    const int remaining = fill_ - shift;
    for (auto& ch : history_)
        if (remaining > 0)
            std::memmove(ch.data(), ch.data() + shift, static_cast<std::size_t>(remaining) * sizeof(float));
    fill_ = remaining;
    position_ -= shift;
}

double SincResampler::bufferedInputFrames() const noexcept {
    return std::max(0.0, static_cast<double>(fill_) - position_);
}

// ---- DriftController ----------------------------------------------------------------------------------

void DriftController::reset(double nominalRatio, double targetFillFrames, double framesPerSecond,
                            double calibrationSeconds) noexcept {
    nominalRatio_ = nominalRatio > 0.0 ? nominalRatio : 1.0;
    ratio_ = nominalRatio_;
    target_ = targetFillFrames;
    framesPerSecond_ = framesPerSecond > 0.0 ? framesPerSecond : 48000.0;
    smoothedFill_ = targetFillFrames;
    integral_ = 0.0;
    calibrationSeconds_ = calibrationSeconds;
    calibrationTime_ = 0.0;
    calibrationSum_ = 0.0;
    calibrating_ = calibrationSeconds > 0.0;
}

double DriftController::update(double measuredFillFrames, double blockSeconds) noexcept {
    // Closed loop e'' + kp e' + ki e = 0 -> wn = 0.05 rad/s, zeta = 1: no overshoot, and a 200 ppm
    // crystal mismatch costs at most ~2 ms of transient buffer error. Corrections stay far below the
    // audibility of pitch change (+-0.1 % is 1.7 cents and is only reached on pathological drift).
    constexpr double kProportional = 0.1;
    constexpr double kIntegral = 0.0025;
    constexpr double kMaxCorrection = 0.005;
    constexpr double kSmoothingSeconds = 0.5;

    if (calibrating_) {
        calibrationSum_ += measuredFillFrames * blockSeconds;
        calibrationTime_ += blockSeconds;
        if (calibrationTime_ >= calibrationSeconds_) {
            target_ = calibrationSum_ / calibrationTime_;
            smoothedFill_ = target_;
            calibrating_ = false;
        }
        ratio_ = nominalRatio_;
        return ratio_;
    }

    const double blend = 1.0 - std::exp(-blockSeconds / kSmoothingSeconds);
    smoothedFill_ += blend * (measuredFillFrames - smoothedFill_);

    const double errorSeconds = (smoothedFill_ - target_) / framesPerSecond_;
    integral_ += errorSeconds * blockSeconds;
    integral_ = std::clamp(integral_, -kMaxCorrection / kIntegral, kMaxCorrection / kIntegral);

    const double correction =
        std::clamp(kProportional * errorSeconds + kIntegral * integral_, -kMaxCorrection, kMaxCorrection);
    ratio_ = nominalRatio_ * (1.0 + correction);
    return ratio_;
}

}  // namespace reka
