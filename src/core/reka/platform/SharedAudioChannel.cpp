#include "reka/platform/SharedAudioChannel.h"

#include <algorithm>
#include <cstring>

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <ctime>
#include <fcntl.h>
#include <semaphore.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace reka {

static_assert(sizeof(SharedAudioChannel::Header) == 64, "header must stay one cache line");
static_assert(std::atomic<std::uint32_t>::is_always_lock_free, "cross-process atomics must be lock-free");

namespace {
std::size_t bytesFor(int maxFrames) {
    return sizeof(SharedAudioChannel::Header) +
           static_cast<std::size_t>(SharedAudioChannel::kMaxChannels) * static_cast<std::size_t>(maxFrames) * sizeof(float);
}

#if defined(_WIN32)
std::wstring widen(const std::string& text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length);
    return out;
}
std::wstring objectName(const std::string& name, const char* suffix) { return L"Local\\" + widen(name + suffix); }
#else
std::string objectName(const std::string& name, const char* suffix) { return "/" + name + suffix; }

bool semWaitMicros(sem_t* sem, long long timeoutMicros) {
    if (timeoutMicros <= 0) return sem_trywait(sem) == 0;
#if defined(__linux__)
    timespec deadline{};
    clock_gettime(CLOCK_REALTIME, &deadline);
    const long long ns = deadline.tv_nsec + (timeoutMicros % 1000000) * 1000;
    deadline.tv_sec += static_cast<time_t>(timeoutMicros / 1000000 + ns / 1000000000);
    deadline.tv_nsec = static_cast<long>(ns % 1000000000);
    while (sem_timedwait(sem, &deadline) != 0) {
        if (errno != EINTR) return false;
    }
    return true;
#else
    // macOS has no sem_timedwait: poll. Only used for development builds there.
    for (long long waited = 0; waited < timeoutMicros; waited += 100) {
        if (sem_trywait(sem) == 0) return true;
        usleep(100);
    }
    return sem_trywait(sem) == 0;
#endif
}
#endif
}  // namespace

SharedAudioChannel::~SharedAudioChannel() { close(); }

int SharedAudioChannel::maxFrames() const noexcept { return header_ != nullptr ? header_->maxFrames : 0; }

float* SharedAudioChannel::samples(int channel) noexcept {
    auto* base = reinterpret_cast<float*>(reinterpret_cast<std::uint8_t*>(header_) + sizeof(Header));
    return base + static_cast<std::size_t>(channel) * static_cast<std::size_t>(header_->maxFrames);
}

bool SharedAudioChannel::create(const std::string& name, int maxFrames, std::string& error) {
    close();
    maxFrames = std::max(32, maxFrames);
    if (!mapShared(name, bytesFor(maxFrames), true, error)) return false;
    owner_ = true;
    name_ = name;
    std::memset(static_cast<void*>(header_), 0, bytesFor(maxFrames));
    header_->magic = kMagic;
    header_->version = kVersion;
    header_->maxFrames = maxFrames;
    header_->numChannels = kMaxChannels;
    return true;
}

bool SharedAudioChannel::open(const std::string& name, std::string& error) {
    close();
    // Map the header first to learn the size, then remap the whole thing.
    if (!mapShared(name, sizeof(Header), false, error)) return false;
    if (header_->magic != kMagic || header_->version != kVersion) {
        error = "shared audio channel version mismatch";
        close();
        return false;
    }
    const int frames = header_->maxFrames;
    close();
    if (!mapShared(name, bytesFor(frames), false, error)) return false;
    name_ = name;
    return true;
}

// ---- host side ---------------------------------------------------------------------------------------------

SharedAudioChannel::Result SharedAudioChannel::process(float* const* channels, int numChannels, int numFrames,
                                                       int timeoutMicros) noexcept {
    if (header_ == nullptr || header_->shutdown.load(std::memory_order_acquire) != 0) return Result::Disconnected;
    // The plugin host is still busy with an older block (we timed out on it): never touch the buffer
    // it is processing in place. Pass this block through dry instead.
    const std::uint32_t previous = header_->requestSeq.load(std::memory_order_relaxed);
    if (header_->responseSeq.load(std::memory_order_acquire) != previous) return Result::Timeout;

    const int frames = std::min(numFrames, header_->maxFrames);
    const int chCount = std::min(numChannels, kMaxChannels);
    for (int ch = 0; ch < chCount; ++ch)
        std::memcpy(samples(ch), channels[ch], static_cast<std::size_t>(frames) * sizeof(float));
    header_->numFrames.store(frames, std::memory_order_relaxed);
    header_->activeChannels.store(chCount, std::memory_order_relaxed);
    const std::uint32_t seq = previous + 1;
    header_->requestSeq.store(seq, std::memory_order_release);
    if (!signalRequest()) return Result::Disconnected;

    // Bounded wait. Stale "done" signals from older blocks are skipped by checking the sequence number.
    int remaining = timeoutMicros;
    for (int attempts = 0; attempts < 4 && remaining > 0; ++attempts) {
        if (header_->responseSeq.load(std::memory_order_acquire) == seq) break;
        if (!waitDone(remaining)) return Result::Timeout;
        remaining /= 2;
    }
    if (header_->responseSeq.load(std::memory_order_acquire) != seq) return Result::Timeout;

    for (int ch = 0; ch < chCount; ++ch)
        std::memcpy(channels[ch], samples(ch), static_cast<std::size_t>(frames) * sizeof(float));
    return Result::Ok;
}

// ---- plugin-host side ------------------------------------------------------------------------------------

int SharedAudioChannel::waitForRequest(int timeoutMs) noexcept {
    if (header_ == nullptr) return -1;
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (header_->shutdown.load(std::memory_order_acquire) != 0) return -1;
        if (header_->requestSeq.load(std::memory_order_acquire) != header_->responseSeq.load(std::memory_order_relaxed))
            return std::max(0, header_->numFrames.load(std::memory_order_relaxed));
        if (!waitRequest(timeoutMs)) return 0;
    }
    return 0;
}

float* SharedAudioChannel::channel(int index) noexcept { return header_ != nullptr ? samples(index) : nullptr; }

int SharedAudioChannel::requestChannels() const noexcept {
    return header_ != nullptr ? header_->activeChannels.load(std::memory_order_relaxed) : 0;
}

void SharedAudioChannel::completeRequest() noexcept {
    if (header_ == nullptr) return;
    header_->responseSeq.store(header_->requestSeq.load(std::memory_order_relaxed), std::memory_order_release);
    signalDone();
}

void SharedAudioChannel::setLatency(int samplesCount) noexcept {
    if (header_ != nullptr) header_->latency.store(samplesCount, std::memory_order_relaxed);
}

int SharedAudioChannel::latency() const noexcept {
    return header_ != nullptr ? header_->latency.load(std::memory_order_relaxed) : 0;
}

void SharedAudioChannel::requestShutdown() noexcept {
    if (header_ == nullptr) return;
    header_->shutdown.store(1, std::memory_order_release);
    signalRequest();
    signalDone();
}

// ---- platform ------------------------------------------------------------------------------------------

#if defined(_WIN32)

bool SharedAudioChannel::mapShared(const std::string& name, std::size_t bytes, bool create, std::string& error) {
    const std::wstring mappingName = objectName(name, "-shm");
    HANDLE mapping = create ? CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                                 static_cast<DWORD>(static_cast<std::uint64_t>(bytes) >> 32),
                                                 static_cast<DWORD>(bytes & 0xFFFFFFFFu), mappingName.c_str())
                            : OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, mappingName.c_str());
    if (mapping == nullptr) {
        error = "shared memory mapping failed (" + std::to_string(GetLastError()) + ")";
        return false;
    }
    void* view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, bytes);
    if (view == nullptr) {
        error = "MapViewOfFile failed (" + std::to_string(GetLastError()) + ")";
        CloseHandle(mapping);
        return false;
    }
    const std::wstring requestName = objectName(name, "-req");
    const std::wstring doneName = objectName(name, "-done");
    HANDLE request = create ? CreateEventW(nullptr, FALSE, FALSE, requestName.c_str())
                            : OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, requestName.c_str());
    HANDLE done = create ? CreateEventW(nullptr, FALSE, FALSE, doneName.c_str())
                         : OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, doneName.c_str());
    if (request == nullptr || done == nullptr) {
        error = "event creation failed (" + std::to_string(GetLastError()) + ")";
        if (request != nullptr) CloseHandle(request);
        if (done != nullptr) CloseHandle(done);
        UnmapViewOfFile(view);
        CloseHandle(mapping);
        return false;
    }
    mapping_ = mapping;
    requestEvent_ = request;
    doneEvent_ = done;
    header_ = static_cast<Header*>(view);
    mappingBytes_ = bytes;
    return true;
}

void SharedAudioChannel::close() noexcept {
    if (header_ != nullptr) UnmapViewOfFile(header_);
    if (mapping_ != nullptr) CloseHandle(static_cast<HANDLE>(mapping_));
    if (requestEvent_ != nullptr) CloseHandle(static_cast<HANDLE>(requestEvent_));
    if (doneEvent_ != nullptr) CloseHandle(static_cast<HANDLE>(doneEvent_));
    header_ = nullptr;
    mapping_ = requestEvent_ = doneEvent_ = nullptr;
    mappingBytes_ = 0;
    owner_ = false;
}

bool SharedAudioChannel::signalRequest() noexcept { return SetEvent(static_cast<HANDLE>(requestEvent_)) != 0; }
bool SharedAudioChannel::signalDone() noexcept { return SetEvent(static_cast<HANDLE>(doneEvent_)) != 0; }

bool SharedAudioChannel::waitRequest(int timeoutMs) noexcept {
    return WaitForSingleObject(static_cast<HANDLE>(requestEvent_), static_cast<DWORD>(std::max(0, timeoutMs))) == WAIT_OBJECT_0;
}

bool SharedAudioChannel::waitDone(int timeoutMicros) noexcept {
    const DWORD ms = static_cast<DWORD>((std::max(0, timeoutMicros) + 999) / 1000);
    return WaitForSingleObject(static_cast<HANDLE>(doneEvent_), ms) == WAIT_OBJECT_0;
}

#else

bool SharedAudioChannel::mapShared(const std::string& name, std::size_t bytes, bool create, std::string& error) {
    const std::string shmName = objectName(name, "-shm");
    const std::string requestName = objectName(name, "-req");
    const std::string doneName = objectName(name, "-done");
    if (create) {
        shm_unlink(shmName.c_str());
        sem_unlink(requestName.c_str());
        sem_unlink(doneName.c_str());
    }
    const int fd = shm_open(shmName.c_str(), create ? (O_CREAT | O_RDWR) : O_RDWR, 0600);
    if (fd < 0) {
        error = "shm_open failed: " + std::string(std::strerror(errno));
        return false;
    }
    if (create && ftruncate(fd, static_cast<off_t>(bytes)) != 0) {
        error = "ftruncate failed: " + std::string(std::strerror(errno));
        ::close(fd);
        return false;
    }
    void* view = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (view == MAP_FAILED) {
        error = "mmap failed: " + std::string(std::strerror(errno));
        ::close(fd);
        return false;
    }
    sem_t* request = create ? sem_open(requestName.c_str(), O_CREAT, 0600, 0) : sem_open(requestName.c_str(), 0);
    sem_t* done = create ? sem_open(doneName.c_str(), O_CREAT, 0600, 0) : sem_open(doneName.c_str(), 0);
    if (request == SEM_FAILED || done == SEM_FAILED) {
        error = "sem_open failed: " + std::string(std::strerror(errno));
        if (request != SEM_FAILED) sem_close(request);
        if (done != SEM_FAILED) sem_close(done);
        munmap(view, bytes);
        ::close(fd);
        return false;
    }
    fd_ = fd;
    requestEvent_ = request;
    doneEvent_ = done;
    header_ = static_cast<Header*>(view);
    mappingBytes_ = bytes;
    return true;
}

void SharedAudioChannel::close() noexcept {
    if (header_ != nullptr) munmap(header_, mappingBytes_);
    if (fd_ >= 0) ::close(fd_);
    if (requestEvent_ != nullptr) sem_close(static_cast<sem_t*>(requestEvent_));
    if (doneEvent_ != nullptr) sem_close(static_cast<sem_t*>(doneEvent_));
    if (owner_ && !name_.empty()) {
        shm_unlink(objectName(name_, "-shm").c_str());
        sem_unlink(objectName(name_, "-req").c_str());
        sem_unlink(objectName(name_, "-done").c_str());
    }
    header_ = nullptr;
    fd_ = -1;
    requestEvent_ = doneEvent_ = nullptr;
    mappingBytes_ = 0;
    owner_ = false;
}

bool SharedAudioChannel::signalRequest() noexcept { return sem_post(static_cast<sem_t*>(requestEvent_)) == 0; }
bool SharedAudioChannel::signalDone() noexcept { return sem_post(static_cast<sem_t*>(doneEvent_)) == 0; }
bool SharedAudioChannel::waitRequest(int timeoutMs) noexcept {
    return semWaitMicros(static_cast<sem_t*>(requestEvent_), static_cast<long long>(timeoutMs) * 1000);
}
bool SharedAudioChannel::waitDone(int timeoutMicros) noexcept {
    return semWaitMicros(static_cast<sem_t*>(doneEvent_), timeoutMicros);
}

#endif

}  // namespace reka
