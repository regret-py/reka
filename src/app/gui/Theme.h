#pragma once

#include "reka/common/Types.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <nlohmann/json.hpp>

#include <map>
#include <optional>
#include <vector>

namespace reka::app {

enum class GainStyle { Fader, Knob };
enum class MeterStyle { Peak, Rms, PeakRms, Vu, Lufs };

GainStyle gainStyleFromString(const juce::String& text) noexcept;
juce::String toString(GainStyle style);
MeterStyle meterStyleFromString(const juce::String& text) noexcept;
juce::String toString(MeterStyle style);

struct GradientSpec {
    float angleDegrees = 90.0f;  // 90 = top -> bottom
    std::vector<std::pair<float, juce::Colour>> stops;

    bool isValid() const noexcept { return stops.size() >= 2; }
    juce::ColourGradient toGradient(juce::Rectangle<float> area) const;
};

// A complete visual theme, loaded from JSON (see docs/THEMES.md). Every field has a default, so a theme
// file only needs to override what it changes; unknown keys are ignored.
struct Theme {
    juce::String id = "midnight-glass";
    juce::String name = "Midnight Glass";
    bool dark = true;

    std::map<juce::String, juce::Colour> colours;
    std::map<juce::String, GradientSpec> gradients;

    struct Glass {
        bool enabled = true;
        float blurRadius = 18.0f;     // px, applied to the backdrop behind panels
        float tintOpacity = 0.55f;    // panel fill opacity over the blurred backdrop
        float borderOpacity = 0.18f;
        float highlight = 0.08f;      // top inner highlight strength
        float shadow = 0.35f;
    } glass;

    struct Metrics {
        float cornerRadius = 10.0f;
        int stripWidth = 104;
        int stripSpacing = 6;
        int panelPadding = 8;
        float fontSize = 13.0f;
        float headerFontSize = 15.0f;
        float controlRadius = 6.0f;
    } metrics;

    struct Fonts {
        juce::String family;       // empty = platform default UI font
        juce::String monoFamily;
    } fonts;

    struct Meters {
        MeterStyle style = MeterStyle::PeakRms;
        float decayDbPerSecond = 24.0f;
        int peakHoldMs = 1500;
        float floorDb = -60.0f;
        float midDb = -12.0f;
        float highDb = -3.0f;
        bool gradient = true;
        bool showLufs = true;
        int segments = 0;  // 0 = continuous bar, >0 = LED segments
    } meters;

    struct Controls {
        GainStyle gainStyle = GainStyle::Fader;
        float knobArcThickness = 3.0f;
        float faderCapHeight = 26.0f;
        float faderTrackWidth = 4.0f;
    } controls;

    juce::String windowBackdrop = "none";  // none | mica | acrylic | micaAlt (Windows 11)

    juce::Colour colour(const juce::String& key) const;
    juce::Font font(float height, bool bold = false) const;
    juce::Font monoFont(float height) const;

    static Theme defaults();
    static Theme fromJson(const nlohmann::json& json, const Theme& base = defaults());
    nlohmann::json toJson() const;

    // Every colour key a theme may define (used by the theme editor).
    static juce::StringArray colourKeys();
};

// ---- layout ---------------------------------------------------------------------------------------------

struct PanelLayout {
    juce::String dock = "center";  // left | center | right | bottom
    bool visible = true;
    int size = 320;                // width for left/right, height for bottom
};

struct StripOverride {
    std::optional<GainStyle> gainStyle;
    int width = 0;
    bool hidden = false;
};

// The "ui" section of a profile: which panels exist and where, how strips are composed.
struct UiLayout {
    juce::String theme = "midnight-glass";
    std::map<juce::String, PanelLayout> panels;
    juce::StringArray stripTemplate;
    GainStyle gainStyle = GainStyle::Fader;
    int stripWidth = 104;
    std::map<StripId, StripOverride> overrides;

    static UiLayout defaults();
    static UiLayout fromJson(const nlohmann::json& json);
    nlohmann::json toJson() const;

    GainStyle gainStyleFor(StripId id) const;
    int widthFor(StripId id) const;
    bool isHidden(StripId id) const;
};

}  // namespace reka::app
