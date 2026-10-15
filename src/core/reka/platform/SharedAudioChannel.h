#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace reka {

// Cross-process audio channel used by the plugin sandbox (REKA.exe <-> REKA-PluginHost.exe).
//
// Shared memory: a header with sequence counters + one planar float buffer per channel. Two named,
// auto-reset kernel events/semaphores carry the "request" and "done" signals. The host's audio thread
// writes the block, signals the request and waits (bounded) for the done signal; the plugin host's audio
// thread processes in place. A late answer for an older block is recognised by its sequence number and
// discarded, so a stalled plugin can never desynchronise the stream; the host just passes audio through.
class SharedAudioChannel {
public:
    static constexpr int kMaxChannels = 2;
    static constexpr std::uint32_t kMagic = 0x524B5342u;  // "RKSB"
    static constexpr std::uint32_t kVersion = 1;

    enum class Result { Ok, Timeout, Disconnected };

    SharedAudioChannel() = default;
    ~SharedAudioChannel();
    SharedAudioChannel(const SharedAudioChannel&) = delete;
    SharedAudioChannel& operator=(const SharedAudioChannel&) = delete;

    // Host side: creates the shared objects. `name` must be unique per instance (e.g. "reka-sbx-<pid>-<n>").
    bool create(const std::string& name, int maxFrames, std::string& error);
    // Plugin-host side.
    bool open(const std::string& name, std::string& error);
    void close() noexcept;
    bool isOpen() const noexcept { return header_ != nullptr; }
    int maxFrames() const noexcept;

    // ---- host, audio thread ----
    // Copies `channels` in, waits up to timeoutMicros for the plugin host, copies the result back.
    // On Timeout/Disconnected the buffers are left untouched (dry pass-through).
    Result process(float* const* channels, int numChannels, int numFrames, int timeoutMicros) noexcept;

    // ---- plugin host, audio thread ----
    // Blocks up to timeoutMs for a request. Returns the frame count, 0 on timeout, -1 on shutdown.
    int waitForRequest(int timeoutMs) noexcept;
    float* channel(int index) noexcept;
    int requestChannels() const noexcept;
    void completeRequest() noexcept;

    // ---- both ----
    void setLatency(int samples) noexcept;
    int latency() const noexcept;
    void requestShutdown() noexcept;  // wakes a waiting plugin-host thread

    struct Header {
        std::uint32_t magic;
        std::uint32_t version;
        std::int32_t maxFrames;
        std::int32_t numChannels;
        std::atomic<std::uint32_t> requestSeq;
        std::atomic<std::uint32_t> responseSeq;
        std::atomic<std::int32_t> numFrames;
        std::atomic<std::int32_t> activeChannels;
        std::atomic<std::int32_t> latency;
        std::atomic<std::int32_t> shutdown;
        std::uint8_t reserved[64 - 40];
        // float samples[numChannels][maxFrames] follow
    };

private:
    float* samples(int channel) noexcept;
    bool signalRequest() noexcept;
    bool waitRequest(int timeoutMs) noexcept;
    bool signalDone() noexcept;
    bool waitDone(int timeoutMicros) noexcept;
    bool mapShared(const std::string& name, std::size_t bytes, bool create, std::string& error);

    Header* header_ = nullptr;
    std::size_t mappingBytes_ = 0;
    bool owner_ = false;
    std::string name_;
    // Platform handles (HANDLE / sem_t* / fd), stored opaquely.
    void* mapping_ = nullptr;
    void* requestEvent_ = nullptr;
    void* doneEvent_ = nullptr;
    int fd_ = -1;
};

}  // namespace reka
