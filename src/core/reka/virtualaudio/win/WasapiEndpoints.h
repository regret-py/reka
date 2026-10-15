#pragma once

// MMDevice / audio-session queries behind RekaVirtualAudio::enumerateEndpoints() and
// RekaVirtualAudio::listAudioProcesses(). Internal; Windows only.

#if defined(_WIN32)

#include "reka/virtualaudio/RekaVirtualAudio.h"

#include <cstdint>
#include <string>
#include <vector>

namespace reka::win {

// Active render + capture endpoints with default-device flags and their shared-mode format.
// `rekaEndpointNames` are the endpoint names the REKA driver reports (may be empty).
std::vector<SystemEndpoint> enumerateSystemEndpoints(const std::vector<std::string>& rekaEndpointNames, std::string* error);

// Processes that own an audio session on any active render endpoint, one entry per pid, REKA itself and
// the system-sounds session excluded.
std::vector<AudioProcess> listAudioSessionProcesses();

// "Discord.exe" for a pid; empty when the process does not exist or cannot be queried.
std::string processExecutableName(std::uint32_t pid);

}  // namespace reka::win

#endif  // _WIN32
