#pragma once

// User-mode client of the REKA Virtual Audio kernel driver (see RekaDriverInterface.h): control-device
// handshake, endpoint listing, and the ring pump behind RekaVirtualAudio::createVirtualEndpoint().
// Internal; Windows only.

#if defined(_WIN32)

#include "reka/virtualaudio/RekaDriverInterface.h"
#include "reka/virtualaudio/RekaVirtualAudio.h"

#include <windows.h>

#include <memory>
#include <string>
#include <vector>

namespace reka::win {

// One handle to \\.\RekaVirtualAudio. The driver ties endpoints and ring mappings to the handle that
// created them, so every virtual endpoint stream owns its own connection.
class RekaDriverConnection {
public:
    RekaDriverConnection() = default;
    ~RekaDriverConnection() { close(); }
    RekaDriverConnection(const RekaDriverConnection&) = delete;
    RekaDriverConnection& operator=(const RekaDriverConnection&) = delete;

    // Opens the control device and performs the IOCTL_REKA_GET_INFO version handshake. Anything but
    // Ready leaves the connection closed and explains why in `error`.
    DriverState open(std::string& error);
    void close() noexcept;
    bool isOpen() const noexcept { return handle_ != INVALID_HANDLE_VALUE; }

    const REKA_DRIVER_INFO& info() const noexcept { return info_; }
    std::string versionString() const;  // "1.0.0"

    // DeviceIoControl wrapper. On failure `error` holds the Win32 error text.
    bool control(DWORD code, const void* in, DWORD inBytes, void* out, DWORD outBytes, DWORD& returned,
                 std::string& error) const;

    // Friendly names of the endpoints the driver currently exposes ("REKA Input 1", ...).
    std::vector<std::string> listEndpointNames(std::string* error = nullptr) const;

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    REKA_DRIVER_INFO info_{};
};

// Creates a driver endpoint, maps its ring and starts the pump thread that feeds an AsyncEndpointPort.
std::unique_ptr<IVirtualStream> createDriverEndpointStream(const VirtualEndpointSpec& spec, double engineSampleRate,
                                                           int engineMaxBlock, std::string& error);

}  // namespace reka::win

#endif  // _WIN32
