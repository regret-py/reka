#include "gui/components/InsertRack.h"

#include "AppServices.h"
#include "gui/RekaGuiEngine.h"

namespace reka::app {

class InsertRack::Slot final : public juce::Component {
public:
    Slot(InsertRack& rack, int index, InsertSlotPtr slot) : rack_(rack), index_(index), slot_(std::move(slot)) {
        bypass_.setClickingTogglesState(true);
        bypass_.setTooltip("Bypass (latency-compensated)");
        bypass_.onClick = [this] { slot_->bypassed.store(!bypass_.getToggleState()); };
        addAndMakeVisible(bypass_);
        refresh();
    }

    void refresh() {
        bypass_.setToggleState(!slot_->bypassed.load(), juce::dontSendNotification);
        failed_ = slot_->processor->hasFailed();
        repaint();
    }

    void paint(juce::Graphics& g) override {
        const auto& theme = rack_.gui_.theme();
        auto area = getLocalBounds().toFloat().reduced(1.0f);
        g.setColour(failed_ ? theme.colour("error").withAlpha(0.35f) : theme.colour("button"));
        g.fillRoundedRectangle(area, theme.metrics.controlRadius * 0.6f);
        area.removeFromLeft(18.0f);
        const auto& descriptor = slot_->processor->descriptor();
        if (descriptor.sandboxed) {
            g.setColour(theme.colour("success").withAlpha(0.8f));
            g.fillEllipse(area.removeFromRight(10.0f).withSizeKeepingCentre(6.0f, 6.0f));
        }
        g.setColour(slot_->bypassed.load() ? theme.colour("textDim") : theme.colour("text"));
        g.setFont(theme.font(11.0f));
        g.drawFittedText(failed_ ? juce::String("crashed: ") + descriptor.name : juce::String(descriptor.name),
                         area.reduced(3.0f, 0.0f).toNearestInt(), juce::Justification::centredLeft, 1, 0.8f);
    }

    void resized() override { bypass_.setBounds(getLocalBounds().removeFromLeft(18).reduced(3)); }

    void mouseUp(const juce::MouseEvent& event) override {
        if (event.mods.isPopupMenu()) rack_.showSlotMenu(index_);
        else if (!event.mouseWasDraggedSinceMouseDown()) rack_.app_.plugins().showEditor(*slot_->processor);
    }

    const InsertSlotPtr& slot() const noexcept { return slot_; }

private:
    InsertRack& rack_;
    int index_;
    InsertSlotPtr slot_;
    juce::ToggleButton bypass_;
    bool failed_ = false;
};

InsertRack::InsertRack(AppServices& app, RekaGuiEngine& gui, StripId strip) : app_(app), gui_(gui), strip_(strip) {
    addButton_.setTooltip("Add a plugin (or drag one from the plugin browser)");
    addButton_.onClick = [this] { showAddMenu(); };
    addAndMakeVisible(addButton_);
    rebuild();
}

void InsertRack::rebuild() {
    slots_.clear();
    if (const StripInfo* info = app_.mixer().strip(strip_)) {
        for (std::size_t i = 0; i < info->inserts.size(); ++i)
            addAndMakeVisible(slots_.add(new Slot(*this, static_cast<int>(i), info->inserts[i])));
    }
    resized();
}

void InsertRack::refresh() {
    for (auto* slot : slots_) slot->refresh();
}

int InsertRack::preferredHeight() const { return 20 * (slots_.size() + 1) + 4; }

void InsertRack::paint(juce::Graphics& g) {
    if (!dragHover_) return;
    g.setColour(gui_.theme().colour("accent"));
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(1.0f), 4.0f, 2.0f);
}

void InsertRack::resized() {
    auto area = getLocalBounds().reduced(1);
    for (auto* slot : slots_) slot->setBounds(area.removeFromTop(20));
    addButton_.setBounds(area.removeFromTop(20).reduced(0, 1));
}

void InsertRack::showAddMenu() {
    auto& host = app_.plugins();
    const auto effects = host.effectPlugins();
    juce::PopupMenu menu;
    if (effects.isEmpty()) {
        menu.addItem(1, host.isScanning() ? "Scanning plugins..." : "No plugins found - open the plugin browser to scan", false);
    }
    // Group by manufacturer; item id = index + 100. Sandboxed by default, Alt/Option forces in-process.
    std::map<juce::String, juce::PopupMenu> byVendor;
    for (int i = 0; i < effects.size(); ++i) byVendor[effects[i].manufacturerName.isEmpty() ? juce::String("Unknown") : effects[i].manufacturerName].addItem(i + 100, effects[i].name);
    for (auto& [vendor, sub] : byVendor) menu.addSubMenu(vendor, sub);

    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&addButton_),
                       [this, effects, safe = juce::Component::SafePointer<InsertRack>(this)](int result) {
                           if (safe == nullptr || result < 100) return;
                           const bool inProcess = juce::ModifierKeys::currentModifiers.isAltDown();
                           const bool sandboxed = app_.plugins().sandboxByDefault() && !inProcess;
                           app_.addPlugin(strip_, effects[result - 100], sandboxed);
                       });
}

void InsertRack::showSlotMenu(int index) {
    const StripInfo* info = app_.mixer().strip(strip_);
    if (info == nullptr || index >= static_cast<int>(info->inserts.size())) return;
    const InsertSlotPtr slot = info->inserts[static_cast<std::size_t>(index)];
    juce::PopupMenu menu;
    menu.addSectionHeader(juce::String(slot->processor->descriptor().name));
    menu.addItem(1, "Open editor");
    menu.addItem(2, "Bypass", true, slot->bypassed.load());
    menu.addSeparator();
    menu.addItem(3, "Move up", index > 0);
    menu.addItem(4, "Move down", index + 1 < static_cast<int>(info->inserts.size()));
    menu.addSeparator();
    menu.addItem(5, slot->processor->descriptor().sandboxed ? "Reload in-process (faster, less isolated)" : "Reload sandboxed (crash-safe)");
    menu.addItem(6, "Remove");
    menu.addSeparator();
    menu.addItem(7, "Latency: " + juce::String(slot->processor->latencySamples()) + " samples", false);

    menu.showMenuAsync(juce::PopupMenu::Options(), [this, index, slot](int result) {
        auto& mixer = app_.mixer();
        switch (result) {
            case 1: app_.plugins().showEditor(*slot->processor); break;
            case 2: slot->bypassed.store(!slot->bypassed.load()); refresh(); break;
            case 3: mixer.moveInsert(strip_, index, index - 1); break;
            case 4: mixer.moveInsert(strip_, index, index + 1); break;
            case 5: {
                InsertDescriptor descriptor = slot->processor->descriptor();
                descriptor.sandboxed = !descriptor.sandboxed;
                const auto state = slot->processor->saveState();
                std::string error;
                auto replacement = app_.plugins().createInsert(descriptor, mixer.sampleRate(), mixer.maxBlockSize(), error);
                if (replacement == nullptr) {
                    app_.log("Could not reload " + juce::String(descriptor.name) + ": " + juce::String(error));
                    break;
                }
                replacement->restoreState(state);
                auto newSlot = std::make_shared<InsertSlot>(std::move(replacement));
                newSlot->bypassed.store(slot->bypassed.load());
                app_.plugins().closeEditor(*slot->processor);
                mixer.replaceInsert(slot.get(), std::move(newSlot));
                break;
            }
            case 6:
                app_.plugins().closeEditor(*slot->processor);
                mixer.removeInsert(strip_, index);
                break;
            default: break;
        }
    });
}

bool InsertRack::isInterestedInDragSource(const SourceDetails& details) {
    return details.description.toString().startsWith(kDragPrefix);
}

void InsertRack::itemDropped(const SourceDetails& details) {
    dragHover_ = false;
    repaint();
    const juce::String identifier = details.description.toString().fromFirstOccurrenceOf(kDragPrefix, false, false);
    if (auto description = app_.plugins().findDescription(identifier)) {
        const bool inProcess = juce::ModifierKeys::currentModifiers.isAltDown();
        app_.addPlugin(strip_, *description, app_.plugins().sandboxByDefault() && !inProcess);
    }
}

}  // namespace reka::app
