#include "reka/virtualaudio/win/ProcessLoopbackCapture.h"

#if defined(_WIN32)

#include "reka/platform/ThreadPriority.h"
#include "reka/virtualaudio/win/AudioClientActivationParams.h"
#include "reka/virtualaudio/win/ComUtil.h"

#include <windows.h>
#include <mmreg.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <initializer_list>
#include <mutex>
#include <new>
#include <thread>
#include <vector>

namespace reka::win {

namespace {

constexpr int kChannels = 2;
constexpr DWORD kSampleRate = 48000;
constexpr REFERENCE_TIME kBufferDuration = 200000;  // 20 ms in 100 ns units, as in Microsoft's sample
constexpr DWORD kActivationTimeoutMs = 10000;
constexpr DWORD kCapturePollMs = 20;  // safety net in case the loopback event is not signalled

// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT {00000003-0000-0010-8000-00AA00389B71}
constexpr GUID kSubtypeIeeeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"  // COM objects die through Release(), never through a base pointer
#endif

// Receives the IAudioClient from ActivateAudioInterfaceAsync. Must be agile: Windows calls it on one of its
// own worker threads. Reference counted by hand; the object deletes itself on the last Release().
class ActivationHandler final : public IActivateAudioInterfaceCompletionHandler, public IAgileObject {
public:
    static ActivationHandler* create() noexcept {
        auto* handler = new (std::nothrow) ActivationHandler();
        if (handler != nullptr && handler->done_ == nullptr) {
            handler->Release();
            return nullptr;
        }
        return handler;
    }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
        if (object == nullptr) return E_POINTER;
        if (IsEqualIID(riid, __uuidof(IUnknown)) || IsEqualIID(riid, __uuidof(IActivateAudioInterfaceCompletionHandler))) {
            *object = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
        } else if (IsEqualIID(riid, __uuidof(IAgileObject))) {
            *object = static_cast<IAgileObject*>(this);
        } else {
            *object = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&references_)); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG remaining = InterlockedDecrement(&references_);
        if (remaining == 0) delete this;
        return static_cast<ULONG>(remaining);
    }

    // IActivateAudioInterfaceCompletionHandler
    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation* operation) override {
        HRESULT activateResult = E_FAIL;
        IUnknown* activated = nullptr;
        HRESULT hr = operation != nullptr ? operation->GetActivateResult(&activateResult, &activated) : E_POINTER;
        if (SUCCEEDED(hr)) hr = activateResult;
        if (SUCCEEDED(hr) && activated == nullptr) hr = E_NOINTERFACE;
        if (SUCCEEDED(hr)) hr = activated->QueryInterface(__uuidof(IAudioClient), client_.putVoid());
        if (activated != nullptr) activated->Release();
        result_ = hr;
        SetEvent(done_);  // publishes result_ / client_ to the waiting thread
        return S_OK;
    }

    HANDLE doneEvent() const noexcept { return done_; }
    HRESULT result() const noexcept { return result_; }
    ComPtr<IAudioClient> takeClient() noexcept { return ComPtr<IAudioClient>::adopt(client_.detach()); }

private:
    ActivationHandler() noexcept : done_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
    ~ActivationHandler() {
        if (done_ != nullptr) CloseHandle(done_);
    }

    LONG references_ = 1;
    HANDLE done_ = nullptr;
    HRESULT result_ = E_PENDING;
    ComPtr<IAudioClient> client_;
};

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

WAVEFORMATEXTENSIBLE captureFormat() {
    WAVEFORMATEXTENSIBLE format{};
    format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    format.Format.nChannels = kChannels;
    format.Format.nSamplesPerSec = kSampleRate;
    format.Format.wBitsPerSample = 32;
    format.Format.nBlockAlign = static_cast<WORD>(kChannels * sizeof(float));
    format.Format.nAvgBytesPerSec = kSampleRate * format.Format.nBlockAlign;
    format.Format.cbSize = static_cast<WORD>(sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX));
    format.Samples.wValidBitsPerSample = 32;
    format.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    format.SubFormat = kSubtypeIeeeFloat;
    return format;
}

class ProcessLoopbackStream final : public IVirtualStream {
public:
    ProcessLoopbackStream(std::uint32_t pid, std::string executable) : pid_(pid), executable_(std::move(executable)) {}

    ~ProcessLoopbackStream() override {
        stop();
        for (HANDLE handle : {stopEvent_, audioEvent_, processHandle_})
            if (handle != nullptr) CloseHandle(handle);
    }

    ProcessLoopbackStream(const ProcessLoopbackStream&) = delete;
    ProcessLoopbackStream& operator=(const ProcessLoopbackStream&) = delete;

    bool start(double engineSampleRate, int engineMaxBlock, std::string& error);

    std::shared_ptr<AsyncEndpointPort> port() const override { return port_; }

    PortInfo portInfo() const override {
        PortInfo info;
        info.key = "app:" + executable_ + ":" + std::to_string(pid_);
        info.displayName = executable_;
        info.type = PortType::ProcessCapture;
        info.numInputs = kChannels;
        info.numOutputs = 0;
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
    }

private:
    void threadMain() noexcept;
    void run();
    bool openClient(ComPtr<IAudioClient>& client, ComPtr<IAudioCaptureClient>& capture, UINT32& bufferFrames,
                    std::string& error);
    void captureLoop(IAudioCaptureClient* capture, int maxBlock);
    void finishStartup(bool ok, const std::string& error);
    void setError(const std::string& message) noexcept;

    const std::uint32_t pid_;
    const std::string executable_;
    std::shared_ptr<AsyncEndpointPort> port_;
    double engineSampleRate_ = 48000.0;
    int engineMaxBlock_ = 512;

    HANDLE stopEvent_ = nullptr;
    HANDLE audioEvent_ = nullptr;
    HANDLE processHandle_ = nullptr;  // SYNCHRONIZE handle: notices the application exiting
    std::thread thread_;
    std::atomic<bool> running_{false};

    std::mutex startupMutex_;
    std::condition_variable startupCondition_;
    bool startupDone_ = false;
    bool startupOk_ = false;
    std::string startupError_;

    mutable std::mutex errorMutex_;
    std::string lastError_;
};

bool ProcessLoopbackStream::start(double engineSampleRate, int engineMaxBlock, std::string& error) {
    if (!(engineSampleRate > 0.0) || engineMaxBlock <= 0) {
        error = "Cannot capture " + executable_ + ": the audio engine is not running";
        return false;
    }
    engineSampleRate_ = engineSampleRate;
    engineMaxBlock_ = engineMaxBlock;
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    audioEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (stopEvent_ == nullptr || audioEvent_ == nullptr) {
        error = "Cannot capture " + executable_ + ": " + win32ErrorMessage(GetLastError());
        return false;
    }
    processHandle_ = OpenProcess(SYNCHRONIZE, FALSE, pid_);  // optional; protected processes refuse it
    port_ = std::make_shared<AsyncEndpointPort>();

    try {
        thread_ = std::thread([this] { threadMain(); });
    } catch (const std::exception& e) {
        error = "Cannot capture " + executable_ + ": could not start the capture thread (" + e.what() + ")";
        return false;
    }

    std::unique_lock<std::mutex> lock(startupMutex_);
    startupCondition_.wait(lock, [this] { return startupDone_; });
    if (!startupOk_) {
        error = startupError_;
        lock.unlock();
        thread_.join();
        return false;
    }
    return true;
}

void ProcessLoopbackStream::finishStartup(bool ok, const std::string& error) {
    {
        std::lock_guard<std::mutex> lock(startupMutex_);
        if (startupDone_) return;
        startupOk_ = ok;
        startupError_ = error;
        startupDone_ = true;
    }
    startupCondition_.notify_all();
}

void ProcessLoopbackStream::setError(const std::string& message) noexcept {
    try {
        std::lock_guard<std::mutex> lock(errorMutex_);
        lastError_ = message;
    } catch (...) {
    }
}

void ProcessLoopbackStream::threadMain() noexcept {
    try {
        run();
    } catch (...) {
        try {
            const std::string message = "Capture of " + executable_ + " stopped after an unexpected error";
            setError(message);
            finishStartup(false, message);
        } catch (...) {
        }
    }
    running_.store(false, std::memory_order_release);
    // Never leave start() waiting, whatever happened above (a no-op once startup was reported).
    try {
        finishStartup(false, "Capture of " + executable_ + " failed to start");
    } catch (...) {
        try {
            finishStartup(false, std::string());  // no allocation: still releases start()
        } catch (...) {
        }
    }
}

void ProcessLoopbackStream::run() {
    // Every COM object of the stream lives and dies on this MTA thread; declared after `com` so they are
    // released before CoUninitialize.
    ScopedCoInit com(COINIT_MULTITHREADED);
    if (!com.ok()) {
        finishStartup(false, "Cannot capture " + executable_ + ": COM initialisation failed (" + hresultMessage(com.result()) + ")");
        return;
    }
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    UINT32 bufferFrames = 0;
    std::string error;
    if (!openClient(client, capture, bufferFrames, error)) {
        finishStartup(false, error);
        return;
    }

    AsyncPortFormat format;
    format.captureChannels = kChannels;
    format.renderChannels = 0;
    format.endpointSampleRate = static_cast<double>(kSampleRate);
    format.endpointMaxBlock = static_cast<int>(bufferFrames);
    port_->configure(format, engineSampleRate_, engineMaxBlock_);  // before any endpoint write, before start() returns

    const HRESULT hr = client->Start();
    if (FAILED(hr)) {
        finishStartup(false, "Cannot capture " + executable_ + ": the audio stream did not start (" + hresultMessage(hr) + ")");
        return;
    }
    running_.store(true, std::memory_order_release);
    finishStartup(true, {});
    {
        ScopedAudioThreadPriority priority;
        captureLoop(capture.get(), static_cast<int>(bufferFrames));
    }
    client->Stop();
}

bool ProcessLoopbackStream::openClient(ComPtr<IAudioClient>& client, ComPtr<IAudioCaptureClient>& capture,
                                       UINT32& bufferFrames, std::string& error) {
    const std::string prefix = "Cannot capture " + executable_ + ": ";

    AUDIOCLIENT_ACTIVATION_PARAMS params{};
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = pid_;
    // Windows offers "this process tree" or "everything except this process tree"; there is no
    // target-only mode, so child processes (browser renderers, game launchers...) are always included.
    params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

    PROPVARIANT activation;
    PropVariantInit(&activation);
    activation.vt = VT_BLOB;
    activation.blob.cbSize = sizeof(params);
    activation.blob.pBlobData = reinterpret_cast<BYTE*>(&params);  // borrowed: never PropVariantClear it

    ActivationHandler* handler = ActivationHandler::create();
    if (handler == nullptr) {
        error = prefix + "out of resources";
        return false;
    }
    ComPtr<IActivateAudioInterfaceAsyncOperation> operation;
    HRESULT hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &activation,
                                             handler, operation.put());
    if (FAILED(hr)) {
        handler->Release();
        error = prefix + "Windows refused the process-loopback activation (" + hresultMessage(hr) + ")";
        return false;
    }
    const HANDLE waits[2] = {handler->doneEvent(), stopEvent_};
    const DWORD waited = WaitForMultipleObjects(2, waits, FALSE, kActivationTimeoutMs);
    if (waited != WAIT_OBJECT_0) {
        handler->Release();  // Windows keeps its own reference until the callback runs
        error = prefix + (waited == WAIT_OBJECT_0 + 1 ? "cancelled" : "Windows did not complete the activation in time");
        return false;
    }
    hr = handler->result();
    client = handler->takeClient();
    handler->Release();
    if (FAILED(hr) || !client) {
        error = prefix + "process-loopback activation failed (" + hresultMessage(FAILED(hr) ? hr : E_NOINTERFACE) + ")";
        return false;
    }

    WAVEFORMATEXTENSIBLE format = captureFormat();
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                            AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
                            kBufferDuration, 0, &format.Format, nullptr);
    if (FAILED(hr)) {
        error = prefix + "the loopback stream could not be initialised (" + hresultMessage(hr) + ")";
        return false;
    }
    hr = client->SetEventHandle(audioEvent_);
    if (FAILED(hr)) {
        error = prefix + "the loopback stream rejected its event (" + hresultMessage(hr) + ")";
        return false;
    }
    // The process-loopback client does not implement every IAudioClient query; fall back to the request.
    if (FAILED(client->GetBufferSize(&bufferFrames)) || bufferFrames == 0)
        bufferFrames = static_cast<UINT32>(kSampleRate * kBufferDuration / 10000000);
    hr = client->GetService(__uuidof(IAudioCaptureClient), capture.putVoid());
    if (FAILED(hr) || !capture) {
        error = prefix + "no capture service (" + hresultMessage(FAILED(hr) ? hr : E_NOINTERFACE) + ")";
        return false;
    }
    return true;
}

void ProcessLoopbackStream::captureLoop(IAudioCaptureClient* capture, int maxBlock) {
    maxBlock = std::max(16, maxBlock);
    const std::vector<float> silence(static_cast<std::size_t>(maxBlock) * kChannels, 0.0f);
    const HANDLE waits[3] = {stopEvent_, audioEvent_, processHandle_};
    const DWORD waitCount = processHandle_ != nullptr ? 3u : 2u;

    for (;;) {
        const DWORD waited = WaitForMultipleObjects(waitCount, waits, FALSE, kCapturePollMs);
        if (waited == WAIT_OBJECT_0) return;
        if (waited == WAIT_OBJECT_0 + 2) {
            setError(executable_ + " has exited");
            return;
        }
        if (waited == WAIT_FAILED) {
            setError("Capture of " + executable_ + " stopped: " + win32ErrorMessage(GetLastError()));
            return;
        }

        HRESULT hr = S_OK;
        for (;;) {
            UINT32 packetFrames = 0;
            hr = capture->GetNextPacketSize(&packetFrames);
            if (FAILED(hr) || packetFrames == 0) break;

            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            hr = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
            if (hr == AUDCLNT_S_BUFFER_EMPTY) {
                hr = S_OK;
                break;
            }
            if (FAILED(hr)) break;

            const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr;
            const auto* samples = reinterpret_cast<const float*>(data);
            for (UINT32 done = 0; done < frames;) {
                const int chunk = static_cast<int>(std::min<UINT32>(frames - done, static_cast<UINT32>(maxBlock)));
                port_->endpointWriteCaptureInterleaved(silent ? silence.data() : samples + static_cast<std::size_t>(done) * kChannels,
                                                       kChannels, chunk);
                done += static_cast<UINT32>(chunk);
            }
            hr = capture->ReleaseBuffer(frames);
            if (FAILED(hr)) break;
        }
        if (FAILED(hr)) {
            setError("Capture of " + executable_ + " stopped: " + hresultMessage(hr));
            return;
        }
    }
}

}  // namespace

std::uint32_t windowsBuildNumber() noexcept {
    using RtlGetVersionFunction = LONG(WINAPI*)(OSVERSIONINFOW*);  // NTSTATUS RtlGetVersion(PRTL_OSVERSIONINFOW)
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr) return 0;
    const FARPROC address = GetProcAddress(ntdll, "RtlGetVersion");
    if (address == nullptr) return 0;
    const auto rtlGetVersion = reinterpret_cast<RtlGetVersionFunction>(reinterpret_cast<void (*)()>(address));
    OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    if (rtlGetVersion(&version) != 0) return 0;
    return version.dwBuildNumber;
}

bool processLoopbackAvailable() noexcept { return windowsBuildNumber() >= 20348; }

std::unique_ptr<IVirtualStream> startProcessLoopbackCapture(std::uint32_t pid, const std::string& executable,
                                                            double engineSampleRate, int engineMaxBlock,
                                                            std::string& error) {
    auto stream = std::make_unique<ProcessLoopbackStream>(pid, executable);
    if (!stream->start(engineSampleRate, engineMaxBlock, error)) return nullptr;
    return stream;
}

}  // namespace reka::win

#endif  // _WIN32
