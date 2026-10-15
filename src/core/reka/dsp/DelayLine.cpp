#include "reka/dsp/DelayLine.h"

#include <algorithm>

namespace reka {

DelayLine::DelayLine(int maxDelayFrames) : capacity_(std::max(0, maxDelayFrames)) {
    size_ = 1;
    while (size_ <= capacity_) size_ <<= 1;
    for (auto& buffer : buffers_) buffer.assign(static_cast<std::size_t>(size_), 0.0f);
}

void DelayLine::clear() noexcept {
    for (auto& buffer : buffers_) std::fill(buffer.begin(), buffer.end(), 0.0f);
    writeIndex_ = 0;
}

void DelayLine::process(float* const* channels, int numChannels, int numFrames, int delayFrames) noexcept {
    const int delay = std::clamp(delayFrames, 0, capacity_);
    const int mask = size_ - 1;
    const int chCount = std::min(numChannels, kStripChannels);
    if (delay == 0) {
        // Keep the history warm so a later delay change does not read stale audio.
        for (int ch = 0; ch < chCount; ++ch) {
            float* line = buffers_[static_cast<std::size_t>(ch)].data();
            int w = writeIndex_;
            for (int i = 0; i < numFrames; ++i) {
                line[w] = channels[ch][i];
                w = (w + 1) & mask;
            }
        }
    } else {
        for (int ch = 0; ch < chCount; ++ch) {
            float* line = buffers_[static_cast<std::size_t>(ch)].data();
            float* data = channels[ch];
            int w = writeIndex_;
            for (int i = 0; i < numFrames; ++i) {
                line[w] = data[i];
                data[i] = line[(w - delay) & mask];
                w = (w + 1) & mask;
            }
        }
    }
    writeIndex_ = (writeIndex_ + numFrames) & mask;
}

}  // namespace reka
