#include "reka/io/AsyncEndpointPort.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace reka {

namespace {
using ChannelPointers = std::array<float*, kMaxAsyncPortChannels>;

void zeroChannels(std::vector<std::vector<float>>& channels, int offset, int numFrames) noexcept {
    for (auto& ch : channels)
        std::memset(ch.data() + offset, 0, static_cast<std::size_t>(numFrames) * sizeof(float));
}
}  // namespace

void AsyncEndpointPort::configure(const AsyncPortFormat& format, double engineSampleRate, int engineMaxBlock) {
    format_ = format;
    format_.captureChannels = std::clamp(format_.captureChannels, 0, kMaxAsyncPortChannels);
    format_.renderChannels = std::clamp(format_.renderChannels, 0, kMaxAsyncPortChannels);
    format_.endpointMaxBlock = std::max(16, format_.endpointMaxBlock);
    engineRate_ = engineSampleRate;
    engineMaxBlock_ = std::max(16, engineMaxBlock);

    const double endpointRate = format_.endpointSampleRate;
    const double captureRatio = endpointRate / engineRate_;  // endpoint frames per engine frame
    const double renderRatio = engineRate_ / endpointRate;   // engine frames per endpoint frame

    auto autoTarget = [&]() {
        if (format_.targetLatencyMs > 0.0) return static_cast<int>(format_.targetLatencyMs * 0.001 * endpointRate);
        return static_cast<int>(format_.endpointMaxBlock + engineMaxBlock_ * captureRatio + 0.002 * endpointRate);
    };
    const int target = std::max(32, autoTarget());
    const int ringFrames = target * 4 + format_.endpointMaxBlock * 4;

    silence_.assign(static_cast<std::size_t>(engineMaxBlock_), 0.0f);
    discard_.assign(static_cast<std::size_t>(engineMaxBlock_), 0.0f);

    if (format_.captureChannels > 0) {
        captureRing_.allocate(format_.captureChannels, ringFrames);
        const int maxChunk = static_cast<int>(std::ceil(engineMaxBlock_ * captureRatio * 1.06)) + SincResampler::kTaps;
        captureResampler_.prepare(format_.captureChannels, captureRatio, maxChunk);
        captureDrift_.reset(captureRatio, target, endpointRate);
        captureBlock_.assign(static_cast<std::size_t>(format_.captureChannels),
                             std::vector<float>(static_cast<std::size_t>(engineMaxBlock_), 0.0f));
        captureTarget_ = target;
        capturePrimed_ = false;
    } else {
        captureBlock_.clear();
    }

    if (format_.renderChannels > 0) {
        renderRing_.allocate(format_.renderChannels, ringFrames);
        renderResampler_.prepare(format_.renderChannels, renderRatio, engineMaxBlock_ + SincResampler::kTaps);
        renderDrift_.reset(renderRatio, target, endpointRate);
        renderBlock_.assign(static_cast<std::size_t>(format_.renderChannels),
                            std::vector<float>(static_cast<std::size_t>(engineMaxBlock_), 0.0f));
        const int maxOut = static_cast<int>(std::ceil((engineMaxBlock_ + SincResampler::kTaps) / (renderRatio * 0.95))) + 16;
        renderScratch_.assign(static_cast<std::size_t>(format_.renderChannels),
                              std::vector<float>(static_cast<std::size_t>(maxOut), 0.0f));
        renderTarget_ = target;
        renderRing_.writeSilence(target);  // pre-roll so the endpoint never starts on an underrun
    } else {
        renderBlock_.clear();
        renderScratch_.clear();
    }
    renderUnderrunFlag_.store(false);
    captureUnderruns_ = captureOverruns_ = renderUnderruns_ = renderOverruns_ = 0;
}

// ---- endpoint side ---------------------------------------------------------------------------------------

void AsyncEndpointPort::endpointWriteCapture(const float* const* source, int numChannels, int numFrames) noexcept {
    if (format_.captureChannels == 0) return;
    const int written = captureRing_.write(source, numChannels, numFrames);
    if (written < numFrames) captureOverruns_.fetch_add(1, std::memory_order_relaxed);
}

void AsyncEndpointPort::endpointWriteCaptureInterleaved(const float* source, int numChannels, int numFrames) noexcept {
    if (format_.captureChannels == 0) return;
    const int written = captureRing_.writeInterleaved(source, numChannels, numFrames);
    if (written < numFrames) captureOverruns_.fetch_add(1, std::memory_order_relaxed);
}

int AsyncEndpointPort::endpointReadRender(float* const* destination, int numChannels, int numFrames) noexcept {
    if (format_.renderChannels == 0) {
        for (int ch = 0; ch < numChannels; ++ch)
            std::memset(destination[ch], 0, static_cast<std::size_t>(numFrames) * sizeof(float));
        return 0;
    }
    // If the engine was stalled and then caught up, drop the excess instead of keeping the latency.
    const int excess = renderRing_.availableToRead() - renderTarget_ * 3;
    if (excess > 0) renderRing_.discard(excess);

    const int got = renderRing_.read(destination, numChannels, numFrames);
    if (got < numFrames) {
        for (int ch = 0; ch < numChannels; ++ch)
            std::memset(destination[ch] + got, 0, static_cast<std::size_t>(numFrames - got) * sizeof(float));
        renderUnderruns_.fetch_add(1, std::memory_order_relaxed);
        renderUnderrunFlag_.store(true, std::memory_order_release);
    }
    return got;
}

int AsyncEndpointPort::endpointReadRenderInterleaved(float* destination, int numChannels, int numFrames) noexcept {
    if (format_.renderChannels == 0) {
        std::memset(destination, 0, static_cast<std::size_t>(numFrames * numChannels) * sizeof(float));
        return 0;
    }
    const int excess = renderRing_.availableToRead() - renderTarget_ * 3;
    if (excess > 0) renderRing_.discard(excess);

    const int got = renderRing_.readInterleaved(destination, numChannels, numFrames);
    if (got < numFrames) {
        std::memset(destination + static_cast<std::size_t>(got) * numChannels, 0,
                    static_cast<std::size_t>((numFrames - got) * numChannels) * sizeof(float));
        renderUnderruns_.fetch_add(1, std::memory_order_relaxed);
        renderUnderrunFlag_.store(true, std::memory_order_release);
    }
    return got;
}

// ---- engine side -----------------------------------------------------------------------------------------

void AsyncEndpointPort::beginBlock(int numFrames) noexcept {
    if (format_.captureChannels > 0) pullCapture(numFrames);
    if (format_.renderChannels > 0) zeroChannels(renderBlock_, 0, numFrames);
    std::memset(discard_.data(), 0, static_cast<std::size_t>(numFrames) * sizeof(float));
}

void AsyncEndpointPort::pullCapture(int numFrames) noexcept {
    const int channels = format_.captureChannels;
    if (!capturePrimed_) {
        if (captureRing_.availableToRead() < captureTarget_) {
            zeroChannels(captureBlock_, 0, numFrames);
            return;
        }
        capturePrimed_ = true;
        captureResampler_.reset();
        captureDrift_.reset(captureResampler_.nominalRatio(), captureTarget_, format_.endpointSampleRate);
    }

    // Resynchronise after a long engine stall instead of carrying the backlog as latency.
    const int excess = captureRing_.availableToRead() - captureTarget_ * 3;
    if (excess > 0) captureRing_.discard(excess);

    ChannelPointers out{};
    int produced = 0;
    while (produced < numFrames) {
        for (int ch = 0; ch < channels; ++ch) out[static_cast<std::size_t>(ch)] = captureBlock_[static_cast<std::size_t>(ch)].data() + produced;
        produced += captureResampler_.process(out.data(), numFrames - produced);
        if (produced >= numFrames) break;

        const int need = std::max(1, captureResampler_.inputNeededFor(numFrames - produced));
        const int take = std::min({need, captureResampler_.inputSpace(), captureRing_.availableToRead()});
        if (take <= 0) {  // underrun: fill with silence and re-prime
            zeroChannels(captureBlock_, produced, numFrames - produced);
            captureUnderruns_.fetch_add(1, std::memory_order_relaxed);
            capturePrimed_ = false;
            return;
        }
        ChannelPointers in{};
        for (int ch = 0; ch < channels; ++ch) in[static_cast<std::size_t>(ch)] = captureResampler_.inputWritePointer(ch);
        const int read = captureRing_.read(in.data(), channels, take);
        captureResampler_.commitInput(read);
    }

    const double fill = captureRing_.availableToRead() + captureResampler_.bufferedInputFrames();
    captureResampler_.setRatio(captureDrift_.update(fill, numFrames / engineRate_));
    captureDriftPpm_.store(static_cast<float>(captureDrift_.correctionPpm()), std::memory_order_relaxed);
    captureLatencyMs_.store(static_cast<float>(captureDrift_.smoothedFill() * 1000.0 / format_.endpointSampleRate),
                            std::memory_order_relaxed);
}

const float* AsyncEndpointPort::inputChannel(int channel) const noexcept {
    if (channel >= 0 && channel < format_.captureChannels) return captureBlock_[static_cast<std::size_t>(channel)].data();
    return silence_.data();
}

float* AsyncEndpointPort::outputChannel(int channel) noexcept {
    if (channel >= 0 && channel < format_.renderChannels) return renderBlock_[static_cast<std::size_t>(channel)].data();
    return discard_.data();
}

void AsyncEndpointPort::endBlock(int numFrames) noexcept {
    if (format_.renderChannels > 0) pushRender(numFrames);
}

void AsyncEndpointPort::pushRender(int numFrames) noexcept {
    const int channels = format_.renderChannels;
    if (renderUnderrunFlag_.exchange(false, std::memory_order_acquire)) {
        const int deficit = renderTarget_ - renderRing_.availableToRead();
        if (deficit > 0) renderRing_.writeSilence(deficit);
        renderDrift_.reset(renderResampler_.nominalRatio(), renderTarget_, format_.endpointSampleRate);
    }

    ChannelPointers in{};
    ChannelPointers out{};
    for (int ch = 0; ch < channels; ++ch) out[static_cast<std::size_t>(ch)] = renderScratch_[static_cast<std::size_t>(ch)].data();
    const int scratchFrames = static_cast<int>(renderScratch_[0].size());

    int consumed = 0;
    while (consumed < numFrames) {
        for (int ch = 0; ch < channels; ++ch) in[static_cast<std::size_t>(ch)] = renderBlock_[static_cast<std::size_t>(ch)].data() + consumed;
        const int accepted = renderResampler_.pushInput(in.data(), numFrames - consumed);
        consumed += accepted;
        const int produced = renderResampler_.process(out.data(), scratchFrames);
        if (produced > 0) {
            const int written = renderRing_.write(out.data(), channels, produced);
            if (written < produced) renderOverruns_.fetch_add(1, std::memory_order_relaxed);
        }
        if (accepted == 0 && produced == 0) break;  // cannot happen with correctly sized buffers
    }

    const double fill = renderRing_.availableToRead() + renderResampler_.bufferedInputFrames() / renderResampler_.ratio();
    renderResampler_.setRatio(renderDrift_.update(fill, numFrames / engineRate_));
    renderDriftPpm_.store(static_cast<float>(renderDrift_.correctionPpm()), std::memory_order_relaxed);
    renderLatencyMs_.store(static_cast<float>(renderDrift_.smoothedFill() * 1000.0 / format_.endpointSampleRate),
                           std::memory_order_relaxed);
}

AsyncEndpointPort::Stats AsyncEndpointPort::stats() const noexcept {
    Stats s;
    s.captureUnderruns = captureUnderruns_.load(std::memory_order_relaxed);
    s.captureOverruns = captureOverruns_.load(std::memory_order_relaxed);
    s.renderUnderruns = renderUnderruns_.load(std::memory_order_relaxed);
    s.renderOverruns = renderOverruns_.load(std::memory_order_relaxed);
    s.captureDriftPpm = captureDriftPpm_.load(std::memory_order_relaxed);
    s.renderDriftPpm = renderDriftPpm_.load(std::memory_order_relaxed);
    s.captureLatencyMs = captureLatencyMs_.load(std::memory_order_relaxed);
    s.renderLatencyMs = renderLatencyMs_.load(std::memory_order_relaxed);
    return s;
}

}  // namespace reka
