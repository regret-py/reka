#include "gui/components/RoutingMatrixPanel.h"

#include "AppServices.h"
#include "gui/RekaGuiEngine.h"
#include "gui/components/GainControl.h"

namespace reka::app {

namespace {
constexpr int kRowHeaderWidth = 120;
constexpr int kColumnHeaderHeight = 64;
constexpr int kCell = 30;
}  // namespace

RoutingMatrixPanel::RoutingMatrixPanel(AppServices& app, RekaGuiEngine& gui) : app_(app), gui_(gui) {
    setRepaintsOnMouseActivity(false);
    startTimerHz(15);
}

void RoutingMatrixPanel::timerCallback() {
    if (app_.mixer().topologyRevision() != seenTopology_) {
        seenTopology_ = app_.mixer().topologyRevision();
        inputs_ = app_.mixer().strips(StripKind::Input);
        buses_ = app_.mixer().strips(StripKind::Bus);
    }
    repaint();  // cheap: a few dozen rectangles, reflects routes changed from strips/profiles
}

juce::Rectangle<int> RoutingMatrixPanel::gridArea() const {
    const int pad = gui_.theme().metrics.panelPadding;
    return {pad + kRowHeaderWidth, pad + 24 + kColumnHeaderHeight, static_cast<int>(buses_.size()) * kCell,
            static_cast<int>(inputs_.size()) * kCell};
}

std::pair<int, int> RoutingMatrixPanel::cellAt(juce::Point<int> position) const {
    const auto grid = gridArea();
    if (!grid.contains(position)) return {-1, -1};
    return {(position.y - grid.getY()) / kCell, (position.x - grid.getX()) / kCell};
}

void RoutingMatrixPanel::paint(juce::Graphics& g) {
    const Theme& theme = gui_.theme();
    auto& mixer = app_.mixer();
    gui_.lookAndFeel().drawPanel(g, *this, getLocalBounds().toFloat(), true);
    g.setColour(theme.colour("panelHeader"));
    g.setFont(theme.font(theme.metrics.headerFontSize - 2.0f, true));
    g.drawText("ROUTING MATRIX", getLocalBounds().reduced(12, 6).removeFromTop(16), juce::Justification::centredLeft);

    const auto grid = gridArea();
    g.setFont(theme.font(11.0f));
    for (std::size_t c = 0; c < buses_.size(); ++c) {
        const StripInfo* bus = mixer.strip(buses_[c]);
        if (bus == nullptr) continue;
        const int x = grid.getX() + static_cast<int>(c) * kCell;
        juce::Graphics::ScopedSaveState state(g);
        g.addTransform(juce::AffineTransform::rotation(-juce::MathConstants<float>::halfPi, static_cast<float>(x + kCell / 2),
                                                       static_cast<float>(grid.getY() - 4)));
        g.setColour(juce::Colour(bus->colour));
        g.drawText(juce::String(bus->name), x + kCell / 2, grid.getY() - 4 - 8, kColumnHeaderHeight, 16, juce::Justification::centredLeft);
    }
    for (std::size_t r = 0; r < inputs_.size(); ++r) {
        const StripInfo* input = mixer.strip(inputs_[r]);
        if (input == nullptr) continue;
        const int y = grid.getY() + static_cast<int>(r) * kCell;
        g.setColour(juce::Colour(input->colour));
        g.fillRect(grid.getX() - kRowHeaderWidth, y + 6, 3, kCell - 12);
        g.setColour(theme.colour("text"));
        g.drawText(juce::String(input->name), grid.getX() - kRowHeaderWidth + 8, y, kRowHeaderWidth - 12, kCell,
                   juce::Justification::centredLeft);

        for (std::size_t c = 0; c < buses_.size(); ++c) {
            const auto cell = juce::Rectangle<int>(grid.getX() + static_cast<int>(c) * kCell, y, kCell, kCell).reduced(3);
            const auto route = mixer.findRoute(inputs_[r], buses_[c]);
            const bool on = route != nullptr && route->enabled();
            const bool hovered = hover_ == std::make_pair(static_cast<int>(r), static_cast<int>(c));
            auto fill = on ? juce::Colour(mixer.strip(buses_[c])->colour) : theme.colour("matrixOff");
            if (hovered) fill = fill.brighter(0.25f);
            g.setColour(fill);
            g.fillRoundedRectangle(cell.toFloat(), 4.0f);
            if (route != nullptr && (route->gainDb() != 0.0f || route->preFader() || route->phaseInvert())) {
                // Non-default send settings: small corner marker.
                g.setColour(on ? juce::Colours::black.withAlpha(0.6f) : theme.colour("textDim"));
                g.fillEllipse(cell.toFloat().removeFromTop(6.0f).removeFromRight(6.0f));
            }
        }
    }
    if (inputs_.empty() || buses_.empty()) {
        g.setColour(theme.colour("textDim"));
        g.drawText("Add inputs and buses to route them", getLocalBounds(), juce::Justification::centred);
    }
}

void RoutingMatrixPanel::mouseMove(const juce::MouseEvent& event) {
    const auto cell = cellAt(event.getPosition());
    if (cell != hover_) {
        hover_ = cell;
        repaint();
    }
}

void RoutingMatrixPanel::mouseExit(const juce::MouseEvent&) {
    hover_ = {-1, -1};
    repaint();
}

void RoutingMatrixPanel::mouseUp(const juce::MouseEvent& event) {
    const auto [row, column] = cellAt(event.getPosition());
    if (row < 0 || row >= static_cast<int>(inputs_.size()) || column < 0 || column >= static_cast<int>(buses_.size())) return;
    const StripId input = inputs_[static_cast<std::size_t>(row)];
    const StripId bus = buses_[static_cast<std::size_t>(column)];
    if (event.mods.isPopupMenu()) {
        app_.mixer().route(input, bus);
        gui_.showCrosspointEditor(input, bus, *this);
        return;
    }
    if (event.getNumberOfClicks() > 1) return;
    auto route = app_.mixer().findRoute(input, bus);
    app_.mixer().setRouteEnabled(input, bus, route == nullptr || !route->enabled());
    repaint();
}

void RoutingMatrixPanel::mouseDoubleClick(const juce::MouseEvent& event) {
    const auto [row, column] = cellAt(event.getPosition());
    if (row < 0 || row >= static_cast<int>(inputs_.size()) || column < 0 || column >= static_cast<int>(buses_.size())) return;
    app_.mixer().route(inputs_[static_cast<std::size_t>(row)], buses_[static_cast<std::size_t>(column)]);
    gui_.showCrosspointEditor(inputs_[static_cast<std::size_t>(row)], buses_[static_cast<std::size_t>(column)], *this);
}

// ---- CrosspointEditor -------------------------------------------------------------------------------------

CrosspointEditor::CrosspointEditor(AppServices& app, RekaGuiEngine& gui, StripId input, StripId bus)
    : app_(app), gui_(gui), input_(input), bus_(bus) {
    auto params = app_.mixer().route(input_, bus_);
    jassert(params != nullptr);
    auto* raw = params.get();

    gain_ = std::make_unique<GainControl>([raw] { return raw->gainDb(); }, [raw](float db) { raw->setGainDb(db); }, GainStyle::Knob);
    addAndMakeVisible(*gain_);

    pan_.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    pan_.setRange(-1.0, 1.0, 0.0);
    pan_.setDoubleClickReturnValue(true, 0.0);
    pan_.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 60, 16);
    pan_.onValueChange = [raw, this] { raw->setPan(static_cast<float>(pan_.getValue())); };
    addAndMakeVisible(pan_);

    enabled_.onClick = [this] { app_.mixer().setRouteEnabled(input_, bus_, enabled_.getToggleState()); };
    preFader_.onClick = [raw, this] { raw->setPreFader(preFader_.getToggleState()); };
    phase_.onClick = [raw, this] { raw->setPhaseInvert(phase_.getToggleState()); };
    eq_.onClick = [this, params] {
        gui_.showEqEditor(params->eq(),
                          juce::String(app_.mixer().strip(input_)->name) + " -> " + juce::String(app_.mixer().strip(bus_)->name) + " EQ",
                          eq_);
    };
    for (auto* c : std::initializer_list<juce::Component*>{&enabled_, &preFader_, &phase_, &eq_}) addAndMakeVisible(c);

    timerCallback();
    setSize(300, 190);
    startTimerHz(10);
}

void CrosspointEditor::timerCallback() {
    auto params = app_.mixer().findRoute(input_, bus_);
    if (params == nullptr) return;
    enabled_.setToggleState(params->enabled(), juce::dontSendNotification);
    preFader_.setToggleState(params->preFader(), juce::dontSendNotification);
    phase_.setToggleState(params->phaseInvert(), juce::dontSendNotification);
    if (!pan_.isMouseButtonDown()) pan_.setValue(params->pan(), juce::dontSendNotification);
    static_cast<GainControl*>(gain_.get())->refresh();
}

void CrosspointEditor::paint(juce::Graphics& g) {
    const Theme& theme = gui_.theme();
    gui_.lookAndFeel().drawPanel(g, *this, getLocalBounds().toFloat(), false);
    g.setColour(theme.colour("text"));
    g.setFont(theme.font(theme.metrics.fontSize, true));
    const StripInfo* in = app_.mixer().strip(input_);
    const StripInfo* out = app_.mixer().strip(bus_);
    if (in != nullptr && out != nullptr)
        g.drawText(juce::String(in->name) + juce::String::fromUTF8("  \xE2\x86\x92  ") + juce::String(out->name),
                   getLocalBounds().reduced(10, 6).removeFromTop(18), juce::Justification::centredLeft);
}

void CrosspointEditor::resized() {
    auto area = getLocalBounds().reduced(10);
    area.removeFromTop(24);
    auto knobs = area.removeFromLeft(170);
    gain_->setBounds(knobs.removeFromLeft(85));
    pan_.setBounds(knobs);
    area.removeFromLeft(8);
    for (auto* c : std::initializer_list<juce::Component*>{&enabled_, &preFader_, &phase_, &eq_}) c->setBounds(area.removeFromTop(28).reduced(0, 2));
}

}  // namespace reka::app
