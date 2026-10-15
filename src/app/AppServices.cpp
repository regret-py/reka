#include "AppServices.h"

#include "ProfileService.h"

namespace reka::app {

AppServices::AppServices() = default;

AppServices::~AppServices() { shutdown(); }

void AppServices::initialise(const juce::String& commandLine) {
    settings_.load();
    devices_ = std::make_unique<RekaDeviceManager>(mixer_);
    devices_->addListener(this);
    virtualIo_ = std::make_unique<VirtualIoManager>(virtualAudio_, mixer_, *devices_);
    plugins_ = std::make_unique<RekaVstHost>(AppPaths::pluginListFile(), AppPaths::pluginHostExecutable());
    plugins_->setSandboxByDefault(settings_.sandboxPluginsByDefault);
    if (settings_.pluginSearchPath.isNotEmpty()) plugins_->setSearchPath(juce::FileSearchPath(settings_.pluginSearchPath));
    midi_ = std::make_unique<MidiControllerBridge>(mixer_);
    profiles_ = std::make_unique<ProfileService>(*this);

    // Profile precedence: command line > last used > built-in default.
    juce::File profile;
    for (const auto& token : juce::StringArray::fromTokens(commandLine, true)) {
        const auto candidate = juce::File::getCurrentWorkingDirectory().getChildFile(token.unquoted());
        if (candidate.hasFileExtension(".reka") && candidate.existsAsFile()) profile = candidate;
    }
    if (profile == juce::File() && settings_.lastProfile.isNotEmpty()) profile = juce::File(settings_.lastProfile);

    juce::String error;
    if (profile.existsAsFile() && profiles_->load(profile, error)) {
        log("Loaded profile " + profile.getFileNameWithoutExtension());
    } else {
        if (error.isNotEmpty()) log("Could not load " + profile.getFileName() + ": " + error);
        profiles_->loadDefault();
    }
    if (plugins_->knownPlugins().getNumTypes() == 0) plugins_->startScan(false);
    startTimerHz(20);
}

void AppServices::shutdown() {
    if (shutDown_) return;
    shutDown_ = true;
    stopTimer();
    if (profiles_ != nullptr) profiles_->writeAutosave();
    settings_.save();

    // Order matters: stop every thread that touches the engine, then drop the graphs (and with them the
    // last references to plugins) while the VST host still exists.
    hardware_.stop();
    if (midi_ != nullptr) midi_->closeAll();
    profiles_.reset();
    if (devices_ != nullptr) devices_->removeListener(this);
    virtualIo_.reset();
    if (devices_ != nullptr) devices_->shutdown();
    if (plugins_ != nullptr) {
        plugins_->cancelScan();
        plugins_->closeAllEditors();
    }
    mixer_.clear();
    engine_.releaseGraphs();
    midi_.reset();
    plugins_.reset();
    devices_.reset();
}

void AppServices::log(const juce::String& message) {
    log_.add(juce::Time::getCurrentTime().toString(false, true, true, true) + "  " + message);
    while (log_.size() > 200) log_.remove(0);
    sendChangeMessage();
}

// ---- hardware -------------------------------------------------------------------------------------------

void AppServices::applyHardwareSettings(const HardwareSettings& settings) {
    hardwareSettings_ = settings;
    refreshHardwareBindings();
    hardware_.start(hardwareSettings_);
}

void AppServices::refreshHardwareBindings() {
    auto bindings = std::make_shared<HardwareBindings>();
    bindings->profileName = profiles_ != nullptr ? profiles_->currentName().toStdString() : std::string("REKA");

    std::vector<StripId> order = hardwareSettings_.map;
    if (order.empty()) {
        // Default surface layout: inputs first, then buses, in on-screen order.
        for (StripKind kind : {StripKind::Input, StripKind::Bus})
            for (StripId id : mixer_.strips(kind)) order.push_back(id);
    }
    for (StripId id : order) {
        HardwareChannelBinding binding;
        if (const StripInfo* info = mixer_.strip(id)) {
            binding.strip = id;
            binding.name = info->name;
            binding.params = info->params;
            binding.processor = info->processor;
        }
        bindings->channels.push_back(std::move(binding));
        if (bindings->channels.size() >= 16) break;
    }
    hardware_.setBindings(std::move(bindings));
    if (midi_ != nullptr) midi_->refreshBindings();
}

// ---- plugins --------------------------------------------------------------------------------------------

juce::String AppServices::addPlugin(StripId strip, const juce::PluginDescription& description, bool sandboxed, int index) {
    std::string error;
    auto insert = plugins_->createInsert(plugins_->describe(description, sandboxed), mixer_.sampleRate(), mixer_.maxBlockSize(), error);
    if (insert == nullptr) {
        log("Could not load " + description.name + ": " + juce::String(error));
        return juce::String(error);
    }
    mixer_.addInsert(strip, std::make_shared<InsertSlot>(std::move(insert)), index);
    log("Loaded " + description.name + (sandboxed ? " (sandboxed)" : ""));
    return {};
}

void AppServices::restartFailedInserts() {
    const auto now = juce::Time::getMillisecondCounter();
    for (StripKind kind : {StripKind::Input, StripKind::Bus}) {
        for (StripId id : mixer_.strips(kind)) {
            const StripInfo* info = mixer_.strip(id);
            for (const InsertSlotPtr& slot : info->inserts) {
                if (!slot->processor->hasFailed()) continue;
                const InsertDescriptor descriptor = slot->processor->descriptor();
                const std::string key = std::to_string(id) + "/" + descriptor.pluginId;
                auto& [count, since] = restartHistory_[key];
                if (now - since > 60000) {
                    count = 0;
                    since = now;
                }
                if (count >= 3) continue;  // crash loop: leave it bypassed, the UI shows it as failed
                ++count;

                const std::vector<std::uint8_t> state = slot->processor->saveState();  // last known good
                std::string error;
                auto replacement = plugins_->createInsert(descriptor, mixer_.sampleRate(), mixer_.maxBlockSize(), error);
                if (replacement == nullptr) {
                    log(juce::String(descriptor.name) + " crashed and could not be restarted: " + juce::String(error));
                    continue;
                }
                replacement->restoreState(state);
                auto newSlot = std::make_shared<InsertSlot>(std::move(replacement));
                newSlot->bypassed.store(slot->bypassed.load());
                log(juce::String(descriptor.name) + " crashed - restarted from its last saved state");
                mixer_.replaceInsert(slot.get(), std::move(newSlot));
                return;  // the strip's insert list changed: resume next tick
            }
        }
    }
}

// ---- timer ------------------------------------------------------------------------------------------------

void AppServices::timerCallback() {
    mixer_.housekeeping();
    if (plugins_->consumeLatencyChange()) mixer_.latencyChanged();
    restartFailedInserts();
    if (mixer_.topologyRevision() != lastTopology_) {
        lastTopology_ = mixer_.topologyRevision();
        refreshHardwareBindings();
    }
}

void AppServices::engineFormatChanged(double sampleRate, int blockSize) {
    log("Engine running at " + juce::String(sampleRate / 1000.0, 1) + " kHz, " + juce::String(blockSize) + " samples");
}

}  // namespace reka::app
