#include "reka/virtualaudio/win/RekaDriverClient.h"

#if defined(_WIN32)

#include "reka/platform/ThreadPriority.h"
#include "reka/platform/Win32Util.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <mutex>
#include <thread>

namespace reka::win {

namespace {

static_assert(sizeof(wchar_t) == sizeof(std::uint16_t), "REKA_ENDPOINT_CONFIG::FriendlyName is UTF-16");
static_assert(offsetof(REKA_RING_HEADER, WritePosition) % 8 == 0 && offsetof(REKA_RING_HEADER, ReadPosition) % 8 == 0,
              "ring positions must be naturally aligned for the Interlocked*64 functions");
static_assert(offsetof(REKA_RING_HEADER, ReadPosition) - offsetof(REKA_RING_HEADER, WritePosition) >= 64,
              "producer and consumer positions must live on separate cache lines");

using Clock = std::chrono::steady_clock;

constexpr DWORD kPumpTimeoutMs = 5;  // fallback when the driver's data-ready event is late or missing

bool isSupportedSampleRate(int rate) {
    switch (rate) {
        case 44100: case 48000: case 88200: case 96000: case 176400: case 192000: return true;
        default: return false;
    }
}

// ~170 ms of ring, a power of two: 8192 frames at 44.1 / 48 kHz.
std::uint32_t ringFramesFor(int sampleRate) {
    std::uint32_t frames = 4096;
    while (frames < static_cast<std::uint32_t>(sampleRate) / 6u) frames <<= 1;
    return frames;
}

// Acquire load / release store of the ring positions shared with the driver (see REKA_RING_HEADER). The
// Interlocked functions are full barriers, which covers acquire and release on x64 and ARM64 alike.
std::int64_t loadAcquire(volatile std::int64_t* position) noexcept { return InterlockedCompareExchange64(position, 0, 0); }
void storeRelease(volatile std::int64_t* position, std::int64_t value) noexcept { InterlockedExchange64(position, value); }

std::uint32_t readShared(const std::uint32_t& value) noexcept { return *static_cast<const volatile std::uint32_t*>(&value); }

bool isPowerOfTwo(std::uint32_t value) { return value != 0 && (value & (value - 1)) == 0; }

const char* flowName(EndpointFlow flow) { return flow == EndpointFlow::Render ? "playback" : "recording"; }

// ---- stream ------------------------------------------------------------------------------------------------

class DriverEndpointStream final : public IVirtualStream {
public:
    DriverEndpointStream() = default;
    ~DriverEndpointStream() override { stop(); }
    DriverEndpointStream(const DriverEndpointStream&) = delete;
    DriverEndpointStream& operator=(const DriverEndpointStream&) = delete;

    bool start(const VirtualEndpointSpec& spec, double engineSampleRate, int engineMaxBlock, std::string& error);

    std::shared_ptr<AsyncEndpointPort> port() const override { return port_; }

    PortInfo portInfo() const override {
        PortInfo info;
        info.key = "virtual:" + spec_.name;
        info.displayName = spec_.name;
        info.type = PortType::VirtualEndpoint;
        // Render endpoint: apps play into Windows, the engine reads (input strip). Capture: the reverse.
        info.numInputs = spec_.flow == EndpointFlow::Render ? channels_ : 0;
        info.numOutputs = spec_.flow == EndpointFlow::Capture ? channels_ : 0;
        return info;
    }

    bool isRunning() const override { return running_.load(std::memory_order_acquire); }

    std::string lastError() const override {
        std::lock_guard<std::mutex> lock(errorMutex_);
        return lastError_;
    }

    void stop() override {
        if (stopEvent_ != nullptr) SetEvent(stopEvent_);
        if (thread_.joinable()) thread_.join();
        running_.store(false, std::memory_order_release);
        releaseDriverObjects();
    }

private:
    bool mapRing(std::string& error);
    bool validateRing(std::uint32_t expectedFlow, std::string& error);
    void releaseDriverObjects() noexcept;

    void pumpMain() noexcept;
    void pumpFromRing() noexcept;  // Render flow: driver (producer) -> port
    void pumpToRing() noexcept;    // Capture flow: port -> driver (consumer)
    void drainWhileIdle() noexcept;
    void trackConsumerPeriod(std::int64_t readPosition) noexcept;
    std::int64_t consumerPeriodEstimate() const noexcept;
    void fail(const char* what) noexcept;

    RekaDriverConnection driver_;
    VirtualEndpointSpec spec_;
    std::shared_ptr<AsyncEndpointPort> port_;
    std::uint32_t endpointId_ = 0;
    bool endpointCreated_ = false;
    bool ringMapped_ = false;
    REKA_RING_MAPPING mapping_{};
    REKA_RING_HEADER* header_ = nullptr;
    float* samples_ = nullptr;
    std::int64_t capacity_ = 0;
    std::int64_t mask_ = 0;
    int channels_ = 0;
    int sampleRate_ = 48000;
    int maxBlock_ = 480;
    HANDLE dataReady_ = nullptr;
    HANDLE stopEvent_ = nullptr;
    std::thread thread_;
    std::atomic<bool> running_{false};
    mutable std::mutex errorMutex_;
    std::string lastError_;

    // Pump-thread state (Capture flow).
    std::vector<float> scratch_;
    std::int64_t lastConsumerPosition_ = 0;
    std::int64_t periodWindowMax_ = 0;
    std::int64_t periodPreviousMax_ = 0;
    std::int64_t periodWindowFrames_ = 0;
    bool wasStreaming_ = false;
    Clock::time_point lastIdleDrain_{};
};

bool DriverEndpointStream::start(const VirtualEndpointSpec& spec, double engineSampleRate, int engineMaxBlock,
                                 std::string& error) {
    spec_ = spec;
    channels_ = spec.channels;
    const std::wstring wideName = toWide(spec.name);
    if (spec.name.empty() || wideName.empty()) {
        error = "A REKA virtual endpoint needs a name";
        return false;
    }
    if (wideName.size() >= REKA_ENDPOINT_NAME_CHARS) {
        error = "The endpoint name \"" + spec.name + "\" is too long (at most " +
                std::to_string(REKA_ENDPOINT_NAME_CHARS - 1) + " characters)";
        return false;
    }
    if (spec.channels < 1 || spec.channels > 8) {
        error = "\"" + spec.name + "\": REKA virtual endpoints support 1 to 8 channels, not " + std::to_string(spec.channels);
        return false;
    }
    if (!isSupportedSampleRate(spec.sampleRate)) {
        error = "\"" + spec.name + "\": unsupported sample rate " + std::to_string(spec.sampleRate) +
                " Hz (44100, 48000, 88200, 96000, 176400 or 192000)";
        return false;
    }
    if (!(engineSampleRate > 0.0) || engineMaxBlock <= 0) {
        error = "\"" + spec.name + "\": the audio engine is not running";
        return false;
    }

    if (driver_.open(error) != DriverState::Ready) return false;

    REKA_ENDPOINT_CONFIG config{};
    config.Size = sizeof(config);
    config.Flow = spec.flow == EndpointFlow::Render ? REKA_FLOW_RENDER : REKA_FLOW_CAPTURE;
    config.Channels = static_cast<std::uint32_t>(spec.channels);
    config.SampleRate = static_cast<std::uint32_t>(spec.sampleRate);
    config.RingFrames = ringFramesFor(spec.sampleRate);
    for (std::size_t i = 0; i < wideName.size(); ++i) config.FriendlyName[i] = static_cast<std::uint16_t>(wideName[i]);

    REKA_ENDPOINT_ID created{};
    DWORD returned = 0;
    std::string ioError;
    if (!driver_.control(IOCTL_REKA_CREATE_ENDPOINT, &config, sizeof(config), &created, sizeof(created), returned, ioError)) {
        error = "The REKA driver could not create the " + std::string(flowName(spec.flow)) + " device \"" + spec.name +
                "\": " + ioError;
        return false;
    }
    if (returned < sizeof(created)) {
        error = "The REKA driver did not return an endpoint id for \"" + spec.name + "\"";
        return false;
    }
    endpointId_ = created.EndpointId;
    endpointCreated_ = true;

    if (!mapRing(error) || !validateRing(config.Flow, error)) {
        releaseDriverObjects();
        return false;
    }

    // The driver may run the ring at its own rate; AsyncEndpointPort converts to the engine rate.
    maxBlock_ = std::max(64, sampleRate_ / 100);  // 10 ms per move
    AsyncPortFormat format;
    format.captureChannels = spec.flow == EndpointFlow::Render ? channels_ : 0;
    format.renderChannels = spec.flow == EndpointFlow::Capture ? channels_ : 0;
    format.endpointSampleRate = static_cast<double>(sampleRate_);
    format.endpointMaxBlock = maxBlock_;
    port_ = std::make_shared<AsyncEndpointPort>();
    port_->configure(format, engineSampleRate, engineMaxBlock);
    scratch_.assign(static_cast<std::size_t>(maxBlock_) * static_cast<std::size_t>(channels_), 0.0f);
    periodPreviousMax_ = sampleRate_ / 100;  // assume a 10 ms driver period until measured

    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (stopEvent_ == nullptr) {
        error = "\"" + spec.name + "\": could not create the pump stop event (" + win32ErrorMessage(GetLastError()) + ")";
        releaseDriverObjects();
        return false;
    }
    running_.store(true, std::memory_order_release);
    try {
        thread_ = std::thread([this] { pumpMain(); });
    } catch (const std::exception& e) {
        running_.store(false, std::memory_order_release);
        error = "\"" + spec.name + "\": could not start the pump thread (" + e.what() + ")";
        releaseDriverObjects();
        return false;
    }
    return true;
}

bool DriverEndpointStream::mapRing(std::string& error) {
    REKA_ENDPOINT_ID request{};
    request.Size = sizeof(request);
    request.EndpointId = endpointId_;
    REKA_RING_MAPPING mapping{};
    DWORD returned = 0;
    std::string ioError;
    if (!driver_.control(IOCTL_REKA_MAP_RING, &request, sizeof(request), &mapping, sizeof(mapping), returned, ioError)) {
        error = "The REKA driver could not share the audio buffer of \"" + spec_.name + "\": " + ioError;
        return false;
    }
    mapping_ = mapping;
    ringMapped_ = true;
    if (mapping.DataReadyEvent != 0)
        dataReady_ = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(mapping.DataReadyEvent));
    if (returned < sizeof(mapping) || mapping.UserAddress == 0) {
        error = "The REKA driver returned an invalid buffer mapping for \"" + spec_.name + "\"";
        return false;
    }
    return true;
}

bool DriverEndpointStream::validateRing(std::uint32_t expectedFlow, std::string& error) {
    const std::string prefix = "The audio buffer of \"" + spec_.name + "\" is invalid: ";
    if (mapping_.UserAddress % 8 != 0) {  // the 64-bit positions must be naturally aligned (Interlocked*64)
        error = prefix + "misaligned mapping";
        return false;
    }
    if (mapping_.MappingBytes < sizeof(REKA_RING_HEADER)) {
        error = prefix + "mapping smaller than its header";
        return false;
    }
    auto* header = reinterpret_cast<REKA_RING_HEADER*>(static_cast<std::uintptr_t>(mapping_.UserAddress));
    if (header->Magic != REKA_RING_MAGIC) {
        error = prefix + "bad magic " + hexCode(header->Magic);
        return false;
    }
    if (header->Version != REKA_DRIVER_INTERFACE_VERSION) {
        error = prefix + "ring version " + std::to_string(header->Version) + ", expected " +
                std::to_string(REKA_DRIVER_INTERFACE_VERSION);
        return false;
    }
    if (header->Flow != expectedFlow) {
        error = prefix + "wrong direction";
        return false;
    }
    if (header->Channels != static_cast<std::uint32_t>(spec_.channels)) {
        error = prefix + std::to_string(header->Channels) + " channels instead of " + std::to_string(spec_.channels);
        return false;
    }
    if (!isSupportedSampleRate(static_cast<int>(header->SampleRate))) {
        error = prefix + "unsupported sample rate " + std::to_string(header->SampleRate);
        return false;
    }
    if (!isPowerOfTwo(header->CapacityFrames) || header->CapacityFrames < 256) {
        error = prefix + "capacity " + std::to_string(header->CapacityFrames) + " is not a power of two >= 256";
        return false;
    }
    if (header->HeaderBytes < sizeof(REKA_RING_HEADER) || header->HeaderBytes % sizeof(float) != 0) {
        error = prefix + "bad header size " + std::to_string(header->HeaderBytes);
        return false;
    }
    const std::uint64_t needed = static_cast<std::uint64_t>(header->HeaderBytes) +
                                 static_cast<std::uint64_t>(header->CapacityFrames) * header->Channels * sizeof(float);
    if (needed > mapping_.MappingBytes) {
        error = prefix + "mapping is " + std::to_string(mapping_.MappingBytes) + " bytes, the ring needs " +
                std::to_string(needed);
        return false;
    }
    header_ = header;
    samples_ = reinterpret_cast<float*>(reinterpret_cast<std::uint8_t*>(header) + header->HeaderBytes);
    capacity_ = header->CapacityFrames;
    mask_ = capacity_ - 1;
    sampleRate_ = static_cast<int>(header->SampleRate);
    return true;
}

void DriverEndpointStream::releaseDriverObjects() noexcept {
    try {
        REKA_ENDPOINT_ID id{};
        id.Size = sizeof(id);
        id.EndpointId = endpointId_;
        DWORD returned = 0;
        std::string ignored;
        if (ringMapped_) {
            driver_.control(IOCTL_REKA_UNMAP_RING, &id, sizeof(id), nullptr, 0, returned, ignored);
            ringMapped_ = false;
        }
        header_ = nullptr;
        samples_ = nullptr;
        if (dataReady_ != nullptr) {
            CloseHandle(dataReady_);  // duplicated into this process by IOCTL_REKA_MAP_RING
            dataReady_ = nullptr;
        }
        if (endpointCreated_) {
            driver_.control(IOCTL_REKA_REMOVE_ENDPOINT, &id, sizeof(id), nullptr, 0, returned, ignored);
            endpointCreated_ = false;
        }
    } catch (...) {
        // Only std::string allocation can throw here; the handles below are still released.
    }
    driver_.close();
    if (stopEvent_ != nullptr) {
        CloseHandle(stopEvent_);
        stopEvent_ = nullptr;
    }
}

void DriverEndpointStream::fail(const char* what) noexcept {
    try {
        std::lock_guard<std::mutex> lock(errorMutex_);
        lastError_ = "\"" + spec_.name + "\": " + what;
    } catch (...) {
    }
}

void DriverEndpointStream::pumpMain() noexcept {
    try {
        ScopedAudioThreadPriority priority;
        HANDLE waits[2] = {stopEvent_, dataReady_};
        const DWORD waitCount = dataReady_ != nullptr ? 2u : 1u;
        lastIdleDrain_ = Clock::now();
        lastConsumerPosition_ = loadAcquire(&header_->ReadPosition);
        // Render flow: whatever the driver produced before the pump existed is stale; start at "now".
        if (spec_.flow == EndpointFlow::Render) storeRelease(&header_->ReadPosition, loadAcquire(&header_->WritePosition));
        for (;;) {
            const DWORD result = WaitForMultipleObjects(waitCount, waits, FALSE, kPumpTimeoutMs);
            if (result == WAIT_OBJECT_0) break;
            if (result == WAIT_FAILED) {
                fail("waiting for the driver failed");
                break;
            }
            // Harmless for an auto-reset event; keeps a manual-reset one from turning the wait into a spin.
            if (result == WAIT_OBJECT_0 + 1) ResetEvent(dataReady_);
            if (readShared(header_->Magic) != REKA_RING_MAGIC) {
                fail("the REKA driver withdrew the audio buffer");
                break;
            }
            if (spec_.flow == EndpointFlow::Render)
                pumpFromRing();
            else
                pumpToRing();
        }
    } catch (...) {
        fail("unexpected error in the pump thread");
    }
    running_.store(false, std::memory_order_release);
}

void DriverEndpointStream::pumpFromRing() noexcept {
    const std::int64_t write = loadAcquire(&header_->WritePosition);
    std::int64_t read = loadAcquire(&header_->ReadPosition);
    std::int64_t available = write - read;
    if (available < 0 || available > capacity_) {
        // The driver lapped us (or restarted its counters): drop the backlog and resynchronise.
        storeRelease(&header_->ReadPosition, write);
        return;
    }
    while (available > 0) {
        const std::int64_t index = read & mask_;
        const int frames =
            static_cast<int>(std::min({available, capacity_ - index, static_cast<std::int64_t>(maxBlock_)}));
        port_->endpointWriteCaptureInterleaved(samples_ + index * channels_, channels_, frames);
        read += frames;
        available -= frames;
    }
    storeRelease(&header_->ReadPosition, read);
}

void DriverEndpointStream::pumpToRing() noexcept {
    const std::int64_t read = loadAcquire(&header_->ReadPosition);
    std::int64_t write = loadAcquire(&header_->WritePosition);
    std::int64_t fill = write - read;
    if (fill < 0 || fill > capacity_) {
        // The driver read past our data (underrun) or restarted its counters: continue at its read position.
        write = read;
        fill = 0;
        storeRelease(&header_->WritePosition, write);
    }

    if (header_->StreamRunning == 0) {
        // No application records from the endpoint: keep consuming the engine's output at the nominal
        // rate so the port's drift controller stays in steady state, and leave the ring alone.
        if (wasStreaming_) {
            wasStreaming_ = false;
            lastIdleDrain_ = Clock::now();
        }
        drainWhileIdle();
        lastConsumerPosition_ = read;
        return;
    }
    if (!wasStreaming_) {
        wasStreaming_ = true;
        lastConsumerPosition_ = read;
    }
    trackConsumerPeriod(read);

    // Stay about two driver periods ahead of the consumer: enough to ride out one late wake-up, no more.
    const std::int64_t target = std::min(capacity_ / 2, 2 * consumerPeriodEstimate());
    std::int64_t toWrite = target - fill;
    while (toWrite > 0) {
        const std::int64_t index = write & mask_;
        const int frames = static_cast<int>(std::min({toWrite, capacity_ - index, static_cast<std::int64_t>(maxBlock_)}));
        port_->endpointReadRenderInterleaved(samples_ + index * channels_, channels_, frames);
        write += frames;
        toWrite -= frames;
    }
    storeRelease(&header_->WritePosition, write);
}

void DriverEndpointStream::drainWhileIdle() noexcept {
    const auto now = Clock::now();
    const double elapsed = std::chrono::duration<double>(now - lastIdleDrain_).count();
    std::int64_t frames = static_cast<std::int64_t>(elapsed * sampleRate_);
    if (frames <= 0) return;
    if (frames > 4 * static_cast<std::int64_t>(maxBlock_)) {  // the pump was stalled: do not catch up
        frames = 4 * static_cast<std::int64_t>(maxBlock_);
        lastIdleDrain_ = now;
    } else {
        lastIdleDrain_ += std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(static_cast<double>(frames) / sampleRate_));
    }
    while (frames > 0) {
        const int chunk = static_cast<int>(std::min(frames, static_cast<std::int64_t>(maxBlock_)));
        port_->endpointReadRenderInterleaved(scratch_.data(), channels_, chunk);
        frames -= chunk;
    }
}

// The driver consumes one timer period per tick; the largest step seen over the last ~1-2 s is a
// robust estimate that adapts to the real period without trusting a single measurement.
void DriverEndpointStream::trackConsumerPeriod(std::int64_t readPosition) noexcept {
    const std::int64_t consumed = readPosition - lastConsumerPosition_;
    lastConsumerPosition_ = readPosition;
    if (consumed <= 0 || consumed > capacity_) return;
    periodWindowMax_ = std::max(periodWindowMax_, consumed);
    periodWindowFrames_ += consumed;
    if (periodWindowFrames_ >= sampleRate_) {
        periodPreviousMax_ = periodWindowMax_;
        periodWindowMax_ = 0;
        periodWindowFrames_ = 0;
    }
}

std::int64_t DriverEndpointStream::consumerPeriodEstimate() const noexcept {
    const std::int64_t floor = std::max<std::int64_t>(16, sampleRate_ / 1000);  // 1 ms
    return std::max({periodPreviousMax_, periodWindowMax_, floor});
}

}  // namespace

// ---- connection --------------------------------------------------------------------------------------------

DriverState RekaDriverConnection::open(std::string& error) {
    close();
    HANDLE handle = CreateFileW(REKA_DRIVER_CONTROL_PATH, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        if (code == ERROR_ACCESS_DENIED) {
            error = "Windows denied access to the REKA Virtual Audio driver";
            return DriverState::AccessDenied;
        }
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
            error = "The REKA Virtual Audio driver is not installed";
            return DriverState::NotInstalled;
        }
        error = "The REKA Virtual Audio driver could not be opened: " + win32ErrorMessage(code);
        return DriverState::NotInstalled;
    }

    // The request carries REKA's own interface version so the driver can log mismatches too.
    REKA_DRIVER_INFO info{};
    info.Size = sizeof(info);
    info.InterfaceVersion = REKA_DRIVER_INTERFACE_VERSION;
    DWORD returned = 0;
    if (!DeviceIoControl(handle, IOCTL_REKA_GET_INFO, &info, sizeof(info), &info, sizeof(info), &returned, nullptr)) {
        const DWORD code = GetLastError();
        CloseHandle(handle);
        if (code == ERROR_ACCESS_DENIED) {
            error = "Windows denied access to the REKA Virtual Audio driver";
            return DriverState::AccessDenied;
        }
        error = "The REKA Virtual Audio driver did not answer the version query (" + win32ErrorMessage(code) +
                "); install the driver that ships with this version of REKA";
        return DriverState::VersionMismatch;
    }
    constexpr DWORD kMinimumReply = offsetof(REKA_DRIVER_INFO, VersionMajor);  // Size + InterfaceVersion
    if (returned < kMinimumReply) {
        CloseHandle(handle);
        error = "The REKA Virtual Audio driver sent a malformed version reply";
        return DriverState::VersionMismatch;
    }
    if (info.InterfaceVersion != REKA_DRIVER_INTERFACE_VERSION) {
        CloseHandle(handle);
        error = "The installed REKA Virtual Audio driver speaks interface version " + std::to_string(info.InterfaceVersion) +
                ", this version of REKA needs version " + std::to_string(REKA_DRIVER_INTERFACE_VERSION) +
                "; install the matching driver";
        return DriverState::VersionMismatch;
    }
    handle_ = handle;
    info_ = info;
    return DriverState::Ready;
}

void RekaDriverConnection::close() noexcept {
    if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
    handle_ = INVALID_HANDLE_VALUE;
}

std::string RekaDriverConnection::versionString() const {
    return std::to_string(info_.VersionMajor) + "." + std::to_string(info_.VersionMinor) + "." +
           std::to_string(info_.VersionPatch);
}

bool RekaDriverConnection::control(DWORD code, const void* in, DWORD inBytes, void* out, DWORD outBytes, DWORD& returned,
                                   std::string& error) const {
    returned = 0;
    if (handle_ == INVALID_HANDLE_VALUE) {
        error = "the driver connection is closed";
        return false;
    }
    if (!DeviceIoControl(handle_, code, const_cast<void*>(in), inBytes, out, outBytes, &returned, nullptr)) {
        error = win32ErrorMessage(GetLastError());
        return false;
    }
    return true;
}

std::vector<std::string> RekaDriverConnection::listEndpointNames(std::string* error) const {
    std::vector<std::string> names;
    auto list = std::make_unique<REKA_ENDPOINT_LIST>();
    list->Size = sizeof(REKA_ENDPOINT_LIST);
    DWORD returned = 0;
    std::string ioError;
    if (!control(IOCTL_REKA_LIST_ENDPOINTS, nullptr, 0, list.get(), sizeof(REKA_ENDPOINT_LIST), returned, ioError)) {
        if (error != nullptr) *error = "Could not list the REKA virtual endpoints: " + ioError;
        return names;
    }
    if (returned < offsetof(REKA_ENDPOINT_LIST, Endpoints)) return names;
    const std::uint32_t count = std::min<std::uint32_t>(list->Count, REKA_MAX_ENDPOINTS);
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto& friendly = list->Endpoints[i].FriendlyName;
        std::wstring name;
        for (std::size_t c = 0; c < REKA_ENDPOINT_NAME_CHARS && friendly[c] != 0; ++c)
            name.push_back(static_cast<wchar_t>(friendly[c]));
        if (!name.empty()) names.push_back(toUtf8(name));
    }
    return names;
}

std::unique_ptr<IVirtualStream> createDriverEndpointStream(const VirtualEndpointSpec& spec, double engineSampleRate,
                                                           int engineMaxBlock, std::string& error) {
    auto stream = std::make_unique<DriverEndpointStream>();
    if (!stream->start(spec, engineSampleRate, engineMaxBlock, error)) return nullptr;
    return stream;
}

}  // namespace reka::win

#endif  // _WIN32
