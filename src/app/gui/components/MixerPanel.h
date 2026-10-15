#pragma once

#include "gui/components/StripComponent.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace reka::app {

class AppServices;
class RekaGuiEngine;

// A scrollable row of strips of one kind (inputs or buses) with an "add strip" button. Rebuilds itself
// when the mixer topology or the layout changes; otherwise polls parameters at 30 Hz so physical faders,
// MIDI and profile reloads show up immediately.
class MixerPanel final : public juce::Component, private juce::Timer, private juce::ChangeListener {
public:
    MixerPanel(AppServices& app, RekaGuiEngine& gui, StripKind kind);
    ~MixerPanel() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    StripKind kind() const noexcept { return kind_; }

private:
    void timerCallback() override;
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void rebuild();
    void layoutStrips();
    void addStrip();

    AppServices& app_;
    RekaGuiEngine& gui_;
    StripKind kind_;
    juce::Viewport viewport_;
    juce::Component content_;
    juce::OwnedArray<StripComponent> strips_;
    juce::TextButton addButton_{"+"};
    juce::TextButton showHidden_;
    std::uint64_t seenTopology_ = 0;
    int selectedChannel_ = -1;
    std::uint32_t seenTouch_ = 0;
};

}  // namespace reka::app
