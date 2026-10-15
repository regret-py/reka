#include "reka/engine/MixerModel.h"

#include <algorithm>
#include <set>
#include <unordered_map>

namespace reka {

MixerModel::MixerModel(RekaAudioEngine& engine) : engine_(engine) {
    sampleRate_ = engine.sampleRate();
    maxBlockSize_ = engine.maxBlockSize();
}

MixerModel::~MixerModel() = default;

// ---- format ------------------------------------------------------------------------------------------

void MixerModel::prepare(double sampleRate, int maxBlockSize) {
    sampleRate_ = sampleRate;
    maxBlockSize_ = maxBlockSize;
    engine_.prepare(sampleRate, maxBlockSize);
    for (auto* list : {&inputs_, &buses_}) {
        for (StripInfo& s : *list) {
            s.processor->prepare(sampleRate, maxBlockSize);
            for (const InsertSlotPtr& slot : s.inserts) prepareInsert(slot);
        }
    }
    for (auto& [key, route] : routes_) route.processor->prepare(sampleRate);
    for (auto& [id, integrator] : integrators_) integrator.reset();
    // Latencies may change with the rate; recreate compensation delays from scratch.
    bypassDelays_.clear();
    pdcDelays_.clear();
    structuralChange();
}

void MixerModel::prepareInsert(const InsertSlotPtr& slot) {
    if (slot && slot->processor) slot->processor->prepare(sampleRate_, maxBlockSize_);
}

// ---- strips ------------------------------------------------------------------------------------------

StripInfo* MixerModel::findStrip(StripId id) {
    for (auto* list : {&inputs_, &buses_})
        for (StripInfo& s : *list)
            if (s.id == id) return &s;
    return nullptr;
}

const StripInfo* MixerModel::findStrip(StripId id) const { return const_cast<MixerModel*>(this)->findStrip(id); }

StripId MixerModel::addStrip(StripKind kind, std::string name, StripId requestedId, int index) {
    StripId id = requestedId;
    if (id == kInvalidStrip || findStrip(id) != nullptr) id = nextStripId_;
    nextStripId_ = std::max(nextStripId_, id + 1);

    StripInfo info;
    info.id = id;
    info.kind = kind;
    info.name = std::move(name);
    info.colour = kind == StripKind::Input ? 0xFF4DA3FFu : 0xFFB47CFFu;
    info.binding.right = 1;
    info.params = std::make_shared<StripParameters>();
    info.processor = std::make_shared<StripProcessor>();
    info.processor->prepare(sampleRate_, maxBlockSize_);

    auto& list = listFor(kind);
    if (index < 0 || index > static_cast<int>(list.size())) list.push_back(std::move(info));
    else list.insert(list.begin() + index, std::move(info));
    integrators_[id];
    structuralChange();
    return id;
}

bool MixerModel::removeStrip(StripId id) {
    for (auto* list : {&inputs_, &buses_}) {
        auto it = std::find_if(list->begin(), list->end(), [id](const StripInfo& s) { return s.id == id; });
        if (it == list->end()) continue;
        list->erase(it);
        for (auto r = routes_.begin(); r != routes_.end();) {
            if (r->first.first == id || r->first.second == id) r = routes_.erase(r);
            else ++r;
        }
        integrators_.erase(id);
        structuralChange();
        return true;
    }
    return false;
}

bool MixerModel::moveStrip(StripId id, int newIndex) {
    for (auto* list : {&inputs_, &buses_}) {
        auto it = std::find_if(list->begin(), list->end(), [id](const StripInfo& s) { return s.id == id; });
        if (it == list->end()) continue;
        StripInfo moved = std::move(*it);
        list->erase(it);
        newIndex = std::clamp(newIndex, 0, static_cast<int>(list->size()));
        list->insert(list->begin() + newIndex, std::move(moved));
        structuralChange();
        return true;
    }
    return false;
}

bool MixerModel::renameStrip(StripId id, std::string name) {
    StripInfo* s = findStrip(id);
    if (s == nullptr) return false;
    s->name = std::move(name);
    ++topologyRevision_;  // names are metadata only: no graph rebuild
    return true;
}

bool MixerModel::setStripColour(StripId id, std::uint32_t argb) {
    StripInfo* s = findStrip(id);
    if (s == nullptr) return false;
    s->colour = argb;
    ++topologyRevision_;
    return true;
}

bool MixerModel::setBinding(StripId id, PortBinding binding) {
    StripInfo* s = findStrip(id);
    if (s == nullptr) return false;
    if (s->binding == binding) return true;
    s->binding = std::move(binding);
    structuralChange();
    return true;
}

void MixerModel::clear() {
    inputs_.clear();
    buses_.clear();
    routes_.clear();
    integrators_.clear();
    structuralChange();
}

const StripInfo* MixerModel::strip(StripId id) const { return findStrip(id); }

std::vector<StripId> MixerModel::strips(StripKind kind) const {
    const auto& list = kind == StripKind::Input ? inputs_ : buses_;
    std::vector<StripId> ids;
    ids.reserve(list.size());
    for (const StripInfo& s : list) ids.push_back(s.id);
    return ids;
}

std::shared_ptr<StripParameters> MixerModel::parameters(StripId id) const {
    const StripInfo* s = findStrip(id);
    return s != nullptr ? s->params : nullptr;
}

std::shared_ptr<StripProcessor> MixerModel::processor(StripId id) const {
    const StripInfo* s = findStrip(id);
    return s != nullptr ? s->processor : nullptr;
}

int MixerModel::indexOf(StripId id) const {
    for (const auto* list : {&inputs_, &buses_})
        for (std::size_t i = 0; i < list->size(); ++i)
            if ((*list)[i].id == id) return static_cast<int>(i);
    return -1;
}

// ---- routing -----------------------------------------------------------------------------------------

std::shared_ptr<CrosspointParameters> MixerModel::route(StripId input, StripId bus) {
    const StripInfo* in = findStrip(input);
    const StripInfo* out = findStrip(bus);
    if (in == nullptr || out == nullptr || in->kind != StripKind::Input || out->kind != StripKind::Bus) return nullptr;
    auto [it, inserted] = routes_.try_emplace(RouteKey{input, bus});
    if (inserted) {
        it->second.params = std::make_shared<CrosspointParameters>();
        it->second.processor = std::make_shared<CrosspointProcessor>();
        it->second.processor->prepare(sampleRate_);
        structuralChange(false);
    }
    return it->second.params;
}

std::shared_ptr<CrosspointParameters> MixerModel::findRoute(StripId input, StripId bus) const {
    auto it = routes_.find(RouteKey{input, bus});
    return it != routes_.end() ? it->second.params : nullptr;
}

bool MixerModel::setRouteEnabled(StripId input, StripId bus, bool enabled) {
    auto params = enabled ? route(input, bus) : findRoute(input, bus);
    if (!params) return !enabled;
    if (params->enabled() == enabled) return true;
    params->setEnabled(enabled);
    // PDC is computed over enabled routes, so re-plan. Audio is not interrupted.
    structuralChange(false);
    return true;
}

bool MixerModel::removeRoute(StripId input, StripId bus) {
    if (routes_.erase(RouteKey{input, bus}) == 0) return false;
    structuralChange(false);
    return true;
}

std::vector<CrosspointInfo> MixerModel::routes() const {
    std::vector<CrosspointInfo> out;
    out.reserve(routes_.size());
    for (const auto& [key, entry] : routes_) out.push_back({key.first, key.second, entry.params});
    return out;
}

// ---- inserts -----------------------------------------------------------------------------------------

bool MixerModel::setInserts(StripId id, std::vector<InsertSlotPtr> inserts) {
    StripInfo* s = findStrip(id);
    if (s == nullptr) return false;
    s->inserts = std::move(inserts);
    structuralChange();
    return true;
}

bool MixerModel::addInsert(StripId id, InsertSlotPtr slot, int index) {
    StripInfo* s = findStrip(id);
    if (s == nullptr || !slot || !slot->processor) return false;
    if (index < 0 || index > static_cast<int>(s->inserts.size())) s->inserts.push_back(std::move(slot));
    else s->inserts.insert(s->inserts.begin() + index, std::move(slot));
    structuralChange();
    return true;
}

bool MixerModel::removeInsert(StripId id, int index) {
    StripInfo* s = findStrip(id);
    if (s == nullptr || index < 0 || index >= static_cast<int>(s->inserts.size())) return false;
    s->inserts.erase(s->inserts.begin() + index);
    structuralChange();
    return true;
}

bool MixerModel::moveInsert(StripId id, int from, int to) {
    StripInfo* s = findStrip(id);
    if (s == nullptr) return false;
    const int count = static_cast<int>(s->inserts.size());
    if (from < 0 || from >= count) return false;
    to = std::clamp(to, 0, count - 1);
    InsertSlotPtr slot = std::move(s->inserts[static_cast<std::size_t>(from)]);
    s->inserts.erase(s->inserts.begin() + from);
    s->inserts.insert(s->inserts.begin() + to, std::move(slot));
    structuralChange();
    return true;
}

bool MixerModel::replaceInsert(const InsertSlot* existing, InsertSlotPtr replacement) {
    for (auto* list : {&inputs_, &buses_}) {
        for (StripInfo& s : *list) {
            for (InsertSlotPtr& slot : s.inserts) {
                if (slot.get() == existing) {
                    slot = std::move(replacement);
                    structuralChange();
                    return true;
                }
            }
        }
    }
    return false;
}

void MixerModel::latencyChanged() {
    dirty_ = true;
    if (batchDepth_ == 0) commit();
}

int MixerModel::chainLatency(StripId id) const {
    const StripInfo* s = findStrip(id);
    if (s == nullptr) return 0;
    int total = 0;
    for (const InsertSlotPtr& slot : s->inserts) total += slot->processor->latencySamples();
    return total;
}

// ---- ports -------------------------------------------------------------------------------------------

PortId MixerModel::registerPort(PortInfo info, std::shared_ptr<IEnginePort> port) {
    if (info.id == kInvalidPort) info.id = nextPortId_++;
    info.numInputs = port ? port->numInputChannels() : 0;
    info.numOutputs = port ? port->numOutputChannels() : 0;
    const PortId id = info.id;
    const PortKey key = info.key;  // C++17 sequences the right operand of '=' first: never index with a moved-from key
    ports_[key] = PortEntry{std::move(info), std::move(port)};
    structuralChange();
    return id;
}

bool MixerModel::unregisterPort(const PortKey& key) {
    if (ports_.erase(key) == 0) return false;
    structuralChange();
    return true;
}

std::vector<PortInfo> MixerModel::ports() const {
    std::vector<PortInfo> out;
    out.reserve(ports_.size());
    for (const auto& [key, entry] : ports_) out.push_back(entry.info);
    return out;
}

std::optional<PortInfo> MixerModel::findPort(const PortKey& key) const {
    auto it = ports_.find(key);
    if (it == ports_.end()) return std::nullopt;
    return it->second.info;
}

// ---- graph -------------------------------------------------------------------------------------------

void MixerModel::structuralChange(bool visibleToUi) {
    if (visibleToUi) ++topologyRevision_;
    else ++routingRevision_;
    dirty_ = true;
    if (batchDepth_ == 0) commit();
}

void MixerModel::commit() {
    if (!dirty_) return;
    dirty_ = false;
    rebuildGraph();
}

std::shared_ptr<DelayLine> MixerModel::delayLineFor(std::map<const void*, std::shared_ptr<DelayLine>>& pool,
                                                    const void* key, int delay) {
    auto& line = pool[key];
    if (!line || line->capacity() < delay) {
        // Grow with headroom; the previous line stays alive inside the old graph until it is retired.
        line = std::make_shared<DelayLine>(std::max(delay * 2, 256));
    }
    return line;
}

void MixerModel::rebuildGraph() {
    auto graph = std::make_unique<RenderGraph>();
    graph->sampleRate = sampleRate_;
    graph->maxBlockSize = maxBlockSize_;

    struct ResolvedPort {
        IEnginePort* port;
        int inputs;
        int outputs;
    };
    std::unordered_map<PortKey, ResolvedPort> resolved;
    for (const auto& [key, entry] : ports_) {
        if (!entry.port) continue;
        graph->ports.push_back(entry.port.get());
        graph->keepAlive.push_back(entry.port);
        resolved[key] = {entry.port.get(), entry.port->numInputChannels(), entry.port->numOutputChannels()};
    }

    std::set<const void*> usedBypass, usedPdc;

    auto addStrip = [&](StripInfo& s, std::vector<GraphStrip>& out) {
        GraphStrip g;
        g.id = s.id;
        g.kind = s.kind;
        g.params = s.params.get();
        g.proc = s.processor.get();
        graph->keepAlive.push_back(s.params);
        graph->keepAlive.push_back(s.processor);

        if (auto it = resolved.find(s.binding.port); it != resolved.end()) {
            const int channels = s.kind == StripKind::Input ? it->second.inputs : it->second.outputs;
            const bool leftOk = s.binding.left >= 0 && s.binding.left < channels;
            const bool rightOk = s.binding.right == -1 || (s.binding.right >= 0 && s.binding.right < channels);
            if (leftOk && rightOk) {
                g.port = it->second.port;
                g.left = s.binding.left;
                g.right = s.binding.right;
            }
        }

        g.firstInsert = static_cast<int>(graph->inserts.size());
        for (const InsertSlotPtr& slot : s.inserts) {
            if (!slot || !slot->processor) continue;
            GraphInsert gi;
            gi.slot = slot.get();
            gi.processor = slot->processor.get();
            gi.latency = std::max(0, slot->processor->latencySamples());
            if (gi.latency > 0) {
                auto line = delayLineFor(bypassDelays_, slot.get(), gi.latency);
                gi.bypassDelay = line.get();
                graph->keepAlive.push_back(line);
                usedBypass.insert(slot.get());
            }
            graph->keepAlive.push_back(slot);
            graph->inserts.push_back(gi);
            g.chainLatency += gi.latency;
        }
        g.numInserts = static_cast<int>(graph->inserts.size()) - g.firstInsert;
        out.push_back(g);
    };

    std::unordered_map<StripId, int> inputIndex, busIndex;
    for (StripInfo& s : inputs_) {
        inputIndex[s.id] = static_cast<int>(graph->inputs.size());
        addStrip(s, graph->inputs);
    }
    for (StripInfo& s : buses_) {
        busIndex[s.id] = static_cast<int>(graph->buses.size());
        addStrip(s, graph->buses);
    }

    // Plugin delay compensation: every enabled route into a bus is delayed up to the slowest one.
    std::vector<int> busMaxLatency(graph->buses.size(), 0);
    for (const auto& [key, entry] : routes_) {
        auto in = inputIndex.find(key.first);
        auto bus = busIndex.find(key.second);
        if (in == inputIndex.end() || bus == busIndex.end() || !entry.params->enabled()) continue;
        auto& maxLatency = busMaxLatency[static_cast<std::size_t>(bus->second)];
        maxLatency = std::max(maxLatency, graph->inputs[static_cast<std::size_t>(in->second)].chainLatency);
    }
    for (const auto& [key, entry] : routes_) {
        auto in = inputIndex.find(key.first);
        auto bus = busIndex.find(key.second);
        if (in == inputIndex.end() || bus == busIndex.end()) continue;
        GraphCrosspoint x;
        x.input = in->second;
        x.bus = bus->second;
        x.params = entry.params.get();
        x.proc = entry.processor.get();
        x.pdcDelay = std::max(0, busMaxLatency[static_cast<std::size_t>(x.bus)] -
                                     graph->inputs[static_cast<std::size_t>(x.input)].chainLatency);
        if (x.pdcDelay > 0) {
            auto line = delayLineFor(pdcDelays_, entry.params.get(), x.pdcDelay);
            x.pdc = line.get();
            graph->keepAlive.push_back(line);
            usedPdc.insert(entry.params.get());
        }
        graph->keepAlive.push_back(entry.params);
        graph->keepAlive.push_back(entry.processor);
        graph->crosspoints.push_back(x);
    }

    // Forget delay lines nobody references any more (graphs still holding them keep them alive).
    std::erase_if(bypassDelays_, [&](const auto& item) { return usedBypass.count(item.first) == 0; });
    std::erase_if(pdcDelays_, [&](const auto& item) { return usedPdc.count(item.first) == 0; });

    engine_.publish(std::move(graph));
}

void MixerModel::housekeeping() {
    engine_.collectGarbage();
    for (auto* list : {&inputs_, &buses_})
        for (StripInfo& s : *list) integrators_[s.id].drain(s.processor->loudness);
}

float MixerModel::integratedLufs(StripId id) const {
    auto it = integrators_.find(id);
    return it != integrators_.end() ? it->second.integratedLufs() : kLoudnessSilence;
}

void MixerModel::resetIntegratedLoudness(StripId id) {
    auto it = integrators_.find(id);
    if (it != integrators_.end()) it->second.reset();
}

}  // namespace reka
