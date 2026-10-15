#pragma once

#include "reka/dsp/SincResampler.h"
#include "reka/io/EnginePort.h"
#include "reka/lockfree/AudioRingBuffer.h"

#include <array>
#include <atomic>
#include <vector>

namespace reka {

inline constexpr int kMaxAsyncPortChannels = 32;

struct AsyncPortFormat {
    int captureChannels = 2;           // endpoint -> engine
    int renderChannels = 2;            // engine -> endpoint
    double endpointSampleRate = 48000.0;
    int endpointMaxBlock = 1024;       // largest chunk the endpoint thread moves at once
    double targetLatencyMs = 0.0;      // 0 = automatic (one endpoint period + one engine block + 2 ms)
};

// Bridges an endpoint that runs on its own thread and clock (a secondary WASAPI device, a REKA virtual
// cable, a per-process loopback capture, ...) into the engine's clock domain.
//
//   endpoint thread --write--> [ring] --SincResampler (drift-locked)--> engine     (capture)
//   engine --SincResampler (drift-locked)--> [ring] --read--> endpoint thread       (render)
//
// One resampler per direction performs both the sample-rate conversion (e.g. 44.1 k device into a 48 k
// engine) and the clock-drift correction, so there is exactly one interpolation stage per hop.
class AsyncEndpointPort final : public IEnginePort {
public:
    struct Stats {
        std::uint32_t captureUnderruns = 0;
        std::uint32_t captureOverruns = 0;
        std::uint32_t renderUnderruns = 0;
        std::uint32_t renderOverruns = 0;
        float captureDriftPpm = 0.0f;
        float renderDriftPpm = 0.0f;
        float captureLatencyMs = 0.0f;
        float renderLatencyMs = 0.0f;
    };

    // Non-real-time. Neither the endpoint thread nor the engine may use the port during this call.
    void configure(const AsyncPortFormat& format, double engineSampleRate, int engineMaxBlock);
    const AsyncPortFormat& format() const noexcept { return format_; }

    // ---- endpoint thread ----
    void endpointWriteCapture(const float* const* source, int numChannels, int numFrames) noexcept;
    void endpointWriteCaptureInterleaved(const float* source, int numChannels, int numFrames) noexcept;
    // Always fills numFrames (silence on underrun). Returns the number of frames of real audio.
    int endpointReadRender(float* const* destination, int numChannels, int numFrames) noexcept;
    int endpointReadRenderInterleaved(float* destination, int numChannels, int numFrames) noexcept;

    // ---- IEnginePort (engine thread) ----
    int numInputChannels() const noexcept override { return format_.captureChannels; }
    int numOutputChannels() const noexcept override { return format_.renderChannels; }
    void beginBlock(int numFrames) noexcept override;
    const float* inputChannel(int channel) const noexcept override;
    float* outputChannel(int channel) noexcept override;
    void endBlock(int numFrames) noexcept override;

    Stats stats() const noexcept;

private:
    void pullCapture(int numFrames) noexcept;
    void pushRender(int numFrames) noexcept;

    AsyncPortFormat format_;
    double engineRate_ = 48000.0;
    int engineMaxBlock_ = 512;

    // capture
    AudioRingBuffer captureRing_;
    SincResampler captureResampler_;
    DriftController captureDrift_;
    std::vector<std::vector<float>> captureBlock_;
    int captureTarget_ = 0;
    bool capturePrimed_ = false;

    // render
    AudioRingBuffer renderRing_;
    SincResampler renderResampler_;
    DriftController renderDrift_;
    std::vector<std::vector<float>> renderBlock_;
    std::vector<std::vector<float>> renderScratch_;
    int renderTarget_ = 0;
    std::atomic<bool> renderUnderrunFlag_{false};

    std::vector<float> silence_;
    std::vector<float> discard_;  // sink for out-of-range render channels

    std::atomic<std::uint32_t> captureUnderruns_{0}, captureOverruns_{0}, renderUnderruns_{0}, renderOverruns_{0};
    std::atomic<float> captureDriftPpm_{0.0f}, renderDriftPpm_{0.0f}, captureLatencyMs_{0.0f}, renderLatencyMs_{0.0f};
};

}  // namespace reka
