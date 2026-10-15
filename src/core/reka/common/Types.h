#pragma once

#include <cstdint>
#include <string>

namespace reka {

// Stable identifiers. StripIds are persisted in .reka profiles, so they never get recycled while a
// profile is loaded. PortIds are runtime-only handles; profiles reference ports by PortKey string.
using StripId = std::uint32_t;
using PortId = std::uint32_t;
using PortKey = std::string;

inline constexpr StripId kInvalidStrip = 0;
inline constexpr PortId kInvalidPort = 0;

// Every strip and bus is processed as a stereo pair. Mono sources are duplicated on entry and mono
// sinks receive (L + R) / 2, which keeps the render loop branch-free and allocation-free.
inline constexpr int kStripChannels = 2;
inline constexpr int kNumEqBands = 8;

enum class StripKind : std::uint8_t { Input = 0, Bus = 1 };

// How the two channels of an input strip are combined before EQ/inserts.
enum class ChannelMode : std::uint8_t {
    Stereo = 0,
    Mono = 1,       // (L + R) / 2 on both channels
    SwapLR = 2,
    LeftOnly = 3,   // L on both channels
    RightOnly = 4,  // R on both channels
};

inline bool isMonoMode(ChannelMode mode) noexcept {
    return mode == ChannelMode::Mono || mode == ChannelMode::LeftOnly || mode == ChannelMode::RightOnly;
}

const char* toString(ChannelMode mode) noexcept;
bool fromString(const std::string& text, ChannelMode& mode) noexcept;

}  // namespace reka
