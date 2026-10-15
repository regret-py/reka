#include "reka/engine/StripProcessor.h"

#include <algorithm>

namespace reka {

void StripProcessor::prepare(double sampleRate, int maxBlockSize) {
    const auto frames = static_cast<std::size_t>(std::max(1, maxBlockSize));
    storage_.assign(frames * 4, 0.0f);
    for (int ch = 0; ch < kStripChannels; ++ch) {
        pre_[static_cast<std::size_t>(ch)] = storage_.data() + frames * static_cast<std::size_t>(ch);
        post_[static_cast<std::size_t>(ch)] = storage_.data() + frames * static_cast<std::size_t>(2 + ch);
    }
    for (auto& p : phase) {
        p.prepare(sampleRate, 5.0);
        p.reset(1.0f);
    }
    for (auto& g : gain) {
        g.prepare(sampleRate, 15.0);
        g.reset(0.0f);  // new strips fade in instead of clicking
    }
    eq.prepare(sampleRate);
    meter.prepare(sampleRate);
    loudness.prepare(sampleRate);
}

}  // namespace reka
