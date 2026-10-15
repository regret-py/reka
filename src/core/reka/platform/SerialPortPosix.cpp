// POSIX serial port (Linux / macOS). Lets the hardware bridge, its tests and the firmware be developed
// without Windows; the shipping Windows implementation lives in SerialPortWin32.cpp.
#include "reka/hardware/SerialPort.h"

#if !defined(_WIN32)

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

namespace reka {

namespace {

speed_t toSpeed(int baud) {
    switch (baud) {
        case 9600: return B9600;
        case 19200: return B19200;
        case 38400: return B38400;
        case 57600: return B57600;
        case 230400: return B230400;
#ifdef B460800
        case 460800: return B460800;
#endif
#ifdef B921600
        case 921600: return B921600;
#endif
        case 115200:
        default: return B115200;
    }
}

class PosixSerialPort final : public ISerialPort {
public:
    ~PosixSerialPort() override { close(); }

    bool open(const std::string& name, int baud, std::string& error) override {
        close();
        fd_ = ::open(name.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
        if (fd_ < 0) {
            error = name + ": " + std::strerror(errno);
            return false;
        }
        termios tty{};
        if (tcgetattr(fd_, &tty) == 0) {
            cfmakeraw(&tty);
            tty.c_cflag |= CLOCAL | CREAD;
            tty.c_cflag &= ~static_cast<tcflag_t>(CSTOPB | PARENB | CRTSCTS);
            tty.c_cc[VMIN] = 0;
            tty.c_cc[VTIME] = 0;
            cfsetispeed(&tty, toSpeed(baud));
            cfsetospeed(&tty, toSpeed(baud));
            tcsetattr(fd_, TCSANOW, &tty);  // fails harmlessly on pseudo-terminals used in tests
        }
        int lines = TIOCM_DTR | TIOCM_RTS;
        ioctl(fd_, TIOCMBIS, &lines);
        tcflush(fd_, TCIOFLUSH);
        return true;
    }

    void close() override {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }

    bool isOpen() const override { return fd_ >= 0; }

    int read(std::uint8_t* buffer, int maxBytes, int timeoutMs) override {
        if (fd_ < 0) return -1;
        pollfd pfd{fd_, POLLIN, 0};
        const int ready = ::poll(&pfd, 1, timeoutMs);
        if (ready < 0) return errno == EINTR ? 0 : -1;
        if (ready == 0) return 0;
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) return -1;
        const ssize_t n = ::read(fd_, buffer, static_cast<std::size_t>(maxBytes));
        if (n < 0) return (errno == EAGAIN || errno == EINTR) ? 0 : -1;
        if (n == 0) return -1;  // EOF: device gone
        return static_cast<int>(n);
    }

    bool write(const std::uint8_t* data, int size) override {
        if (fd_ < 0) return false;
        int offset = 0;
        while (offset < size) {
            const ssize_t n = ::write(fd_, data + offset, static_cast<std::size_t>(size - offset));
            if (n < 0) {
                if (errno == EAGAIN || errno == EINTR) {
                    pollfd pfd{fd_, POLLOUT, 0};
                    if (::poll(&pfd, 1, 50) <= 0) return false;
                    continue;
                }
                return false;
            }
            offset += static_cast<int>(n);
        }
        return true;
    }

private:
    int fd_ = -1;
};

std::uint16_t readHexFile(const std::filesystem::path& path) {
    std::ifstream stream(path);
    unsigned value = 0;
    stream >> std::hex >> value;
    return static_cast<std::uint16_t>(value);
}

}  // namespace

std::unique_ptr<ISerialPort> createSerialPort() { return std::make_unique<PosixSerialPort>(); }

std::vector<SerialPortInfo> enumerateSerialPorts() {
    std::vector<SerialPortInfo> ports;
    std::error_code ec;
#if defined(__linux__)
    for (const auto& entry : std::filesystem::directory_iterator("/sys/class/tty", ec)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind("ttyACM", 0) != 0 && name.rfind("ttyUSB", 0) != 0) continue;
        SerialPortInfo info;
        info.name = "/dev/" + name;
        info.description = name;
        // /sys/class/tty/ttyACM0/device -> .../1-1:1.0 ; idVendor lives one level up.
        const auto usbInterface = std::filesystem::canonical(entry.path() / "device", ec);
        if (!ec) {
            const auto usbDevice = usbInterface.parent_path();
            const std::uint16_t vid = readHexFile(usbDevice / "idVendor");
            info.hardwareId = usbDevice.string();
            info.likelyController = isKnownMicrocontrollerVendor(vid);
            std::ifstream product(usbDevice / "product");
            std::getline(product, info.description);
        }
        ports.push_back(std::move(info));
    }
#else
    for (const auto& entry : std::filesystem::directory_iterator("/dev", ec)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind("cu.usbmodem", 0) == 0 || name.rfind("cu.usbserial", 0) == 0) {
            SerialPortInfo info;
            info.name = entry.path().string();
            info.description = name;
            info.likelyController = true;
            ports.push_back(std::move(info));
        }
    }
#endif
    return ports;
}

}  // namespace reka

#endif  // !_WIN32
