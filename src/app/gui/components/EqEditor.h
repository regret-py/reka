#pragma once

#include "gui/RekaLookAndFeel.h"
#include "reka/dsp/ParametricEq.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

namespace reka::app {

// Interactive 8-band parametric EQ: response curve on a log-frequency grid, draggable band nodes
// (drag = frequency/gain, wheel = Q, double-click = enable/disable) and a detail row for the selected band.
// Works on any EqParameters: strip EQ, bus EQ or a routing crosspoint's EQ.
class EqEditor final : public juce::Component, private juce::Timer {
public:
    EqEditor(RekaLookAndFeel& lookAndFeel, EqParameters& eq, std::shared_ptr<void> keepAlive, double sampleRate,
             juce::String title);

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& event) override;
    void mouseDrag(const juce::MouseEvent& event) override;
    void mouseDoubleClick(const juce::MouseEvent& event) override;
    void mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override;

private:
    void timerCallback() override;
    void selectBand(int index);
    void pushBand();
    juce::Rectangle<float> plotArea() const;
    float frequencyToX(float frequency) const;
    float xToFrequency(float x) const;
    float gainToY(float gainDb) const;
    float yToGain(float y) const;
    int hitTest(juce::Point<float> position) const;

    RekaLookAndFeel& lookAndFeel_;
    EqParameters& eq_;
    std::shared_ptr<void> keepAlive_;
    double sampleRate_;
    juce::String title_;
    EqBandArray bands_;
    std::uint32_t seenVersion_ = 0;
    int selected_ = 0;

    juce::ToggleButton enabled_{"EQ on"};
    juce::ToggleButton bandOn_{"Band on"};
    juce::ComboBox type_;
    juce::Slider frequency_, gain_, q_;
};

}  // namespace reka::app
