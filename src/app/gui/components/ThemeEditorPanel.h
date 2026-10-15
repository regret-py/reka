#pragma once

#include "gui/Theme.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace reka::app {

class RekaGuiEngine;

// Deep theme editor: palette (hex colours with alpha), glass parameters, metrics, meter ballistics and
// control style, all previewed live on the running mixer. "Save as" writes a user theme JSON that can
// also be edited by hand (REKA reloads it automatically when the file changes).
class ThemeEditorPanel final : public juce::Component, private juce::ListBoxModel {
public:
    explicit ThemeEditorPanel(RekaGuiEngine& gui);

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    int getNumRows() override { return keys_.size(); }
    void paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected) override;
    void listBoxItemClicked(int row, const juce::MouseEvent& event) override;

    void loadFrom(const Theme& theme);
    void preview();

    RekaGuiEngine& gui_;
    Theme working_;
    juce::StringArray keys_;

    juce::ComboBox themes_;
    juce::ListBox colours_{"colours", this};
    juce::ToggleButton glass_{"Glass panels"}, gradientMeters_{"Gradient meters"}, showLufs_{"Show LUFS read-out"};
    juce::Slider blur_, tint_, radius_, stripWidth_, fontSize_, decay_, hold_, segments_;
    juce::ComboBox meterStyle_, gainStyle_, backdrop_;
    juce::TextEditor name_;
    juce::TextButton save_{"Save as user theme"}, revert_{"Revert"};
    juce::OwnedArray<juce::Label> labels_;
};

}  // namespace reka::app
