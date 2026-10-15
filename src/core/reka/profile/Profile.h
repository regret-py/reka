#pragma once

#include "reka/common/Types.h"
#include "reka/dsp/PanLaw.h"
#include "reka/dsp/ParametricEq.h"
#include "reka/engine/InsertProcessor.h"
#include "reka/engine/MixerModel.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace reka {

inline constexpr int kProfileFormatVersion = 1;
inline constexpr const char* kProfileFormatTag = "reka-profile";

struct InsertState {
    InsertDescriptor descriptor;
    bool bypassed = false;
    std::vector<std::uint8_t> state;  // opaque plugin chunk (base64 in the file)
};

struct StripState {
    StripId id = kInvalidStrip;
    StripKind kind = StripKind::Input;
    std::string name;
    std::uint32_t colour = 0xFF4DA3FFu;
    PortBinding binding;
    float gainDb = 0.0f;
    float pan = 0.0f;
    bool mute = false;
    bool solo = false;
    bool fxBypass = false;
    bool phaseInvertLeft = false;
    bool phaseInvertRight = false;
    ChannelMode mode = ChannelMode::Stereo;
    bool eqEnabled = true;
    EqBandArray eq = defaultEqBands();
    std::vector<InsertState> inserts;
};

struct RouteState {
    StripId input = kInvalidStrip;
    StripId bus = kInvalidStrip;
    bool enabled = true;
    float gainDb = 0.0f;
    float pan = 0.0f;
    bool phaseInvert = false;
    bool preFader = false;
    bool eqEnabled = true;
    EqBandArray eq = defaultEqBands();
};

struct MixerState {
    PanLaw panLaw = PanLaw::ConstantPower;
    std::vector<StripState> inputs;
    std::vector<StripState> buses;
    std::vector<RouteState> routes;
};

// A complete .reka profile. The mixer section is typed (owned by reka_core); the device, hardware and
// UI sections are JSON objects owned by the subsystems that interpret them, which keeps the core
// independent of JUCE while still round-tripping everything in one file.
struct Profile {
    int version = kProfileFormatVersion;
    std::string name = "Untitled";
    MixerState mixer;
    nlohmann::json audio = nlohmann::json::object();     // device manager / virtual endpoints
    nlohmann::json hardware = nlohmann::json::object();  // control surface + MIDI mapping
    nlohmann::json ui = nlohmann::json::object();        // theme, docking, strip templates
};

// ---- serialisation (throws ProfileError on invalid input) --------------------------------------------
class ProfileError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

nlohmann::json profileToJson(const Profile& profile);
Profile profileFromJson(const nlohmann::json& json);

// File helpers: UTF-8 JSON, atomic replace on save. Return false and fill `error` on failure.
bool loadProfileFile(const std::filesystem::path& path, Profile& profile, std::string& error);
bool saveProfileFile(const std::filesystem::path& path, const Profile& profile, std::string& error);

// ---- live model <-> profile -----------------------------------------------------------------------------

// Snapshot of the running mixer (message thread: plugin states are fetched synchronously).
MixerState captureMixerState(const MixerModel& model);

struct ApplyReport {
    int stripsAdded = 0;
    int stripsRemoved = 0;
    int stripsUpdated = 0;
    int insertsCreated = 0;
    int insertsReused = 0;
    int routesChanged = 0;
    std::vector<std::string> warnings;
};

// Hot-reload: diffs `state` against the running model and applies the difference in a single graph
// publication. Strips, processors and plugin instances whose identity and state are unchanged are kept,
// so audio keeps flowing and filter/plugin tails are not cut. Parameter changes go through the
// de-click smoothers. `factory` may be null (inserts are then skipped with a warning).
ApplyReport applyMixerState(MixerModel& model, const MixerState& state, IInsertFactory* factory);

// ---- colour helpers ("#RRGGBB" / "#AARRGGBB" <-> ARGB) ------------------------------------------------
std::string colourToHex(std::uint32_t argb);
bool colourFromHex(const std::string& text, std::uint32_t& argb);

}  // namespace reka
