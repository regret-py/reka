// Windows implementation of RekaVirtualAudio (the non-Windows stub lives in RekaVirtualAudio.cpp).
#include "reka/virtualaudio/RekaVirtualAudio.h"

#if defined(_WIN32)

#include "reka/virtualaudio/win/ProcessLoopbackCapture.h"
#include "reka/virtualaudio/win/RekaDriverClient.h"
#include "reka/virtualaudio/win/WasapiEndpoints.h"

#include <windows.h>
#include <shellapi.h>

#include <mutex>

namespace reka {

struct RekaVirtualAudio::Impl {
    mutable std::mutex mutex;
    DriverState state = DriverState::NotInstalled;
    std::string version;
    std::string detail;  // why the driver is not Ready

    // Probes \\.\RekaVirtualAudio. The handle is not kept open: an idle REKA must not pin the driver
    // (it would block driver updates); every virtual endpoint stream opens its own connection.
    void probe() {
        win::RekaDriverConnection connection;
        std::string error;
        const DriverState probed = connection.open(error);
        std::lock_guard<std::mutex> lock(mutex);
        state = probed;
        version = probed == DriverState::Ready ? connection.versionString() : std::string();
        detail = probed == DriverState::Ready ? std::string() : error;
    }

    std::vector<std::string> driverEndpointNames() const {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (state != DriverState::Ready) return {};
        }
        win::RekaDriverConnection connection;
        std::string error;
        if (connection.open(error) != DriverState::Ready) return {};
        return connection.listEndpointNames();
    }
};

RekaVirtualAudio::RekaVirtualAudio() : impl_(std::make_unique<Impl>()) { impl_->probe(); }
RekaVirtualAudio::~RekaVirtualAudio() = default;

DriverState RekaVirtualAudio::driverState() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->state;
}

std::string RekaVirtualAudio::driverVersion() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->version;
}

void RekaVirtualAudio::refreshDriver() { impl_->probe(); }

std::unique_ptr<IVirtualStream> RekaVirtualAudio::createVirtualEndpoint(const VirtualEndpointSpec& spec,
                                                                        double engineSampleRate, int engineMaxBlock,
                                                                        std::string& error) {
    if (driverState() != DriverState::Ready) impl_->probe();  // installed since the last refresh?
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->state != DriverState::Ready) {
            error = impl_->detail.empty() ? std::string("The REKA Virtual Audio driver is not available") : impl_->detail;
            return nullptr;
        }
    }
    return win::createDriverEndpointStream(spec, engineSampleRate, engineMaxBlock, error);
}

std::vector<SystemEndpoint> RekaVirtualAudio::enumerateEndpoints(std::string* error) const {
    if (error != nullptr) error->clear();
    return win::enumerateSystemEndpoints(impl_->driverEndpointNames(), error);
}

bool RekaVirtualAudio::processLoopbackSupported() const { return win::processLoopbackAvailable(); }

std::vector<AudioProcess> RekaVirtualAudio::listAudioProcesses() const { return win::listAudioSessionProcesses(); }

std::unique_ptr<IVirtualStream> RekaVirtualAudio::captureProcess(std::uint32_t pid, bool includeChildProcesses,
                                                                 double engineSampleRate, int engineMaxBlock,
                                                                 std::string& error) {
    // Windows' process loopback always captures the whole process tree in include mode (its only other
    // mode captures everything *except* the tree), so child processes are included either way.
    (void)includeChildProcesses;

    if (!win::processLoopbackAvailable()) {
        error = "Capturing a single application needs Windows 11 or Windows 10 build 20348 or newer (this is build " +
                std::to_string(win::windowsBuildNumber()) + ")";
        return nullptr;
    }
    if (pid == 0 || pid == GetCurrentProcessId()) {
        error = pid == 0 ? "No application selected" : "REKA cannot capture its own audio";
        return nullptr;
    }
    std::string executable = win::processExecutableName(pid);
    if (executable.empty()) {
        HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
        if (process == nullptr && GetLastError() == ERROR_INVALID_PARAMETER) {
            error = "No application with process id " + std::to_string(pid) + " is running";
            return nullptr;
        }
        if (process != nullptr) CloseHandle(process);
        executable = "Process " + std::to_string(pid);
    }
    return win::startProcessLoopbackCapture(pid, executable, engineSampleRate, engineMaxBlock, error);
}

bool RekaVirtualAudio::openSoundControlPanel() {
    const HINSTANCE result = ShellExecuteW(nullptr, L"open", L"control.exe", L"mmsys.cpl", nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;  // ShellExecute's documented success test
}

}  // namespace reka

#endif  // _WIN32
