#include "gui/components/PluginBrowserPanel.h"

#include "AppServices.h"
#include "gui/RekaGuiEngine.h"
#include "gui/components/InsertRack.h"

namespace reka::app {

PluginBrowserPanel::PluginBrowserPanel(AppServices& app, RekaGuiEngine& gui) : app_(app), gui_(gui) {
    search_.setTextToShowWhenEmpty("Search plugins...", gui.theme().colour("textDim"));
    search_.onTextChange = [this] { refilter(); };
    addAndMakeVisible(search_);

    list_.setRowHeight(34);
    list_.setMultipleSelectionEnabled(false);
    addAndMakeVisible(list_);

    scan_.onClick = [this] { app_.plugins().startScan(false); };
    rescan_.onClick = [this] { app_.plugins().startScan(true); };
    clearBlacklist_.setTooltip("Plugins that crashed or hung during a scan are blacklisted; clear to retry them");
    clearBlacklist_.onClick = [this] { app_.plugins().clearBlacklist(); };
    sandbox_.setTooltip("Run new plugins in REKA-PluginHost.exe: a crashing plugin cannot take REKA down");
    sandbox_.setToggleState(app_.plugins().sandboxByDefault(), juce::dontSendNotification);
    sandbox_.onClick = [this] {
        app_.plugins().setSandboxByDefault(sandbox_.getToggleState());
        app_.settings().sandboxPluginsByDefault = sandbox_.getToggleState();
    };
    for (auto* c : std::initializer_list<juce::Component*>{&scan_, &rescan_, &clearBlacklist_, &sandbox_, &status_}) addAndMakeVisible(c);
    addChildComponent(progressBar_);
    status_.setFont(gui.theme().font(11.0f));
    status_.setColour(juce::Label::textColourId, gui.theme().colour("textDim"));

    app_.plugins().addChangeListener(this);
    refilter();
    startTimerHz(5);
}

PluginBrowserPanel::~PluginBrowserPanel() { app_.plugins().removeChangeListener(this); }

void PluginBrowserPanel::refilter() {
    const auto query = search_.getText().trim();
    filtered_.clear();
    for (const auto& description : app_.plugins().effectPlugins()) {
        if (query.isEmpty() || description.name.containsIgnoreCase(query) || description.manufacturerName.containsIgnoreCase(query) ||
            description.category.containsIgnoreCase(query))
            filtered_.add(description);
    }
    std::sort(filtered_.begin(), filtered_.end(), [](const juce::PluginDescription& a, const juce::PluginDescription& b) {
        return a.name.compareIgnoreCase(b.name) < 0;
    });
    list_.updateContent();
    list_.repaint();
    status_.setText(juce::String(filtered_.size()) + " effects - drag onto a strip", juce::dontSendNotification);
}

void PluginBrowserPanel::changeListenerCallback(juce::ChangeBroadcaster*) { refilter(); }

void PluginBrowserPanel::timerCallback() {
    auto& host = app_.plugins();
    const bool scanning = host.isScanning();
    progressBar_.setVisible(scanning);
    scan_.setEnabled(!scanning);
    rescan_.setEnabled(!scanning);
    if (scanning) {
        progress_ = host.scanProgress();
        progressBar_.setTextToDisplay(juce::File(host.currentlyScanning()).getFileNameWithoutExtension());
    }
}

void PluginBrowserPanel::paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected) {
    if (row < 0 || row >= filtered_.size()) return;
    const auto& theme = gui_.theme();
    const auto& d = filtered_.getReference(row);
    if (selected) g.fillAll(theme.colour("selection"));
    g.setColour(theme.colour("text"));
    g.setFont(theme.font(theme.metrics.fontSize, true));
    g.drawText(d.name, 8, 2, width - 16, height / 2, juce::Justification::centredLeft);
    g.setColour(theme.colour("textDim"));
    g.setFont(theme.font(11.0f));
    g.drawText(d.manufacturerName + "  |  " + d.pluginFormatName + "  |  " + d.category, 8, height / 2, width - 16, height / 2 - 2,
               juce::Justification::centredLeft);
}

juce::var PluginBrowserPanel::getDragSourceDescription(const juce::SparseSet<int>& rows) {
    if (rows.isEmpty()) return {};
    const int row = rows[0];
    if (row < 0 || row >= filtered_.size()) return {};
    return juce::String(InsertRack::kDragPrefix) + filtered_.getReference(row).createIdentifierString();
}

juce::String PluginBrowserPanel::getTooltipForRow(int row) {
    if (row < 0 || row >= filtered_.size()) return {};
    const auto& d = filtered_.getReference(row);
    return d.name + " " + d.version + "\n" + d.fileOrIdentifier;
}

void PluginBrowserPanel::paint(juce::Graphics& g) {
    const auto& theme = gui_.theme();
    gui_.lookAndFeel().drawPanel(g, *this, getLocalBounds().toFloat(), true);
    g.setColour(theme.colour("panelHeader"));
    g.setFont(theme.font(theme.metrics.headerFontSize - 2.0f, true));
    g.drawText("PLUGINS", getLocalBounds().reduced(12, 6).removeFromTop(16), juce::Justification::centredLeft);
}

void PluginBrowserPanel::resized() {
    auto area = getLocalBounds().reduced(gui_.theme().metrics.panelPadding);
    area.removeFromTop(22);
    search_.setBounds(area.removeFromTop(26));
    area.removeFromTop(4);
    auto buttons = area.removeFromTop(26);
    scan_.setBounds(buttons.removeFromLeft(buttons.getWidth() / 3).reduced(1, 0));
    rescan_.setBounds(buttons.removeFromLeft(buttons.getWidth() / 2).reduced(1, 0));
    clearBlacklist_.setBounds(buttons.reduced(1, 0));
    sandbox_.setBounds(area.removeFromTop(24));
    progressBar_.setBounds(area.removeFromTop(18));
    status_.setBounds(area.removeFromBottom(18));
    list_.setBounds(area);
}

}  // namespace reka::app
