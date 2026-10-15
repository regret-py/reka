#pragma once

#include <juce_core/juce_core.h>

// Control protocol between REKA.exe (coordinator) and REKA-PluginHost.exe (worker).
//
// Transport : juce::ChildProcessCoordinator / ChildProcessWorker (named pipe, with ping-based liveness)
// Messages  : UTF-8 JSON objects. Requests carry "id" and "cmd"; replies echo "id" and carry "ok"
//             (+ "error" on failure). Unsolicited worker notifications have no "id".
// Audio     : never on this channel - see reka::SharedAudioChannel (shared memory + kernel events).
//
//   load       {desc: <PluginDescription XML>, sampleRate, blockSize, shm: <channel name>}
//              -> {ok, latency, name, inputs, outputs}
//   prepare    {sampleRate, blockSize}                  -> {ok, latency}
//   getState   {}                                       -> {ok, state: <base64>}
//   setState   {state: <base64>}                        -> {ok}
//   showEditor {}  / hideEditor {}                      -> {ok}
//   notify     {event: "latency", latency}              (worker -> coordinator)
//   notify     {event: "editorClosed"}                  (worker -> coordinator)
namespace reka::sandbox {

inline constexpr const char* kCommandLineUid = "reka-plugin-sandbox-v1";
inline constexpr const char* kScanArgument = "--scan";
inline constexpr int kLaunchTimeoutMs = 15000;
inline constexpr int kPingTimeoutMs = 10000;

inline juce::MemoryBlock encode(const juce::var& message) {
    const juce::String text = juce::JSON::toString(message, true);
    return {text.toRawUTF8(), text.getNumBytesAsUTF8()};
}

inline juce::var decode(const juce::MemoryBlock& block) { return juce::JSON::parse(block.toString()); }

inline juce::var makeMessage(std::initializer_list<std::pair<juce::Identifier, juce::var>> fields) {
    auto* object = new juce::DynamicObject();
    for (const auto& [key, value] : fields) object->setProperty(key, value);
    return juce::var(object);
}

}  // namespace reka::sandbox
