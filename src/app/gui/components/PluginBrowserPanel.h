#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

namespace reka::app {

class AppServices;
class RekaGuiEngine;

// Plugin catalogue: search, background scan with progress, sandbox default, drag a plugin onto any
// strip's insert rack.
class PluginBrowserPanel final : public juce::Component,
                                 private juce::ListBoxModel,
                                 private juce::ChangeListener,
                                 private juce::Timer {
public:
    PluginBrowserPanel(AppServices& app, RekaGuiEngine& gui);
    ~PluginBrowserPanel() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    int getNumRows() override { return filtered_.size(); }
    void paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected) override;
    juce::var getDragSourceDescription(const juce::SparseSet<int>& rows) override;
    juce::String getTooltipForRow(int row) override;

    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void refilter();

    AppServices& app_;
    RekaGuiEngine& gui_;
    juce::TextEditor search_;
    juce::ListBox list_{"plugins", this};
    juce::TextButton scan_{"Scan"}, rescan_{"Rescan all"}, clearBlacklist_{"Clear blacklist"};
    juce::ToggleButton sandbox_{"Sandbox new plugins"};
    double progress_ = 0.0;
    juce::ProgressBar progressBar_{progress_};
    juce::Label status_;
    juce::Array<juce::PluginDescription> filtered_;
};

}  // namespace reka::app
