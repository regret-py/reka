#pragma once

#include "gui/RekaLookAndFeel.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

namespace reka::app {

class AppServices;
class RekaGuiEngine;
class MixerPanel;
class RoutingMatrixPanel;
class PluginBrowserPanel;

// Main window content: toolbar, dockable panels (inputs, buses, routing matrix, plugin browser) in
// left / center / right / bottom regions with draggable splitters, and a status bar. Also renders the
// blurred backdrop that frosted-glass panels sample.
class MainComponent final : public juce::Component,
                            public juce::DragAndDropContainer,
                            public BackdropProvider,
                            private juce::Timer,
                            private juce::ChangeListener {
public:
    MainComponent(AppServices& app, RekaGuiEngine& gui);
    ~MainComponent() override;

    void paint(juce::Graphics& g) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress& key) override;

    const juce::Image& blurredBackdrop() const override { return blurred_; }

    void openProfile();
    void saveProfileAs();
    void saveProfile();

private:
    class Splitter;

    void timerCallback() override;
    void changeListenerCallback(juce::ChangeBroadcaster* source) override;
    void layoutPanels();
    void rebuildBackdrop();
    void showProfileMenu();
    void showViewMenu();
    juce::Component* panelFor(const juce::String& id) const;

    AppServices& app_;
    RekaGuiEngine& gui_;

    juce::Label logo_, profileName_, cpu_, status_, hardware_;
    juce::TextButton profileButton_{"Profile"}, viewButton_{"View"}, themeButton_{"Theme"}, settingsButton_{"Settings"};

    std::unique_ptr<MixerPanel> inputs_, buses_;
    std::unique_ptr<RoutingMatrixPanel> matrix_;
    std::unique_ptr<PluginBrowserPanel> plugins_;
    juce::OwnedArray<Splitter> splitters_;

    juce::Image blurred_;
    bool backdropDirty_ = true;
    juce::uint32 backdropRequested_ = 0;
    std::unique_ptr<juce::FileChooser> chooser_;
};

}  // namespace reka::app
