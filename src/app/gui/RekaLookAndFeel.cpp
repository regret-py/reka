#include "gui/RekaLookAndFeel.h"

#include "reka/dsp/FaderLaw.h"

namespace reka::app {

RekaLookAndFeel::RekaLookAndFeel() { setTheme(Theme::defaults()); }

void RekaLookAndFeel::setTheme(const Theme& theme) {
    theme_ = theme;
    applyColourScheme();
}

void RekaLookAndFeel::applyColourScheme() {
    const auto& t = theme_;
    const auto bg = t.colour("background");
    const auto text = t.colour("text");
    const auto dim = t.colour("textDim");
    const auto accent = t.colour("accent");
    const auto button = t.colour("button");

    setColourScheme({bg, t.colour("panel").withAlpha(1.0f), button, t.colour("panelBorder"), text, accent, text, button, text});

    setColour(juce::ResizableWindow::backgroundColourId, bg);
    setColour(juce::DocumentWindow::textColourId, text);
    setColour(juce::Label::textColourId, text);
    setColour(juce::Label::textWhenEditingColourId, text);
    setColour(juce::Label::backgroundWhenEditingColourId, t.colour("knobTrack"));
    setColour(juce::Label::outlineWhenEditingColourId, accent);
    setColour(juce::TextButton::buttonColourId, button);
    setColour(juce::TextButton::buttonOnColourId, t.colour("buttonOn"));
    setColour(juce::TextButton::textColourOffId, dim);
    setColour(juce::TextButton::textColourOnId, juce::Colours::black.withAlpha(0.85f));
    setColour(juce::ToggleButton::textColourId, text);
    setColour(juce::ToggleButton::tickColourId, accent);
    setColour(juce::ToggleButton::tickDisabledColourId, dim);
    setColour(juce::ComboBox::backgroundColourId, button);
    setColour(juce::ComboBox::textColourId, text);
    setColour(juce::ComboBox::outlineColourId, t.colour("panelBorder"));
    setColour(juce::ComboBox::arrowColourId, dim);
    setColour(juce::PopupMenu::backgroundColourId, t.colour("background").brighter(0.08f));
    setColour(juce::PopupMenu::textColourId, text);
    setColour(juce::PopupMenu::highlightedBackgroundColourId, accent.withAlpha(0.35f));
    setColour(juce::PopupMenu::highlightedTextColourId, text);
    setColour(juce::PopupMenu::headerTextColourId, dim);
    setColour(juce::Slider::textBoxTextColourId, text);
    setColour(juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour(juce::Slider::thumbColourId, t.colour("faderCap"));
    setColour(juce::Slider::trackColourId, t.colour("faderTrack"));
    setColour(juce::Slider::rotarySliderFillColourId, t.colour("knobArc"));
    setColour(juce::Slider::rotarySliderOutlineColourId, t.colour("knobTrack"));
    setColour(juce::TextEditor::backgroundColourId, t.colour("knobTrack"));
    setColour(juce::TextEditor::textColourId, text);
    setColour(juce::TextEditor::outlineColourId, t.colour("panelBorder"));
    setColour(juce::TextEditor::focusedOutlineColourId, accent);
    setColour(juce::TextEditor::highlightColourId, accent.withAlpha(0.35f));
    setColour(juce::CaretComponent::caretColourId, accent);
    setColour(juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
    setColour(juce::ListBox::textColourId, text);
    setColour(juce::ScrollBar::thumbColourId, dim.withAlpha(0.5f));
    setColour(juce::TooltipWindow::backgroundColourId, bg.brighter(0.15f));
    setColour(juce::TooltipWindow::textColourId, text);
    setColour(juce::TabbedButtonBar::tabTextColourId, dim);
    setColour(juce::TabbedButtonBar::frontTextColourId, text);
    setColour(juce::TabbedComponent::backgroundColourId, juce::Colours::transparentBlack);
    setColour(juce::TabbedComponent::outlineColourId, juce::Colours::transparentBlack);
    setColour(juce::ProgressBar::foregroundColourId, accent);
    setColour(juce::ProgressBar::backgroundColourId, t.colour("knobTrack"));
    setColour(juce::AlertWindow::backgroundColourId, bg.brighter(0.05f));
    setColour(juce::AlertWindow::textColourId, text);
    setColour(juce::GroupComponent::textColourId, dim);
    setColour(juce::GroupComponent::outlineColourId, t.colour("panelBorder"));
}

// ---- shared helpers ----------------------------------------------------------------------------------------

void RekaLookAndFeel::drawBackground(juce::Graphics& g, juce::Rectangle<float> area) const {
    if (auto it = theme_.gradients.find("background"); it != theme_.gradients.end() && it->second.isValid())
        g.setGradientFill(it->second.toGradient(area));
    else
        g.setColour(theme_.colour("background"));
    g.fillRect(area);
}

void RekaLookAndFeel::drawPanel(juce::Graphics& g, juce::Component& component, juce::Rectangle<float> area, bool glass) const {
    const float radius = theme_.metrics.cornerRadius;
    juce::Path shape;
    shape.addRoundedRectangle(area, radius);

    if (glass && theme_.glass.enabled) {
        // Frosted glass: sample the pre-blurred window backdrop behind this component, then tint it.
        if (auto* provider = component.findParentComponentOfClass<BackdropProvider>()) {
            const auto& blurred = provider->blurredBackdrop();
            if (blurred.isValid()) {
                auto* providerComponent = dynamic_cast<juce::Component*>(provider);
                const auto origin = providerComponent != nullptr
                                        ? providerComponent->getLocalPoint(&component, area.getTopLeft().toInt())
                                        : juce::Point<int>();
                juce::Graphics::ScopedSaveState state(g);
                g.reduceClipRegion(shape);
                g.drawImageAt(blurred, juce::roundToInt(area.getX()) - origin.x, juce::roundToInt(area.getY()) - origin.y);
            }
        }
        auto tint = theme_.colour("panel");
        tint = tint.withAlpha(tint.getFloatAlpha() * theme_.glass.tintOpacity);
        if (auto it = theme_.gradients.find("panel"); it != theme_.gradients.end() && it->second.isValid()) {
            auto gradient = it->second.toGradient(area);
            for (int i = 0; i < gradient.getNumColours(); ++i)
                gradient.setColour(i, gradient.getColour(i).withMultipliedAlpha(theme_.glass.tintOpacity));
            g.setGradientFill(gradient);
        } else {
            g.setColour(tint);
        }
        g.fillPath(shape);

        // Top highlight + hairline border sell the glass effect.
        juce::ColourGradient highlight(juce::Colours::white.withAlpha(theme_.glass.highlight), area.getX(), area.getY(),
                                       juce::Colours::transparentWhite, area.getX(), area.getY() + 40.0f, false);
        g.setGradientFill(highlight);
        g.fillPath(shape);
        g.setColour(juce::Colours::white.withAlpha(theme_.glass.borderOpacity));
        g.strokePath(shape, juce::PathStrokeType(1.0f));
        return;
    }

    if (auto it = theme_.gradients.find("panel"); it != theme_.gradients.end() && it->second.isValid())
        g.setGradientFill(it->second.toGradient(area));
    else
        g.setColour(theme_.colour("panel"));
    g.fillPath(shape);
    g.setColour(theme_.colour("panelBorder"));
    g.strokePath(shape, juce::PathStrokeType(1.0f));
}

// ---- controls ---------------------------------------------------------------------------------------------

void RekaLookAndFeel::drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height, float sliderPos,
                                       float startAngle, float endAngle, juce::Slider& slider) {
    const auto bounds = juce::Rectangle<int>(x, y, width, height).toFloat().reduced(3.0f);
    const float radius = juce::jmin(bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const auto centre = bounds.getCentre();
    const float thickness = theme_.controls.knobArcThickness;
    const float angle = startAngle + sliderPos * (endAngle - startAngle);

    // Body
    const float bodyRadius = radius - thickness - 2.0f;
    juce::ColourGradient body(theme_.colour("knob").brighter(0.25f), centre.x, centre.y - bodyRadius,
                              theme_.colour("knob").darker(0.3f), centre.x, centre.y + bodyRadius, false);
    g.setGradientFill(body);
    g.fillEllipse(juce::Rectangle<float>(bodyRadius * 2.0f, bodyRadius * 2.0f).withCentre(centre));

    // Track and value arc. Bipolar sliders (pan) draw the arc from the centre.
    juce::Path track;
    track.addCentredArc(centre.x, centre.y, radius - thickness * 0.5f, radius - thickness * 0.5f, 0.0f, startAngle, endAngle, true);
    g.setColour(theme_.colour("knobTrack"));
    g.strokePath(track, juce::PathStrokeType(thickness, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    const bool bipolar = slider.getMinimum() < 0.0 && slider.getMaximum() > 0.0;
    const float from = bipolar ? (startAngle + endAngle) * 0.5f : startAngle;
    juce::Path arc;
    arc.addCentredArc(centre.x, centre.y, radius - thickness * 0.5f, radius - thickness * 0.5f, 0.0f, juce::jmin(from, angle),
                      juce::jmax(from, angle), true);
    const auto arcColour = slider.findColour(juce::Slider::rotarySliderFillColourId);
    g.setColour(slider.isEnabled() ? arcColour : arcColour.withSaturation(0.0f));
    g.strokePath(arc, juce::PathStrokeType(thickness, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // Pointer
    const juce::Point<float> tip = centre.getPointOnCircumference(bodyRadius * 0.75f, angle);
    g.setColour(theme_.colour("text"));
    g.drawLine(juce::Line<float>(centre.getPointOnCircumference(bodyRadius * 0.25f, angle), tip), 2.0f);
}

void RekaLookAndFeel::drawLinearSlider(juce::Graphics& g, int x, int y, int width, int height, float sliderPos,
                                       float minSliderPos, float maxSliderPos, juce::Slider::SliderStyle style,
                                       juce::Slider& slider) {
    if (style != juce::Slider::LinearVertical) {
        LookAndFeel_V4::drawLinearSlider(g, x, y, width, height, sliderPos, minSliderPos, maxSliderPos, style, slider);
        return;
    }
    const auto area = juce::Rectangle<int>(x, y, width, height).toFloat();
    const float trackWidth = theme_.controls.faderTrackWidth;
    const float capHeight = theme_.controls.faderCapHeight;
    const float top = area.getY() + capHeight * 0.5f;
    const float bottom = area.getBottom() - capHeight * 0.5f;

    // Scale ticks at console positions (shared FaderLaw: identical to the hardware faders).
    g.setFont(theme_.font(9.0f));
    for (float db : {12.0f, 6.0f, 0.0f, -6.0f, -10.0f, -20.0f, -30.0f, -40.0f, -60.0f}) {
        const float position = FaderLaw::dbToPosition(db);
        const float ty = bottom - position * (bottom - top);
        g.setColour(theme_.colour("textDim").withAlpha(std::abs(db) < 0.01f ? 0.9f : 0.45f));
        g.drawHorizontalLine(juce::roundToInt(ty), area.getX() + 2.0f, area.getCentreX() - trackWidth - 3.0f);
        g.drawHorizontalLine(juce::roundToInt(ty), area.getCentreX() + trackWidth + 3.0f, area.getRight() - 2.0f);
    }

    const auto track = juce::Rectangle<float>(trackWidth, bottom - top).withCentre({area.getCentreX(), (top + bottom) * 0.5f});
    g.setColour(theme_.colour("faderTrack"));
    g.fillRoundedRectangle(track, trackWidth * 0.5f);
    g.setColour(slider.findColour(juce::Slider::rotarySliderFillColourId).withAlpha(0.65f));
    g.fillRoundedRectangle(track.withTop(sliderPos), trackWidth * 0.5f);

    // Cap
    const float capWidth = juce::jmin(area.getWidth() - 4.0f, 34.0f);
    const auto cap = juce::Rectangle<float>(capWidth, capHeight).withCentre({area.getCentreX(), sliderPos});
    juce::ColourGradient capFill(theme_.colour("faderCap"), cap.getX(), cap.getY(), theme_.colour("faderCap").darker(0.45f),
                                 cap.getX(), cap.getBottom(), false);
    g.setGradientFill(capFill);
    g.fillRoundedRectangle(cap, theme_.metrics.controlRadius * 0.6f);
    g.setColour(juce::Colours::black.withAlpha(0.55f));
    g.drawHorizontalLine(juce::roundToInt(cap.getCentreY()), cap.getX() + 4.0f, cap.getRight() - 4.0f);
}

void RekaLookAndFeel::drawButtonBackground(juce::Graphics& g, juce::Button& button, const juce::Colour& backgroundColour,
                                           bool highlighted, bool down) {
    const auto area = button.getLocalBounds().toFloat().reduced(0.5f);
    const float radius = theme_.metrics.controlRadius;
    const bool on = button.getToggleState();
    auto fill = on ? button.findColour(juce::TextButton::buttonOnColourId) : backgroundColour;
    if (highlighted) fill = fill.brighter(0.12f);
    if (down) fill = fill.darker(0.15f);
    g.setColour(fill);
    g.fillRoundedRectangle(area, radius);
    if (on) {
        // Soft glow for lit buttons (mute/solo/fx), like illuminated hardware switches.
        g.setColour(fill.withAlpha(0.35f));
        g.drawRoundedRectangle(area.expanded(1.0f), radius + 1.0f, 2.0f);
    } else {
        g.setColour(theme_.colour("panelBorder"));
        g.drawRoundedRectangle(area, radius, 1.0f);
    }
}

void RekaLookAndFeel::drawButtonText(juce::Graphics& g, juce::TextButton& button, bool, bool) {
    g.setFont(getTextButtonFont(button, button.getHeight()));
    g.setColour(button.findColour(button.getToggleState() ? juce::TextButton::textColourOnId : juce::TextButton::textColourOffId)
                    .withMultipliedAlpha(button.isEnabled() ? 1.0f : 0.5f));
    g.drawFittedText(button.getButtonText(), button.getLocalBounds().reduced(3, 1), juce::Justification::centred, 1, 0.8f);
}

void RekaLookAndFeel::drawComboBox(juce::Graphics& g, int width, int height, bool, int, int, int, int, juce::ComboBox& box) {
    const auto area = juce::Rectangle<int>(width, height).toFloat().reduced(0.5f);
    g.setColour(box.findColour(juce::ComboBox::backgroundColourId));
    g.fillRoundedRectangle(area, theme_.metrics.controlRadius);
    g.setColour(box.findColour(juce::ComboBox::outlineColourId));
    g.drawRoundedRectangle(area, theme_.metrics.controlRadius, 1.0f);
    juce::Path arrow;
    const float cx = static_cast<float>(width) - 9.0f, cy = static_cast<float>(height) * 0.5f;
    arrow.addTriangle(cx - 3.5f, cy - 1.5f, cx + 3.5f, cy - 1.5f, cx, cy + 2.5f);
    g.setColour(box.findColour(juce::ComboBox::arrowColourId));
    g.fillPath(arrow);
}

juce::Font RekaLookAndFeel::getComboBoxFont(juce::ComboBox&) { return theme_.font(theme_.metrics.fontSize - 1.0f); }
juce::Font RekaLookAndFeel::getTextButtonFont(juce::TextButton&, int buttonHeight) {
    return theme_.font(juce::jmin(theme_.metrics.fontSize, static_cast<float>(buttonHeight) * 0.6f), true);
}
juce::Font RekaLookAndFeel::getLabelFont(juce::Label& label) {
    return label.getFont().getHeight() > 0.0f ? label.getFont() : theme_.font(theme_.metrics.fontSize);
}
juce::Font RekaLookAndFeel::getPopupMenuFont() { return theme_.font(theme_.metrics.fontSize); }

void RekaLookAndFeel::drawPopupMenuBackground(juce::Graphics& g, int width, int height) {
    g.fillAll(findColour(juce::PopupMenu::backgroundColourId));
    g.setColour(theme_.colour("panelBorder"));
    g.drawRect(0, 0, width, height);
}

void RekaLookAndFeel::drawScrollbar(juce::Graphics& g, juce::ScrollBar& bar, int x, int y, int width, int height,
                                    bool vertical, int thumbStart, int thumbSize, bool over, bool down) {
    juce::Rectangle<int> thumb = vertical ? juce::Rectangle<int>(x + 2, thumbStart, width - 4, thumbSize)
                                          : juce::Rectangle<int>(thumbStart, y + 2, thumbSize, height - 4);
    g.setColour(bar.findColour(juce::ScrollBar::thumbColourId).withMultipliedAlpha(over || down ? 1.0f : 0.6f));
    g.fillRoundedRectangle(thumb.toFloat(), 3.0f);
}

void RekaLookAndFeel::drawTooltip(juce::Graphics& g, const juce::String& text, int width, int height) {
    g.fillAll(findColour(juce::TooltipWindow::backgroundColourId));
    g.setColour(theme_.colour("panelBorder"));
    g.drawRect(0, 0, width, height);
    g.setColour(findColour(juce::TooltipWindow::textColourId));
    g.setFont(theme_.font(theme_.metrics.fontSize - 1.0f));
    g.drawFittedText(text, juce::Rectangle<int>(width, height).reduced(6, 3), juce::Justification::centredLeft, 4);
}

}  // namespace reka::app
