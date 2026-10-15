#pragma once

#include "gui/RekaLookAndFeel.h"
#include "gui/Theme.h"
#include "reka/dsp/ParametricEq.h"

#include <juce_gui_extra/juce_gui_extra.h>

#include <map>
#include <memory>

namespace reka::app {

class AppServices;
class MainWindow;

// Dynamic UI & Layout Engine.
//   * Themes   : built-in themes (embedded JSON) + user themes in %APPDATA%/REKA/Themes. The active user
//                theme file is watched, so editing it in any text editor restyles REKA live.
//   * Layout   : the profile's "ui" section (UiLayout) decides which panels exist, where they dock, how
//                each strip is composed (template), fader vs knob and strip widths.
//   * Windows  : main window (optional Windows 11 Mica/Acrylic backdrop), tray icon, editors/dialogs.
class RekaGuiEngine final : public juce::ChangeBroadcaster, private juce::Timer {
public:
    explicit RekaGuiEngine(AppServices& app);
    ~RekaGuiEngine() override;

    void createMainWindow(bool minimised);
    void bringToFront();

    AppServices& app() noexcept { return app_; }
    RekaLookAndFeel& lookAndFeel() noexcept { return lookAndFeel_; }
    const Theme& theme() const noexcept { return lookAndFeel_.theme(); }

    // ---- themes ----
    juce::StringArray themeIds() const;
    juce::String themeName(const juce::String& id) const;
    void setTheme(const juce::String& id);
    void previewTheme(const Theme& theme);  // live, unsaved (theme editor)
    bool saveUserTheme(const Theme& theme, juce::String& error);
    void reloadThemes();

    // ---- layout ----
    const UiLayout& layout() const noexcept { return layout_; }
    void setLayout(UiLayout layout);
    // notify = false is used while dragging splitters (no rebuild of strips mid-drag).
    void updateLayout(const std::function<void(UiLayout&)>& change, bool notify = true);

    // ---- editors & dialogs ----
    void showSettings();
    void showThemeEditor();
    // `owner` keeps the parameter block alive while the editor is open (the strip may be deleted).
    void showEqEditor(EqParameters& eq, std::shared_ptr<void> owner, const juce::String& title, juce::Component& anchor);
    void showCrosspointEditor(StripId input, StripId bus, juce::Component& anchor);

private:
    void timerCallback() override;
    void applyTheme(const Theme& theme);

    AppServices& app_;
    RekaLookAndFeel lookAndFeel_;
    UiLayout layout_ = UiLayout::defaults();
    std::map<juce::String, Theme> themes_;
    std::map<juce::String, juce::File> themeFiles_;  // user themes only
    juce::Time watchedThemeTime_;
    std::unique_ptr<MainWindow> window_;
    std::unique_ptr<juce::SystemTrayIconComponent> tray_;
    juce::Component::SafePointer<juce::DialogWindow> settingsWindow_, themeWindow_;
};

}  // namespace reka::app
