#include "gui/MainComponent.h"

#include "AppServices.h"
#include "ProfileService.h"
#include "gui/RekaGuiEngine.h"
#include "gui/components/MixerPanel.h"
#include "gui/components/PluginBrowserPanel.h"
#include "gui/components/RoutingMatrixPanel.h"

namespace reka::app {

namespace {
constexpr int kToolbarHeight = 44;
constexpr int kStatusHeight = 24;
constexpr int kSplitter = 6;

// Separable box blur, three passes approximate a Gaussian. Operates in place on an ARGB image.
void boxBlur(juce::Image& image, int radius) {
    if (radius < 1) return;
    juce::Image::BitmapData data(image, juce::Image::BitmapData::readWrite);
    const int w = data.width, h = data.height;
    std::vector<int> sums(4);
    std::vector<juce::uint8> line(static_cast<std::size_t>(juce::jmax(w, h)) * 4);
    auto blurLine = [&](auto getPixel, auto setPixel, int length) {
        for (int i = 0; i < length; ++i) {
            const juce::uint8* p = getPixel(i);
            std::copy(p, p + 4, line.begin() + i * 4);
        }
        std::fill(sums.begin(), sums.end(), 0);
        const int window = radius * 2 + 1;
        for (int i = -radius; i <= radius; ++i) {
            const int idx = juce::jlimit(0, length - 1, i);
            for (int c = 0; c < 4; ++c) sums[static_cast<std::size_t>(c)] += line[static_cast<std::size_t>(idx * 4 + c)];
        }
        for (int i = 0; i < length; ++i) {
            juce::uint8* out = setPixel(i);
            for (int c = 0; c < 4; ++c) out[c] = static_cast<juce::uint8>(sums[static_cast<std::size_t>(c)] / window);
            const int add = juce::jlimit(0, length - 1, i + radius + 1);
            const int remove = juce::jlimit(0, length - 1, i - radius);
            for (int c = 0; c < 4; ++c)
                sums[static_cast<std::size_t>(c)] += line[static_cast<std::size_t>(add * 4 + c)] - line[static_cast<std::size_t>(remove * 4 + c)];
        }
    };
    for (int pass = 0; pass < 3; ++pass) {
        for (int y = 0; y < h; ++y)
            blurLine([&](int x) { return data.getPixelPointer(x, y); }, [&](int x) { return data.getPixelPointer(x, y); }, w);
        for (int x = 0; x < w; ++x)
            blurLine([&](int y) { return data.getPixelPointer(x, y); }, [&](int y) { return data.getPixelPointer(x, y); }, h);
    }
}
}  // namespace

// ---- splitter ---------------------------------------------------------------------------------------------

class MainComponent::Splitter final : public juce::Component {
public:
    Splitter(MainComponent& owner, juce::String region) : owner_(owner), region_(std::move(region)) {
        setMouseCursor(region_ == "bottom" ? juce::MouseCursor::UpDownResizeCursor : juce::MouseCursor::LeftRightResizeCursor);
        setRepaintsOnMouseActivity(true);
    }
    const juce::String& region() const noexcept { return region_; }

    void paint(juce::Graphics& g) override {
        if (!isMouseOverOrDragging()) return;
        g.setColour(owner_.gui_.theme().colour("accent").withAlpha(0.6f));
        if (region_ == "bottom") g.fillRect(getLocalBounds().withSizeKeepingCentre(getWidth(), 2));
        else g.fillRect(getLocalBounds().withSizeKeepingCentre(2, getHeight()));
    }
    void mouseDown(const juce::MouseEvent&) override {
        startSize_ = 320;
        for (const auto& [id, panel] : owner_.gui_.layout().panels)
            if (panel.dock == region_ && panel.visible) startSize_ = panel.size;
    }
    void mouseDrag(const juce::MouseEvent& event) override {
        const int delta = region_ == "left" ? event.getDistanceFromDragStartX()
                                            : (region_ == "right" ? -event.getDistanceFromDragStartX() : -event.getDistanceFromDragStartY());
        const int size = juce::jlimit(160, 1400, startSize_ + delta);
        owner_.gui_.updateLayout(
            [&](UiLayout& layout) {
                for (auto& [id, panel] : layout.panels)
                    if (panel.dock == region_) panel.size = size;
            },
            false);
        owner_.layoutPanels();
    }
    void mouseUp(const juce::MouseEvent&) override { owner_.backdropDirty_ = true; }

private:
    MainComponent& owner_;
    juce::String region_;
    int startSize_ = 320;
};

// ---- MainComponent ----------------------------------------------------------------------------------------

MainComponent::MainComponent(AppServices& app, RekaGuiEngine& gui) : app_(app), gui_(gui) {
    setOpaque(true);
    setWantsKeyboardFocus(true);

    logo_.setText("REKA", juce::dontSendNotification);
    profileName_.setJustificationType(juce::Justification::centredLeft);
    cpu_.setJustificationType(juce::Justification::centredRight);
    hardware_.setJustificationType(juce::Justification::centred);
    for (auto* label : {&logo_, &profileName_, &cpu_, &status_, &hardware_}) addAndMakeVisible(label);

    profileButton_.onClick = [this] { showProfileMenu(); };
    viewButton_.onClick = [this] { showViewMenu(); };
    themeButton_.onClick = [this] { gui_.showThemeEditor(); };
    settingsButton_.onClick = [this] { gui_.showSettings(); };
    for (auto* button : {&profileButton_, &viewButton_, &themeButton_, &settingsButton_}) addAndMakeVisible(button);

    inputs_ = std::make_unique<MixerPanel>(app_, gui_, StripKind::Input);
    buses_ = std::make_unique<MixerPanel>(app_, gui_, StripKind::Bus);
    matrix_ = std::make_unique<RoutingMatrixPanel>(app_, gui_);
    plugins_ = std::make_unique<PluginBrowserPanel>(app_, gui_);
    for (juce::Component* panel : {static_cast<juce::Component*>(inputs_.get()), static_cast<juce::Component*>(buses_.get()),
                                   static_cast<juce::Component*>(matrix_.get()), static_cast<juce::Component*>(plugins_.get())})
        addChildComponent(panel);
    for (auto* region : {"left", "right", "bottom"}) addChildComponent(splitters_.add(new Splitter(*this, region)));

    gui_.addChangeListener(this);
    app_.addChangeListener(this);
    changeListenerCallback(nullptr);
    startTimerHz(4);
}

MainComponent::~MainComponent() {
    gui_.removeChangeListener(this);
    app_.removeChangeListener(this);
}

juce::Component* MainComponent::panelFor(const juce::String& id) const {
    if (id == "inputs") return inputs_.get();
    if (id == "buses") return buses_.get();
    if (id == "matrix") return matrix_.get();
    if (id == "plugins") return plugins_.get();
    return nullptr;
}

void MainComponent::paint(juce::Graphics& g) {
    if (blurred_.isValid() && blurred_.getWidth() == getWidth() && blurred_.getHeight() == getHeight())
        g.drawImageAt(blurred_, 0, 0);
    else
        gui_.lookAndFeel().drawBackground(g, getLocalBounds().toFloat());

    const Theme& theme = gui_.theme();
    auto toolbar = getLocalBounds().removeFromTop(kToolbarHeight).toFloat();
    g.setColour(theme.colour("background").withAlpha(0.55f));
    g.fillRect(toolbar);
    g.setColour(theme.colour("panelBorder"));
    g.drawHorizontalLine(kToolbarHeight - 1, 0.0f, static_cast<float>(getWidth()));
    auto status = getLocalBounds().removeFromBottom(kStatusHeight).toFloat();
    g.setColour(theme.colour("background").withAlpha(0.7f));
    g.fillRect(status);
}

void MainComponent::resized() {
    auto toolbar = getLocalBounds().removeFromTop(kToolbarHeight).reduced(10, 7);
    logo_.setBounds(toolbar.removeFromLeft(70));
    profileButton_.setBounds(toolbar.removeFromLeft(86).reduced(2, 0));
    viewButton_.setBounds(toolbar.removeFromLeft(70).reduced(2, 0));
    themeButton_.setBounds(toolbar.removeFromLeft(76).reduced(2, 0));
    settingsButton_.setBounds(toolbar.removeFromLeft(90).reduced(2, 0));
    cpu_.setBounds(toolbar.removeFromRight(220));
    profileName_.setBounds(toolbar.withTrimmedLeft(12));

    auto statusBar = getLocalBounds().removeFromBottom(kStatusHeight).reduced(10, 2);
    hardware_.setBounds(statusBar.removeFromRight(statusBar.getWidth() / 3));
    status_.setBounds(statusBar);

    layoutPanels();
    backdropDirty_ = true;
    backdropRequested_ = juce::Time::getMillisecondCounter();
}

void MainComponent::layoutPanels() {
    const auto& layout = gui_.layout();
    const int pad = gui_.theme().metrics.panelPadding;
    auto area = getLocalBounds().withTrimmedTop(kToolbarHeight).withTrimmedBottom(kStatusHeight).reduced(pad);

    std::map<juce::String, std::vector<juce::Component*>> regions;
    std::map<juce::String, int> regionSize;
    for (const auto* id : {"plugins", "inputs", "buses", "matrix"}) {
        juce::Component* panel = panelFor(id);
        const auto it = layout.panels.find(id);
        const PanelLayout p = it != layout.panels.end() ? it->second : PanelLayout{};
        panel->setVisible(p.visible);
        if (!p.visible) continue;
        regions[p.dock].push_back(panel);
        regionSize[p.dock] = juce::jmax(regionSize[p.dock], p.size);
    }

    auto stack = [](std::vector<juce::Component*>& panels, juce::Rectangle<int> r, bool vertical, int gap) {
        const int n = static_cast<int>(panels.size());
        for (int i = 0; i < n; ++i) {
            const int remaining = n - i;
            juce::Rectangle<int> cell = vertical ? r.removeFromTop((r.getHeight() - gap * (remaining - 1)) / remaining)
                                                 : r.removeFromLeft((r.getWidth() - gap * (remaining - 1)) / remaining);
            panels[static_cast<std::size_t>(i)]->setBounds(cell);
            if (vertical) r.removeFromTop(gap);
            else r.removeFromLeft(gap);
        }
    };

    for (auto* splitter : splitters_) splitter->setVisible(false);
    auto placeSplitter = [this](const juce::String& region, juce::Rectangle<int> bounds) {
        for (auto* s : splitters_)
            if (s->region() == region) {
                s->setBounds(bounds);
                s->setVisible(true);
                s->toFront(false);
            }
    };

    if (!regions["left"].empty()) {
        const int w = juce::jmin(regionSize["left"], area.getWidth() / 2);
        stack(regions["left"], area.removeFromLeft(w), true, pad);
        placeSplitter("left", area.removeFromLeft(kSplitter));
    }
    if (!regions["right"].empty()) {
        const int w = juce::jmin(regionSize["right"], area.getWidth() / 2);
        stack(regions["right"], area.removeFromRight(w), true, pad);
        placeSplitter("right", area.removeFromRight(kSplitter));
    }
    if (!regions["bottom"].empty()) {
        const int h = juce::jmin(regionSize["bottom"], area.getHeight() - 120);
        stack(regions["bottom"], area.removeFromBottom(juce::jmax(120, h)), false, pad);
        placeSplitter("bottom", area.removeFromBottom(kSplitter));
    }
    if (!regions["center"].empty()) stack(regions["center"], area, false, pad);
}

void MainComponent::rebuildBackdrop() {
    const int w = getWidth(), h = getHeight();
    if (w <= 0 || h <= 0) return;
    const Theme& theme = gui_.theme();
    // Render and blur at quarter resolution (fast), then upscale: blurred content has no detail to lose.
    const int sw = juce::jmax(1, w / 4), sh = juce::jmax(1, h / 4);
    juce::Image small(juce::Image::ARGB, sw, sh, true);
    {
        juce::Graphics g(small);
        gui_.lookAndFeel().drawBackground(g, small.getBounds().toFloat());
        if (theme.glass.enabled) {
            auto blob = [&](juce::Colour colour, float cx, float cy, float radius, float alpha) {
                juce::ColourGradient gradient(colour.withAlpha(alpha), cx * sw, cy * sh, colour.withAlpha(0.0f), cx * sw + radius * sw, cy * sh, true);
                g.setGradientFill(gradient);
                g.fillRect(small.getBounds());
            };
            blob(theme.colour("accent"), 0.18f, 0.28f, 0.55f, 0.45f);
            blob(theme.colour("accentAlt"), 0.85f, 0.78f, 0.6f, 0.40f);
            blob(theme.colour("fx"), 0.55f, 0.05f, 0.35f, 0.18f);
        }
    }
    boxBlur(small, juce::jmax(1, juce::roundToInt(theme.glass.blurRadius / 4.0f)));
    blurred_ = juce::Image(juce::Image::ARGB, w, h, true);
    juce::Graphics g(blurred_);
    g.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
    g.drawImage(small, 0, 0, w, h, 0, 0, sw, sh);
    backdropDirty_ = false;
    repaint();
}

void MainComponent::timerCallback() {
    if (backdropDirty_ && juce::Time::getMillisecondCounter() - backdropRequested_ > 150) rebuildBackdrop();

    const Theme& theme = gui_.theme();
    const float load = app_.engine().cpuLoad();
    cpu_.setText("DSP " + juce::String(juce::roundToInt(load * 100.0f)) + " %   " +
                     juce::String(app_.devices().sampleRate() / 1000.0, 1) + " kHz / " + juce::String(app_.devices().blockSize()),
                 juce::dontSendNotification);
    cpu_.setColour(juce::Label::textColourId, load > 0.8f ? theme.colour("error") : theme.colour("textDim"));

    const auto status = app_.hardware().status();
    hardware_.setText(juce::String::fromUTF8(status == HardwareStatus::Connected ? "\xE2\x97\x8F " : "\xE2\x97\x8B ") +
                          juce::String(app_.hardware().statusText()),
                      juce::dontSendNotification);
    hardware_.setColour(juce::Label::textColourId, status == HardwareStatus::Connected ? theme.colour("success") : theme.colour("textDim"));
}

void MainComponent::changeListenerCallback(juce::ChangeBroadcaster*) {
    const Theme& theme = gui_.theme();
    logo_.setFont(theme.font(20.0f, true));
    logo_.setColour(juce::Label::textColourId, theme.colour("accent"));
    profileName_.setFont(theme.font(theme.metrics.fontSize + 1.0f, true));
    const auto& file = app_.profiles().currentFile();
    profileName_.setText(app_.profiles().currentName() + (file.existsAsFile() ? "   -   " + file.getFileName() : juce::String("   (unsaved)")),
                         juce::dontSendNotification);
    const auto log = app_.recentLog();
    // One line only: multi-line entries (profile warnings) are summarised; the full log is in Settings.
    const juce::String last = log.isEmpty() ? juce::String("Ready") : log[log.size() - 1];
    status_.setText(last.containsChar('\n') ? last.upToFirstOccurrenceOf("\n", false, false) + "  (see Settings > Plugins & general)" : last,
                    juce::dontSendNotification);
    status_.setColour(juce::Label::textColourId, theme.colour("textDim"));
    for (auto* label : {&status_, &hardware_, &cpu_}) label->setFont(theme.font(11.5f));
    if (auto* window = dynamic_cast<juce::DocumentWindow*>(getTopLevelComponent()))
        window->setName("REKA - " + app_.profiles().currentName());
    layoutPanels();
    backdropDirty_ = true;
    repaint();
}

// ---- menus & files ---------------------------------------------------------------------------------------

void MainComponent::showProfileMenu() {
    juce::PopupMenu recent;
    const auto recentFiles = app_.settings().recentProfiles;
    for (int i = 0; i < recentFiles.size(); ++i) recent.addItem(100 + i, juce::File(recentFiles[i]).getFileNameWithoutExtension());

    juce::PopupMenu menu;
    menu.addItem(1, "New from starter template");
    menu.addItem(2, "Open...                Ctrl+O");
    menu.addItem(3, "Save                   Ctrl+S");
    menu.addItem(4, "Save as...             Ctrl+Shift+S");
    menu.addSubMenu("Open recent", recent, !recentFiles.isEmpty());
    menu.addItem(5, "Reload from disk", app_.profiles().currentFile().existsAsFile());
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&profileButton_), [this, recentFiles](int result) {
        juce::String error;
        if (result == 1) app_.profiles().loadDefault();
        else if (result == 2) openProfile();
        else if (result == 3) saveProfile();
        else if (result == 4) saveProfileAs();
        else if (result == 5) app_.profiles().load(app_.profiles().currentFile(), error);
        else if (result >= 100 && !app_.profiles().load(juce::File(recentFiles[result - 100]), error))
            app_.log("Could not open profile: " + error);
    });
}

void MainComponent::showViewMenu() {
    juce::PopupMenu menu;
    const std::pair<const char*, const char*> panels[] = {{"inputs", "Inputs"}, {"buses", "Buses"}, {"matrix", "Routing matrix"}, {"plugins", "Plugin browser"}};
    int id = 1;
    for (const auto& [key, title] : panels) {
        const auto& layout = gui_.layout();
        const auto it = layout.panels.find(key);
        const PanelLayout p = it != layout.panels.end() ? it->second : PanelLayout{};
        juce::PopupMenu dock;
        for (const auto* region : {"left", "center", "right", "bottom"})
            dock.addItem(id * 10 + juce::StringArray{"left", "center", "right", "bottom"}.indexOf(region) + 1,
                         juce::String(region).substring(0, 1).toUpperCase() + juce::String(region).substring(1), true, p.dock == region);
        menu.addItem(id * 10, title, true, p.visible);
        menu.addSubMenu(juce::String("   Dock ") + title, dock);
        ++id;
    }
    menu.addSeparator();
    menu.addItem(900, "Show hidden strips");
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&viewButton_), [this](int result) {
        static const char* keys[] = {"inputs", "buses", "matrix", "plugins"};
        static const char* regions[] = {"left", "center", "right", "bottom"};
        if (result == 900) {
            gui_.updateLayout([](UiLayout& layout) {
                for (auto& [stripId, o] : layout.overrides) o.hidden = false;
            });
            return;
        }
        const int panel = result / 10 - 1;
        const int option = result % 10;
        if (panel < 0 || panel > 3) return;
        gui_.updateLayout([&](UiLayout& layout) {
            auto& p = layout.panels[keys[panel]];
            if (option == 0) p.visible = !p.visible;
            else {
                p.dock = regions[option - 1];
                p.visible = true;
            }
        });
    });
}

void MainComponent::openProfile() {
    chooser_ = std::make_unique<juce::FileChooser>("Open REKA profile", AppPaths::profilesDirectory(), "*.reka");
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this](const juce::FileChooser& fc) {
        const auto file = fc.getResult();
        if (file == juce::File()) return;
        juce::String error;
        if (!app_.profiles().load(file, error))
            juce::AlertWindow::showAsync(juce::MessageBoxOptions::makeOptionsOk(juce::MessageBoxIconType::WarningIcon, "Open profile", error), nullptr);
    });
}

void MainComponent::saveProfile() {
    juce::String error;
    if (!app_.profiles().currentFile().existsAsFile()) {
        saveProfileAs();
        return;
    }
    if (!app_.profiles().saveCurrent(error)) app_.log("Save failed: " + error);
}

void MainComponent::saveProfileAs() {
    const auto suggestion = AppPaths::profilesDirectory().getChildFile(juce::File::createLegalFileName(app_.profiles().currentName()) + ".reka");
    chooser_ = std::make_unique<juce::FileChooser>("Save REKA profile", suggestion, "*.reka");
    chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this](const juce::FileChooser& fc) {
                              auto file = fc.getResult();
                              if (file == juce::File()) return;
                              if (!file.hasFileExtension(".reka")) file = file.withFileExtension(".reka");
                              app_.profiles().setName(file.getFileNameWithoutExtension());
                              juce::String error;
                              if (!app_.profiles().save(file, error)) app_.log("Save failed: " + error);
                          });
}

bool MainComponent::keyPressed(const juce::KeyPress& key) {
    const auto mods = key.getModifiers();
    if (!mods.isCommandDown()) return false;
    if (key.getKeyCode() == 'S' && mods.isShiftDown()) saveProfileAs();
    else if (key.getKeyCode() == 'S') saveProfile();
    else if (key.getKeyCode() == 'O') openProfile();
    else if (key.getKeyCode() == ',') gui_.showSettings();
    else return false;
    return true;
}

}  // namespace reka::app
