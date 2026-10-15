#include "hardware/MidiControllerBridge.h"

#include "reka/dsp/FaderLaw.h"

namespace reka::app {

const char* MidiControllerBridge::toString(Target target) noexcept {
    switch (target) {
        case Target::Gain: return "gain";
        case Target::Pan: return "pan";
        case Target::Mute: return "mute";
        case Target::Solo: return "solo";
        case Target::FxBypass: return "fxBypass";
    }
    return "gain";
}

MidiControllerBridge::Target MidiControllerBridge::targetFromString(const std::string& text) noexcept {
    if (text == "pan") return Target::Pan;
    if (text == "mute") return Target::Mute;
    if (text == "solo") return Target::Solo;
    if (text == "fxBypass") return Target::FxBypass;
    return Target::Gain;
}

MidiControllerBridge::MidiControllerBridge(MixerModel& mixer) : mixer_(mixer), bindings_(std::make_shared<BindingList>()) {
    startTimerHz(30);
}

MidiControllerBridge::~MidiControllerBridge() {
    stopTimer();
    closeAll();
}

bool MidiControllerBridge::openInput(const juce::String& identifier) {
    if (input_ != nullptr) input_->stop();
    input_ = juce::MidiInput::openDevice(identifier, this);
    inputId_ = input_ != nullptr ? identifier : juce::String();
    if (input_ != nullptr) input_->start();
    return input_ != nullptr;
}

bool MidiControllerBridge::openOutput(const juce::String& identifier) {
    output_ = juce::MidiOutput::openDevice(identifier);
    outputId_ = output_ != nullptr ? identifier : juce::String();
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& b : *bindings_) b.lastSent = -1;  // push full state to the new device
    return output_ != nullptr;
}

void MidiControllerBridge::closeAll() {
    if (input_ != nullptr) input_->stop();
    input_.reset();
    output_.reset();
    inputId_.clear();
    outputId_.clear();
}

void MidiControllerBridge::setMappings(std::vector<Mapping> mappings) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        mappings_ = std::move(mappings);
    }
    rebuild();
}

std::vector<MidiControllerBridge::Mapping> MidiControllerBridge::mappings() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return mappings_;
}

void MidiControllerBridge::refreshBindings() { rebuild(); }

void MidiControllerBridge::rebuild() {
    auto list = std::make_shared<BindingList>();
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& m : mappings_) {
        Binding b;
        b.mapping = m;
        b.params = mixer_.parameters(m.strip);
        if (b.params != nullptr) list->push_back(std::move(b));
    }
    bindings_ = std::move(list);
}

void MidiControllerBridge::startLearn(StripId strip, Target target) {
    learnStrip_ = strip;
    learnTarget_ = target;
    learnedNumber_.store(-1);
    learning_.store(true);
}

void MidiControllerBridge::cancelLearn() { learning_.store(false); }

void MidiControllerBridge::handleIncomingMidiMessage(juce::MidiInput*, const juce::MidiMessage& message) {
    // MIDI thread.
    const bool isCc = message.isController();
    const bool isNoteOn = message.isNoteOn();
    if (!isCc && !isNoteOn && !message.isNoteOff()) return;
    const int channel = message.getChannel();
    const int number = isCc ? message.getControllerNumber() : message.getNoteNumber();

    if (learning_.load() && (isCc || isNoteOn)) {
        learnedChannel_.store(channel);
        learnedIsNote_.store(!isCc);
        learnedNumber_.store(number);
        return;  // finalised on the message thread
    }

    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& b : *bindings_) {
        const Mapping& m = b.mapping;
        if (m.channel != channel || m.number != number || m.isNote == isCc) continue;
        StripParameters& p = *b.params;
        if (isCc) {
            const float value = static_cast<float>(message.getControllerValue()) / 127.0f;
            switch (m.target) {
                case Target::Gain: {
                    const float current = FaderLaw::dbToPosition(p.gainDb());
                    if (b.pickup.onHardwareMove(value, current)) p.setGainDb(FaderLaw::positionToDb(value));
                    break;
                }
                case Target::Pan: p.setPan(value * 2.0f - 1.0f); break;
                // Buttons sending CC: toggle on press (value >= 64).
                case Target::Mute: if (value >= 0.5f) p.toggleMute(); break;
                case Target::Solo: if (value >= 0.5f) p.toggleSolo(); break;
                case Target::FxBypass: if (value >= 0.5f) p.toggleFxBypass(); break;
            }
        } else if (isNoteOn) {
            switch (m.target) {
                case Target::Mute: p.toggleMute(); break;
                case Target::Solo: p.toggleSolo(); break;
                case Target::FxBypass: p.toggleFxBypass(); break;
                case Target::Gain:
                case Target::Pan: break;  // continuous targets ignore note messages
            }
        }
    }
}

void MidiControllerBridge::timerCallback() {
    if (learning_.load() && learnedNumber_.load() >= 0) {
        Mapping mapping;
        mapping.channel = learnedChannel_.load();
        mapping.isNote = learnedIsNote_.load();
        mapping.number = learnedNumber_.load();
        mapping.strip = learnStrip_;
        mapping.target = learnTarget_;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // One control drives one target: replace any previous use of this control or target.
            std::erase_if(mappings_, [&](const Mapping& m) {
                return (m.channel == mapping.channel && m.number == mapping.number && m.isNote == mapping.isNote) ||
                       (m.strip == mapping.strip && m.target == mapping.target);
            });
            mappings_.push_back(mapping);
        }
        learning_.store(false);
        rebuild();
    }

    // Feedback + external-change detection for soft takeover.
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& b : *bindings_) {
        const StripParameters& p = *b.params;
        int value = 0;
        switch (b.mapping.target) {
            case Target::Gain: value = juce::roundToInt(FaderLaw::dbToPosition(p.gainDb()) * 127.0f); break;
            case Target::Pan: value = juce::roundToInt((p.pan() + 1.0f) * 63.5f); break;
            case Target::Mute: value = p.mute() ? 127 : 0; break;
            case Target::Solo: value = p.solo() ? 127 : 0; break;
            case Target::FxBypass: value = p.fxBypass() ? 127 : 0; break;
        }
        if (value == b.lastSent) continue;
        if (b.mapping.target == Target::Gain && b.lastSent >= 0) b.pickup.onExternalChange(value / 127.0f);
        b.lastSent = value;
        if (output_ == nullptr) continue;
        const auto message = b.mapping.isNote
                                 ? juce::MidiMessage::noteOn(b.mapping.channel, b.mapping.number, static_cast<juce::uint8>(value))
                                 : juce::MidiMessage::controllerEvent(b.mapping.channel, b.mapping.number, value);
        output_->sendMessageNow(message);
    }
}

nlohmann::json MidiControllerBridge::saveState() const {
    auto list = nlohmann::json::array();
    for (const auto& m : mappings())
        list.push_back({{"channel", m.channel},
                        {"type", m.isNote ? "note" : "cc"},
                        {"number", m.number},
                        {"strip", m.strip},
                        {"target", toString(m.target)}});
    return {{"input", inputId_.toStdString()}, {"output", outputId_.toStdString()}, {"mappings", list}};
}

void MidiControllerBridge::restoreState(const nlohmann::json& section) {
    std::vector<Mapping> list;
    if (section.is_object()) {
        for (const auto& m : section.value("mappings", nlohmann::json::array())) {
            Mapping mapping;
            mapping.channel = juce::jlimit(1, 16, m.value("channel", 1));
            mapping.isNote = m.value("type", std::string("cc")) == "note";
            mapping.number = juce::jlimit(0, 127, m.value("number", 0));
            mapping.strip = m.value("strip", kInvalidStrip);
            mapping.target = targetFromString(m.value("target", std::string("gain")));
            list.push_back(mapping);
        }
        const juce::String in(section.value("input", std::string()));
        const juce::String out(section.value("output", std::string()));
        if (in.isNotEmpty() && in != inputId_) openInput(in);
        if (out.isNotEmpty() && out != outputId_) openOutput(out);
    }
    setMappings(std::move(list));
}

}  // namespace reka::app
