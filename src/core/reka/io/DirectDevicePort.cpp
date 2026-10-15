#include "reka/io/DirectDevicePort.h"

#include <algorithm>
#include <cstring>

namespace reka {

void DirectDevicePort::configure(int numInputs, int numOutputs, int maxBlockSize) {
    numInputs_ = std::max(0, numInputs);
    numOutputs_ = std::max(0, numOutputs);
    silence_.assign(static_cast<std::size_t>(std::max(1, maxBlockSize)), 0.0f);
    scratch_.assign(static_cast<std::size_t>(std::max(1, maxBlockSize)), 0.0f);
}

void DirectDevicePort::setDeviceBuffers(const float* const* inputs, int numInputs, float* const* outputs,
                                        int numOutputs, int numFrames) noexcept {
    deviceInputs_ = inputs;
    deviceOutputs_ = outputs;
    deviceInputCount_ = inputs != nullptr ? numInputs : 0;
    deviceOutputCount_ = outputs != nullptr ? numOutputs : 0;
    deviceFrames_ = numFrames;
    offset_ = 0;
    // Outputs are accumulated into, so clear the whole callback buffer once.
    for (int ch = 0; ch < deviceOutputCount_; ++ch)
        if (outputs[ch] != nullptr) std::memset(outputs[ch], 0, static_cast<std::size_t>(numFrames) * sizeof(float));
}

void DirectDevicePort::beginBlock(int numFrames) noexcept {
    // The scratch sink absorbs writes to channels the driver did not provide; keep it bounded.
    std::memset(scratch_.data(), 0, static_cast<std::size_t>(std::min<int>(numFrames, static_cast<int>(scratch_.size()))) * sizeof(float));
}

const float* DirectDevicePort::inputChannel(int channel) const noexcept {
    if (channel >= 0 && channel < deviceInputCount_ && deviceInputs_[channel] != nullptr)
        return deviceInputs_[channel] + offset_;
    return silence_.data();
}

float* DirectDevicePort::outputChannel(int channel) noexcept {
    if (channel >= 0 && channel < deviceOutputCount_ && deviceOutputs_[channel] != nullptr)
        return deviceOutputs_[channel] + offset_;
    return scratch_.data();
}

void DirectDevicePort::endBlock(int numFrames) noexcept { offset_ = std::min(deviceFrames_, offset_ + numFrames); }

}  // namespace reka
