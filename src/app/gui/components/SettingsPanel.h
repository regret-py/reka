#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace reka::app {

class AppServices;
class RekaGuiEngine;

// Settings dialog: Audio engine (master + secondary devices), Virtual I/O (REKA endpoints, per-app
// capture, detected cables), Control surface (serial controller + MIDI learn), Plugins, General.
class SettingsPanel final : public juce::Component {
public:
    SettingsPanel(AppServices& app, RekaGuiEngine& gui);
    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    RekaGuiEngine& gui_;
    juce::TabbedComponent tabs_{juce::TabbedButtonBar::TabsAtTop};
};

}  // namespace reka::app
