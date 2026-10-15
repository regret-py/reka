#include "gui/RekaGuiEngine.h"

#include "AppServices.h"
#include "BinaryData.h"
#include "ProfileService.h"
#include "gui/MainComponent.h"
#include "gui/components/EqEditor.h"
#include "gui/components/RoutingMatrixPanel.h"
#include "gui/components/SettingsPanel.h"
#include "gui/components/ThemeEditorPanel.h"
#include "reka/platform/WindowBackdrop.h"

namespace reka::app {

// ---- main window -------------------------------------------------------------------------------------------

class MainWindow final : public juce::DocumentWindow {
public:
    MainWindow(AppServices& app, RekaGuiEngine& gui)
        : juce::DocumentWindow("REKA", gui.theme().colour("background"), juce::DocumentWindow::allButtons), app_(app) {
        setUsingNativeTitleBar(true);
        setContentOwned(new MainComponent(app, gui), false);
        setResizable(true, false);
        setResizeLimits(900, 560, 16384, 16384);
        const auto& s = app.settings();
        if (s.windowX >= 0 && s.windowY >= 0) setBounds(s.windowX, s.windowY, s.windowWidth, s.windowHeight);
        else centreWithSize(s.windowWidth, s.windowHeight);
    }

    ~MainWindow() override {
        auto& s = app_.settings();
        s.windowX = getX();
        s.windowY = getY();
        s.windowWidth = getWidth();
        s.windowHeight = getHeight();
    }

    void closeButtonPressed() override {
        // Like Voicemeeter, REKA keeps mixing in the background; the tray icon brings it back.
        if (app_.settings().minimiseToTray) setVisible(false);
        else juce::JUCEApplication::getInstance()->systemRequestedQuit();
    }

    void applyBackdrop(const Theme& theme) {
        if (auto* peer = getPeer()) applyWindowBackdrop(peer->getNativeHandle(), windowBackdropFromString(theme.windowBackdrop.toStdString()), theme.dark);
    }

private:
    AppServices& app_;
};

// ---- tray icon ----------------------------------------------------------------------------------------------

namespace {

juce::Image makeTrayIcon(juce::Colour accent) {
    juce::Image image(juce::Image::ARGB, 32, 32, true);
    juce::Graphics g(image);
    g.setColour(accent);
    g.fillEllipse(1.0f, 1.0f, 30.0f, 30.0f);
    g.setColour(juce::Colours::black.withAlpha(0.85f));
    g.setFont(juce::Font(juce::FontOptions(22.0f, juce::Font::bold)));
    g.drawText("R", image.getBounds(), juce::Justification::centred);
    return image;
}

class TrayIcon final : public juce::SystemTrayIconComponent {
public:
    TrayIcon(AppServices& app, RekaGuiEngine& gui) : app_(app), gui_(gui) {
        const auto icon = makeTrayIcon(gui.theme().colour("accent"));
        setIconImage(icon, icon);
        setIconTooltip("REKA audio mixer");
    }

    void mouseUp(const juce::MouseEvent& event) override {
        if (!event.mods.isPopupMenu()) {
            gui_.bringToFront();
            return;
        }
        juce::PopupMenu menu;
        menu.addSectionHeader("REKA - " + app_.profiles().currentName());
        menu.addItem(1, "Show REKA");
        juce::PopupMenu recent;
        const auto files = app_.settings().recentProfiles;
        for (int i = 0; i < files.size(); ++i) recent.addItem(100 + i, juce::File(files[i]).getFileNameWithoutExtension());
        menu.addSubMenu("Load profile", recent, !files.isEmpty());
        menu.addSeparator();
        menu.addItem(2, "Quit REKA");
        menu.showMenuAsync(juce::PopupMenu::Options(), [this, files](int result) {
            juce::String error;
            if (result == 1) gui_.bringToFront();
            else if (result == 2) juce::JUCEApplication::getInstance()->systemRequestedQuit();
            else if (result >= 100) app_.profiles().load(juce::File(files[result - 100]), error);
        });
    }

private:
    AppServices& app_;
    RekaGuiEngine& gui_;
};

}  // namespace

// ---- RekaGuiEngine ---------------------------------------------------------------------------------------------

RekaGuiEngine::RekaGuiEngine(AppServices& app) : app_(app) {
    juce::LookAndFeel::setDefaultLookAndFeel(&lookAndFeel_);
    reloadThemes();
    setTheme(layout_.theme);
    // The UI layout and theme travel inside .reka profiles.
    app_.captureUiState = [this] { return layout_.toJson(); };
    app_.applyUiState = [this](const nlohmann::json& json) { setLayout(UiLayout::fromJson(json)); };
    startTimer(1000);
}

RekaGuiEngine::~RekaGuiEngine() {
    stopTimer();
    app_.captureUiState = nullptr;
    app_.applyUiState = nullptr;
    if (settingsWindow_ != nullptr) delete settingsWindow_.getComponent();
    if (themeWindow_ != nullptr) delete themeWindow_.getComponent();
    tray_.reset();
    window_.reset();
    juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
}

void RekaGuiEngine::createMainWindow(bool minimised) {
    window_ = std::make_unique<MainWindow>(app_, *this);
    tray_ = std::make_unique<TrayIcon>(app_, *this);
    window_->setVisible(!minimised);
    window_->applyBackdrop(theme());
}

void RekaGuiEngine::bringToFront() {
    if (window_ == nullptr) return;
    window_->setVisible(true);
    window_->setMinimised(false);
    window_->toFront(true);
}

// ---- themes --------------------------------------------------------------------------------------------------------

void RekaGuiEngine::reloadThemes() {
    themes_.clear();
    themeFiles_.clear();
    auto add = [this](const std::string& text, const juce::File& file) {
        try {
            Theme theme = Theme::fromJson(nlohmann::json::parse(text, nullptr, true, true));
            if (file != juce::File()) themeFiles_[theme.id] = file;
            themes_[theme.id] = theme;
        } catch (const std::exception& e) {
            app_.log("Theme " + (file != juce::File() ? file.getFileName() : juce::String("(built-in)")) + " is invalid: " + e.what());
        }
    };
    for (int i = 0; i < RekaBinary::namedResourceListSize; ++i) {
        const char* resource = RekaBinary::namedResourceList[i];
        if (!juce::String(RekaBinary::getNamedResourceOriginalFilename(resource)).endsWithIgnoreCase(".json")) continue;
        int size = 0;
        const char* data = RekaBinary::getNamedResource(resource, size);
        add(std::string(data, static_cast<std::size_t>(size)), {});
    }
    for (const auto& file : AppPaths::themesDirectory().findChildFiles(juce::File::findFiles, false, "*.json"))
        add(file.loadFileAsString().toStdString(), file);
    if (themes_.empty()) themes_[Theme::defaults().id] = Theme::defaults();
}

juce::StringArray RekaGuiEngine::themeIds() const {
    juce::StringArray ids;
    for (const auto& [id, theme] : themes_) ids.add(id);
    return ids;
}

juce::String RekaGuiEngine::themeName(const juce::String& id) const {
    auto it = themes_.find(id);
    if (it == themes_.end()) return id;
    return it->second.name + (themeFiles_.count(id) > 0 ? " (user)" : "");
}

void RekaGuiEngine::setTheme(const juce::String& id) {
    auto it = themes_.find(id);
    const Theme theme = it != themes_.end() ? it->second : (themes_.empty() ? Theme::defaults() : themes_.begin()->second);
    layout_.theme = theme.id;
    if (auto file = themeFiles_.find(theme.id); file != themeFiles_.end()) watchedThemeTime_ = file->second.getLastModificationTime();
    applyTheme(theme);
}

void RekaGuiEngine::previewTheme(const Theme& theme) {
    layout_.stripWidth = theme.metrics.stripWidth;
    layout_.gainStyle = theme.controls.gainStyle;
    applyTheme(theme);
}

void RekaGuiEngine::applyTheme(const Theme& theme) {
    lookAndFeel_.setTheme(theme);
    if (window_ != nullptr) {
        window_->setBackgroundColour(theme.colour("background"));
        window_->applyBackdrop(theme);
        window_->repaint();
    }
    sendChangeMessage();
}

bool RekaGuiEngine::saveUserTheme(const Theme& theme, juce::String& error) {
    if (theme.id.isEmpty()) {
        error = "the theme needs a name";
        return false;
    }
    const auto file = AppPaths::themesDirectory().getChildFile(theme.id + ".json");
    if (!file.replaceWithText(juce::String(theme.toJson().dump(2)))) {
        error = "could not write " + file.getFullPathName();
        return false;
    }
    reloadThemes();
    setTheme(theme.id);
    app_.log("Saved theme " + theme.name + " to " + file.getFullPathName());
    return true;
}

void RekaGuiEngine::timerCallback() {
    // Live theme editing: a user theme JSON edited in any editor restyles REKA within a second.
    auto it = themeFiles_.find(layout_.theme);
    if (it == themeFiles_.end()) return;
    const auto modified = it->second.getLastModificationTime();
    if (modified == watchedThemeTime_) return;
    watchedThemeTime_ = modified;
    reloadThemes();
    setTheme(layout_.theme);
    app_.log("Theme reloaded from " + it->second.getFileName());
}

// ---- layout ----------------------------------------------------------------------------------------------------------

void RekaGuiEngine::setLayout(UiLayout layout) {
    const juce::String previousTheme = layout_.theme;
    layout_ = std::move(layout);
    if (layout_.stripTemplate.isEmpty()) layout_.stripTemplate = UiLayout::defaults().stripTemplate;
    if (layout_.theme != previousTheme || lookAndFeel_.theme().id != layout_.theme) {
        setTheme(layout_.theme);
        return;  // setTheme broadcasts
    }
    sendChangeMessage();
}

void RekaGuiEngine::updateLayout(const std::function<void(UiLayout&)>& change, bool notify) {
    change(layout_);
    if (notify) sendChangeMessage();
}

// ---- dialogs ------------------------------------------------------------------------------------------------------------

void RekaGuiEngine::showSettings() {
    if (settingsWindow_ != nullptr) {
        settingsWindow_->toFront(true);
        return;
    }
    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(new SettingsPanel(app_, *this));
    options.dialogTitle = "REKA Settings";
    options.dialogBackgroundColour = theme().colour("background");
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = true;
    settingsWindow_ = options.launchAsync();
}

void RekaGuiEngine::showThemeEditor() {
    if (themeWindow_ != nullptr) {
        themeWindow_->toFront(true);
        return;
    }
    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(new ThemeEditorPanel(*this));
    options.dialogTitle = "REKA Theme Editor";
    options.dialogBackgroundColour = theme().colour("background");
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = true;
    themeWindow_ = options.launchAsync();
}

void RekaGuiEngine::showEqEditor(EqParameters& eq, std::shared_ptr<void> owner, const juce::String& title, juce::Component& anchor) {
    auto editor = std::make_unique<EqEditor>(lookAndFeel_, eq, std::move(owner), app_.mixer().sampleRate(), title);
    juce::CallOutBox::launchAsynchronously(std::move(editor), anchor.getScreenBounds(), nullptr);
}

void RekaGuiEngine::showCrosspointEditor(StripId input, StripId bus, juce::Component& anchor) {
    if (app_.mixer().route(input, bus) == nullptr) return;
    auto editor = std::make_unique<CrosspointEditor>(app_, *this, input, bus);
    juce::CallOutBox::launchAsynchronously(std::move(editor), anchor.getScreenBounds(), nullptr);
}

}  // namespace reka::app
