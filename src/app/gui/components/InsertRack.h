#pragma once

#include "reka/common/Types.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace reka::app {

class AppServices;
class RekaGuiEngine;

// The per-strip insert rack: unlimited slots, click a slot to open the plugin editor, toggle bypass,
// reorder, remove, "+" menu or drag a plugin from the browser. Sandboxed slots carry a badge; a crashed
// plugin is shown in red while the watchdog restarts it.
class InsertRack final : public juce::Component, public juce::DragAndDropTarget {
public:
    InsertRack(AppServices& app, RekaGuiEngine& gui, StripId strip);
    ~InsertRack() override;

    void rebuild();
    void refresh();  // bypass / failed states
    int preferredHeight() const;

    void paint(juce::Graphics& g) override;
    void resized() override;

    bool isInterestedInDragSource(const SourceDetails& details) override;
    void itemDropped(const SourceDetails& details) override;
    void itemDragEnter(const SourceDetails&) override { dragHover_ = true; repaint(); }
    void itemDragExit(const SourceDetails&) override { dragHover_ = false; repaint(); }

    static constexpr const char* kDragPrefix = "plugin:";

private:
    class Slot;
    void showAddMenu();
    void showSlotMenu(int index);

    AppServices& app_;
    RekaGuiEngine& gui_;
    StripId strip_;
    juce::OwnedArray<Slot> slots_;
    juce::TextButton addButton_{"+ FX"};
    bool dragHover_ = false;
};

}  // namespace reka::app
