#pragma once

#include "reka/profile/Profile.h"

#include <juce_events/juce_events.h>

namespace reka::app {

class AppServices;

// Profile & state management for .reka files.
//   * capture(): mixer + devices + virtual I/O + control surface + MIDI + UI layout -> one Profile
//   * apply()  : hot reload - every subsystem diffs against its running state, so loading a profile
//                that only changes faders, routes or plugins never stops the audio stream
//   * watches the current file and reloads it when it is edited externally
//   * autosaves to %APPDATA%/REKA/autosave.reka every minute and on exit (crash recovery)
class ProfileService final : private juce::Timer {
public:
    explicit ProfileService(AppServices& app);
    ~ProfileService() override;

    bool load(const juce::File& file, juce::String& error);
    void loadDefault();
    juce::String apply(const Profile& profile);  // returns warnings
    bool save(const juce::File& file, juce::String& error);
    bool saveCurrent(juce::String& error);
    Profile capture() const;

    const juce::File& currentFile() const noexcept { return file_; }
    juce::String currentName() const { return name_; }
    void setName(const juce::String& name) { name_ = name; }

    void writeAutosave();

private:
    void timerCallback() override;

    AppServices& app_;
    juce::File file_;
    juce::String name_ = "Default";
    juce::Time knownModification_;
    juce::uint32 lastAutosave_ = 0;
};

}  // namespace reka::app
