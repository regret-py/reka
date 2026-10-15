#pragma once

#include "reka/common/Types.h"

#include <array>
#include <atomic>

namespace reka {

// Every meter consumer gets its own peak accumulator so the UI and the hardware bridge can both
// "read and reset" without stealing peaks from each other.
enum class MeterReader : int { Ui = 0, Hardware = 1 };
inline constexpr int kNumMeterReaders = 2;

struct MeterReading {
    std::array<float, kStripChannels> peak{};  // linear, max since this reader's previous read
    std::array<float, kStripChannels> rms{};   // linear, exponential window (default 300 ms)
    bool clipped = false;                      // any sample >= 1.0 since previous read
};

// Real-time peak / RMS tap. process() runs on the audio thread, read() on any one thread per reader.
class MeterTap {
public:
    void prepare(double sampleRate, double rmsWindowMs = 300.0) noexcept;
    void reset() noexcept;

    void process(const float* const* channels, int numChannels, int numFrames) noexcept;

    MeterReading read(MeterReader reader) noexcept;

private:
    std::array<std::array<std::atomic<float>, kStripChannels>, kNumMeterReaders> peak_{};
    std::array<std::atomic<bool>, kNumMeterReaders> clip_{};
    std::array<std::atomic<float>, kStripChannels> rms_{};
    std::array<double, kStripChannels> meanSquare_{};
    double sampleRate_ = 48000.0;
    double windowSamples_ = 14400.0;
};

}  // namespace reka
