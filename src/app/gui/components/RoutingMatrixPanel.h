#pragma once

#include "reka/common/Types.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace reka::app {

class AppServices;
class RekaGuiEngine;

// Free-form routing matrix: rows are inputs, columns are buses. Click a cell to toggle the route,
// right-click (or double-click) for its send level, pan, polarity, pre/post-fader and 8-band EQ.
class RoutingMatrixPanel final : public juce::Component, private juce::Timer {
public:
    RoutingMatrixPanel(AppServices& app, RekaGuiEngine& gui);

    void paint(juce::Graphics& g) override;
    void mouseUp(const juce::MouseEvent& event) override;
    void mouseDoubleClick(const juce::MouseEvent& event) override;
    void mouseMove(const juce::MouseEvent& event) override;
    void mouseExit(const juce::MouseEvent&) override;

private:
    void timerCallback() override;
    juce::Rectangle<int> gridArea() const;
    std::pair<int, int> cellAt(juce::Point<int> position) const;  // (row, column) or (-1, -1)

    AppServices& app_;
    RekaGuiEngine& gui_;
    std::vector<StripId> inputs_, buses_;
    std::pair<int, int> hover_{-1, -1};
    std::uint64_t seenTopology_ = 0;
};

// Popup editor for one crosspoint (opened from the matrix or a strip's quick-route button).
class CrosspointEditor final : public juce::Component, private juce::Timer {
public:
    CrosspointEditor(AppServices& app, RekaGuiEngine& gui, StripId input, StripId bus);
    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void timerCallback() override;

    AppServices& app_;
    RekaGuiEngine& gui_;
    StripId input_, bus_;
    juce::ToggleButton enabled_{"Route on"}, preFader_{"Pre-fader"}, phase_{"Invert polarity"};
    juce::TextButton eq_{"EQ..."};
    juce::Slider pan_;
    std::unique_ptr<juce::Component> gain_;
};

}  // namespace reka::app
