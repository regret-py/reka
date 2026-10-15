#pragma once

#include "reka/common/Types.h"

#include <string>

namespace reka {

enum class PortType : std::uint8_t {
    MasterDevice = 0,      // the hardware device whose callback clocks the engine (ASIO / WASAPI)
    SecondaryDevice = 1,   // another hardware device, drift-locked through an AsyncEndpointPort
    VirtualEndpoint = 2,   // REKA virtual cable (kernel driver) or a third-party cable
    ProcessCapture = 3,    // WASAPI per-process loopback capture
    Internal = 4,          // tests, sandbox, generators
};

struct PortInfo {
    PortId id = kInvalidPort;
    PortKey key;  // stable identity persisted in profiles, e.g. "device:Speakers (Realtek)"
    std::string displayName;
    PortType type = PortType::Internal;
    int numInputs = 0;   // channels the engine can read (capture side)
    int numOutputs = 0;  // channels the engine can write (render side)
};

// An endpoint as seen from the real-time engine. All methods are called on the audio thread, once per
// engine (sub-)block, in the order beginBlock -> inputChannel / outputChannel... -> endBlock.
class IEnginePort {
public:
    virtual ~IEnginePort() = default;

    virtual int numInputChannels() const noexcept = 0;
    virtual int numOutputChannels() const noexcept = 0;

    // Makes `numFrames` frames of capture audio available and zeroes the render accumulators.
    virtual void beginBlock(int numFrames) noexcept = 0;
    // Valid between beginBlock and endBlock. Never null for a valid channel index.
    virtual const float* inputChannel(int channel) const noexcept = 0;
    // Buses accumulate (+=) into these.
    virtual float* outputChannel(int channel) noexcept = 0;
    // Hands the rendered audio to the endpoint.
    virtual void endBlock(int numFrames) noexcept = 0;
};

}  // namespace reka
