#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace reka {

// What a profile needs to recreate an insert.
struct InsertDescriptor {
    std::string pluginId;     // e.g. JUCE PluginDescription::createIdentifierString()
    std::string name;
    std::string format;       // "VST3", "Internal", ...
    bool sandboxed = false;   // hosted in REKA-PluginHost.exe instead of in-process
};

// A processing unit in a strip's insert rack (VST3 plugin, sandboxed plugin, built-in effect).
// The engine never knows which: JUCE/IPC specifics stay in the app layer.
//
// Threading: prepare/release/saveState/restoreState on the message thread; process() and
// latencySamples() on the audio thread. Destruction happens on the message thread (garbage collector).
class IInsertProcessor {
public:
    virtual ~IInsertProcessor() = default;

    virtual void prepare(double sampleRate, int maxBlockSize) = 0;
    virtual void release() = 0;

    // In-place stereo processing. Must be wait-free / allocation-free.
    virtual void process(float* const* channels, int numChannels, int numFrames) noexcept = 0;

    virtual int latencySamples() const noexcept = 0;

    virtual std::vector<std::uint8_t> saveState() = 0;
    virtual bool restoreState(const std::vector<std::uint8_t>& state) = 0;

    virtual const InsertDescriptor& descriptor() const noexcept = 0;

    // Set by implementations when they detect a fatal error (e.g. sandbox process died). The app's
    // watchdog replaces failed inserts; the engine treats a failed insert as bypassed.
    bool hasFailed() const noexcept { return failed_.load(std::memory_order_acquire); }

protected:
    void markFailed() noexcept { failed_.store(true, std::memory_order_release); }

private:
    std::atomic<bool> failed_{false};
};

// One slot of an insert rack: the processor plus its bypass switch.
struct InsertSlot {
    explicit InsertSlot(std::shared_ptr<IInsertProcessor> p) : processor(std::move(p)) {}

    const std::shared_ptr<IInsertProcessor> processor;
    std::atomic<bool> bypassed{false};
};

using InsertSlotPtr = std::shared_ptr<InsertSlot>;

// Creates inserts from descriptors (implemented by the VST host; mocked in tests).
class IInsertFactory {
public:
    virtual ~IInsertFactory() = default;
    virtual std::shared_ptr<IInsertProcessor> createInsert(const InsertDescriptor& descriptor, double sampleRate,
                                                           int maxBlockSize, std::string& error) = 0;
};

}  // namespace reka
