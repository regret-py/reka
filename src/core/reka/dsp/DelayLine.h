#pragma once

#include "reka/common/Types.h"

#include <array>
#include <vector>

namespace reka {

// Stereo integer-sample delay used for plugin delay compensation (PDC) and latency-correct bypass.
// Capacity is fixed at construction (non-real-time); process() is real-time safe.
class DelayLine {
public:
    explicit DelayLine(int maxDelayFrames = 0);

    int capacity() const noexcept { return capacity_; }
    void clear() noexcept;

    // In-place delay of `numFrames` samples on up to two channels by `delayFrames` (clamped to capacity).
    void process(float* const* channels, int numChannels, int numFrames, int delayFrames) noexcept;

private:
    int capacity_ = 0;
    int size_ = 0;  // power of two > capacity
    int writeIndex_ = 0;
    std::array<std::vector<float>, kStripChannels> buffers_;
};

}  // namespace reka
