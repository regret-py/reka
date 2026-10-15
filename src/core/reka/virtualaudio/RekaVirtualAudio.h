#pragma once

#include "reka/io/AsyncEndpointPort.h"
#include "reka/io/EnginePort.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace reka {

// Direction as Windows sees the endpoint:
//   Render  : a playback device. Apps play into it; REKA reads it into an input strip.
//   Capture : a recording device. A REKA bus writes into it; Discord / OBS / Zoom record from it.
enum class EndpointFlow : std::uint8_t { Render = 0, Capture = 1 };

struct SystemEndpoint {
    std::string id;    // IMMDevice endpoint id
    std::string name;  // friendly name, UTF-8
    EndpointFlow flow = EndpointFlow::Render;
    bool isDefault = false;
    bool isDefaultCommunications = false;
    bool isRekaVirtual = false;      // created by the REKA driver
    bool isThirdPartyCable = false;  // VB-CABLE, Virtual Audio Cable, Voicemeeter, ...
    int channels = 0;
    int sampleRate = 0;
};

struct AudioProcess {
    std::uint32_t pid = 0;
    std::string executable;   // "Discord.exe"
    std::string displayName;  // session display name when available
};

enum class DriverState : std::uint8_t {
    NotInstalled,     // \\.\RekaVirtualAudio not present
    Ready,
    VersionMismatch,  // driver speaks a different REKA_DRIVER_INTERFACE_VERSION
    AccessDenied,
    Unsupported,      // not Windows
};

struct VirtualEndpointSpec {
    std::string name;  // "REKA Input 1"
    EndpointFlow flow = EndpointFlow::Render;
    int channels = 2;
    int sampleRate = 48000;
};

// A running endpoint stream (driver ring pump or process-loopback capture) feeding an AsyncEndpointPort.
// Register port() with MixerModel::registerPort(portInfo(), port()).
class IVirtualStream {
public:
    virtual ~IVirtualStream() = default;
    virtual std::shared_ptr<AsyncEndpointPort> port() const = 0;
    virtual PortInfo portInfo() const = 0;  // key: "virtual:<name>" or "app:<exe>:<pid>"
    virtual bool isRunning() const = 0;
    virtual std::string lastError() const = 0;
    virtual void stop() = 0;
};

// Virtual Driver Bridge. Three ways to get audio in and out of other applications:
//   1. REKA Virtual Audio driver (best): dedicated "REKA Input N" / "REKA Output N" endpoints created on
//      demand through IOCTLs, data exchanged through mapped lock-free rings.
//   2. Third-party cables (VB-CABLE etc.): flagged by enumerateEndpoints() and opened by the device
//      manager as ordinary secondary WASAPI devices.
//   3. WASAPI process loopback (Windows 10 build 20348+ / Windows 11): capture one application's audio
//      (e.g. a game or Spotify) straight into an input strip, no driver needed.
class RekaVirtualAudio {
public:
    RekaVirtualAudio();
    ~RekaVirtualAudio();
    RekaVirtualAudio(const RekaVirtualAudio&) = delete;
    RekaVirtualAudio& operator=(const RekaVirtualAudio&) = delete;

    // ---- REKA driver ----
    DriverState driverState() const;
    std::string driverVersion() const;  // "1.0.0" when Ready
    void refreshDriver();
    std::unique_ptr<IVirtualStream> createVirtualEndpoint(const VirtualEndpointSpec& spec, double engineSampleRate,
                                                          int engineMaxBlock, std::string& error);

    // ---- system endpoints (MMDevice API) ----
    std::vector<SystemEndpoint> enumerateEndpoints(std::string* error = nullptr) const;

    // ---- per-application capture (WASAPI process loopback) ----
    bool processLoopbackSupported() const;
    std::vector<AudioProcess> listAudioProcesses() const;
    std::unique_ptr<IVirtualStream> captureProcess(std::uint32_t pid, bool includeChildProcesses,
                                                   double engineSampleRate, int engineMaxBlock, std::string& error);

    // Opens the classic Sound control panel (mmsys.cpl) so the user can pick default devices.
    static bool openSoundControlPanel();

    static bool looksLikeThirdPartyCable(const std::string& endpointName);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace reka
