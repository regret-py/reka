#pragma once

#include "gui/Theme.h"
#include "reka/engine/Parameters.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

namespace reka::app {

// Fader or rotary knob for a gain value, using the FaderLaw shared with physical faders and MIDI.
// Works for strip gain and for routing-matrix crosspoint gain through getter/setter callbacks.
class GainControl final : public juce::Component {
public:
    GainControl(std::function<float()> getDb, std::function<void(float)> setDb, GainStyle style);
    static std::unique_ptr<GainControl> forStrip(std::shared_ptr<StripParameters> params, GainStyle style);

    void setStyle(GainStyle style);
    GainStyle style() const noexcept { return style_; }
    void refresh();  // pull from the parameter (hardware / MIDI / profile changes)
    void setAccentColour(juce::Colour colour);

    void resized() override;

private:
    std::function<float()> getDb_;
    std::function<void(float)> setDb_;
    std::shared_ptr<void> keepAlive_;
    GainStyle style_;
    juce::Slider slider_;
    juce::Label value_;
};

// Pan / balance knob bound to a StripParameters.
class PanKnob final : public juce::Component {
public:
    PanKnob(std::function<float()> getPan, std::function<void(float)> setPan);
    void refresh();
    void resized() override;
    void setAccentColour(juce::Colour colour);

private:
    std::function<float()> getPan_;
    std::function<void(float)> setPan_;
    juce::Slider slider_;
};

}  // namespace reka::app
