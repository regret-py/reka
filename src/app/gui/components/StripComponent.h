#pragma once

#include "gui/Theme.h"
#include "reka/common/Types.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <vector>

namespace reka::app {

class AppServices;
class RekaGuiEngine;

// One mixer strip (input or bus), assembled from the layout's strip template:
//   header | source | mode | eq | inserts | sends | pan | gain | buttons
// Templates come from the profile's "ui.strip.template", so a different visual design (e.g. from a user
// mockup) can reorder, drop or repeat sections without code changes. Each section is a Widget; the
// "gain" section is flexible and takes the remaining height.
class StripComponent final : public juce::Component {
public:
    StripComponent(AppServices& app, RekaGuiEngine& gui, StripId id);
    ~StripComponent() override;

    StripId id() const noexcept { return id_; }
    void refresh();  // 30 Hz poll: parameters, routes, insert states
    void setSelected(bool selected);

    void paint(juce::Graphics& g) override;
    void resized() override;

    class Widget : public juce::Component {
    public:
        virtual int preferredHeight() const = 0;
        virtual bool isFlexible() const { return false; }
        virtual void refresh() {}
    };

private:
    void showContextMenu();

    AppServices& app_;
    RekaGuiEngine& gui_;
    StripId id_;
    bool selected_ = false;
    std::vector<std::unique_ptr<Widget>> widgets_;
};

}  // namespace reka::app
