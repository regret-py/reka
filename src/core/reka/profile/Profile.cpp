#include "reka/profile/Profile.h"

#include "reka/common/Base64.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace reka {

using nlohmann::json;

// ---- colours -----------------------------------------------------------------------------------------
// CSS order: "#RRGGBB" or "#RRGGBBAA".

std::string colourToHex(std::uint32_t argb) {
    char buffer[12];
    const unsigned a = (argb >> 24) & 0xFF, r = (argb >> 16) & 0xFF, g = (argb >> 8) & 0xFF, b = argb & 0xFF;
    if (a == 0xFF) std::snprintf(buffer, sizeof(buffer), "#%02X%02X%02X", r, g, b);
    else std::snprintf(buffer, sizeof(buffer), "#%02X%02X%02X%02X", r, g, b, a);
    return buffer;
}

bool colourFromHex(const std::string& text, std::uint32_t& argb) {
    if (text.size() != 7 && text.size() != 9) return false;
    if (text[0] != '#') return false;
    std::uint32_t value = 0;
    for (std::size_t i = 1; i < text.size(); ++i) {
        const char c = text[i];
        std::uint32_t digit;
        if (c >= '0' && c <= '9') digit = static_cast<std::uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f') digit = static_cast<std::uint32_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') digit = static_cast<std::uint32_t>(c - 'A' + 10);
        else return false;
        value = (value << 4) | digit;
    }
    argb = text.size() == 7 ? (0xFF000000u | value) : ((value & 0xFFu) << 24) | (value >> 8);
    return true;
}

// ---- JSON helpers --------------------------------------------------------------------------------------

namespace {

template <typename T>
T get(const json& object, const char* key, T fallback) {
    auto it = object.find(key);
    if (it == object.end() || it->is_null()) return fallback;
    try {
        return it->get<T>();
    } catch (const json::exception& e) {
        throw ProfileError(std::string("field '") + key + "': " + e.what());
    }
}

json bandsToJson(bool enabled, const EqBandArray& bands) {
    json out;
    out["enabled"] = enabled;
    json list = json::array();
    for (const EqBand& b : bands) {
        list.push_back({{"on", b.enabled},
                        {"type", toString(b.type)},
                        {"freq", b.frequency},
                        {"gain", b.gainDb},
                        {"q", b.q}});
    }
    out["bands"] = std::move(list);
    return out;
}

void bandsFromJson(const json& object, bool& enabled, EqBandArray& bands) {
    bands = defaultEqBands();
    enabled = true;
    if (!object.is_object()) return;
    enabled = get<bool>(object, "enabled", true);
    auto it = object.find("bands");
    if (it == object.end() || !it->is_array()) return;
    for (std::size_t i = 0; i < bands.size() && i < it->size(); ++i) {
        const json& b = (*it)[i];
        EqBand band = bands[i];
        band.enabled = get<bool>(b, "on", false);
        FilterType type = band.type;
        if (fromString(get<std::string>(b, "type", toString(band.type)), type)) band.type = type;
        band.frequency = std::clamp(get<float>(b, "freq", band.frequency), 10.0f, 96000.0f);
        band.gainDb = std::clamp(get<float>(b, "gain", 0.0f), -36.0f, 36.0f);
        band.q = std::clamp(get<float>(b, "q", band.q), 0.05f, 40.0f);
        bands[i] = band;
    }
}

json bindingToJson(const PortBinding& binding) {
    return {{"port", binding.port}, {"left", binding.left}, {"right", binding.right}};
}

PortBinding bindingFromJson(const json& object) {
    PortBinding binding;
    if (!object.is_object()) return binding;
    binding.port = get<std::string>(object, "port", "");
    binding.left = std::max(0, get<int>(object, "left", 0));
    binding.right = std::max(-1, get<int>(object, "right", 1));
    return binding;
}

json stripToJson(const StripState& s) {
    json out;
    out["id"] = s.id;
    out["name"] = s.name;
    out["colour"] = colourToHex(s.colour);
    out[s.kind == StripKind::Input ? "source" : "sink"] = bindingToJson(s.binding);
    out["gainDb"] = s.gainDb;
    out["pan"] = s.pan;
    out["mute"] = s.mute;
    out["solo"] = s.solo;
    out["fxBypass"] = s.fxBypass;
    out["phase"] = {s.phaseInvertLeft, s.phaseInvertRight};
    out["mode"] = toString(s.mode);
    out["eq"] = bandsToJson(s.eqEnabled, s.eq);
    json inserts = json::array();
    for (const InsertState& insert : s.inserts) {
        inserts.push_back({{"plugin", insert.descriptor.pluginId},
                           {"name", insert.descriptor.name},
                           {"format", insert.descriptor.format},
                           {"sandboxed", insert.descriptor.sandboxed},
                           {"bypassed", insert.bypassed},
                           {"state", base64Encode(insert.state)}});
    }
    out["inserts"] = std::move(inserts);
    return out;
}

StripState stripFromJson(const json& object, StripKind kind) {
    if (!object.is_object()) throw ProfileError("strip entry is not an object");
    StripState s;
    s.kind = kind;
    s.id = get<StripId>(object, "id", kInvalidStrip);
    s.name = get<std::string>(object, "name", kind == StripKind::Input ? "Input" : "Bus");
    std::uint32_t colour = s.colour;
    if (colourFromHex(get<std::string>(object, "colour", ""), colour)) s.colour = colour;
    auto binding = object.find(kind == StripKind::Input ? "source" : "sink");
    if (binding != object.end()) s.binding = bindingFromJson(*binding);
    s.gainDb = clampGainDb(get<float>(object, "gainDb", 0.0f));
    s.pan = std::clamp(get<float>(object, "pan", 0.0f), -1.0f, 1.0f);
    s.mute = get<bool>(object, "mute", false);
    s.solo = get<bool>(object, "solo", false);
    s.fxBypass = get<bool>(object, "fxBypass", false);
    if (auto phase = object.find("phase"); phase != object.end() && phase->is_array() && phase->size() == 2) {
        s.phaseInvertLeft = (*phase)[0].get<bool>();
        s.phaseInvertRight = (*phase)[1].get<bool>();
    }
    ChannelMode mode = ChannelMode::Stereo;
    if (fromString(get<std::string>(object, "mode", "stereo"), mode)) s.mode = mode;
    if (auto eq = object.find("eq"); eq != object.end()) bandsFromJson(*eq, s.eqEnabled, s.eq);
    if (auto inserts = object.find("inserts"); inserts != object.end() && inserts->is_array()) {
        for (const json& entry : *inserts) {
            InsertState insert;
            insert.descriptor.pluginId = get<std::string>(entry, "plugin", "");
            insert.descriptor.name = get<std::string>(entry, "name", "");
            insert.descriptor.format = get<std::string>(entry, "format", "VST3");
            insert.descriptor.sandboxed = get<bool>(entry, "sandboxed", false);
            insert.bypassed = get<bool>(entry, "bypassed", false);
            auto decoded = base64Decode(get<std::string>(entry, "state", ""));
            if (!decoded) throw ProfileError("insert '" + insert.descriptor.name + "' has an invalid state blob");
            insert.state = std::move(*decoded);
            if (insert.descriptor.pluginId.empty()) throw ProfileError("insert without plugin identifier");
            s.inserts.push_back(std::move(insert));
        }
    }
    return s;
}

}  // namespace

// ---- Profile <-> JSON ---------------------------------------------------------------------------------

json profileToJson(const Profile& profile) {
    json out;
    out["format"] = kProfileFormatTag;
    out["version"] = kProfileFormatVersion;
    out["name"] = profile.name;
    out["audio"] = profile.audio.is_object() ? profile.audio : json::object();

    json mixer;
    mixer["panLaw"] = toString(profile.mixer.panLaw);
    json inputs = json::array(), buses = json::array(), routes = json::array();
    for (const StripState& s : profile.mixer.inputs) inputs.push_back(stripToJson(s));
    for (const StripState& s : profile.mixer.buses) buses.push_back(stripToJson(s));
    for (const RouteState& r : profile.mixer.routes) {
        routes.push_back({{"from", r.input},
                          {"to", r.bus},
                          {"enabled", r.enabled},
                          {"gainDb", r.gainDb},
                          {"pan", r.pan},
                          {"phase", r.phaseInvert},
                          {"preFader", r.preFader},
                          {"eq", bandsToJson(r.eqEnabled, r.eq)}});
    }
    mixer["inputs"] = std::move(inputs);
    mixer["buses"] = std::move(buses);
    mixer["routes"] = std::move(routes);
    out["mixer"] = std::move(mixer);
    out["hardware"] = profile.hardware.is_object() ? profile.hardware : json::object();
    out["ui"] = profile.ui.is_object() ? profile.ui : json::object();
    return out;
}

Profile profileFromJson(const json& root) {
    if (!root.is_object()) throw ProfileError("profile root is not a JSON object");
    if (get<std::string>(root, "format", "") != kProfileFormatTag) throw ProfileError("not a REKA profile");
    const int version = get<int>(root, "version", 0);
    if (version < 1) throw ProfileError("missing or invalid profile version");
    if (version > kProfileFormatVersion)
        throw ProfileError("profile was written by a newer REKA (format v" + std::to_string(version) + ")");
    // Future format migrations hook in here: if (version == 1) migrateV1toV2(root) ...

    Profile profile;
    profile.version = version;
    profile.name = get<std::string>(root, "name", "Untitled");
    if (auto it = root.find("audio"); it != root.end() && it->is_object()) profile.audio = *it;
    if (auto it = root.find("hardware"); it != root.end() && it->is_object()) profile.hardware = *it;
    if (auto it = root.find("ui"); it != root.end() && it->is_object()) profile.ui = *it;

    const json& mixer = root.contains("mixer") ? root.at("mixer") : json::object();
    profile.mixer.panLaw = panLawFromString(get<std::string>(mixer, "panLaw", "constantPower"));

    std::set<StripId> ids;
    auto readStrips = [&](const char* key, StripKind kind, std::vector<StripState>& out) {
        auto it = mixer.find(key);
        if (it == mixer.end()) return;
        if (!it->is_array()) throw ProfileError(std::string("mixer.") + key + " must be an array");
        for (const json& entry : *it) {
            StripState s = stripFromJson(entry, kind);
            if (s.id == kInvalidStrip) throw ProfileError("strip '" + s.name + "' has no id");
            if (!ids.insert(s.id).second) throw ProfileError("duplicate strip id " + std::to_string(s.id));
            out.push_back(std::move(s));
        }
    };
    readStrips("inputs", StripKind::Input, profile.mixer.inputs);
    readStrips("buses", StripKind::Bus, profile.mixer.buses);

    if (auto it = mixer.find("routes"); it != mixer.end() && it->is_array()) {
        for (const json& entry : *it) {
            RouteState r;
            r.input = get<StripId>(entry, "from", kInvalidStrip);
            r.bus = get<StripId>(entry, "to", kInvalidStrip);
            if (ids.count(r.input) == 0 || ids.count(r.bus) == 0) continue;  // dangling: ignore
            r.enabled = get<bool>(entry, "enabled", true);
            r.gainDb = clampGainDb(get<float>(entry, "gainDb", 0.0f));
            r.pan = std::clamp(get<float>(entry, "pan", 0.0f), -1.0f, 1.0f);
            r.phaseInvert = get<bool>(entry, "phase", false);
            r.preFader = get<bool>(entry, "preFader", false);
            if (auto eq = entry.find("eq"); eq != entry.end()) bandsFromJson(*eq, r.eqEnabled, r.eq);
            profile.mixer.routes.push_back(r);
        }
    }
    return profile;
}

bool loadProfileFile(const std::filesystem::path& path, Profile& profile, std::string& error) {
    try {
        std::ifstream stream(path, std::ios::binary);
        if (!stream) {
            error = "cannot open " + path.string();
            return false;
        }
        profile = profileFromJson(json::parse(stream, nullptr, true, /*ignore_comments=*/true));
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

bool saveProfileFile(const std::filesystem::path& path, const Profile& profile, std::string& error) {
    try {
        const std::string text = profileToJson(profile).dump(2);
        std::filesystem::path temp = path;
        temp += ".tmp";
        {
            std::ofstream stream(temp, std::ios::binary | std::ios::trunc);
            if (!stream) {
                error = "cannot write " + temp.string();
                return false;
            }
            stream << text << '\n';
            if (!stream.good()) {
                error = "write failed for " + temp.string();
                return false;
            }
        }
        std::filesystem::rename(temp, path);  // atomic replace on NTFS and POSIX filesystems
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

// ---- capture -------------------------------------------------------------------------------------------

MixerState captureMixerState(const MixerModel& model) {
    MixerState state;
    state.panLaw = const_cast<MixerModel&>(model).engine().panLaw();

    auto capture = [&](StripKind kind, std::vector<StripState>& out) {
        for (StripId id : model.strips(kind)) {
            const StripInfo* info = model.strip(id);
            const StripParameters& p = *info->params;
            StripState s;
            s.id = id;
            s.kind = kind;
            s.name = info->name;
            s.colour = info->colour;
            s.binding = info->binding;
            s.gainDb = p.gainDb();
            s.pan = p.pan();
            s.mute = p.mute();
            s.solo = p.solo();
            s.fxBypass = p.fxBypass();
            s.phaseInvertLeft = p.phaseInvertLeft();
            s.phaseInvertRight = p.phaseInvertRight();
            s.mode = p.channelMode();
            s.eqEnabled = p.eq().enabled();
            s.eq = p.eq().bands();
            for (const InsertSlotPtr& slot : info->inserts) {
                InsertState insert;
                insert.descriptor = slot->processor->descriptor();
                insert.bypassed = slot->bypassed.load();
                insert.state = slot->processor->saveState();
                s.inserts.push_back(std::move(insert));
            }
            out.push_back(std::move(s));
        }
    };
    capture(StripKind::Input, state.inputs);
    capture(StripKind::Bus, state.buses);

    for (const CrosspointInfo& route : model.routes()) {
        const CrosspointParameters& p = *route.params;
        RouteState r;
        r.input = route.input;
        r.bus = route.bus;
        r.enabled = p.enabled();
        r.gainDb = p.gainDb();
        r.pan = p.pan();
        r.phaseInvert = p.phaseInvert();
        r.preFader = p.preFader();
        r.eqEnabled = p.eq().enabled();
        r.eq = p.eq().bands();
        state.routes.push_back(r);
    }
    return state;
}

// ---- hot apply -------------------------------------------------------------------------------------------

namespace {

void applyStripParameters(StripParameters& p, const StripState& s) {
    // Setters are cheap and smoothed on the audio thread; skip unchanged values to keep revisions quiet.
    if (p.gainDb() != s.gainDb) p.setGainDb(s.gainDb);
    if (p.pan() != s.pan) p.setPan(s.pan);
    if (p.mute() != s.mute) p.setMute(s.mute);
    if (p.solo() != s.solo) p.setSolo(s.solo);
    if (p.fxBypass() != s.fxBypass) p.setFxBypass(s.fxBypass);
    if (p.phaseInvertLeft() != s.phaseInvertLeft) p.setPhaseInvertLeft(s.phaseInvertLeft);
    if (p.phaseInvertRight() != s.phaseInvertRight) p.setPhaseInvertRight(s.phaseInvertRight);
    if (p.channelMode() != s.mode) p.setChannelMode(s.mode);
    if (p.eq().enabled() != s.eqEnabled) p.eq().setEnabled(s.eqEnabled);
    if (p.eq().bands() != s.eq) p.eq().setBands(s.eq);
}

std::vector<InsertSlotPtr> resolveInserts(MixerModel& model, const StripInfo& current, const StripState& desired,
                                          IInsertFactory* factory, ApplyReport& report) {
    std::vector<InsertSlotPtr> result;
    std::vector<bool> used(current.inserts.size(), false);
    for (const InsertState& want : desired.inserts) {
        InsertSlotPtr chosen;
        for (std::size_t i = 0; i < current.inserts.size() && !chosen; ++i) {
            const InsertSlotPtr& candidate = current.inserts[i];
            if (used[i] || candidate->processor->descriptor().pluginId != want.descriptor.pluginId) continue;
            if (candidate->processor->descriptor().sandboxed != want.descriptor.sandboxed) continue;
            if (candidate->processor->saveState() != want.state) continue;
            used[i] = true;
            chosen = candidate;
            ++report.insertsReused;
        }
        if (!chosen) {
            if (factory == nullptr) {
                report.warnings.push_back("no plugin host available for '" + want.descriptor.name + "'");
                continue;
            }
            std::string error;
            auto processor = factory->createInsert(want.descriptor, model.sampleRate(), model.maxBlockSize(), error);
            if (!processor) {
                report.warnings.push_back("could not load '" + want.descriptor.name + "': " + error);
                continue;
            }
            if (!want.state.empty() && !processor->restoreState(want.state))
                report.warnings.push_back("'" + want.descriptor.name + "' rejected its saved state");
            chosen = std::make_shared<InsertSlot>(std::move(processor));
            ++report.insertsCreated;
        }
        chosen->bypassed.store(want.bypassed);
        result.push_back(std::move(chosen));
    }
    return result;
}

}  // namespace

ApplyReport applyMixerState(MixerModel& model, const MixerState& state, IInsertFactory* factory) {
    ApplyReport report;
    auto batch = model.batch();  // one graph publication for the whole profile
    model.engine().setPanLaw(state.panLaw);

    std::set<StripId> wanted;
    for (const auto* list : {&state.inputs, &state.buses})
        for (const StripState& s : *list) wanted.insert(s.id);

    for (StripKind kind : {StripKind::Input, StripKind::Bus}) {
        for (StripId id : model.strips(kind)) {
            if (wanted.count(id) == 0) {
                model.removeStrip(id);
                ++report.stripsRemoved;
            }
        }
    }

    auto applyList = [&](const std::vector<StripState>& list, StripKind kind) {
        int index = 0;
        for (const StripState& desired : list) {
            const StripInfo* existing = model.strip(desired.id);
            if (existing != nullptr && existing->kind != kind) {
                model.removeStrip(desired.id);
                existing = nullptr;
            }
            if (existing == nullptr) {
                model.addStrip(kind, desired.name, desired.id, index);
                ++report.stripsAdded;
            } else {
                ++report.stripsUpdated;
                if (existing->name != desired.name) model.renameStrip(desired.id, desired.name);
                if (model.indexOf(desired.id) != index) model.moveStrip(desired.id, index);
            }
            const StripInfo* info = model.strip(desired.id);
            if (info->colour != desired.colour) model.setStripColour(desired.id, desired.colour);
            model.setBinding(desired.id, desired.binding);
            applyStripParameters(*info->params, desired);

            auto inserts = resolveInserts(model, *info, desired, factory, report);
            if (inserts != info->inserts) model.setInserts(desired.id, std::move(inserts));
            ++index;
        }
    };
    applyList(state.inputs, StripKind::Input);
    applyList(state.buses, StripKind::Bus);

    std::set<std::pair<StripId, StripId>> wantedRoutes;
    for (const RouteState& r : state.routes) wantedRoutes.insert({r.input, r.bus});
    for (const CrosspointInfo& existing : model.routes()) {
        if (wantedRoutes.count({existing.input, existing.bus}) == 0) {
            model.removeRoute(existing.input, existing.bus);
            ++report.routesChanged;
        }
    }
    for (const RouteState& r : state.routes) {
        auto params = model.route(r.input, r.bus);
        if (!params) {
            report.warnings.push_back("route " + std::to_string(r.input) + " -> " + std::to_string(r.bus) +
                                      " does not connect an input to a bus");
            continue;
        }
        if (params->gainDb() != r.gainDb) params->setGainDb(r.gainDb);
        if (params->pan() != r.pan) params->setPan(r.pan);
        if (params->phaseInvert() != r.phaseInvert) params->setPhaseInvert(r.phaseInvert);
        if (params->preFader() != r.preFader) params->setPreFader(r.preFader);
        if (params->eq().enabled() != r.eqEnabled) params->eq().setEnabled(r.eqEnabled);
        if (params->eq().bands() != r.eq) params->eq().setBands(r.eq);
        if (params->enabled() != r.enabled) {
            model.setRouteEnabled(r.input, r.bus, r.enabled);
            ++report.routesChanged;
        }
    }
    return report;
}

}  // namespace reka
