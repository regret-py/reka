#include "reka/engine/RekaAudioEngine.h"

#include "reka/common/Denormals.h"
#include "reka/dsp/Decibels.h"
#include "reka/dsp/Smoothing.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace reka {

namespace {
inline void copyChannel(float* dest, const float* source, int numFrames) noexcept {
    std::memcpy(dest, source, static_cast<std::size_t>(numFrames) * sizeof(float));
}
inline void clearChannel(float* dest, int numFrames) noexcept {
    std::memset(dest, 0, static_cast<std::size_t>(numFrames) * sizeof(float));
}
}  // namespace

RekaAudioEngine::RekaAudioEngine() { prepare(48000.0, 512); }

RekaAudioEngine::~RekaAudioEngine() {
    // Audio must already be stopped (the owner stops the device first).
    collectGarbage();
    delete pending_.exchange(nullptr);
    delete current_;
}

void RekaAudioEngine::prepare(double sampleRate, int maxBlockSize) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    maxBlockSize_ = std::max(16, maxBlockSize);
    const auto frames = static_cast<std::size_t>(maxBlockSize_);
    scratchStorage_.assign(frames * kStripChannels, 0.0f);
    for (int ch = 0; ch < kStripChannels; ++ch)
        scratch_[static_cast<std::size_t>(ch)] = scratchStorage_.data() + frames * static_cast<std::size_t>(ch);
}

void RekaAudioEngine::publish(std::unique_ptr<RenderGraph> graph) {
    RenderGraph* previous = pending_.exchange(graph.release(), std::memory_order_acq_rel);
    // A pending graph the audio thread never picked up was never touched by it: delete it here.
    delete previous;
}

void RekaAudioEngine::collectGarbage() {
    RenderGraph* graph = nullptr;
    while (retired_.pop(graph)) delete graph;
}

void RekaAudioEngine::releaseGraphs() {
    collectGarbage();
    delete pending_.exchange(nullptr);
    delete current_;
    current_ = nullptr;
}

RenderGraph* RekaAudioEngine::acquireGraph() noexcept {
    if (pending_.load(std::memory_order_relaxed) != nullptr && retired_.hasSpace()) {
        if (RenderGraph* next = pending_.exchange(nullptr, std::memory_order_acq_rel)) {
            if (current_ != nullptr) retired_.push(current_);  // cannot fail: hasSpace() checked
            current_ = next;
            swaps_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    return current_;
}

void RekaAudioEngine::processBlock(int numFrames) noexcept {
    ScopedFlushDenormals noDenormals;
    const auto start = std::chrono::steady_clock::now();

    RenderGraph* graph = acquireGraph();
    if (graph == nullptr || numFrames <= 0) return;

    for (int offset = 0; offset < numFrames;) {
        const int chunk = std::min(numFrames - offset, maxBlockSize_);
        processChunk(*graph, chunk);
        offset += chunk;
    }

    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const double budget = numFrames / sampleRate_;
    const float instantaneous = static_cast<float>(elapsed / budget);
    const float previous = cpuLoad_.load(std::memory_order_relaxed);
    // Fast attack, slow release: overload spikes stay visible on the UI.
    cpuLoad_.store(instantaneous > previous ? instantaneous : previous + 0.05f * (instantaneous - previous),
                   std::memory_order_relaxed);
    blocks_.fetch_add(1, std::memory_order_relaxed);
}

void RekaAudioEngine::processChunk(RenderGraph& graph, int numFrames) noexcept {
    for (IEnginePort* port : graph.ports) port->beginBlock(numFrames);

    const PanLaw law = panLaw_.load(std::memory_order_relaxed);

    bool anySoloInput = false;
    for (const GraphStrip& s : graph.inputs) anySoloInput = anySoloInput || s.params->solo();
    for (GraphStrip& s : graph.inputs) processStrip(graph, s, numFrames, anySoloInput, law);

    for (GraphStrip& bus : graph.buses)
        for (int ch = 0; ch < kStripChannels; ++ch) clearChannel(bus.proc->pre()[ch], numFrames);
    for (GraphCrosspoint& x : graph.crosspoints) processCrosspoint(graph, x, numFrames);

    bool anySoloBus = false;
    for (const GraphStrip& b : graph.buses) anySoloBus = anySoloBus || b.params->solo();
    for (GraphStrip& b : graph.buses) processStrip(graph, b, numFrames, anySoloBus, law);

    for (IEnginePort* port : graph.ports) port->endBlock(numFrames);
}

void RekaAudioEngine::processStrip(RenderGraph& graph, GraphStrip& s, int numFrames, bool anySolo,
                                   PanLaw law) noexcept {
    StripProcessor& x = *s.proc;
    const StripParameters& p = *s.params;
    float* const* pre = x.pre();
    float* L = pre[0];
    float* R = pre[1];

    // 1. Source (inputs only; buses arrive here with their crosspoint sum already in `pre`).
    bool monoSource = false;
    if (s.kind == StripKind::Input) {
        if (s.port != nullptr) {
            copyChannel(L, s.port->inputChannel(s.left), numFrames);
            if (s.right >= 0) {
                copyChannel(R, s.port->inputChannel(s.right), numFrames);
            } else {
                copyChannel(R, L, numFrames);
                monoSource = true;
            }
        } else {
            clearChannel(L, numFrames);
            clearChannel(R, numFrames);
        }
    }

    // 2. Polarity (ramped through zero, so flipping it does not click).
    applyGain(L, numFrames, x.phase[0].advance(p.phaseInvertLeft() ? -1.0f : 1.0f, numFrames));
    applyGain(R, numFrames, x.phase[1].advance(p.phaseInvertRight() ? -1.0f : 1.0f, numFrames));

    // 3. Stereo / mono summing matrix.
    const ChannelMode mode = p.channelMode();
    switch (mode) {
        case ChannelMode::Stereo: break;
        case ChannelMode::Mono:
            for (int i = 0; i < numFrames; ++i) L[i] = R[i] = 0.5f * (L[i] + R[i]);
            break;
        case ChannelMode::SwapLR:
            for (int i = 0; i < numFrames; ++i) std::swap(L[i], R[i]);
            break;
        case ChannelMode::LeftOnly: copyChannel(R, L, numFrames); break;
        case ChannelMode::RightOnly: copyChannel(L, R, numFrames); break;
    }

    // 4. 8-band EQ, 5. insert rack.
    if (x.eq.update(p.eq())) x.eq.process(pre, kStripChannels, numFrames);
    runInserts(graph, s, pre, numFrames, p.fxBypass());

    // 6. Fader, pan/balance, mute and solo-in-place, all through the same de-click ramps.
    const bool muted = p.mute() || (anySolo && !p.solo());
    const float gain = muted ? 0.0f : dbToGain(p.gainDb());
    const StereoGains pan = (monoSource || isMonoMode(mode)) ? panMono(p.pan(), law) : balance(p.pan());
    float* const* post = x.post();
    copyWithGain(post[0], L, numFrames, x.gain[0].advance(gain * pan.left, numFrames));
    copyWithGain(post[1], R, numFrames, x.gain[1].advance(gain * pan.right, numFrames));

    // 7. Metering.
    x.meter.process(post, kStripChannels, numFrames);
    x.loudness.process(post, kStripChannels, numFrames);

    // 8. Buses render into their sink port (accumulating: several buses may share a device).
    if (s.kind == StripKind::Bus && s.port != nullptr) {
        float* outL = s.port->outputChannel(s.left);
        if (s.right >= 0) {
            float* outR = s.port->outputChannel(s.right);
            for (int i = 0; i < numFrames; ++i) {
                outL[i] += post[0][i];
                outR[i] += post[1][i];
            }
        } else {
            for (int i = 0; i < numFrames; ++i) outL[i] += 0.5f * (post[0][i] + post[1][i]);
        }
    }
}

void RekaAudioEngine::processCrosspoint(RenderGraph& graph, GraphCrosspoint& x, int numFrames) noexcept {
    CrosspointProcessor& c = *x.proc;
    const CrosspointParameters& p = *x.params;

    float target = p.enabled() ? dbToGain(p.gainDb()) : 0.0f;
    if (p.phaseInvert()) target = -target;
    const StereoGains bal = balance(p.pan());
    const GainSpan spanL = c.gain[0].advance(target * bal.left, numFrames);
    const GainSpan spanR = c.gain[1].advance(target * bal.right, numFrames);
    if (spanL.isSilent() && spanR.isSilent()) {
        c.wasSilent = true;  // disabled routes cost nothing
        return;
    }

    GraphStrip& input = graph.inputs[static_cast<std::size_t>(x.input)];
    GraphStrip& bus = graph.buses[static_cast<std::size_t>(x.bus)];
    float* const* source = p.preFader() ? input.proc->pre() : input.proc->post();

    const bool eqActive = c.eq.update(p.eq());
    const bool needsWork = eqActive || x.pdcDelay > 0;
    float* const* work = source;
    if (needsWork) {
        for (int ch = 0; ch < kStripChannels; ++ch) copyChannel(scratch_[static_cast<std::size_t>(ch)], source[ch], numFrames);
        work = scratch_.data();
        if (c.wasSilent) {  // resuming: discard stale filter/delay history
            c.eq.reset();
            if (x.pdc != nullptr) x.pdc->clear();
        }
        if (eqActive) c.eq.process(work, kStripChannels, numFrames);
        if (x.pdc != nullptr && x.pdcDelay > 0) x.pdc->process(work, kStripChannels, numFrames, x.pdcDelay);
    }
    c.wasSilent = false;

    float* const* sum = bus.proc->pre();
    addWithGain(sum[0], work[0], numFrames, spanL);
    addWithGain(sum[1], work[1], numFrames, spanR);
}

void RekaAudioEngine::runInserts(RenderGraph& graph, const GraphStrip& strip, float* const* channels, int numFrames,
                                 bool bypassAll) noexcept {
    for (int i = 0; i < strip.numInserts; ++i) {
        GraphInsert& insert = graph.inserts[static_cast<std::size_t>(strip.firstInsert + i)];
        const bool bypassed = bypassAll || insert.slot->bypassed.load(std::memory_order_relaxed) ||
                              insert.processor->hasFailed();
        if (!bypassed) {
            insert.processor->process(channels, kStripChannels, numFrames);
        } else if (insert.latency > 0 && insert.bypassDelay != nullptr) {
            // A bypassed latent plugin is replaced by an equal delay so PDC stays valid. The line is
            // cleared on engage so it never replays audio from a previous bypass period.
            if (!insert.wasBypassed) insert.bypassDelay->clear();
            insert.bypassDelay->process(channels, kStripChannels, numFrames, insert.latency);
        }
        insert.wasBypassed = bypassed;
    }
}

}  // namespace reka
