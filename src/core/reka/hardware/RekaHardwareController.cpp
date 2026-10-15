#include "reka/hardware/RekaHardwareController.h"

#include "RekaProtocol.h"  // firmware/REKA/RekaProtocol.h: the wire contract shared with REKA.ino
#include "reka/dsp/Decibels.h"
#include "reka/dsp/FaderLaw.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace reka {

namespace proto = reka_proto;
using namespace std::chrono_literals;

namespace {
constexpr auto kFeedbackInterval = 33ms;
constexpr auto kPingInterval = 1000ms;
constexpr auto kSilenceTimeout = 3500ms;
constexpr auto kHandshakeTimeout = 3000ms;  // ESP32 boards reboot when the port opens
constexpr auto kRetryDelay = 2000ms;
constexpr auto kClipHold = 1000ms;
}  // namespace

// ---- settings ----------------------------------------------------------------------------------------

HardwareSettings HardwareSettings::fromJson(const nlohmann::json& section) {
    HardwareSettings s;
    if (!section.is_object()) return s;
    if (auto serial = section.find("serial"); serial != section.end() && serial->is_object()) {
        s.enabled = serial->value("enabled", s.enabled);
        s.port = serial->value("port", s.port);
        s.baud = serial->value("baud", s.baud);
    }
    if (auto map = section.find("map"); map != section.end() && map->is_array())
        for (const auto& id : *map)
            if (id.is_number_unsigned()) s.map.push_back(id.get<StripId>());
    if (auto display = section.find("display"); display != section.end() && display->is_object()) {
        s.ledBrightness = std::clamp(display->value("ledBrightness", s.ledBrightness), 0, 255);
        s.oledContrast = std::clamp(display->value("oledContrast", s.oledContrast), 0, 255);
    }
    return s;
}

void HardwareSettings::writeTo(nlohmann::json& section) const {
    if (!section.is_object()) section = nlohmann::json::object();
    section["serial"] = {{"enabled", enabled}, {"port", port}, {"baud", baud}};
    section["map"] = map;
    section["display"] = {{"ledBrightness", ledBrightness}, {"oledContrast", oledContrast}};
}

// ---- lifecycle -----------------------------------------------------------------------------------------

RekaHardwareController::RekaHardwareController(PortFactory factory, PortEnumerator enumerator)
    : factory_(std::move(factory)), enumerator_(std::move(enumerator)) {}

RekaHardwareController::~RekaHardwareController() { stop(); }

void RekaHardwareController::start(const HardwareSettings& settings) {
    stop();
    settings_ = settings;
    if (!settings_.enabled) {
        setStatus(HardwareStatus::Stopped, "Disabled");
        return;
    }
    running_.store(true);
    thread_ = std::thread([this] { threadMain(); });
}

void RekaHardwareController::stop() {
    running_.store(false);
    if (thread_.joinable()) thread_.join();
    setStatus(HardwareStatus::Stopped, "Stopped");
}

void RekaHardwareController::setBindings(std::shared_ptr<const HardwareBindings> bindings) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        bindings_ = std::move(bindings);
    }
    bindingsGeneration_.fetch_add(1);
}

void RekaHardwareController::setSelectedChannel(int channel) {
    selected_.store(channel);
    selectedDirty_.store(true);
}

std::shared_ptr<const HardwareBindings> RekaHardwareController::currentBindings() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return bindings_;
}

std::string RekaHardwareController::statusText() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return statusText_;
}

HardwareDeviceInfo RekaHardwareController::deviceInfo() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return device_;
}

void RekaHardwareController::setStatus(HardwareStatus status, std::string text) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        statusText_ = std::move(text);
    }
    status_.store(status);
}

// ---- I/O thread ------------------------------------------------------------------------------------------

void RekaHardwareController::threadMain() {
    while (running_.load()) {
        setStatus(HardwareStatus::Searching, "Searching for a REKA controller...");
        std::unique_ptr<ISerialPort> port = connect();
        if (!port) {
            for (auto waited = 0ms; waited < kRetryDelay && running_.load(); waited += 100ms)
                std::this_thread::sleep_for(100ms);
            continue;
        }
        runSession(*port);
        port->close();
    }
}

std::unique_ptr<ISerialPort> RekaHardwareController::connect() {
    std::vector<std::string> candidates;
    if (settings_.port.empty() || settings_.port == "auto") {
        auto ports = enumerator_();
        // Known microcontroller vendors first; everything else after.
        std::stable_partition(ports.begin(), ports.end(), [](const SerialPortInfo& p) { return p.likelyController; });
        for (const auto& p : ports) candidates.push_back(p.name);
    } else {
        candidates.push_back(settings_.port);
    }

    for (const std::string& name : candidates) {
        if (!running_.load()) return nullptr;
        auto port = factory_();
        std::string error;
        if (!port || !port->open(name, settings_.baud, error)) continue;
        if (handshake(*port, name)) return port;
        port->close();
    }
    return nullptr;
}

bool RekaHardwareController::handshake(ISerialPort& port, const std::string& name) {
    proto::FrameParser parser;
    proto::FrameWriter writer;
    std::uint8_t buffer[128];
    auto lastRequest = Clock::time_point{};
    const auto deadline = Clock::now() + kHandshakeTimeout;

    while (Clock::now() < deadline && running_.load()) {
        if (Clock::now() - lastRequest >= 500ms) {
            writer.begin(proto::MSG_HELLO_REQUEST);
            writer.u8(proto::kProtocolVersion);
            txLength_ = writer.finish(txBuffer_);
            if (!sendFrame(port)) return false;
            lastRequest = Clock::now();
        }
        const int n = port.read(buffer, sizeof(buffer), 50);
        if (n < 0) return false;
        for (int i = 0; i < n; ++i) {
            if (!parser.feed(buffer[i]) || parser.type() != proto::MSG_HELLO || parser.payloadSize() < 9) continue;
            const std::uint8_t* p = parser.payload();
            if (std::memcmp(p + 5, proto::kMagic, 4) != 0) continue;
            HardwareDeviceInfo info;
            info.port = name;
            info.protocolVersion = p[0];
            info.firmwareMajor = p[1];
            info.firmwareMinor = p[2];
            info.numChannels = std::min<int>(p[3], proto::kMaxChannels);
            info.capabilities = p[4];
            if (info.protocolVersion != proto::kProtocolVersion) {
                setStatus(HardwareStatus::Error, "Controller on " + name + " speaks protocol v" +
                                                     std::to_string(info.protocolVersion) + "; update REKA.ino");
                return false;
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                device_ = info;
            }
            deviceChannels_ = info.numChannels;
            return true;
        }
    }
    return false;
}

void RekaHardwareController::runSession(ISerialPort& port) {
    {
        const auto info = deviceInfo();
        setStatus(HardwareStatus::Connected, "Connected: " + info.port + " (" + std::to_string(info.numChannels) +
                                                 " channels, firmware " + std::to_string(info.firmwareMajor) + "." +
                                                 std::to_string(info.firmwareMinor) + ")");
    }
    for (auto& ch : channels_) ch = ChannelState{};
    sessionGeneration_ = ~0ull;  // force a full resync
    lastHeard_ = Clock::now();

    // The fader snapshot that follows the handshake HELLO may have been consumed by the handshake
    // parser; ask again so pickup starts from real physical positions.
    {
        proto::FrameWriter writer;
        writer.begin(proto::MSG_HELLO_REQUEST);
        writer.u8(proto::kProtocolVersion);
        txLength_ = writer.finish(txBuffer_);
        sendFrame(port);
    }

    proto::FrameParser parser;
    std::uint8_t buffer[256];
    auto nextFeedback = Clock::now();
    auto nextPing = Clock::now() + kPingInterval;
    std::uint8_t pingToken = 0;

    while (running_.load()) {
        const int n = port.read(buffer, sizeof(buffer), 5);
        if (n < 0) break;  // unplugged
        for (int i = 0; i < n; ++i) {
            if (parser.feed(buffer[i])) {
                lastHeard_ = Clock::now();
                framesReceived_.fetch_add(1, std::memory_order_relaxed);
                handleFrame(parser.type(), parser.payload(), parser.payloadSize(), port);
            }
        }
        frameErrors_.store(parser.errorCount(), std::memory_order_relaxed);

        const auto now = Clock::now();
        const std::uint64_t generation = bindingsGeneration_.load();
        if (generation != sessionGeneration_) {
            sessionGeneration_ = generation;
            sessionBindings_ = currentBindings();
            for (std::size_t ch = 0; ch < channels_.size(); ++ch) {
                ChannelState& state = channels_[ch];
                state.nameSent = false;
                state.sentFlags = -1;
                state.gainKnown = false;
                // Physical positions do not change when the mapping does: re-evaluate pickup against the
                // newly mapped strips.
                const bool mapped = sessionBindings_ && ch < sessionBindings_->channels.size() &&
                                    sessionBindings_->channels[ch].params;
                if (mapped && state.pickup.hardwarePositionKnown())
                    state.pickup.setHardwarePosition(state.pickup.hardwarePosition(),
                                                     FaderLaw::dbToPosition(sessionBindings_->channels[ch].params->gainDb()));
                else if (!mapped)
                    state.pickup.reset();
            }
            resyncRequested_ = true;
        }
        if (resyncRequested_) {
            resyncRequested_ = false;
            sendFullState(port);
        }
        if (now >= nextFeedback) {
            sendFeedback(port, false);
            sendMeters(port);
            nextFeedback = now + kFeedbackInterval;
        }
        if (now >= nextPing) {
            proto::FrameWriter writer;
            writer.begin(proto::MSG_PING);
            writer.u8(pingToken++);
            txLength_ = writer.finish(txBuffer_);
            if (!sendFrame(port)) break;
            nextPing = now + kPingInterval;
        }
        if (now - lastHeard_ > kSilenceTimeout) {
            setStatus(HardwareStatus::Searching, "Controller stopped responding");
            break;
        }
    }
}

bool RekaHardwareController::sendFrame(ISerialPort& port) {
    if (txLength_ == 0) return true;
    const bool ok = port.write(txBuffer_, txLength_);
    txLength_ = 0;
    return ok;
}

// ---- inbound ------------------------------------------------------------------------------------------

void RekaHardwareController::handleFrame(std::uint8_t type, const std::uint8_t* payload, std::uint8_t size,
                                         ISerialPort& port) {
    switch (type) {
        case proto::MSG_FADER:
            if (size >= 3) handleFader(payload[0], proto::readU16(payload + 1) / 65535.0f);
            break;
        case proto::MSG_FADER_SNAPSHOT: {
            if (size < 2) break;
            const int first = payload[0];
            const int count = std::min<int>(payload[1], (size - 2) / 2);
            const auto bindings = sessionBindings_;
            for (int i = 0; i < count; ++i) {
                const int channel = first + i;
                if (channel < 0 || channel >= static_cast<int>(channels_.size())) continue;
                const float position = proto::readU16(payload + 2 + 2 * i) / 65535.0f;
                float parameterPosition = 0.0f;
                if (bindings && channel < static_cast<int>(bindings->channels.size()) && bindings->channels[static_cast<std::size_t>(channel)].params)
                    parameterPosition = FaderLaw::dbToPosition(bindings->channels[static_cast<std::size_t>(channel)].params->gainDb());
                channels_[static_cast<std::size_t>(channel)].pickup.setHardwarePosition(position, parameterPosition);
                channels_[static_cast<std::size_t>(channel)].sentPickup = 0x7F;  // refresh the hint
            }
            break;
        }
        case proto::MSG_BUTTON:
            if (size >= 3) handleButton(payload[0], payload[1], payload[2] != 0);
            break;
        case proto::MSG_HELLO:
            // Device rebooted (or answered our request): resend names, states and settings.
            if (size >= 4) deviceChannels_ = std::min<int>(payload[3], proto::kMaxChannels);
            resyncRequested_ = true;
            break;
        case proto::MSG_PONG:
        default:
            break;
    }
    (void)port;
}

void RekaHardwareController::handleFader(int channel, float position) {
    const auto& bindings = sessionBindings_;
    if (!bindings || channel < 0 || channel >= static_cast<int>(bindings->channels.size()) ||
        channel >= static_cast<int>(channels_.size()))
        return;
    const HardwareChannelBinding& binding = bindings->channels[static_cast<std::size_t>(channel)];
    if (!binding.params) return;
    ChannelState& state = channels_[static_cast<std::size_t>(channel)];

    const float parameterPosition = FaderLaw::dbToPosition(binding.params->gainDb());
    if (state.pickup.onHardwareMove(position, parameterPosition)) {
        binding.params->setGainDb(FaderLaw::positionToDb(position));
        state.lastAppliedGainDb = binding.params->gainDb();
        state.gainKnown = true;
    }
    lastTouched_.store(channel);
    touchCounter_.fetch_add(1);
}

void RekaHardwareController::handleButton(int channel, int button, bool pressed) {
    const auto& bindings = sessionBindings_;
    if (!pressed || !bindings || channel < 0 || channel >= static_cast<int>(bindings->channels.size())) return;
    const auto& params = bindings->channels[static_cast<std::size_t>(channel)].params;
    if (!params) return;
    switch (button) {
        case proto::BUTTON_MUTE: params->toggleMute(); break;
        case proto::BUTTON_SOLO: params->toggleSolo(); break;
        case proto::BUTTON_FX: params->toggleFxBypass(); break;
        case proto::BUTTON_SELECT: setSelectedChannel(channel); break;
        default: break;
    }
    lastTouched_.store(channel);
    touchCounter_.fetch_add(1);
}

// ---- outbound -----------------------------------------------------------------------------------------

void RekaHardwareController::sendFullState(ISerialPort& port) {
    proto::FrameWriter writer;
    const auto& bindings = sessionBindings_;
    writer.begin(proto::MSG_PROFILE_NAME);
    writer.str(bindings ? bindings->profileName.c_str() : "", proto::kProfileNameLength);
    txLength_ = writer.finish(txBuffer_);
    sendFrame(port);

    const int mapped = bindings ? std::min<int>(static_cast<int>(bindings->channels.size()), deviceChannels_) : 0;
    writer.begin(proto::MSG_DISPLAY_CONFIG);
    writer.u8(static_cast<std::uint8_t>(settings_.ledBrightness));
    writer.u8(static_cast<std::uint8_t>(settings_.oledContrast));
    writer.u8(static_cast<std::uint8_t>(mapped));
    txLength_ = writer.finish(txBuffer_);
    sendFrame(port);

    selectedDirty_.store(true);
    sendFeedback(port, true);
}

void RekaHardwareController::sendFeedback(ISerialPort& port, bool forceAll) {
    proto::FrameWriter writer;
    const auto& bindings = sessionBindings_;
    const int count = std::min(deviceChannels_, static_cast<int>(channels_.size()));
    const auto now = Clock::now();

    bool anySolo = false;
    if (bindings)
        for (const auto& b : bindings->channels) anySolo = anySolo || (b.params && b.params->solo());

    for (int ch = 0; ch < count; ++ch) {
        ChannelState& state = channels_[static_cast<std::size_t>(ch)];
        const HardwareChannelBinding* binding =
            (bindings && ch < static_cast<int>(bindings->channels.size()) && bindings->channels[static_cast<std::size_t>(ch)].params)
                ? &bindings->channels[static_cast<std::size_t>(ch)]
                : nullptr;

        // Names
        const std::string name = binding != nullptr ? binding->name : std::string();
        if (forceAll || !state.nameSent || state.sentName != name) {
            writer.begin(proto::MSG_CHANNEL_NAME);
            writer.u8(static_cast<std::uint8_t>(ch));
            writer.str(name.c_str(), proto::kNameLength);
            txLength_ = writer.finish(txBuffer_);
            sendFrame(port);
            state.sentName = name;
            state.nameSent = true;
        }

        // Flags, gain and pickup hint
        int flags = 0;
        int gain = proto::kGainMinusInfinity;
        int pickup = 0;
        if (binding != nullptr) {
            const StripParameters& p = *binding->params;
            const float gainDb = p.gainDb();
            const float position = FaderLaw::dbToPosition(gainDb);
            if (!state.gainKnown) {
                state.lastAppliedGainDb = gainDb;
                state.gainKnown = true;
            } else if (gainDb != state.lastAppliedGainDb) {  // changed on screen / by a profile / by MIDI
                state.pickup.onExternalChange(position);
                state.lastAppliedGainDb = gainDb;
            }
            flags |= proto::FLAG_MAPPED;
            if (p.mute()) flags |= proto::FLAG_MUTE;
            if (p.solo()) flags |= proto::FLAG_SOLO;
            if (p.fxBypass()) flags |= proto::FLAG_FX_BYPASS;
            if (anySolo && !p.solo()) flags |= proto::FLAG_SOLO_SAFE;
            if (state.clipLatched && now < state.clipUntil) flags |= proto::FLAG_CLIP;
            gain = proto::encodeGainDb(gainDb);
            pickup = state.pickup.hint(position);
        }
        if (ch == selected_.load()) flags |= proto::FLAG_SELECTED;

        if (forceAll || flags != state.sentFlags || gain != state.sentGain || pickup != state.sentPickup) {
            writer.begin(proto::MSG_CHANNEL_STATE);
            writer.u8(static_cast<std::uint8_t>(ch));
            writer.u8(static_cast<std::uint8_t>(flags));
            writer.i16(static_cast<std::int16_t>(gain));
            writer.i8(static_cast<std::int8_t>(pickup));
            txLength_ = writer.finish(txBuffer_);
            sendFrame(port);
            state.sentFlags = flags;
            state.sentGain = gain;
            state.sentPickup = pickup;
        }
    }

    if (selectedDirty_.exchange(false)) {
        writer.begin(proto::MSG_SELECT);
        writer.u8(static_cast<std::uint8_t>(std::clamp(selected_.load(), 0, 255)));
        txLength_ = writer.finish(txBuffer_);
        sendFrame(port);
    }
}

void RekaHardwareController::sendMeters(ISerialPort& port) {
    const auto& bindings = sessionBindings_;
    if (!bindings) return;
    const int count = std::min({deviceChannels_, static_cast<int>(bindings->channels.size()),
                                static_cast<int>(proto::kMaxChannels)});
    if (count <= 0) return;

    const auto now = Clock::now();
    proto::FrameWriter writer;
    writer.begin(proto::MSG_METERS);
    writer.u8(0);
    writer.u8(static_cast<std::uint8_t>(count));
    for (int ch = 0; ch < count; ++ch) {
        const auto& binding = bindings->channels[static_cast<std::size_t>(ch)];
        std::uint8_t level = 0;
        if (binding.processor) {
            const MeterReading reading = binding.processor->meter.read(MeterReader::Hardware);
            const float peak = std::max(reading.peak[0], reading.peak[1]);
            level = proto::encodeMeterDb(gainToDb(peak));
            ChannelState& state = channels_[static_cast<std::size_t>(ch)];
            if (reading.clipped) {
                level = proto::kMeterClip;
                state.clipLatched = true;
                state.clipUntil = now + kClipHold;
            }
        }
        writer.u8(level);
    }
    txLength_ = writer.finish(txBuffer_);
    sendFrame(port);
}

}  // namespace reka
