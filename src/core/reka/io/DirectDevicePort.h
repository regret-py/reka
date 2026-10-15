#pragma once

#include "reka/io/EnginePort.h"

#include <vector>

namespace reka {

// The master device: its callback buffers are used in place (zero copy, zero added latency).
// The device callback calls setDeviceBuffers() and then RekaAudioEngine::processBlock(); if the driver
// hands over more frames than the engine's max block, the engine walks the buffers in sub-blocks and this
// port advances its offset in endBlock().
class DirectDevicePort final : public IEnginePort {
public:
    // Non-real-time: channel counts of the opened device and the engine's max block size.
    void configure(int numInputs, int numOutputs, int maxBlockSize);

    // Audio thread, once per device callback.
    void setDeviceBuffers(const float* const* inputs, int numInputs, float* const* outputs, int numOutputs,
                          int numFrames) noexcept;

    int numInputChannels() const noexcept override { return numInputs_; }
    int numOutputChannels() const noexcept override { return numOutputs_; }
    void beginBlock(int numFrames) noexcept override;
    const float* inputChannel(int channel) const noexcept override;
    float* outputChannel(int channel) noexcept override;
    void endBlock(int numFrames) noexcept override;

private:
    int numInputs_ = 0;
    int numOutputs_ = 0;
    const float* const* deviceInputs_ = nullptr;
    float* const* deviceOutputs_ = nullptr;
    int deviceInputCount_ = 0;
    int deviceOutputCount_ = 0;
    int deviceFrames_ = 0;
    int offset_ = 0;
    std::vector<float> silence_;  // returned for channels the driver did not supply
    std::vector<float> scratch_;  // sink for channels the driver did not supply
};

}  // namespace reka
