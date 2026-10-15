#pragma once

#include "gui/Theme.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace reka::app {

// Supplies the pre-blurred window backdrop that glass panels sample from (implemented by MainComponent).
class BackdropProvider {
public:
    virtual ~BackdropProvider() = default;
    virtual const juce::Image& blurredBackdrop() const = 0;
};

// Theme-driven look and feel. Every colour, radius, font and control style comes from the active Theme,
// so a new visual design is mostly a JSON file plus, at most, a few overrides here.
class RekaLookAndFeel final : public juce::LookAndFeel_V4 {
public:
    RekaLookAndFeel();

    void setTheme(const Theme& theme);
    const Theme& theme() const noexcept { return theme_; }

    // Shared painting helpers used by custom components.
    void drawPanel(juce::Graphics& g, juce::Component& component, juce::Rectangle<float> area, bool glass) const;
    void drawBackground(juce::Graphics& g, juce::Rectangle<float> area) const;

    // ---- juce::LookAndFeel overrides ----
    void drawRotarySlider(juce::Graphics&, int x, int y, int width, int height, float sliderPos, float startAngle,
                          float endAngle, juce::Slider&) override;
    void drawLinearSlider(juce::Graphics&, int x, int y, int width, int height, float sliderPos, float minSliderPos,
                          float maxSliderPos, juce::Slider::SliderStyle, juce::Slider&) override;
    void drawButtonBackground(juce::Graphics&, juce::Button&, const juce::Colour& backgroundColour,
                              bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;
    void drawButtonText(juce::Graphics&, juce::TextButton&, bool highlighted, bool down) override;
    void drawComboBox(juce::Graphics&, int width, int height, bool isButtonDown, int buttonX, int buttonY, int buttonW,
                      int buttonH, juce::ComboBox&) override;
    juce::Font getComboBoxFont(juce::ComboBox&) override;
    juce::Font getTextButtonFont(juce::TextButton&, int buttonHeight) override;
    juce::Font getLabelFont(juce::Label&) override;
    juce::Font getPopupMenuFont() override;
    void drawPopupMenuBackground(juce::Graphics&, int width, int height) override;
    void drawScrollbar(juce::Graphics&, juce::ScrollBar&, int x, int y, int width, int height, bool isScrollbarVertical,
                       int thumbStartPosition, int thumbSize, bool isMouseOver, bool isMouseDown) override;
    void drawTooltip(juce::Graphics&, const juce::String& text, int width, int height) override;

private:
    void applyColourScheme();
    Theme theme_;
};

}  // namespace reka::app
