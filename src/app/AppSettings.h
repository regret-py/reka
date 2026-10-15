#pragma once

#include <juce_core/juce_core.h>

#include <nlohmann/json.hpp>

namespace reka::app {

// Well-known locations (all under %APPDATA%\REKA on Windows, ~/.config/REKA elsewhere).
struct AppPaths {
    static juce::File dataDirectory();
    static juce::File profilesDirectory();
    static juce::File themesDirectory();
    static juce::File settingsFile();
    static juce::File pluginListFile();
    static juce::File autosaveFile();
    static juce::File pluginHostExecutable();
};

// Machine-level preferences that do not belong in a .reka profile.
struct AppSettings {
    juce::String lastProfile;
    juce::StringArray recentProfiles;
    juce::String pluginSearchPath;   // ';'-separated, empty = format defaults
    bool sandboxPluginsByDefault = true;
    bool startMinimised = false;
    bool minimiseToTray = true;
    bool watchProfileForChanges = true;  // hot-reload when the .reka file is edited externally
    int windowX = -1, windowY = -1, windowWidth = 1480, windowHeight = 860;

    void load();
    void save() const;
    void addRecent(const juce::File& profile);
};

}  // namespace reka::app
