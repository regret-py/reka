#pragma once

#include "reka/engine/InsertProcessor.h"
#include "reka/io/EnginePort.h"

#include <cmath>
#include <vector>

namespace reka::test {

inline constexpr double kPi = 3.14159265358979323846;

// An engine port backed by plain vectors: tests fill `inputs` and inspect `outputs`.
class VectorPort final : public IEnginePort {
public:
    VectorPort(int numInputs, int numOutputs, int maxFrames)
        : inputs(static_cast<std::size_t>(numInputs), std::vector<float>(static_cast<std::size_t>(maxFrames), 0.0f)),
          outputs(static_cast<std::size_t>(numOutputs), std::vector<float>(static_cast<std::size_t>(maxFrames), 0.0f)) {}

    int numInputChannels() const noexcept override { return static_cast<int>(inputs.size()); }
    int numOutputChannels() const noexcept override { return static_cast<int>(outputs.size()); }
    void beginBlock(int numFrames) noexcept override {
        for (auto& ch : outputs) std::fill(ch.begin(), ch.begin() + numFrames, 0.0f);
        ++blocks;
    }
    const float* inputChannel(int channel) const noexcept override { return inputs[static_cast<std::size_t>(channel)].data(); }
    float* outputChannel(int channel) noexcept override { return outputs[static_cast<std::size_t>(channel)].data(); }
    void endBlock(int) noexcept override {}

    void fill(int channel, float value) {
        for (float& s : inputs[static_cast<std::size_t>(channel)]) s = value;
    }

    std::vector<std::vector<float>> inputs;
    std::vector<std::vector<float>> outputs;
    int blocks = 0;
};

// A plugin stand-in: constant gain plus a fixed reported latency (implemented as a real delay).
class GainDelayInsert final : public IInsertProcessor {
public:
    GainDelayInsert(float gain, int latency, std::string id = "test.gain")
        : gain_(gain), latency_(latency) {
        descriptor_.pluginId = std::move(id);
        descriptor_.name = "GainDelay";
        descriptor_.format = "Internal";
    }
    void prepare(double, int) override {
        for (auto& line : history_) line.assign(static_cast<std::size_t>(latency_ + 1), 0.0f);
        prepared = true;
    }
    void release() override {}
    void process(float* const* channels, int numChannels, int numFrames) noexcept override {
        for (int ch = 0; ch < numChannels && ch < 2; ++ch) {
            auto& line = history_[static_cast<std::size_t>(ch)];
            for (int i = 0; i < numFrames; ++i) {
                const float in = channels[ch][i] * gain_;
                if (latency_ == 0) {
                    channels[ch][i] = in;
                    continue;
                }
                channels[ch][i] = line[static_cast<std::size_t>(position_[static_cast<std::size_t>(ch)])];
                line[static_cast<std::size_t>(position_[static_cast<std::size_t>(ch)])] = in;
                position_[static_cast<std::size_t>(ch)] = (position_[static_cast<std::size_t>(ch)] + 1) % latency_;
            }
        }
        ++processed;
    }
    int latencySamples() const noexcept override { return latency_; }
    std::vector<std::uint8_t> saveState() override { return state; }
    bool restoreState(const std::vector<std::uint8_t>& s) override {
        state = s;
        return true;
    }
    const InsertDescriptor& descriptor() const noexcept override { return descriptor_; }
    void fail() { markFailed(); }

    std::vector<std::uint8_t> state;
    bool prepared = false;
    int processed = 0;

private:
    float gain_;
    int latency_;
    InsertDescriptor descriptor_;
    std::vector<float> history_[2];
    int position_[2] = {0, 0};
};

inline double rms(const std::vector<float>& data, std::size_t begin, std::size_t end) {
    double sum = 0.0;
    for (std::size_t i = begin; i < end; ++i) sum += static_cast<double>(data[i]) * data[i];
    return std::sqrt(sum / static_cast<double>(end - begin));
}

}  // namespace reka::test
