#include "reka/virtualaudio/RekaVirtualAudio.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace reka {

bool RekaVirtualAudio::looksLikeThirdPartyCable(const std::string& endpointName) {
    std::string lower(endpointName);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    static constexpr std::array<const char*, 7> kMarkers = {
        "cable input", "cable output", "vb-audio", "virtual audio cable", "voicemeeter", "line 1 (virtual", "blackhole"};
    for (const char* marker : kMarkers)
        if (lower.find(marker) != std::string::npos) return true;
    return false;
}

}  // namespace reka

#if !defined(_WIN32)
// ---- Non-Windows build: the virtual-audio layer is Windows-only. These stubs keep the API usable so the
// rest of REKA (and its tests) builds everywhere; the UI shows the driver as "Unsupported".
namespace reka {

struct RekaVirtualAudio::Impl {};

RekaVirtualAudio::RekaVirtualAudio() : impl_(std::make_unique<Impl>()) {}
RekaVirtualAudio::~RekaVirtualAudio() = default;

DriverState RekaVirtualAudio::driverState() const { return DriverState::Unsupported; }
std::string RekaVirtualAudio::driverVersion() const { return {}; }
void RekaVirtualAudio::refreshDriver() {}

std::unique_ptr<IVirtualStream> RekaVirtualAudio::createVirtualEndpoint(const VirtualEndpointSpec&, double, int,
                                                                        std::string& error) {
    error = "REKA virtual endpoints require Windows";
    return nullptr;
}

std::vector<SystemEndpoint> RekaVirtualAudio::enumerateEndpoints(std::string* error) const {
    if (error != nullptr) *error = "endpoint enumeration requires Windows";
    return {};
}

bool RekaVirtualAudio::processLoopbackSupported() const { return false; }
std::vector<AudioProcess> RekaVirtualAudio::listAudioProcesses() const { return {}; }

std::unique_ptr<IVirtualStream> RekaVirtualAudio::captureProcess(std::uint32_t, bool, double, int, std::string& error) {
    error = "process loopback capture requires Windows";
    return nullptr;
}

bool RekaVirtualAudio::openSoundControlPanel() { return false; }

}  // namespace reka
#endif
