#pragma once

#include "reka/dsp/LoudnessMeter.h"
#include "reka/engine/InsertProcessor.h"
#include "reka/engine/Parameters.h"
#include "reka/engine/RekaAudioEngine.h"
#include "reka/engine/StripProcessor.h"
#include "reka/io/EnginePort.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace reka {

// Which endpoint channels a strip reads (input) or writes (bus). Ports are referenced by their stable
// key, so a binding survives the device being unplugged and re-appearing with a new PortId.
struct PortBinding {
    PortKey port;
    int left = 0;
    int right = 1;  // -1 = mono endpoint channel
    bool operator==(const PortBinding&) const = default;
};

struct StripInfo {
    StripId id = kInvalidStrip;
    StripKind kind = StripKind::Input;
    std::string name;
    std::uint32_t colour = 0xFF4DA3FFu;  // ARGB
    PortBinding binding;
    std::vector<InsertSlotPtr> inserts;
    std::shared_ptr<StripParameters> params;
    std::shared_ptr<StripProcessor> processor;
};

struct CrosspointInfo {
    StripId input = kInvalidStrip;
    StripId bus = kInvalidStrip;
    std::shared_ptr<CrosspointParameters> params;
};

// The authoritative, message-thread-only model of the mixer: strips, buses, the routing matrix,
// insert racks and endpoint ports. Every structural change rebuilds an immutable RenderGraph and
// publishes it to the engine without stopping audio. Parameter changes (StripParameters,
// CrosspointParameters) bypass the model entirely and are picked up by the audio thread lock-free.
class MixerModel {
public:
    explicit MixerModel(RekaAudioEngine& engine);
    ~MixerModel();
    MixerModel(const MixerModel&) = delete;
    MixerModel& operator=(const MixerModel&) = delete;

    RekaAudioEngine& engine() noexcept { return engine_; }

    // ---- format ---- (audio must be stopped: re-prepares every processor and insert)
    void prepare(double sampleRate, int maxBlockSize);
    double sampleRate() const noexcept { return sampleRate_; }
    int maxBlockSize() const noexcept { return maxBlockSize_; }

    // ---- batching ---- Defers graph publication until the outermost Batch is destroyed.
    class Batch {
    public:
        explicit Batch(MixerModel& model) : model_(&model) { ++model_->batchDepth_; }
        ~Batch() {
            if (model_ != nullptr && --model_->batchDepth_ == 0) model_->commit();
        }
        Batch(Batch&& other) noexcept : model_(std::exchange(other.model_, nullptr)) {}
        Batch(const Batch&) = delete;
        Batch& operator=(const Batch&) = delete;
        Batch& operator=(Batch&&) = delete;

    private:
        MixerModel* model_;
    };
    [[nodiscard]] Batch batch() { return Batch(*this); }

    // ---- strips ----
    StripId addStrip(StripKind kind, std::string name, StripId requestedId = kInvalidStrip, int index = -1);
    bool removeStrip(StripId id);
    bool moveStrip(StripId id, int newIndex);  // index within its kind
    bool renameStrip(StripId id, std::string name);
    bool setStripColour(StripId id, std::uint32_t argb);
    bool setBinding(StripId id, PortBinding binding);
    void clear();

    const StripInfo* strip(StripId id) const;
    std::vector<StripId> strips(StripKind kind) const;
    std::shared_ptr<StripParameters> parameters(StripId id) const;
    std::shared_ptr<StripProcessor> processor(StripId id) const;
    int indexOf(StripId id) const;

    // ---- routing matrix ----
    std::shared_ptr<CrosspointParameters> route(StripId input, StripId bus);  // get or create
    std::shared_ptr<CrosspointParameters> findRoute(StripId input, StripId bus) const;
    bool setRouteEnabled(StripId input, StripId bus, bool enabled);
    bool removeRoute(StripId input, StripId bus);
    std::vector<CrosspointInfo> routes() const;

    // ---- insert racks ----
    bool setInserts(StripId id, std::vector<InsertSlotPtr> inserts);
    bool addInsert(StripId id, InsertSlotPtr slot, int index = -1);
    bool removeInsert(StripId id, int index);
    bool moveInsert(StripId id, int from, int to);
    bool replaceInsert(const InsertSlot* existing, InsertSlotPtr replacement);
    // Plugins report latency changes asynchronously; the app calls this to recompute PDC.
    void latencyChanged();
    int chainLatency(StripId id) const;

    // ---- endpoint ports ----
    PortId registerPort(PortInfo info, std::shared_ptr<IEnginePort> port);  // replaces an existing key
    bool unregisterPort(const PortKey& key);
    std::vector<PortInfo> ports() const;
    std::optional<PortInfo> findPort(const PortKey& key) const;

    // ---- bookkeeping ----
    // Increments when strips, names, colours, bindings, inserts or ports change - i.e. whenever the UI or
    // the hardware bridge must rebuild. Routing toggles and latency changes rebuild the render graph but
    // do not bump it (route state is polled from CrosspointParameters).
    std::uint64_t topologyRevision() const noexcept { return topologyRevision_; }
    std::uint64_t routingRevision() const noexcept { return routingRevision_; }
    void commit();
    // Message-thread timer (10-30 Hz): frees retired graphs, integrates loudness.
    void housekeeping();
    float integratedLufs(StripId id) const;
    void resetIntegratedLoudness(StripId id);

private:
    struct PortEntry {
        PortInfo info;
        std::shared_ptr<IEnginePort> port;
    };
    using RouteKey = std::pair<StripId, StripId>;
    struct RouteEntry {
        std::shared_ptr<CrosspointParameters> params;
        std::shared_ptr<CrosspointProcessor> processor;
    };

    StripInfo* findStrip(StripId id);
    const StripInfo* findStrip(StripId id) const;
    std::vector<StripInfo>& listFor(StripKind kind) { return kind == StripKind::Input ? inputs_ : buses_; }
    void structuralChange(bool visibleToUi = true);
    void rebuildGraph();
    void prepareInsert(const InsertSlotPtr& slot);
    std::shared_ptr<DelayLine> delayLineFor(std::map<const void*, std::shared_ptr<DelayLine>>& pool,
                                            const void* key, int delay);

    RekaAudioEngine& engine_;
    double sampleRate_ = 48000.0;
    int maxBlockSize_ = 512;

    std::vector<StripInfo> inputs_;
    std::vector<StripInfo> buses_;
    std::map<RouteKey, RouteEntry> routes_;
    std::map<PortKey, PortEntry> ports_;
    std::map<StripId, LoudnessIntegrator> integrators_;

    std::map<const void*, std::shared_ptr<DelayLine>> bypassDelays_;
    std::map<const void*, std::shared_ptr<DelayLine>> pdcDelays_;

    StripId nextStripId_ = 1;
    PortId nextPortId_ = 1;
    int batchDepth_ = 0;
    bool dirty_ = false;
    std::uint64_t topologyRevision_ = 1;
    std::uint64_t routingRevision_ = 1;
};

}  // namespace reka
