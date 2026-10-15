#pragma once

#include "reka/dsp/PanLaw.h"
#include "reka/engine/RenderGraph.h"
#include "reka/lockfree/SpscQueue.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace reka {

// The real-time mixing engine.
//
// Threading contract
//   * processBlock()                    audio thread (the master device callback or the software clock)
//   * prepare(), publish(), collectGarbage(), destructor
//                                       message thread
//   * setPanLaw(), cpuLoad(), counters  any thread
//
// Graph hand-off is RCU-style: publish() swaps a pointer the audio thread picks up at its next block
// boundary; the audio thread pushes the graph it stops using onto a lock-free queue and collectGarbage()
// deletes it on the message thread. The audio thread never allocates, frees, locks or waits.
class RekaAudioEngine {
public:
    RekaAudioEngine();
    ~RekaAudioEngine();
    RekaAudioEngine(const RekaAudioEngine&) = delete;
    RekaAudioEngine& operator=(const RekaAudioEngine&) = delete;

    // Audio must be stopped. Allocates scratch memory for blocks of up to maxBlockSize frames.
    void prepare(double sampleRate, int maxBlockSize);
    double sampleRate() const noexcept { return sampleRate_; }
    int maxBlockSize() const noexcept { return maxBlockSize_; }

    void publish(std::unique_ptr<RenderGraph> graph);
    // Deletes graphs the audio thread has finished with. Call from a message-thread timer (10-30 Hz).
    void collectGarbage();
    // Audio must be stopped. Drops every graph (and with them the last references to plugins, ports and
    // processors) so the owner can tear subsystems down in a defined order.
    void releaseGraphs();

    // Renders one block of numFrames (any size: larger blocks are split internally).
    void processBlock(int numFrames) noexcept;

    void setPanLaw(PanLaw law) noexcept { panLaw_.store(law, std::memory_order_relaxed); }
    PanLaw panLaw() const noexcept { return panLaw_.load(std::memory_order_relaxed); }

    float cpuLoad() const noexcept { return cpuLoad_.load(std::memory_order_relaxed); }  // 1.0 == 100 %
    std::uint64_t blocksProcessed() const noexcept { return blocks_.load(std::memory_order_relaxed); }
    std::uint32_t graphSwaps() const noexcept { return swaps_.load(std::memory_order_relaxed); }

private:
    RenderGraph* acquireGraph() noexcept;
    void processChunk(RenderGraph& graph, int numFrames) noexcept;
    void processStrip(RenderGraph& graph, GraphStrip& strip, int numFrames, bool anySolo, PanLaw law) noexcept;
    void processCrosspoint(RenderGraph& graph, GraphCrosspoint& crosspoint, int numFrames) noexcept;
    void runInserts(RenderGraph& graph, const GraphStrip& strip, float* const* channels, int numFrames,
                    bool bypassAll) noexcept;

    double sampleRate_ = 48000.0;
    int maxBlockSize_ = 512;
    std::vector<float> scratchStorage_;
    std::array<float*, kStripChannels> scratch_{};

    std::atomic<RenderGraph*> pending_{nullptr};
    RenderGraph* current_ = nullptr;  // audio thread only (or message thread while stopped)
    SpscQueue<RenderGraph*, 256> retired_;

    std::atomic<PanLaw> panLaw_{PanLaw::ConstantPower};
    std::atomic<float> cpuLoad_{0.0f};
    std::atomic<std::uint64_t> blocks_{0};
    std::atomic<std::uint32_t> swaps_{0};
};

}  // namespace reka
