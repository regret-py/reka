#include "AppSettings.h"

#include <fstream>

namespace reka::app {

juce::File AppPaths::dataDirectory() {
    auto dir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("REKA");
    dir.createDirectory();
    return dir;
}

juce::File AppPaths::profilesDirectory() {
    auto dir = dataDirectory().getChildFile("Profiles");
    dir.createDirectory();
    return dir;
}

juce::File AppPaths::themesDirectory() {
    auto dir = dataDirectory().getChildFile("Themes");
    dir.createDirectory();
    return dir;
}

juce::File AppPaths::settingsFile() { return dataDirectory().getChildFile("settings.json"); }
juce::File AppPaths::pluginListFile() { return dataDirectory().getChildFile("plugins.xml"); }
juce::File AppPaths::autosaveFile() { return dataDirectory().getChildFile("autosave.reka"); }

juce::File AppPaths::pluginHostExecutable() {
    const auto self = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
#if JUCE_WINDOWS
    return self.getSiblingFile("REKA-PluginHost.exe");
#elif JUCE_MAC
    return self.getParentDirectory().getParentDirectory().getParentDirectory().getSiblingFile(
        "REKA-PluginHost.app/Contents/MacOS/REKA-PluginHost");
#else
    return self.getSiblingFile("REKA-PluginHost");
#endif
}

void AppSettings::load() {
    const auto file = AppPaths::settingsFile();
    if (!file.existsAsFile()) return;
    try {
        const auto json = nlohmann::json::parse(file.loadFileAsString().toStdString());
        lastProfile = juce::String(json.value("lastProfile", std::string()));
        recentProfiles.clear();
        for (const auto& entry : json.value("recentProfiles", nlohmann::json::array()))
            if (entry.is_string()) recentProfiles.add(juce::String(entry.get<std::string>()));
        pluginSearchPath = juce::String(json.value("pluginSearchPath", std::string()));
        sandboxPluginsByDefault = json.value("sandboxPluginsByDefault", sandboxPluginsByDefault);
        startMinimised = json.value("startMinimised", startMinimised);
        minimiseToTray = json.value("minimiseToTray", minimiseToTray);
        watchProfileForChanges = json.value("watchProfileForChanges", watchProfileForChanges);
        if (auto window = json.find("window"); window != json.end() && window->is_object()) {
            windowX = window->value("x", windowX);
            windowY = window->value("y", windowY);
            windowWidth = window->value("width", windowWidth);
            windowHeight = window->value("height", windowHeight);
        }
    } catch (const std::exception&) {
        // Corrupt settings are not fatal: defaults are used and the file is rewritten on exit.
    }
}

void AppSettings::save() const {
    nlohmann::json json;
    json["lastProfile"] = lastProfile.toStdString();
    auto recent = nlohmann::json::array();
    for (const auto& path : recentProfiles) recent.push_back(path.toStdString());
    json["recentProfiles"] = recent;
    json["pluginSearchPath"] = pluginSearchPath.toStdString();
    json["sandboxPluginsByDefault"] = sandboxPluginsByDefault;
    json["startMinimised"] = startMinimised;
    json["minimiseToTray"] = minimiseToTray;
    json["watchProfileForChanges"] = watchProfileForChanges;
    json["window"] = {{"x", windowX}, {"y", windowY}, {"width", windowWidth}, {"height", windowHeight}};
    AppPaths::settingsFile().replaceWithText(juce::String(json.dump(2)));
}

void AppSettings::addRecent(const juce::File& profile) {
    recentProfiles.removeString(profile.getFullPathName());
    recentProfiles.insert(0, profile.getFullPathName());
    while (recentProfiles.size() > 10) recentProfiles.remove(recentProfiles.size() - 1);
    lastProfile = profile.getFullPathName();
}

}  // namespace reka::app
