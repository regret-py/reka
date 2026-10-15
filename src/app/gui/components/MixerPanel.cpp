#include "gui/components/MixerPanel.h"

#include "AppServices.h"
#include "gui/RekaGuiEngine.h"

namespace reka::app {

MixerPanel::MixerPanel(AppServices& app, RekaGuiEngine& gui, StripKind kind) : app_(app), gui_(gui), kind_(kind) {
    viewport_.setViewedComponent(&content_, false);
    viewport_.setScrollBarsShown(false, true);
    viewport_.setScrollBarThickness(8);
    addAndMakeVisible(viewport_);

    addButton_.setTooltip(kind == StripKind::Input ? "Add an input strip" : "Add an output bus");
    addButton_.onClick = [this] { addStrip(); };
    content_.addAndMakeVisible(addButton_);

    showHidden_.onClick = [this] {
        gui_.updateLayout([this](UiLayout& layout) {
            for (StripId id : app_.mixer().strips(kind_)) layout.overrides[id].hidden = false;
        });
    };
    addChildComponent(showHidden_);

    gui_.addChangeListener(this);
    rebuild();
    startTimerHz(30);
}

MixerPanel::~MixerPanel() { gui_.removeChangeListener(this); }

void MixerPanel::changeListenerCallback(juce::ChangeBroadcaster*) { rebuild(); }  // theme or layout changed

void MixerPanel::rebuild() {
    const int scroll = viewport_.getViewPositionX();
    strips_.clear();
    int hidden = 0;
    for (StripId id : app_.mixer().strips(kind_)) {
        if (gui_.layout().isHidden(id)) {
            ++hidden;
            continue;
        }
        content_.addAndMakeVisible(strips_.add(new StripComponent(app_, gui_, id)));
    }
    showHidden_.setButtonText("Show " + juce::String(hidden) + " hidden");
    showHidden_.setVisible(hidden > 0);
    seenTopology_ = app_.mixer().topologyRevision();
    layoutStrips();
    viewport_.setViewPosition(scroll, 0);
    repaint();
}

void MixerPanel::layoutStrips() {
    const auto& theme = gui_.theme();
    const int spacing = theme.metrics.stripSpacing;
    const int height = viewport_.getHeight() - (viewport_.isHorizontalScrollBarShown() ? viewport_.getScrollBarThickness() : 0);
    int x = spacing;
    for (auto* strip : strips_) {
        const int width = gui_.layout().widthFor(strip->id());
        strip->setBounds(x, 0, width, juce::jmax(200, height));
        x += width + spacing;
    }
    addButton_.setBounds(x, 8, 32, 32);
    content_.setSize(x + 40, juce::jmax(200, height));
}

void MixerPanel::paint(juce::Graphics& g) {
    const auto& theme = gui_.theme();
    gui_.lookAndFeel().drawPanel(g, *this, getLocalBounds().toFloat(), true);
    g.setColour(theme.colour("panelHeader"));
    g.setFont(theme.font(theme.metrics.headerFontSize - 2.0f, true));
    g.drawText(kind_ == StripKind::Input ? "INPUTS" : "BUSES / OUTPUTS", getLocalBounds().reduced(12, 6).removeFromTop(16),
               juce::Justification::centredLeft);
}

void MixerPanel::resized() {
    auto area = getLocalBounds().reduced(gui_.theme().metrics.panelPadding);
    auto header = area.removeFromTop(20);
    showHidden_.setBounds(header.removeFromRight(110).reduced(0, 1));
    viewport_.setBounds(area);
    layoutStrips();
}

void MixerPanel::timerCallback() {
    if (app_.mixer().topologyRevision() != seenTopology_) {
        rebuild();
        return;
    }
    for (auto* strip : strips_) strip->refresh();

    // Highlight the strip last touched on the control surface.
    const std::uint32_t touches = app_.hardware().touchCounter();
    if (touches != seenTouch_) {
        seenTouch_ = touches;
        const int channel = app_.hardware().lastTouchedChannel();
        std::vector<StripId> order = app_.hardwareSettings().map;
        if (order.empty())
            for (StripKind k : {StripKind::Input, StripKind::Bus})
                for (StripId id : app_.mixer().strips(k)) order.push_back(id);
        const StripId touched = channel >= 0 && channel < static_cast<int>(order.size()) ? order[static_cast<std::size_t>(channel)] : kInvalidStrip;
        for (auto* strip : strips_) {
            strip->setSelected(strip->id() == touched);
            if (strip->id() == touched && !viewport_.getViewArea().contains(strip->getBounds()))
                viewport_.setViewPosition(juce::jmax(0, strip->getX() - 8), 0);
        }
    }
}

void MixerPanel::addStrip() {
    auto& mixer = app_.mixer();
    const int count = static_cast<int>(mixer.strips(kind_).size());
    const std::string name = kind_ == StripKind::Input ? "Input " + std::to_string(count + 1) : "Bus " + std::to_string(count + 1);
    const StripId id = mixer.addStrip(kind_, name);
    if (kind_ == StripKind::Bus) {
        // A new bus starts with its name as a short label (A2, B3...) friendly for the quick-route buttons.
        mixer.renameStrip(id, "B" + std::to_string(count + 1));
    }
    app_.log("Added " + juce::String(mixer.strip(id)->name));
}

}  // namespace reka::app
