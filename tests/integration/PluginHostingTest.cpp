// End-to-end test of REKA's plugin hosting: out-of-process scan, in-process insert, sandboxed insert
// (shared-memory audio + IPC), state round trip, latency reporting and crash containment.
//   PluginHostingTest <folder containing the test .vst3> <REKA-PluginHost executable>
#include "reka/engine/InsertProcessor.h"
#include "vst/RekaVstHost.h"
#include "vst/SandboxedInsertProcessor.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <cmath>
#include <iostream>

namespace {
int failures = 0;
void check(bool condition, const juce::String& what) {
    std::cout << (condition ? "  ok    " : "  FAIL  ") << what << std::endl;
    if (!condition) ++failures;
}

void pump(int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil(ms); }

// Feeds an impulse and returns (gain, delay) observed at the output. For sandboxed inserts a block that
// misses its deadline is passed through dry by design (busy CI machines); such measurements are retried.
std::pair<float, int> measureOnce(reka::IInsertProcessor& insert, int blocks) {
    std::vector<float> l(256), r(256);
    float* ch[2] = {l.data(), r.data()};
    float peak = 0.0f;
    int at = -1;
    for (int b = 0; b < blocks; ++b) {
        std::fill(l.begin(), l.end(), 0.0f);
        std::fill(r.begin(), r.end(), 0.0f);
        if (b == 1) l[10] = r[10] = 1.0f;  // block 0 warms the path up
        insert.process(ch, 2, 256);
        for (int i = 0; i < 256; ++i)
            if (std::abs(l[static_cast<size_t>(i)]) > peak) {
                peak = std::abs(l[static_cast<size_t>(i)]);
                at = b * 256 + i;
            }
    }
    return {peak, at - 256 - 10};
}

std::pair<float, int> measure(reka::IInsertProcessor& insert, int blocks = 5) {
    auto* sandbox = dynamic_cast<reka::app::SandboxedInsertProcessor*>(&insert);
    std::pair<float, int> result;
    for (int attempt = 0; attempt < 10; ++attempt) {
        const auto missedBefore = sandbox != nullptr ? sandbox->missedBlocks() : 0u;
        result = measureOnce(insert, blocks);
        if (sandbox == nullptr || sandbox->missedBlocks() == missedBefore) return result;
        juce::Thread::sleep(20);
    }
    return result;
}

// Builds a state blob in the *host's* format (JUCE's VST3 chunk) by configuring a scratch instance.
std::vector<uint8_t> makeState(reka::app::RekaVstHost& host, const juce::PluginDescription& description, float gain, bool crash) {
    juce::String error;
    auto instance = host.formats().createPluginInstance(description, 48000.0, 256, error);
    if (instance == nullptr) return {};
    for (auto* parameter : instance->getParameters()) {
        if (parameter->getName(32) == "Gain") parameter->setValueNotifyingHost(gain);
        if (parameter->getName(32) == "Crash") parameter->setValueNotifyingHost(crash ? 1.0f : 0.0f);
    }
    juce::MemoryBlock block;
    instance->getStateInformation(block);
    const auto* bytes = static_cast<const uint8_t*>(block.getData());
    return {bytes, bytes + block.getSize()};
}
}  // namespace

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "usage: PluginHostingTest <vst3 folder> <REKA-PluginHost>" << std::endl;
        return 2;
    }
    juce::ScopedJuceInitialiser_GUI gui;
    const juce::File listFile = juce::File::createTempFile(".xml");
    reka::app::RekaVstHost host(listFile, juce::File(juce::String(argv[2])));
    host.setSearchPath(juce::FileSearchPath(juce::String(argv[1])));

    std::cout << "scan (out of process)" << std::endl;
    host.startScan(true);
    for (int i = 0; i < 600 && host.isScanning(); ++i) pump(100);
    pump(200);
    const auto effects = host.effectPlugins();
    check(!host.isScanning(), "scan finished");
    check(effects.size() == 1 && effects[0].name == "REKA Test Gain", "test plugin found by the out-of-process scanner");
    if (effects.isEmpty()) return 1;

    for (bool sandboxed : {false, true}) {
        std::cout << (sandboxed ? "sandboxed insert" : "in-process insert") << std::endl;
        std::string error;
        auto insert = host.createInsert(host.describe(effects[0], sandboxed), 48000.0, 256, error);
        check(insert != nullptr, "insert created " + juce::String(error));
        if (insert == nullptr) continue;
        check(insert->latencySamples() == 64, "reports 64 samples latency (got " + juce::String(insert->latencySamples()) + ")");
        auto [gain, delay] = measure(*insert);
        check(std::abs(gain - 0.5f) < 1e-4f, "default gain 0.5 (got " + juce::String(gain) + ")");
        check(delay == 64, "audio delayed by its reported latency (got " + juce::String(delay) + ")");

        const auto state = insert->saveState();
        check(!state.empty(), "state saved (" + juce::String(static_cast<int>(state.size())) + " bytes)");
        check(insert->restoreState(makeState(host, effects[0], 0.25f, false)), "state restored");
        std::tie(gain, delay) = measure(*insert);
        check(std::abs(gain - 0.25f) < 1e-4f, "restored state changes processing (gain " + juce::String(gain) + ")");
        check(!insert->hasFailed(), "insert healthy");

        if (sandboxed) {
            auto* sandbox = dynamic_cast<reka::app::SandboxedInsertProcessor*>(insert.get());
            check(sandbox != nullptr, "insert is sandboxed");
            if (sandbox != nullptr)
                std::cout << "  info  blocks that missed the sandbox deadline (passed dry): " << sandbox->missedBlocks() << std::endl;
            std::cout << "crash containment" << std::endl;
            const auto healthyState = insert->saveState();
            insert->restoreState(makeState(host, effects[0], 0.25f, true));
            std::vector<float> l(256, 0.3f), r(256, 0.3f);
            float* ch[2] = {l.data(), r.data()};
            for (int i = 0; i < 200 && !insert->hasFailed(); ++i) {
                insert->process(ch, 2, 256);  // the plugin aborts inside the sandbox process
                pump(10);
            }
            check(insert->hasFailed(), "sandbox death detected: insert marked failed (engine bypasses it)");
            check(!insert->saveState().empty(), "last known state still available for the watchdog restart");

            // What the AppServices watchdog does: relaunch from the last healthy state.
            std::string relaunchError;
            auto replacement = host.createInsert(host.describe(effects[0], true), 48000.0, 256, relaunchError);
            check(replacement != nullptr && replacement->restoreState(healthyState), "relaunched sandbox restored");
            if (replacement != nullptr) {
                std::tie(gain, delay) = measure(*replacement);
                check(std::abs(gain - 0.25f) < 1e-4f && !replacement->hasFailed(), "relaunched plugin processes again (gain " + juce::String(gain) + ")");
            }
        }
    }
    listFile.deleteFile();
    std::cout << (failures == 0 ? "ALL PASSED" : juce::String(failures) + " FAILURE(S)") << std::endl;
    return failures == 0 ? 0 : 1;
}
