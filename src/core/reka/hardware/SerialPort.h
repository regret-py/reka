#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace reka {

struct SerialPortInfo {
    std::string name;         // "COM7" on Windows, "/dev/ttyACM0" on Linux
    std::string description;  // friendly name when the OS provides one
    std::string hardwareId;   // e.g. "USB\\VID_1B4F&PID_9206" (Windows) / sysfs path (Linux)
    bool likelyController = false;  // VID matches a known microcontroller vendor (SparkFun, PJRC, Espressif, ...)
};

// Minimal blocking serial port used by the hardware I/O thread.
class ISerialPort {
public:
    virtual ~ISerialPort() = default;

    // Opens 8N1 at `baud` with DTR and RTS asserted (required by ATmega32U4 `if (Serial)`, and the
    // reset-neutral state for ESP32 auto-reset circuits).
    virtual bool open(const std::string& name, int baud, std::string& error) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;

    // Waits up to timeoutMs for at least one byte. Returns bytes read, 0 on timeout, -1 if the port
    // failed (e.g. the USB device was unplugged).
    virtual int read(std::uint8_t* buffer, int maxBytes, int timeoutMs) = 0;
    // Returns false if the port failed.
    virtual bool write(const std::uint8_t* data, int size) = 0;
};

// Platform implementations (SerialPortWin32.cpp / SerialPortPosix.cpp).
std::unique_ptr<ISerialPort> createSerialPort();
std::vector<SerialPortInfo> enumerateSerialPorts();

// USB vendor ids of common Arduino-compatible boards, used to sort auto-detection candidates.
bool isKnownMicrocontrollerVendor(std::uint16_t vendorId) noexcept;

}  // namespace reka
