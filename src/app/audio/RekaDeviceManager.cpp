#include "audio/RekaDeviceManager.h"

namespace reka::app {

namespace {
juce::String jstr(const nlohmann::json& json, const char* key) { return juce::String(json.value(key, std::string())); }
}  // namespace

// ---- setups ---------------------------------------------------------------------------------------------

nlohmann::json MasterSetup::toJson() const {
    return {{"type", type.toStdString()},
            {"input", input.toStdString()},
            {"output", output.toStdString()},
            {"sampleRate", sampleRate},
            {"bufferSize", bufferSize}};
}

MasterSetup MasterSetup::fromJson(const nlohmann::json& json) {
    MasterSetup s;
    if (!json.is_object()) return s;
    s.type = jstr(json, "type");
    s.input = jstr(json, "input");
    s.output = jstr(json, "output");
    s.sampleRate = json.value("sampleRate", s.sampleRate);
    s.bufferSize = json.value("bufferSize", s.bufferSize);
    return s;
}

nlohmann::json SecondarySetup::toJson() const {
    return {{"type", type.toStdString()}, {"device", device.toStdString()}, {"input", isInput}, {"bufferSize", bufferSize}};
}

SecondarySetup SecondarySetup::fromJson(const nlohmann::json& json) {
    SecondarySetup s;
    if (!json.is_object()) return s;
    s.type = jstr(json, "type");
    s.device = jstr(json, "device");
    s.isInput = json.value("input", false);
    s.bufferSize = json.value("bufferSize", 0);
    return s;
}

// ---- secondary device --------------------------------------------------------------------------------------

struct RekaDeviceManager::Secondary final : public juce::AudioIODeviceCallback {
    SecondarySetup setup;
    std::unique_ptr<juce::AudioIODevice> device;
    std::shared_ptr<AsyncEndpointPort> port = std::make_shared<AsyncEndpointPort>();
    juce::String error;                    // message thread only
    std::atomic<bool> deviceError{false};  // set by the device thread

    // Device thread: only touches the lock-free side of the port.
    void audioDeviceIOCallbackWithContext(const float* const* inputs, int numInputs, float* const* outputs, int numOutputs,
                                          int numSamples, const juce::AudioIODeviceCallbackContext&) override {
        if (setup.isInput) {
            port->endpointWriteCapture(inputs, numInputs, numSamples);
        } else {
            port->endpointReadRender(outputs, numOutputs, numSamples);
        }
        if (!setup.isInput) return;
        for (int ch = 0; ch < numOutputs; ++ch)
            if (outputs[ch] != nullptr) juce::FloatVectorOperations::clear(outputs[ch], numSamples);
    }
    void audioDeviceAboutToStart(juce::AudioIODevice*) override {}
    void audioDeviceStopped() override {}
    void audioDeviceError(const juce::String&) override { deviceError.store(true); }
};

// ---- lifecycle ------------------------------------------------------------------------------------------

RekaDeviceManager::RekaDeviceManager(MixerModel& mixer) : mixer_(mixer), masterPort_(std::make_shared<DirectDevicePort>()) {
    juce::AudioDeviceManager helper;
    helper.createAudioDeviceTypes(types_);
    for (auto* type : types_) {
        type->scanForDevices();
        type->addListener(this);
    }
    startTimer(2000);
}

RekaDeviceManager::~RekaDeviceManager() { shutdown(); }

void RekaDeviceManager::shutdown() {
    stopTimer();
    stopAll();
    for (auto& secondary : secondaries_) {
        if (secondary->device != nullptr) secondary->device->close();
        mixer_.unregisterPort(secondary->setup.key());
    }
    secondaries_.clear();
    if (masterDevice_ != nullptr) masterDevice_->close();
    masterDevice_.reset();
    for (auto* type : types_) type->removeListener(this);
}

// ---- discovery ------------------------------------------------------------------------------------------

juce::AudioIODeviceType* RekaDeviceManager::findType(const juce::String& name) const {
    for (auto* type : types_)
        if (type->getTypeName() == name) return type;
    return nullptr;
}

juce::StringArray RekaDeviceManager::typeNames() const {
    juce::StringArray names;
    for (auto* type : types_) names.add(type->getTypeName());
    return names;
}

juce::StringArray RekaDeviceManager::deviceNames(const juce::String& type, bool inputs) const {
    if (auto* t = findType(type)) return t->getDeviceNames(inputs);
    return {};
}

juce::Array<double> RekaDeviceManager::sampleRates(const MasterSetup& setup) const {
    if (auto* type = findType(setup.type))
        if (std::unique_ptr<juce::AudioIODevice> device{type->createDevice(setup.output, setup.input)})
            return device->getAvailableSampleRates();
    return {44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0};
}

juce::Array<int> RekaDeviceManager::bufferSizes(const MasterSetup& setup) const {
    if (auto* type = findType(setup.type))
        if (std::unique_ptr<juce::AudioIODevice> device{type->createDevice(setup.output, setup.input)})
            return device->getAvailableBufferSizes();
    return {32, 64, 128, 256, 480, 512, 1024};
}

// ---- master ---------------------------------------------------------------------------------------------

void RekaDeviceManager::stopAll() {
    if (masterDevice_ != nullptr) masterDevice_->stop();
    softwareClock_.stop();
    for (auto& secondary : secondaries_)
        if (secondary->device != nullptr) secondary->device->stop();
}

void RekaDeviceManager::startAll() {
    for (auto& secondary : secondaries_)
        if (secondary->device != nullptr && secondary->device->isOpen()) secondary->device->start(secondary.get());
    if (masterDevice_ != nullptr && masterDevice_->isOpen()) {
        masterDevice_->start(this);
    } else {
        softwareClock_.start(sampleRate_, blockSize_, [this](int numFrames) { mixer_.engine().processBlock(numFrames); });
    }
}

juce::String RekaDeviceManager::setMaster(const MasterSetup& setup) {
    listeners_.call([](Listener& l) { l.engineFormatWillChange(); });
    stopAll();
    if (masterDevice_ != nullptr) masterDevice_->close();
    masterDevice_.reset();
    masterErrored_.store(false);
    masterError_.clear();
    masterSetup_ = setup;

    double rate = setup.sampleRate > 0.0 ? setup.sampleRate : 48000.0;
    int block = setup.bufferSize > 0 ? setup.bufferSize : 256;
    juce::String error;

    if (setup.type.isNotEmpty()) {
        auto* type = findType(setup.type);
        if (type == nullptr) {
            error = "audio driver type '" + setup.type + "' is not available";
        } else {
            type->scanForDevices();
            masterDevice_.reset(type->createDevice(setup.output, setup.input));
            if (masterDevice_ == nullptr) {
                error = "could not create device '" + (setup.output.isNotEmpty() ? setup.output : setup.input) + "'";
            } else {
                juce::BigInteger ins, outs;
                ins.setRange(0, masterDevice_->getInputChannelNames().size(), true);
                outs.setRange(0, masterDevice_->getOutputChannelNames().size(), true);
                error = masterDevice_->open(ins, outs, rate, block);
                if (error.isEmpty()) {
                    rate = masterDevice_->getCurrentSampleRate();
                    block = masterDevice_->getCurrentBufferSizeSamples();
                } else {
                    masterDevice_.reset();
                }
            }
        }
    }

    sampleRate_ = rate;
    blockSize_ = block;
    // Some drivers deliver more frames than announced; the engine splits those, so the announced size
    // is the right preparation size.
    mixer_.prepare(sampleRate_, blockSize_);

    if (masterDevice_ != nullptr) {
        masterPort_->configure(masterDevice_->getActiveInputChannels().countNumberOfSetBits(),
                               masterDevice_->getActiveOutputChannels().countNumberOfSetBits(), blockSize_);
        PortInfo info;
        info.key = kMasterPortKey;
        info.displayName = (masterDevice_->getName() + " (" + setup.type + ")").toStdString();
        info.type = PortType::MasterDevice;
        mixer_.registerPort(info, masterPort_);
    } else {
        mixer_.unregisterPort(kMasterPortKey);
        masterError_ = error;
    }

    for (auto& secondary : secondaries_) {
        if (secondary->device != nullptr) secondary->device->close();
        secondary->device.reset();
        openSecondary(*secondary);
    }

    startAll();
    listeners_.call([this](Listener& l) { l.engineFormatChanged(sampleRate_, blockSize_); });
    listeners_.call([](Listener& l) { l.deviceStateChanged(); });
    return error;
}

bool RekaDeviceManager::isMasterRunning() const {
    return masterDevice_ != nullptr ? masterDevice_->isPlaying() : softwareClock_.isRunning();
}

juce::String RekaDeviceManager::masterStatus() const {
    if (masterDevice_ == nullptr)
        return masterError_.isNotEmpty() ? "Software clock (" + masterError_ + ")" : "Software clock (no hardware master)";
    juce::String status = masterDevice_->getName() + " - " + juce::String(sampleRate_ / 1000.0, 1) + " kHz / " +
                          juce::String(blockSize_) + " samples";
    if (!masterDevice_->isPlaying()) status << " (stopped" << (masterError_.isNotEmpty() ? ": " + masterError_ : juce::String()) << ")";
    return status;
}

juce::StringArray RekaDeviceManager::masterChannelNames(bool inputs) const {
    if (masterDevice_ == nullptr) return {};
    return inputs ? masterDevice_->getInputChannelNames() : masterDevice_->getOutputChannelNames();
}

double RekaDeviceManager::outputLatencyMs() const {
    if (masterDevice_ == nullptr || sampleRate_ <= 0.0) return 0.0;
    return 1000.0 * (masterDevice_->getOutputLatencyInSamples() + blockSize_) / sampleRate_;
}

int RekaDeviceManager::xruns() const { return masterDevice_ != nullptr ? masterDevice_->getXRunCount() : -1; }

void RekaDeviceManager::audioDeviceIOCallbackWithContext(const float* const* inputs, int numInputs, float* const* outputs,
                                                         int numOutputs, int numSamples,
                                                         const juce::AudioIODeviceCallbackContext&) {
    masterPort_->setDeviceBuffers(inputs, numInputs, outputs, numOutputs, numSamples);
    mixer_.engine().processBlock(numSamples);
}

void RekaDeviceManager::audioDeviceError(const juce::String& message) {
    // May be called on the audio thread: just flag it; the timer restarts the device.
    juce::ignoreUnused(message);
    masterErrored_.store(true);
}

// ---- secondary devices ------------------------------------------------------------------------------------

juce::String RekaDeviceManager::openSecondary(Secondary& secondary) {
    secondary.error.clear();
    auto* type = findType(secondary.setup.type);
    if (type == nullptr) return secondary.error = "driver type '" + secondary.setup.type + "' not available";
    type->scanForDevices();
    if (!type->getDeviceNames(secondary.setup.isInput).contains(secondary.setup.device))
        return secondary.error = "device not connected";

    secondary.device.reset(secondary.setup.isInput ? type->createDevice({}, secondary.setup.device)
                                                   : type->createDevice(secondary.setup.device, {}));
    if (secondary.device == nullptr) return secondary.error = "could not create device";

    juce::BigInteger ins, outs;
    if (secondary.setup.isInput) ins.setRange(0, secondary.device->getInputChannelNames().size(), true);
    else outs.setRange(0, secondary.device->getOutputChannelNames().size(), true);

    // Prefer the engine rate (no conversion at all); otherwise the device's own rate is resampled.
    const auto rates = secondary.device->getAvailableSampleRates();
    const double rate = rates.contains(sampleRate_) || rates.isEmpty() ? sampleRate_ : rates.getFirst();
    const int buffer = secondary.setup.bufferSize > 0 ? secondary.setup.bufferSize : secondary.device->getDefaultBufferSize();
    const juce::String openError = secondary.device->open(ins, outs, rate, buffer);
    if (openError.isNotEmpty()) {
        secondary.device.reset();
        return secondary.error = openError;
    }

    AsyncPortFormat format;
    format.captureChannels = secondary.setup.isInput ? secondary.device->getActiveInputChannels().countNumberOfSetBits() : 0;
    format.renderChannels = secondary.setup.isInput ? 0 : secondary.device->getActiveOutputChannels().countNumberOfSetBits();
    format.endpointSampleRate = secondary.device->getCurrentSampleRate();
    format.endpointMaxBlock = secondary.device->getCurrentBufferSizeSamples();
    // The port must not be in use while it is reconfigured: replace it with a fresh one.
    secondary.port = std::make_shared<AsyncEndpointPort>();
    secondary.port->configure(format, sampleRate_, blockSize_);

    PortInfo info;
    info.key = secondary.setup.key();
    info.displayName = (secondary.setup.device + (secondary.setup.isInput ? " (in)" : " (out)")).toStdString();
    info.type = PortType::SecondaryDevice;
    mixer_.registerPort(info, secondary.port);
    return {};
}

juce::String RekaDeviceManager::addSecondary(const SecondarySetup& setup) {
    for (const auto& existing : secondaries_)
        if (existing->setup.key() == setup.key()) return "already added";
    auto secondary = std::make_unique<Secondary>();
    secondary->setup = setup;
    const juce::String error = openSecondary(*secondary);
    if (secondary->device != nullptr) secondary->device->start(secondary.get());
    secondaries_.push_back(std::move(secondary));
    listeners_.call([](Listener& l) { l.deviceStateChanged(); });
    return error;
}

void RekaDeviceManager::removeSecondary(const PortKey& key) {
    for (auto it = secondaries_.begin(); it != secondaries_.end(); ++it) {
        if ((*it)->setup.key() != key) continue;
        if ((*it)->device != nullptr) {
            (*it)->device->stop();
            (*it)->device->close();
        }
        mixer_.unregisterPort(key);
        secondaries_.erase(it);
        listeners_.call([](Listener& l) { l.deviceStateChanged(); });
        return;
    }
}

std::vector<SecondarySetup> RekaDeviceManager::secondaries() const {
    std::vector<SecondarySetup> out;
    for (const auto& s : secondaries_) out.push_back(s->setup);
    return out;
}

juce::String RekaDeviceManager::secondaryStatus(const PortKey& key) const {
    for (const auto& s : secondaries_) {
        if (s->setup.key() != key) continue;
        if (s->device == nullptr) return "offline: " + s->error;
        return juce::String(s->device->getCurrentSampleRate() / 1000.0, 1) + " kHz, " +
               juce::String(s->device->getCurrentBufferSizeSamples()) + " samples" +
               (s->device->isPlaying() ? "" : " (stopped)");
    }
    return {};
}

std::optional<AsyncEndpointPort::Stats> RekaDeviceManager::secondaryStats(const PortKey& key) const {
    for (const auto& s : secondaries_)
        if (s->setup.key() == key && s->device != nullptr) return s->port->stats();
    return std::nullopt;
}

// ---- hot-plug & recovery -----------------------------------------------------------------------------------

void RekaDeviceManager::audioDeviceListChanged() {
    // Message thread. A device that came back is retried right away.
    timerCallback();
    listeners_.call([](Listener& l) { l.deviceStateChanged(); });
}

void RekaDeviceManager::timerCallback() {
    if (masterErrored_.exchange(false) || (masterDevice_ != nullptr && !masterDevice_->isPlaying() && masterSetup_.type.isNotEmpty())) {
        masterError_ = "device error - restarting";
        setMaster(masterSetup_);
        return;
    }
    if (masterDevice_ == nullptr && masterSetup_.type.isNotEmpty() && masterError_.isNotEmpty()) {
        // The configured master is missing (unplugged): reopen it as soon as it reappears.
        if (auto* type = findType(masterSetup_.type)) {
            type->scanForDevices();
            const auto name = masterSetup_.output.isNotEmpty() ? masterSetup_.output : masterSetup_.input;
            const bool isOutput = masterSetup_.output.isNotEmpty();
            if (type->getDeviceNames(!isOutput).contains(name)) setMaster(masterSetup_);
        }
    }
    bool changed = false;
    for (auto& secondary : secondaries_) {
        const bool failed = secondary->device == nullptr || secondary->deviceError.exchange(false) ||
                            !secondary->device->isPlaying();
        if (!failed) continue;
        if (secondary->device != nullptr) {
            secondary->device->stop();
            secondary->device->close();
            secondary->device.reset();
        }
        if (openSecondary(*secondary).isEmpty()) {
            secondary->device->start(secondary.get());
            changed = true;
        }
    }
    if (changed) listeners_.call([](Listener& l) { l.deviceStateChanged(); });
}

// ---- persistence ------------------------------------------------------------------------------------------

nlohmann::json RekaDeviceManager::saveState() const {
    nlohmann::json out;
    out["master"] = masterSetup_.toJson();
    auto list = nlohmann::json::array();
    for (const auto& s : secondaries_) list.push_back(s->setup.toJson());
    out["secondary"] = list;
    return out;
}

juce::String RekaDeviceManager::restoreState(const nlohmann::json& audio) {
    juce::StringArray warnings;
    if (!audio.is_object()) return {};
    if (auto master = audio.find("master"); master != audio.end()) {
        const MasterSetup wanted = MasterSetup::fromJson(*master);
        // Hot reload: only restart the clock when the profile actually asks for something different.
        if (!(wanted == masterSetup_) || !isMasterRunning()) {
            const auto error = setMaster(wanted);
            if (error.isNotEmpty()) warnings.add("Master device: " + error);
        }
    }
    std::vector<SecondarySetup> wanted;
    if (auto list = audio.find("secondary"); list != audio.end() && list->is_array())
        for (const auto& entry : *list) wanted.push_back(SecondarySetup::fromJson(entry));

    for (const auto& existing : secondaries()) {
        const bool keep = std::any_of(wanted.begin(), wanted.end(), [&](const SecondarySetup& w) { return w.key() == existing.key(); });
        if (!keep) removeSecondary(existing.key());
    }
    for (const auto& w : wanted) {
        const bool present = std::any_of(secondaries_.begin(), secondaries_.end(),
                                         [&](const std::unique_ptr<Secondary>& s) { return s->setup.key() == w.key(); });
        if (present) continue;
        const auto error = addSecondary(w);
        if (error.isNotEmpty()) warnings.add(w.device + ": " + error);
    }
    return warnings.joinIntoString("\n");
}

}  // namespace reka::app
