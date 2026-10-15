#include "gui/components/GainControl.h"

#include "reka/dsp/FaderLaw.h"

namespace reka::app {

namespace {
juce::String formatDb(float db) {
    if (db <= kSilenceDb + 0.01f) return "-inf";
    return (db > 0.05f ? "+" : "") + juce::String(db, 1);
}
}  // namespace

GainControl::GainControl(std::function<float()> getDb, std::function<void(float)> setDb, GainStyle style)
    : getDb_(std::move(getDb)), setDb_(std::move(setDb)), style_(style) {
    slider_.setRange(0.0, 1.0, 0.0);
    slider_.setDoubleClickReturnValue(true, FaderLaw::dbToPosition(0.0f));
    slider_.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    slider_.setPopupDisplayEnabled(false, false, nullptr);
    slider_.setVelocityBasedMode(false);
    slider_.setScrollWheelEnabled(true);
    slider_.onValueChange = [this] {
        const float db = FaderLaw::positionToDb(static_cast<float>(slider_.getValue()));
        setDb_(db);
        value_.setText(formatDb(db), juce::dontSendNotification);
    };
    addAndMakeVisible(slider_);

    value_.setJustificationType(juce::Justification::centred);
    value_.setEditable(false, true);  // double-click to type a dB value
    value_.onTextChange = [this] {
        const auto text = value_.getText().trim();
        const float db = text.startsWithIgnoreCase("-inf") ? kSilenceDb : juce::jlimit(kSilenceDb, kMaxGainDb, text.getFloatValue());
        setDb_(db);
        refresh();
    };
    addAndMakeVisible(value_);
    setStyle(style);
    refresh();
}

std::unique_ptr<GainControl> GainControl::forStrip(std::shared_ptr<StripParameters> params, GainStyle style) {
    auto* raw = params.get();
    auto control = std::make_unique<GainControl>([raw] { return raw->gainDb(); }, [raw](float db) { raw->setGainDb(db); }, style);
    control->keepAlive_ = std::move(params);
    return control;
}

void GainControl::setStyle(GainStyle style) {
    style_ = style;
    slider_.setSliderStyle(style == GainStyle::Fader ? juce::Slider::LinearVertical : juce::Slider::RotaryHorizontalVerticalDrag);
    slider_.setMouseDragSensitivity(style == GainStyle::Fader ? 250 : 180);
    resized();
}

void GainControl::setAccentColour(juce::Colour colour) {
    slider_.setColour(juce::Slider::rotarySliderFillColourId, colour);
    repaint();
}

void GainControl::refresh() {
    if (slider_.isMouseButtonDown()) return;  // never fight the user
    const float db = getDb_();
    const double position = FaderLaw::dbToPosition(db);
    if (std::abs(slider_.getValue() - position) > 1.0e-4) slider_.setValue(position, juce::dontSendNotification);
    if (!value_.isBeingEdited()) value_.setText(formatDb(db), juce::dontSendNotification);
}

void GainControl::resized() {
    auto area = getLocalBounds();
    value_.setBounds(area.removeFromBottom(18));
    slider_.setBounds(area);
}

// ---- PanKnob -----------------------------------------------------------------------------------------------

PanKnob::PanKnob(std::function<float()> getPan, std::function<void(float)> setPan)
    : getPan_(std::move(getPan)), setPan_(std::move(setPan)) {
    slider_.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    slider_.setRange(-1.0, 1.0, 0.0);
    slider_.setDoubleClickReturnValue(true, 0.0);
    slider_.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    slider_.setPopupDisplayEnabled(true, true, nullptr);
    slider_.textFromValueFunction = [](double v) {
        if (std::abs(v) < 0.005) return juce::String("C");
        return (v < 0.0 ? "L" : "R") + juce::String(juce::roundToInt(std::abs(v) * 100.0));
    };
    slider_.onValueChange = [this] { setPan_(static_cast<float>(slider_.getValue())); };
    addAndMakeVisible(slider_);
    refresh();
}

void PanKnob::refresh() {
    if (slider_.isMouseButtonDown()) return;
    const double pan = getPan_();
    if (std::abs(slider_.getValue() - pan) > 1.0e-4) slider_.setValue(pan, juce::dontSendNotification);
}

void PanKnob::resized() { slider_.setBounds(getLocalBounds()); }

void PanKnob::setAccentColour(juce::Colour colour) { slider_.setColour(juce::Slider::rotarySliderFillColourId, colour); }

}  // namespace reka::app
