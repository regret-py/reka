#include "gui/components/ThemeEditorPanel.h"

#include "gui/RekaGuiEngine.h"

namespace reka::app {

namespace {
// Colour picker shown in a call-out; pushes every change straight back to the editor.
class ColourPicker final : public juce::Component, private juce::ChangeListener {
public:
    ColourPicker(juce::Colour initial, std::function<void(juce::Colour)> onChange) : onChange_(std::move(onChange)) {
        selector_.setCurrentColour(initial, juce::dontSendNotification);
        selector_.addChangeListener(this);
        addAndMakeVisible(selector_);
        setSize(300, 320);
    }
    void resized() override { selector_.setBounds(getLocalBounds()); }

private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override { onChange_(selector_.getCurrentColour()); }
    juce::ColourSelector selector_{juce::ColourSelector::showColourAtTop | juce::ColourSelector::showSliders |
                                   juce::ColourSelector::showColourspace | juce::ColourSelector::showAlphaChannel};
    std::function<void(juce::Colour)> onChange_;
};
}  // namespace

ThemeEditorPanel::ThemeEditorPanel(RekaGuiEngine& gui) : gui_(gui), keys_(Theme::colourKeys()) {
    for (const auto& id : gui_.themeIds()) themes_.addItem(gui_.themeName(id), themes_.getNumItems() + 1);
    themes_.onChange = [this] {
        const auto ids = gui_.themeIds();
        const int index = themes_.getSelectedItemIndex();
        if (index >= 0 && index < ids.size()) {
            gui_.setTheme(ids[index]);
            loadFrom(gui_.theme());
        }
    };
    colours_.setRowHeight(24);

    auto setupSlider = [this](juce::Slider& s, double min, double max, double step) {
        s.setRange(min, max, step);
        s.setTextBoxStyle(juce::Slider::TextBoxRight, false, 56, 20);
        s.onValueChange = [this] { preview(); };
        addAndMakeVisible(s);
    };
    setupSlider(blur_, 0, 48, 1);
    setupSlider(tint_, 0, 1, 0.01);
    setupSlider(radius_, 0, 24, 1);
    setupSlider(stripWidth_, 72, 200, 1);
    setupSlider(fontSize_, 9, 20, 0.5);
    setupSlider(decay_, 4, 120, 1);
    setupSlider(hold_, 0, 5000, 50);
    setupSlider(segments_, 0, 48, 1);

    for (auto* style : {"peak", "rms", "peakRms", "vu", "lufs"}) meterStyle_.addItem(style, meterStyle_.getNumItems() + 1);
    gainStyle_.addItem("fader", 1);
    gainStyle_.addItem("knob", 2);
    for (auto* backdrop : {"none", "mica", "acrylic", "micaAlt"}) backdrop_.addItem(backdrop, backdrop_.getNumItems() + 1);
    for (auto* combo : {&meterStyle_, &gainStyle_, &backdrop_}) {
        combo->onChange = [this] { preview(); };
        addAndMakeVisible(combo);
    }
    for (auto* toggle : {&glass_, &gradientMeters_, &showLufs_}) {
        toggle->onClick = [this] { preview(); };
        addAndMakeVisible(toggle);
    }

    save_.onClick = [this] {
        preview();
        working_.name = name_.getText().trim().isEmpty() ? "My theme" : name_.getText().trim();
        working_.id = working_.name.toLowerCase().replaceCharacters(" ", "-").retainCharacters("abcdefghijklmnopqrstuvwxyz0123456789-");
        juce::String error;
        if (!gui_.saveUserTheme(working_, error))
            juce::AlertWindow::showAsync(juce::MessageBoxOptions::makeOptionsOk(juce::MessageBoxIconType::WarningIcon, "Theme", error), nullptr);
    };
    revert_.onClick = [this] {
        gui_.setTheme(gui_.layout().theme);
        loadFrom(gui_.theme());
    };
    for (auto* c : std::initializer_list<juce::Component*>{&themes_, &colours_, &name_, &save_, &revert_}) addAndMakeVisible(c);

    for (auto* text : {"Theme", "Backdrop blur", "Glass tint", "Corner radius", "Strip width", "Font size", "Meter decay dB/s",
                       "Peak hold ms", "LED segments", "Meter style", "Gain control", "Window backdrop", "Name"}) {
        auto* l = labels_.add(new juce::Label({}, text));
        l->setColour(juce::Label::textColourId, gui_.theme().colour("textDim"));
        addAndMakeVisible(l);
    }

    loadFrom(gui_.theme());
    const auto ids = gui_.themeIds();
    themes_.setSelectedItemIndex(ids.indexOf(gui_.theme().id), juce::dontSendNotification);
    setSize(860, 600);
}

void ThemeEditorPanel::loadFrom(const Theme& theme) {
    working_ = theme;
    glass_.setToggleState(theme.glass.enabled, juce::dontSendNotification);
    gradientMeters_.setToggleState(theme.meters.gradient, juce::dontSendNotification);
    showLufs_.setToggleState(theme.meters.showLufs, juce::dontSendNotification);
    blur_.setValue(theme.glass.blurRadius, juce::dontSendNotification);
    tint_.setValue(theme.glass.tintOpacity, juce::dontSendNotification);
    radius_.setValue(theme.metrics.cornerRadius, juce::dontSendNotification);
    stripWidth_.setValue(theme.metrics.stripWidth, juce::dontSendNotification);
    fontSize_.setValue(theme.metrics.fontSize, juce::dontSendNotification);
    decay_.setValue(theme.meters.decayDbPerSecond, juce::dontSendNotification);
    hold_.setValue(theme.meters.peakHoldMs, juce::dontSendNotification);
    segments_.setValue(theme.meters.segments, juce::dontSendNotification);
    meterStyle_.setText(toString(theme.meters.style), juce::dontSendNotification);
    gainStyle_.setText(toString(theme.controls.gainStyle), juce::dontSendNotification);
    backdrop_.setText(theme.windowBackdrop, juce::dontSendNotification);
    name_.setText(theme.name, juce::dontSendNotification);
    colours_.updateContent();
    colours_.repaint();
}

void ThemeEditorPanel::preview() {
    working_.glass.enabled = glass_.getToggleState();
    working_.meters.gradient = gradientMeters_.getToggleState();
    working_.meters.showLufs = showLufs_.getToggleState();
    working_.glass.blurRadius = static_cast<float>(blur_.getValue());
    working_.glass.tintOpacity = static_cast<float>(tint_.getValue());
    working_.metrics.cornerRadius = static_cast<float>(radius_.getValue());
    working_.metrics.stripWidth = static_cast<int>(stripWidth_.getValue());
    working_.metrics.fontSize = static_cast<float>(fontSize_.getValue());
    working_.meters.decayDbPerSecond = static_cast<float>(decay_.getValue());
    working_.meters.peakHoldMs = static_cast<int>(hold_.getValue());
    working_.meters.segments = static_cast<int>(segments_.getValue());
    working_.meters.style = meterStyleFromString(meterStyle_.getText());
    working_.controls.gainStyle = gainStyleFromString(gainStyle_.getText());
    working_.windowBackdrop = backdrop_.getText();
    gui_.previewTheme(working_);
    repaint();
}

void ThemeEditorPanel::paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected) {
    if (row < 0 || row >= keys_.size()) return;
    const auto& theme = gui_.theme();
    if (selected) g.fillAll(theme.colour("selection"));
    const auto colour = working_.colour(keys_[row]);
    g.setColour(colour);
    g.fillRoundedRectangle(juce::Rectangle<float>(6.0f, 4.0f, 28.0f, static_cast<float>(height) - 8.0f), 3.0f);
    g.setColour(theme.colour("panelBorder"));
    g.drawRoundedRectangle(juce::Rectangle<float>(6.0f, 4.0f, 28.0f, static_cast<float>(height) - 8.0f), 3.0f, 1.0f);
    g.setColour(theme.colour("text"));
    g.setFont(theme.font(12.0f));
    g.drawText(keys_[row], 42, 0, width - 140, height, juce::Justification::centredLeft);
    g.setColour(theme.colour("textDim"));
    g.setFont(theme.monoFont(11.0f));
    g.drawText("#" + colour.toDisplayString(true).substring(2) + colour.toDisplayString(true).substring(0, 2), width - 100, 0, 94, height,
               juce::Justification::centredRight);
}

void ThemeEditorPanel::listBoxItemClicked(int row, const juce::MouseEvent&) {
    if (row < 0 || row >= keys_.size()) return;
    const juce::String key = keys_[row];
    auto picker = std::make_unique<ColourPicker>(working_.colour(key), [this, key](juce::Colour c) {
        working_.colours[key] = c;
        colours_.repaint();
        preview();
    });
    juce::CallOutBox::launchAsynchronously(std::move(picker), colours_.getRowPosition(row, true).withX(colours_.getRight() - 40),
                                           getTopLevelComponent());
}

void ThemeEditorPanel::paint(juce::Graphics& g) { gui_.lookAndFeel().drawBackground(g, getLocalBounds().toFloat()); }

void ThemeEditorPanel::resized() {
    auto area = getLocalBounds().reduced(16);
    auto left = area.removeFromLeft(320);
    auto top = left.removeFromTop(28);
    labels_[0]->setBounds(top.removeFromLeft(60));
    themes_.setBounds(top);
    left.removeFromTop(8);
    colours_.setBounds(left);

    area.removeFromLeft(16);
    auto bottom = area.removeFromBottom(30);
    labels_[12]->setBounds(bottom.removeFromLeft(60));
    name_.setBounds(bottom.removeFromLeft(220).withTrimmedRight(8));
    save_.setBounds(bottom.removeFromLeft(170).withTrimmedRight(8));
    revert_.setBounds(bottom.removeFromLeft(90));

    auto toggles = area.removeFromTop(26);
    glass_.setBounds(toggles.removeFromLeft(140));
    gradientMeters_.setBounds(toggles.removeFromLeft(160));
    showLufs_.setBounds(toggles);
    area.removeFromTop(8);
    juce::Component* controls[] = {&blur_, &tint_, &radius_, &stripWidth_, &fontSize_, &decay_, &hold_, &segments_, &meterStyle_, &gainStyle_, &backdrop_};
    for (int i = 0; i < 11; ++i) {
        auto row = area.removeFromTop(30);
        labels_[i + 1]->setBounds(row.removeFromLeft(150));
        controls[i]->setBounds(row.reduced(0, 3));
        area.removeFromTop(4);
    }
}

}  // namespace reka::app
