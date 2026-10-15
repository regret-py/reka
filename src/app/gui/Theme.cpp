#include "gui/Theme.h"

#include "reka/profile/Profile.h"

namespace reka::app {

namespace {
juce::Colour parseColour(const std::string& text, juce::Colour fallback) {
    std::uint32_t argb = 0;
    return colourFromHex(text, argb) ? juce::Colour(argb) : fallback;
}

std::string colourText(juce::Colour c) { return colourToHex(c.getARGB()); }

juce::String jstr(const nlohmann::json& json, const char* key, const juce::String& fallback) {
    if (!json.is_object()) return fallback;
    auto it = json.find(key);
    return it != json.end() && it->is_string() ? juce::String(it->get<std::string>()) : fallback;
}

template <typename T>
T jnum(const nlohmann::json& json, const char* key, T fallback) {
    if (!json.is_object()) return fallback;
    auto it = json.find(key);
    return it != json.end() && it->is_number() ? it->get<T>() : fallback;
}

bool jbool(const nlohmann::json& json, const char* key, bool fallback) {
    if (!json.is_object()) return fallback;
    auto it = json.find(key);
    return it != json.end() && it->is_boolean() ? it->get<bool>() : fallback;
}
}  // namespace

GainStyle gainStyleFromString(const juce::String& text) noexcept {
    return text.equalsIgnoreCase("knob") ? GainStyle::Knob : GainStyle::Fader;
}
juce::String toString(GainStyle style) { return style == GainStyle::Knob ? "knob" : "fader"; }

MeterStyle meterStyleFromString(const juce::String& text) noexcept {
    if (text == "peak") return MeterStyle::Peak;
    if (text == "rms") return MeterStyle::Rms;
    if (text == "vu") return MeterStyle::Vu;
    if (text == "lufs") return MeterStyle::Lufs;
    return MeterStyle::PeakRms;
}

juce::String toString(MeterStyle style) {
    switch (style) {
        case MeterStyle::Peak: return "peak";
        case MeterStyle::Rms: return "rms";
        case MeterStyle::Vu: return "vu";
        case MeterStyle::Lufs: return "lufs";
        case MeterStyle::PeakRms: break;
    }
    return "peakRms";
}

juce::ColourGradient GradientSpec::toGradient(juce::Rectangle<float> area) const {
    const float radians = juce::degreesToRadians(angleDegrees);
    const auto centre = area.getCentre();
    const float halfLength = 0.5f * (std::abs(area.getWidth() * std::cos(radians)) + std::abs(area.getHeight() * std::sin(radians)));
    const juce::Point<float> delta(std::cos(radians) * halfLength, std::sin(radians) * halfLength);
    juce::ColourGradient gradient(stops.front().second, centre - delta, stops.back().second, centre + delta, false);
    for (std::size_t i = 1; i + 1 < stops.size(); ++i) gradient.addColour(stops[i].first, stops[i].second);
    return gradient;
}

// ---- Theme -----------------------------------------------------------------------------------------------

juce::StringArray Theme::colourKeys() {
    return {"background", "backgroundTop", "backgroundBottom", "panel", "panelBorder", "panelHeader", "text", "textDim",
            "accent", "accentAlt", "mute", "solo", "fx", "fader", "faderTrack", "faderCap", "knob", "knobArc",
            "knobTrack", "button", "buttonOn", "meterLow", "meterMid", "meterHigh", "meterBackground", "meterPeak",
            "meterRms", "clip", "matrixOn", "matrixOff", "selection", "warning", "error", "success"};
}

Theme Theme::defaults() {
    Theme t;
    const std::pair<const char*, juce::uint32> palette[] = {
        {"background", 0xFF0E1117},      {"backgroundTop", 0xFF151B28},  {"backgroundBottom", 0xFF080A0F},
        {"panel", 0xCC1A2030},           {"panelBorder", 0x30FFFFFF},    {"panelHeader", 0xFF8A93A6},
        {"text", 0xFFE6EAF2},            {"textDim", 0xFF8A93A6},        {"accent", 0xFF4DA3FF},
        {"accentAlt", 0xFFB47CFF},       {"mute", 0xFFFF4D5E},           {"solo", 0xFFFFC94D},
        {"fx", 0xFF41D6A0},              {"fader", 0xFF2A3142},          {"faderTrack", 0xFF0A0D12},
        {"faderCap", 0xFFE6EAF2},        {"knob", 0xFF2A3142},           {"knobArc", 0xFF4DA3FF},
        {"knobTrack", 0xFF10141C},       {"button", 0xFF222A3A},         {"buttonOn", 0xFF4DA3FF},
        {"meterLow", 0xFF2EE6A6},        {"meterMid", 0xFFFFD24D},       {"meterHigh", 0xFFFF4D5E},
        {"meterBackground", 0xFF07090D}, {"meterPeak", 0xFFFFFFFF},      {"meterRms", 0x80FFFFFF},
        {"clip", 0xFFFF2D3D},            {"matrixOn", 0xFF4DA3FF},       {"matrixOff", 0xFF1C2230},
        {"selection", 0x404DA3FF},       {"warning", 0xFFFFB347},        {"error", 0xFFFF4D5E},
        {"success", 0xFF41D6A0}};
    for (const auto& [key, argb] : palette) t.colours[key] = juce::Colour(argb);
    t.gradients["background"] = {90.0f, {{0.0f, juce::Colour(0xFF151B28)}, {1.0f, juce::Colour(0xFF080A0F)}}};
    t.gradients["panel"] = {90.0f, {{0.0f, juce::Colour(0xE61E2536)}, {1.0f, juce::Colour(0xE6141924)}}};
    return t;
}

juce::Colour Theme::colour(const juce::String& key) const {
    if (auto it = colours.find(key); it != colours.end()) return it->second;
    static const Theme fallback = defaults();
    if (auto it = fallback.colours.find(key); it != fallback.colours.end()) return it->second;
    return juce::Colours::magenta;  // a missing key is obvious on screen
}

juce::Font Theme::font(float height, bool bold) const {
    juce::FontOptions options = fonts.family.isNotEmpty() ? juce::FontOptions(fonts.family, height, juce::Font::plain)
                                                          : juce::FontOptions(height);
    if (bold) options = options.withStyle("Bold");
    return juce::Font(options);
}

juce::Font Theme::monoFont(float height) const {
    const juce::String family = fonts.monoFamily.isNotEmpty() ? fonts.monoFamily : juce::Font::getDefaultMonospacedFontName();
    return juce::Font(juce::FontOptions(family, height, juce::Font::plain));
}

Theme Theme::fromJson(const nlohmann::json& json, const Theme& base) {
    Theme t = base;
    if (!json.is_object()) return t;
    t.id = jstr(json, "id", t.id);
    t.name = jstr(json, "name", t.name);
    t.dark = jbool(json, "dark", t.dark);

    if (auto colours = json.find("colours"); colours != json.end() && colours->is_object())
        for (const auto& [key, value] : colours->items())
            if (value.is_string()) t.colours[juce::String(key)] = parseColour(value.get<std::string>(), t.colour(juce::String(key)));

    if (auto gradients = json.find("gradients"); gradients != json.end() && gradients->is_object()) {
        for (const auto& [key, value] : gradients->items()) {
            GradientSpec spec;
            spec.angleDegrees = jnum(value, "angle", 90.0f);
            for (const auto& stop : value.value("stops", nlohmann::json::array()))
                if (stop.is_array() && stop.size() == 2 && stop[0].is_number() && stop[1].is_string())
                    spec.stops.emplace_back(stop[0].get<float>(), parseColour(stop[1].get<std::string>(), juce::Colours::black));
            if (spec.isValid()) t.gradients[juce::String(key)] = spec;
        }
    }

    const auto& glass = json.contains("glass") ? json.at("glass") : nlohmann::json();
    t.glass.enabled = jbool(glass, "enabled", t.glass.enabled);
    t.glass.blurRadius = juce::jlimit(0.0f, 64.0f, jnum(glass, "blurRadius", t.glass.blurRadius));
    t.glass.tintOpacity = juce::jlimit(0.0f, 1.0f, jnum(glass, "tintOpacity", t.glass.tintOpacity));
    t.glass.borderOpacity = juce::jlimit(0.0f, 1.0f, jnum(glass, "borderOpacity", t.glass.borderOpacity));
    t.glass.highlight = juce::jlimit(0.0f, 1.0f, jnum(glass, "highlight", t.glass.highlight));
    t.glass.shadow = juce::jlimit(0.0f, 1.0f, jnum(glass, "shadow", t.glass.shadow));

    const auto& metrics = json.contains("metrics") ? json.at("metrics") : nlohmann::json();
    t.metrics.cornerRadius = juce::jlimit(0.0f, 32.0f, jnum(metrics, "cornerRadius", t.metrics.cornerRadius));
    t.metrics.stripWidth = juce::jlimit(64, 240, jnum(metrics, "stripWidth", t.metrics.stripWidth));
    t.metrics.stripSpacing = juce::jlimit(0, 32, jnum(metrics, "stripSpacing", t.metrics.stripSpacing));
    t.metrics.panelPadding = juce::jlimit(0, 32, jnum(metrics, "panelPadding", t.metrics.panelPadding));
    t.metrics.fontSize = juce::jlimit(8.0f, 24.0f, jnum(metrics, "fontSize", t.metrics.fontSize));
    t.metrics.headerFontSize = juce::jlimit(8.0f, 32.0f, jnum(metrics, "headerFontSize", t.metrics.headerFontSize));
    t.metrics.controlRadius = juce::jlimit(0.0f, 16.0f, jnum(metrics, "controlRadius", t.metrics.controlRadius));

    const auto& fonts = json.contains("fonts") ? json.at("fonts") : nlohmann::json();
    t.fonts.family = jstr(fonts, "family", t.fonts.family);
    t.fonts.monoFamily = jstr(fonts, "monoFamily", t.fonts.monoFamily);

    const auto& meters = json.contains("meters") ? json.at("meters") : nlohmann::json();
    t.meters.style = meterStyleFromString(jstr(meters, "style", toString(t.meters.style)));
    t.meters.decayDbPerSecond = juce::jlimit(1.0f, 200.0f, jnum(meters, "decayDbPerSecond", t.meters.decayDbPerSecond));
    t.meters.peakHoldMs = juce::jlimit(0, 10000, jnum(meters, "peakHoldMs", t.meters.peakHoldMs));
    t.meters.floorDb = juce::jlimit(-96.0f, -20.0f, jnum(meters, "floorDb", t.meters.floorDb));
    t.meters.midDb = jnum(meters, "midDb", t.meters.midDb);
    t.meters.highDb = jnum(meters, "highDb", t.meters.highDb);
    t.meters.gradient = jbool(meters, "gradient", t.meters.gradient);
    t.meters.showLufs = jbool(meters, "showLufs", t.meters.showLufs);
    t.meters.segments = juce::jlimit(0, 64, jnum(meters, "segments", t.meters.segments));

    const auto& controls = json.contains("controls") ? json.at("controls") : nlohmann::json();
    t.controls.gainStyle = gainStyleFromString(jstr(controls, "gainStyle", toString(t.controls.gainStyle)));
    t.controls.knobArcThickness = juce::jlimit(1.0f, 12.0f, jnum(controls, "knobArcThickness", t.controls.knobArcThickness));
    t.controls.faderCapHeight = juce::jlimit(10.0f, 60.0f, jnum(controls, "faderCapHeight", t.controls.faderCapHeight));
    t.controls.faderTrackWidth = juce::jlimit(1.0f, 16.0f, jnum(controls, "faderTrackWidth", t.controls.faderTrackWidth));

    const auto& window = json.contains("window") ? json.at("window") : nlohmann::json();
    t.windowBackdrop = jstr(window, "backdrop", t.windowBackdrop);
    return t;
}

nlohmann::json Theme::toJson() const {
    nlohmann::json json;
    json["id"] = id.toStdString();
    json["name"] = name.toStdString();
    json["dark"] = dark;
    auto c = nlohmann::json::object();
    for (const auto& [key, value] : colours) c[key.toStdString()] = colourText(value);
    json["colours"] = c;
    auto g = nlohmann::json::object();
    for (const auto& [key, spec] : gradients) {
        auto stops = nlohmann::json::array();
        for (const auto& [position, colour] : spec.stops) stops.push_back({position, colourText(colour)});
        g[key.toStdString()] = {{"angle", spec.angleDegrees}, {"stops", stops}};
    }
    json["gradients"] = g;
    json["glass"] = {{"enabled", glass.enabled}, {"blurRadius", glass.blurRadius}, {"tintOpacity", glass.tintOpacity},
                     {"borderOpacity", glass.borderOpacity}, {"highlight", glass.highlight}, {"shadow", glass.shadow}};
    json["metrics"] = {{"cornerRadius", metrics.cornerRadius}, {"stripWidth", metrics.stripWidth},
                       {"stripSpacing", metrics.stripSpacing}, {"panelPadding", metrics.panelPadding},
                       {"fontSize", metrics.fontSize}, {"headerFontSize", metrics.headerFontSize},
                       {"controlRadius", metrics.controlRadius}};
    json["fonts"] = {{"family", fonts.family.toStdString()}, {"monoFamily", fonts.monoFamily.toStdString()}};
    json["meters"] = {{"style", toString(meters.style).toStdString()}, {"decayDbPerSecond", meters.decayDbPerSecond},
                      {"peakHoldMs", meters.peakHoldMs}, {"floorDb", meters.floorDb}, {"midDb", meters.midDb},
                      {"highDb", meters.highDb}, {"gradient", meters.gradient}, {"showLufs", meters.showLufs},
                      {"segments", meters.segments}};
    json["controls"] = {{"gainStyle", toString(controls.gainStyle).toStdString()},
                        {"knobArcThickness", controls.knobArcThickness}, {"faderCapHeight", controls.faderCapHeight},
                        {"faderTrackWidth", controls.faderTrackWidth}};
    json["window"] = {{"backdrop", windowBackdrop.toStdString()}};
    return json;
}

// ---- UiLayout ----------------------------------------------------------------------------------------------

UiLayout UiLayout::defaults() {
    UiLayout l;
    l.panels["inputs"] = {"center", true, 0};
    l.panels["buses"] = {"bottom", true, 360};
    l.panels["matrix"] = {"right", false, 420};
    l.panels["plugins"] = {"left", false, 280};
    l.stripTemplate = {"header", "source", "mode", "eq", "inserts", "sends", "pan", "gain", "buttons"};
    return l;
}

UiLayout UiLayout::fromJson(const nlohmann::json& json) {
    UiLayout l = defaults();
    if (!json.is_object()) return l;
    l.theme = jstr(json, "theme", l.theme);
    if (auto panels = json.find("panels"); panels != json.end() && panels->is_object()) {
        for (const auto& [key, value] : panels->items()) {
            PanelLayout p = l.panels.count(juce::String(key)) ? l.panels[juce::String(key)] : PanelLayout{};
            p.dock = jstr(value, "dock", p.dock);
            p.visible = jbool(value, "visible", p.visible);
            p.size = juce::jlimit(120, 2000, jnum(value, "size", p.size == 0 ? 320 : p.size));
            l.panels[juce::String(key)] = p;
        }
    }
    if (auto strip = json.find("strip"); strip != json.end() && strip->is_object()) {
        if (auto tmpl = strip->find("template"); tmpl != strip->end() && tmpl->is_array()) {
            l.stripTemplate.clear();
            for (const auto& item : *tmpl)
                if (item.is_string()) l.stripTemplate.add(juce::String(item.get<std::string>()));
        }
        l.gainStyle = gainStyleFromString(jstr(*strip, "gainStyle", toString(l.gainStyle)));
        l.stripWidth = juce::jlimit(64, 240, jnum(*strip, "width", l.stripWidth));
    }
    if (auto overrides = json.find("stripOverrides"); overrides != json.end() && overrides->is_object()) {
        for (const auto& [key, value] : overrides->items()) {
            StripOverride o;
            if (value.contains("gainStyle")) o.gainStyle = gainStyleFromString(jstr(value, "gainStyle", "fader"));
            o.width = jnum(value, "width", 0);
            o.hidden = jbool(value, "hidden", false);
            l.overrides[static_cast<StripId>(juce::String(key).getLargeIntValue())] = o;
        }
    }
    return l;
}

nlohmann::json UiLayout::toJson() const {
    nlohmann::json json;
    json["theme"] = theme.toStdString();
    auto panelsJson = nlohmann::json::object();
    for (const auto& [key, p] : panels)
        panelsJson[key.toStdString()] = {{"dock", p.dock.toStdString()}, {"visible", p.visible}, {"size", p.size}};
    json["panels"] = panelsJson;
    auto tmpl = nlohmann::json::array();
    for (const auto& item : stripTemplate) tmpl.push_back(item.toStdString());
    json["strip"] = {{"template", tmpl}, {"gainStyle", toString(gainStyle).toStdString()}, {"width", stripWidth}};
    auto o = nlohmann::json::object();
    for (const auto& [id, value] : overrides) {
        nlohmann::json entry = {{"width", value.width}, {"hidden", value.hidden}};
        if (value.gainStyle) entry["gainStyle"] = toString(*value.gainStyle).toStdString();
        o[std::to_string(id)] = entry;
    }
    json["stripOverrides"] = o;
    return json;
}

GainStyle UiLayout::gainStyleFor(StripId id) const {
    if (auto it = overrides.find(id); it != overrides.end() && it->second.gainStyle) return *it->second.gainStyle;
    return gainStyle;
}

int UiLayout::widthFor(StripId id) const {
    if (auto it = overrides.find(id); it != overrides.end() && it->second.width > 0) return it->second.width;
    return stripWidth;
}

bool UiLayout::isHidden(StripId id) const {
    auto it = overrides.find(id);
    return it != overrides.end() && it->second.hidden;
}

}  // namespace reka::app
