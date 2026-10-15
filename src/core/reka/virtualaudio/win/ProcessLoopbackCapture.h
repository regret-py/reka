#pragma once

// WASAPI process-loopback capture (one application's audio into an input strip) behind
// RekaVirtualAudio::captureProcess(). Internal; Windows only.

#if defined(_WIN32)

#include "reka/virtualaudio/RekaVirtualAudio.h"

#include <cstdint>
#include <memory>
#include <string>

namespace reka::win {

// Windows build number from RtlGetVersion (not subject to the manifest-based GetVersionEx lie); 0 if unknown.
std::uint32_t windowsBuildNumber() noexcept;

// Process loopback (VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK) exists from Windows 10 build 20348 / Windows 11.
bool processLoopbackAvailable() noexcept;

// Starts capturing `pid` (and, as Windows always does in include mode, its child processes) as float32
// stereo 48 kHz into a new AsyncEndpointPort. Returns nullptr with a readable `error` on failure.
std::unique_ptr<IVirtualStream> startProcessLoopbackCapture(std::uint32_t pid, const std::string& executable,
                                                            double engineSampleRate, int engineMaxBlock,
                                                            std::string& error);

}  // namespace reka::win

#endif  // _WIN32
