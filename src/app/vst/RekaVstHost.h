#pragma once

#include "reka/engine/InsertProcessor.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <atomic>
#include <map>
#include <memory>
#include <optional>

namespace reka::app {

class JuceInsertProcessor;
class SandboxedInsertProcessor;

// VST3 Host Module.
//   * Catalog   : juce::KnownPluginList persisted to %APPDATA%/REKA/plugins.xml
//   * Scanning  : background thread; every plugin file is probed in a separate REKA-PluginHost.exe so a
//                 crashing plugin is blacklisted instead of taking REKA down
//   * Hosting   : IInsertFactory - in-process (JuceInsertProcessor) or sandboxed
//                 (SandboxedInsertProcessor, one REKA-PluginHost.exe per instance)
//   * PDC       : plugins report latency changes asynchronously; consumeLatencyChange() lets the
//                 message-thread timer ask the MixerModel to recompute compensation
//   * Editors   : one floating window per plugin (sandboxed editors live in the sandbox process)
class RekaVstHost final : public IInsertFactory, public juce::ChangeBroadcaster, private juce::AsyncUpdater {
public:
    RekaVstHost(juce::File pluginListFile, juce::File hostExecutable);
    ~RekaVstHost() override;

    juce::AudioPluginFormatManager& formats() noexcept { return formats_; }
    juce::KnownPluginList& knownPlugins() noexcept { return knownPlugins_; }
    juce::Array<juce::PluginDescription> effectPlugins() const;  // excludes instruments
    std::optional<juce::PluginDescription> findDescription(const juce::String& identifier) const;

    // ---- scanning ----
    juce::FileSearchPath searchPath() const;
    void setSearchPath(const juce::FileSearchPath& path);
    void startScan(bool rescanEverything);
    void cancelScan();
    bool isScanning() const noexcept { return scanning_.load(); }
    float scanProgress() const noexcept { return progress_.load(); }
    juce::String currentlyScanning() const;
    void clearBlacklist();

    // ---- hosting ----
    bool sandboxByDefault() const noexcept { return sandboxByDefault_; }
    void setSandboxByDefault(bool enabled) noexcept { sandboxByDefault_ = enabled; }
    InsertDescriptor describe(const juce::PluginDescription& description, bool sandboxed) const;
    std::shared_ptr<IInsertProcessor> createInsert(const InsertDescriptor& descriptor, double sampleRate, int maxBlockSize,
                                                   std::string& error) override;
    bool consumeLatencyChange() noexcept { return latencyChanged_.exchange(false); }

    // ---- editors ----
    void showEditor(IInsertProcessor& insert);
    void closeEditor(IInsertProcessor& insert);
    void closeAllEditors();

private:
    class ScanThread;
    class OutOfProcessScanner;
    class PluginWindow;

    void handleAsyncUpdate() override;
    void savePluginList();
    void scanFinished();

    juce::File pluginListFile_;
    juce::File hostExecutable_;
    juce::AudioPluginFormatManager formats_;
    juce::KnownPluginList knownPlugins_;
    juce::FileSearchPath searchPath_;
    bool sandboxByDefault_ = true;

    std::unique_ptr<ScanThread> scanThread_;
    std::atomic<bool> scanning_{false};
    std::atomic<float> progress_{0.0f};
    mutable juce::CriticalSection scanNameLock_;
    juce::String scanName_;

    std::atomic<bool> latencyChanged_{false};
    std::map<const IInsertProcessor*, std::unique_ptr<PluginWindow>> windows_;
};

}  // namespace reka::app
