#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace reka::app {

// TextButton that remembers the modifiers of the click that triggered it, so onClick handlers can tell a
// right-click (context action) from a normal click. juce::ModifierKeys::currentModifiers no longer holds
// the mouse button by the time onClick runs.
class ModsTextButton : public juce::TextButton {
public:
    using juce::TextButton::TextButton;
    juce::ModifierKeys lastClickModifiers;

    // Buttons ignore right-clicks unless asked; route them through the normal click path.
    void mouseUp(const juce::MouseEvent& event) override {
        lastClickModifiers = event.mods;
        juce::TextButton::mouseUp(event);
    }

protected:
    void clicked(const juce::ModifierKeys& modifiers) override {
        lastClickModifiers = modifiers;
        juce::TextButton::clicked(modifiers);
    }
};

}  // namespace reka::app
