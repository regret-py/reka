#pragma once

#include "reka/common/Types.h"
#include "reka/dsp/DelayLine.h"
#include "reka/engine/InsertProcessor.h"
#include "reka/engine/Parameters.h"
#include "reka/engine/StripProcessor.h"
#include "reka/io/EnginePort.h"

#include <memory>
#include <vector>

namespace reka {

// An immutable, flattened execution plan built by MixerModel on the message thread and handed to the
// audio thread through RekaAudioEngine::publish(). The audio thread only walks plain arrays of raw
// pointers; `keepAlive` owns everything those pointers reference until the graph is retired and
// deleted back on the message thread.

struct GraphInsert {
    InsertSlot* slot = nullptr;
    IInsertProcessor* processor = nullptr;
    DelayLine* bypassDelay = nullptr;  // keeps timing identical when the insert is bypassed
    int latency = 0;
    bool wasBypassed = false;          // audio-thread scratch state
};

struct GraphStrip {
    StripId id = kInvalidStrip;
    StripKind kind = StripKind::Input;
    StripParameters* params = nullptr;
    StripProcessor* proc = nullptr;
    IEnginePort* port = nullptr;  // source for inputs, sink for buses; null = unassigned
    int left = 0;
    int right = 1;                // -1 = mono endpoint
    int firstInsert = 0;
    int numInserts = 0;
    int chainLatency = 0;
};

struct GraphCrosspoint {
    int input = 0;  // index into RenderGraph::inputs
    int bus = 0;    // index into RenderGraph::buses
    CrosspointParameters* params = nullptr;
    CrosspointProcessor* proc = nullptr;
    DelayLine* pdc = nullptr;  // plugin delay compensation
    int pdcDelay = 0;
};

struct RenderGraph {
    double sampleRate = 48000.0;
    int maxBlockSize = 512;
    std::vector<IEnginePort*> ports;
    std::vector<GraphStrip> inputs;
    std::vector<GraphStrip> buses;
    std::vector<GraphInsert> inserts;
    std::vector<GraphCrosspoint> crosspoints;
    std::vector<std::shared_ptr<const void>> keepAlive;
};

}  // namespace reka
