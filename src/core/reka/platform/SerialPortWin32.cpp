// Windows serial port: overlapped I/O on \\.\COMn plus SetupAPI enumeration. The POSIX twin lives in
// SerialPortPosix.cpp; both give ISerialPort::read() the same contract (bytes / 0 on timeout / -1 when the
// device is gone).
#include "reka/hardware/SerialPort.h"

#if defined(_WIN32)

#include "reka/platform/Win32Util.h"

#include <windows.h>
#include <setupapi.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

namespace reka {

namespace {

// Spelled out instead of pulling DEFINE_GUID instances from ntddser.h / devguid.h (INITGUID games).
// GUID_DEVINTERFACE_COMPORT {86E0D1E0-8089-11D0-9CE4-08003E301F73}
constexpr GUID kGuidDevInterfaceComPort = {0x86E0D1E0, 0x8089, 0x11D0, {0x9C, 0xE4, 0x08, 0x00, 0x3E, 0x30, 0x1F, 0x73}};
// GUID_DEVCLASS_PORTS {4D36E978-E325-11CE-BFC1-08002BE10318}
constexpr GUID kGuidDevClassPorts = {0x4D36E978, 0xE325, 0x11CE, {0xBF, 0xC1, 0x08, 0x00, 0x2B, 0xE1, 0x03, 0x18}};

// Extra wait on top of the COMMTIMEOUTS read timeout, for drivers that do not honour it.
constexpr DWORD kReadWaitSlackMs = 50;
constexpr int kMaxReadTimeoutMs = 60000;

class Win32SerialPort final : public ISerialPort {
public:
    Win32SerialPort() noexcept
        : readEvent_(CreateEventW(nullptr, TRUE, FALSE, nullptr)), writeEvent_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}

    ~Win32SerialPort() override {
        close();
        if (readEvent_ != nullptr) CloseHandle(readEvent_);
        if (writeEvent_ != nullptr) CloseHandle(writeEvent_);
    }

    Win32SerialPort(const Win32SerialPort&) = delete;
    Win32SerialPort& operator=(const Win32SerialPort&) = delete;

    bool open(const std::string& name, int baud, std::string& error) override {
        close();
        if (readEvent_ == nullptr || writeEvent_ == nullptr) {
            error = name + ": could not create I/O events";
            return false;
        }
        if (name.empty()) {
            error = "no serial port name given";
            return false;
        }

        // "COM7", "com7" and "\\.\COM7" are all accepted; the \\.\ prefix is mandatory from COM10 upwards.
        std::wstring path = win::toWide(name);
        if (path.rfind(L"\\\\.\\", 0) != 0) path = L"\\\\.\\" + path;

        HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_OVERLAPPED, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            const DWORD code = GetLastError();
            if (code == ERROR_ACCESS_DENIED || code == ERROR_SHARING_VIOLATION)
                error = name + ": the port is in use by another application";
            else if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND)
                error = name + ": no such serial port";
            else
                error = name + ": " + win::win32ErrorMessage(code);
            return false;
        }

        SetupComm(handle, 4096, 4096);  // advisory buffer sizes; failure is harmless

        DCB dcb{};
        dcb.DCBlength = sizeof(dcb);
        if (!GetCommState(handle, &dcb)) {
            error = name + ": not a serial port (" + win::win32ErrorMessage(GetLastError()) + ")";
            CloseHandle(handle);
            return false;
        }
        baud_ = baud > 0 ? baud : 115200;
        dcb.BaudRate = static_cast<DWORD>(baud_);
        dcb.ByteSize = 8;
        dcb.Parity = NOPARITY;
        dcb.StopBits = ONESTOPBIT;
        dcb.fBinary = TRUE;
        dcb.fParity = FALSE;
        dcb.fOutxCtsFlow = FALSE;
        dcb.fOutxDsrFlow = FALSE;
        dcb.fDtrControl = DTR_CONTROL_ENABLE;  // ATmega32U4 `if (Serial)` waits for DTR
        dcb.fDsrSensitivity = FALSE;
        dcb.fTXContinueOnXoff = TRUE;
        dcb.fOutX = FALSE;
        dcb.fInX = FALSE;
        dcb.fErrorChar = FALSE;
        dcb.fNull = FALSE;
        dcb.fRtsControl = RTS_CONTROL_ENABLE;  // reset-neutral state of ESP32 auto-reset circuits
        dcb.fAbortOnError = FALSE;             // a framing error must not abort every pending read
        if (!SetCommState(handle, &dcb)) {
            error = name + ": could not configure " + std::to_string(baud_) + " baud 8N1 (" +
                    win::win32ErrorMessage(GetLastError()) + ")";
            CloseHandle(handle);
            return false;
        }
        EscapeCommFunction(handle, SETDTR);
        EscapeCommFunction(handle, SETRTS);

        handle_ = handle;
        currentReadTimeout_ = -1;
        if (!applyReadTimeout(0)) {
            error = name + ": could not set port timeouts (" + win::win32ErrorMessage(GetLastError()) + ")";
            close();
            return false;
        }
        PurgeComm(handle_, PURGE_RXABORT | PURGE_TXABORT | PURGE_RXCLEAR | PURGE_TXCLEAR);
        DWORD errors = 0;
        ClearCommError(handle_, &errors, nullptr);
        return true;
    }

    void close() override {
        if (handle_ == INVALID_HANDLE_VALUE) return;
        CancelIoEx(handle_, nullptr);
        CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
        currentReadTimeout_ = -1;
    }

    bool isOpen() const override { return handle_ != INVALID_HANDLE_VALUE; }

    int read(std::uint8_t* buffer, int maxBytes, int timeoutMs) override {
        if (handle_ == INVALID_HANDLE_VALUE) return -1;
        if (buffer == nullptr || maxBytes <= 0) return 0;
        timeoutMs = std::clamp(timeoutMs, 0, kMaxReadTimeoutMs);
        // SetCommTimeouts fails once the USB device has been unplugged. On a live port the read below still
        // works with the previous timeouts, bounded by the overlapped wait and the deadline.
        if (!applyReadTimeout(timeoutMs) && !commAlive()) return -1;

        using Clock = std::chrono::steady_clock;
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
        for (;;) {
            const int n = readOnce(buffer, maxBytes, timeoutMs);
            if (n != 0) return n;
            // Zero bytes: normally the COMMTIMEOUTS timeout expired. Some drivers keep completing reads
            // with zero bytes after a surprise removal instead of failing them; ClearCommError does fail.
            if (!commAlive()) return -1;
            if (Clock::now() >= deadline) return 0;
            // A driver that completes reads early: wait out the rest of the timeout without spinning.
            Sleep(1);
        }
    }

    bool write(const std::uint8_t* data, int size) override {
        if (handle_ == INVALID_HANDLE_VALUE) return false;
        if (data == nullptr || size <= 0) return true;
        // Wire time at the configured baud (10 bits per byte) plus generous USB scheduling slack.
        const DWORD budgetMs =
            100u + static_cast<DWORD>(static_cast<unsigned long long>(size) * 10000ull / static_cast<unsigned long long>(baud_));
        int offset = 0;
        while (offset < size) {
            OVERLAPPED overlapped{};
            overlapped.hEvent = writeEvent_;
            ResetEvent(writeEvent_);
            if (!WriteFile(handle_, data + offset, static_cast<DWORD>(size - offset), nullptr, &overlapped)) {
                if (GetLastError() != ERROR_IO_PENDING) return false;
                if (WaitForSingleObject(writeEvent_, budgetMs) != WAIT_OBJECT_0) {
                    CancelIoEx(handle_, &overlapped);
                    DWORD ignored = 0;
                    GetOverlappedResult(handle_, &overlapped, &ignored, TRUE);  // the OVERLAPPED must outlive the I/O
                    return false;
                }
            }
            DWORD written = 0;
            if (!GetOverlappedResult(handle_, &overlapped, &written, TRUE)) return false;
            if (written == 0) return false;  // write timeout reported by the driver
            offset += static_cast<int>(written);
        }
        return true;
    }

private:
    // read(timeoutMs) semantics are delegated to the driver: "return as soon as at least one byte is
    // available, or after timeoutMs with nothing" is exactly ReadIntervalTimeout = ReadTotalTimeoutMultiplier
    // = MAXDWORD with ReadTotalTimeoutConstant = timeoutMs. timeoutMs == 0 means "return immediately".
    bool applyReadTimeout(int timeoutMs) {
        if (timeoutMs == currentReadTimeout_) return true;
        COMMTIMEOUTS timeouts{};
        timeouts.ReadIntervalTimeout = MAXDWORD;
        timeouts.ReadTotalTimeoutMultiplier = timeoutMs > 0 ? MAXDWORD : 0;
        timeouts.ReadTotalTimeoutConstant = static_cast<DWORD>(timeoutMs);
        timeouts.WriteTotalTimeoutMultiplier = 0;
        timeouts.WriteTotalTimeoutConstant = 0;  // writes are bounded by the overlapped wait in write()
        if (!SetCommTimeouts(handle_, &timeouts)) return false;
        currentReadTimeout_ = timeoutMs;
        return true;
    }

    int readOnce(std::uint8_t* buffer, int maxBytes, int timeoutMs) {
        OVERLAPPED overlapped{};
        overlapped.hEvent = readEvent_;
        ResetEvent(readEvent_);
        if (!ReadFile(handle_, buffer, static_cast<DWORD>(maxBytes), nullptr, &overlapped)) {
            // ERROR_ACCESS_DENIED / ERROR_BAD_COMMAND / ERROR_OPERATION_ABORTED / ERROR_GEN_FAILURE ... after an
            // unplug. With fAbortOnError = FALSE no line error aborts a read, so any failure means "gone".
            if (GetLastError() != ERROR_IO_PENDING) return -1;
            if (WaitForSingleObject(readEvent_, static_cast<DWORD>(timeoutMs) + kReadWaitSlackMs) != WAIT_OBJECT_0) {
                // The driver ignored the timeout. Cancel; bytes that arrived meanwhile are still reported.
                CancelIoEx(handle_, &overlapped);
                DWORD got = 0;
                if (GetOverlappedResult(handle_, &overlapped, &got, TRUE)) return static_cast<int>(got);
                return GetLastError() == ERROR_OPERATION_ABORTED ? 0 : -1;
            }
        }
        DWORD got = 0;
        if (!GetOverlappedResult(handle_, &overlapped, &got, TRUE)) return -1;
        return static_cast<int>(got);
    }

    bool commAlive() {
        DWORD errors = 0;
        COMSTAT status{};
        return ClearCommError(handle_, &errors, &status) != FALSE;
    }

    HANDLE handle_ = INVALID_HANDLE_VALUE;
    HANDLE readEvent_ = nullptr;
    HANDLE writeEvent_ = nullptr;
    int baud_ = 115200;
    int currentReadTimeout_ = -1;
};

// ---- enumeration -------------------------------------------------------------------------------------------

bool equalsIgnoreCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; };
        if (lower(a[i]) != lower(b[i])) return false;
    }
    return true;
}

bool isComPortName(const std::wstring& name) {
    return name.size() >= 4 && _wcsnicmp(name.c_str(), L"COM", 3) == 0;  // skips LPTn / ECP printer ports
}

bool alreadyListed(const std::vector<SerialPortInfo>& ports, const std::string& name) {
    return std::any_of(ports.begin(), ports.end(), [&](const SerialPortInfo& p) { return equalsIgnoreCase(p.name, name); });
}

int comPortNumber(const std::string& name) {
    if (name.size() < 4) return 0x7FFFFFFF;
    int number = 0;
    for (std::size_t i = 3; i < name.size(); ++i) {
        if (name[i] < '0' || name[i] > '9') return 0x7FFFFFFF;
        number = number * 10 + (name[i] - '0');
        if (number > 100000) return 0x7FFFFFFF;
    }
    return number;
}

int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// "USB\VID_1B4F&PID_9206&REV_0100", "FTDIBUS\VID_0403+PID_6001+A50285BIA\0000" -> 0x1B4F / 0x0403
bool parseVendorId(const std::string& hardwareId, std::uint16_t& vendorId) {
    for (std::size_t pos = 0; pos + 8 <= hardwareId.size(); ++pos) {
        if (!equalsIgnoreCase(hardwareId.substr(pos, 4), "vid_")) continue;
        unsigned value = 0;
        bool valid = true;
        for (std::size_t i = pos + 4; i < pos + 8; ++i) {
            const int digit = hexDigit(hardwareId[i]);
            if (digit < 0) {
                valid = false;
                break;
            }
            value = (value << 4) | static_cast<unsigned>(digit);
        }
        if (valid) {
            vendorId = static_cast<std::uint16_t>(value);
            return true;
        }
    }
    return false;
}

// REG_SZ / REG_MULTI_SZ device property; for a multi-string the first (most specific) entry.
std::string deviceRegistryString(HDEVINFO set, SP_DEVINFO_DATA& device, DWORD property) {
    DWORD type = 0;
    DWORD required = 0;
    SetupDiGetDeviceRegistryPropertyW(set, &device, property, &type, nullptr, 0, &required);
    if (required == 0) return {};
    std::vector<wchar_t> buffer(required / sizeof(wchar_t) + 2, L'\0');
    if (!SetupDiGetDeviceRegistryPropertyW(set, &device, property, &type, reinterpret_cast<BYTE*>(buffer.data()),
                                           static_cast<DWORD>((buffer.size() - 2) * sizeof(wchar_t)), nullptr))
        return {};
    if (type != REG_SZ && type != REG_MULTI_SZ && type != REG_EXPAND_SZ) return {};
    return win::toUtf8(buffer.data());
}

std::wstring devicePortName(HDEVINFO set, SP_DEVINFO_DATA& device) {
    HKEY key = SetupDiOpenDevRegKey(set, &device, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
    if (key == nullptr || key == INVALID_HANDLE_VALUE) return {};
    wchar_t value[256] = {};
    DWORD type = 0;
    DWORD bytes = sizeof(value) - sizeof(wchar_t);
    const LSTATUS status = RegQueryValueExW(key, L"PortName", nullptr, &type, reinterpret_cast<BYTE*>(value), &bytes);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS || type != REG_SZ) return {};
    value[std::min<std::size_t>(bytes / sizeof(wchar_t), 255)] = L'\0';
    return value;
}

void collectFromSetupApi(const GUID& guid, DWORD flags, std::vector<SerialPortInfo>& ports) {
    HDEVINFO set = SetupDiGetClassDevsW(&guid, nullptr, nullptr, flags);
    if (set == INVALID_HANDLE_VALUE) return;
    SP_DEVINFO_DATA device{};
    device.cbSize = sizeof(device);
    for (DWORD index = 0; SetupDiEnumDeviceInfo(set, index, &device); ++index) {
        const std::wstring portName = devicePortName(set, device);
        if (!isComPortName(portName)) continue;
        SerialPortInfo info;
        info.name = win::toUtf8(portName);
        if (alreadyListed(ports, info.name)) continue;
        info.description = deviceRegistryString(set, device, SPDRP_FRIENDLYNAME);  // "USB Serial Device (COM7)"
        if (info.description.empty()) info.description = deviceRegistryString(set, device, SPDRP_DEVICEDESC);
        if (info.description.empty()) info.description = info.name;
        info.hardwareId = deviceRegistryString(set, device, SPDRP_HARDWAREID);
        std::uint16_t vendorId = 0;
        if (parseVendorId(info.hardwareId, vendorId)) info.likelyController = isKnownMicrocontrollerVendor(vendorId);
        ports.push_back(std::move(info));
    }
    SetupDiDestroyDeviceInfoList(set);
}

// Every port the serial drivers registered, including ones SetupAPI does not report (some virtual ports).
void collectFromDeviceMap(std::vector<SerialPortInfo>& ports) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ, &key) != ERROR_SUCCESS) return;
    for (DWORD index = 0;; ++index) {
        wchar_t valueName[256] = {};
        DWORD nameChars = 255;
        wchar_t data[256] = {};
        DWORD dataBytes = sizeof(data) - sizeof(wchar_t);
        DWORD type = 0;
        const LSTATUS status =
            RegEnumValueW(key, index, valueName, &nameChars, nullptr, &type, reinterpret_cast<BYTE*>(data), &dataBytes);
        if (status == ERROR_NO_MORE_ITEMS) break;
        if (status != ERROR_SUCCESS || type != REG_SZ) continue;  // ERROR_MORE_DATA: absurdly long entry
        data[std::min<std::size_t>(dataBytes / sizeof(wchar_t), 255)] = L'\0';
        const std::wstring portName = data;
        if (!isComPortName(portName)) continue;
        SerialPortInfo info;
        info.name = win::toUtf8(portName);
        if (alreadyListed(ports, info.name)) continue;
        info.description = win::toUtf8(valueName);  // "\Device\USBSER000"
        ports.push_back(std::move(info));
    }
    RegCloseKey(key);
}

}  // namespace

std::unique_ptr<ISerialPort> createSerialPort() { return std::make_unique<Win32SerialPort>(); }

std::vector<SerialPortInfo> enumerateSerialPorts() {
    std::vector<SerialPortInfo> ports;
    collectFromSetupApi(kGuidDevInterfaceComPort, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE, ports);
    collectFromSetupApi(kGuidDevClassPorts, DIGCF_PRESENT, ports);
    collectFromDeviceMap(ports);
    std::stable_sort(ports.begin(), ports.end(), [](const SerialPortInfo& a, const SerialPortInfo& b) {
        return comPortNumber(a.name) < comPortNumber(b.name);
    });
    return ports;
}

}  // namespace reka

#endif  // _WIN32
