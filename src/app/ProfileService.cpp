#include "ProfileService.h"

#include "AppServices.h"
#include "BinaryData.h"

namespace reka::app {

namespace {
// Unicode-safe juce::File -> std::filesystem::path (a narrow std::string is the ANSI code page on Windows).
std::filesystem::path toPath(const juce::File& file) {
#if JUCE_WINDOWS
    return std::filesystem::path(file.getFullPathName().toWideCharPointer());
#else
    return std::filesystem::path(file.getFullPathName().toStdString());
#endif
}
}  // namespace

ProfileService::ProfileService(AppServices& app) : app_(app) { startTimer(1000); }

ProfileService::~ProfileService() { stopTimer(); }

bool ProfileService::load(const juce::File& file, juce::String& error) {
    Profile profile;
    std::string loadError;
    if (!loadProfileFile(toPath(file), profile, loadError)) {
        error = juce::String(loadError);
        return false;
    }
    file_ = file;
    knownModification_ = file.getLastModificationTime();
    name_ = juce::String(profile.name);
    const juce::String warnings = apply(profile);
    if (warnings.isNotEmpty()) app_.log("Profile warnings:\n" + warnings);
    app_.settings().addRecent(file);
    return true;
}

void ProfileService::loadDefault() {
    try {
        const std::string text(RekaBinary::default_reka, static_cast<std::size_t>(RekaBinary::default_rekaSize));
        const Profile profile = profileFromJson(nlohmann::json::parse(text));
        file_ = juce::File();
        name_ = juce::String(profile.name);
        const juce::String warnings = apply(profile);
        if (warnings.isNotEmpty()) app_.log("Default profile:\n" + warnings);
    } catch (const std::exception& e) {
        app_.log("Built-in default profile is invalid: " + juce::String(e.what()));
    }
}

juce::String ProfileService::apply(const Profile& profile) {
    juce::StringArray warnings;

    // 1. Audio devices: only restarted if the profile asks for a different master/secondary set.
    if (auto w = app_.devices().restoreState(profile.audio); w.isNotEmpty()) warnings.add(w);
    if (auto it = profile.audio.find("virtual"); it != profile.audio.end())
        if (auto w = app_.virtualIo().restoreState(*it); w.isNotEmpty()) warnings.add(w);

    // 2. Mixer: diffed in one graph publication; unchanged plugins and DSP state are kept.
    const ApplyReport report = applyMixerState(app_.mixer(), profile.mixer, &app_.plugins());
    for (const auto& w : report.warnings) warnings.add(juce::String(w));

    // 3. Control surface + MIDI. The serial thread restarts only if the connection settings changed.
    const HardwareSettings wanted = HardwareSettings::fromJson(profile.hardware);
    const HardwareSettings& current = app_.hardwareSettings();
    const bool reconnect = wanted.enabled != current.enabled || wanted.port != current.port || wanted.baud != current.baud ||
                           app_.hardware().status() == HardwareStatus::Stopped;
    if (reconnect) {
        app_.applyHardwareSettings(wanted);
    } else {
        HardwareSettings merged = current;
        merged.map = wanted.map;
        merged.ledBrightness = wanted.ledBrightness;
        merged.oledContrast = wanted.oledContrast;
        app_.applyHardwareSettings(merged);
    }
    if (auto midi = profile.hardware.find("midi"); midi != profile.hardware.end()) app_.midi().restoreState(*midi);

    // 4. UI layout and theme.
    if (app_.applyUiState) app_.applyUiState(profile.ui);
    app_.refreshHardwareBindings();
    app_.sendChangeMessage();

    juce::String summary;
    summary << "Profile '" << profile.name << "': +" << report.stripsAdded << " / -" << report.stripsRemoved << " strips, "
            << report.insertsReused << " plugins kept, " << report.insertsCreated << " loaded";
    app_.log(summary);
    return warnings.joinIntoString("\n");
}

Profile ProfileService::capture() const {
    Profile profile;
    profile.name = name_.toStdString();
    profile.mixer = captureMixerState(app_.mixer());
    profile.audio = app_.devices().saveState();
    profile.audio["virtual"] = app_.virtualIo().saveState();
    nlohmann::json hardware = nlohmann::json::object();
    app_.hardwareSettings().writeTo(hardware);
    hardware["midi"] = app_.midi().saveState();
    profile.hardware = hardware;
    if (app_.captureUiState) profile.ui = app_.captureUiState();
    return profile;
}

bool ProfileService::save(const juce::File& file, juce::String& error) {
    std::string saveError;
    if (!saveProfileFile(toPath(file), capture(), saveError)) {
        error = juce::String(saveError);
        return false;
    }
    file_ = file;
    knownModification_ = file.getLastModificationTime();  // our own write must not trigger a reload
    app_.settings().addRecent(file);
    app_.log("Saved " + file.getFileName());
    return true;
}

bool ProfileService::saveCurrent(juce::String& error) {
    if (file_ == juce::File()) {
        error = "this profile has not been saved yet";
        return false;
    }
    return save(file_, error);
}

void ProfileService::writeAutosave() {
    std::string ignored;
    saveProfileFile(toPath(AppPaths::autosaveFile()), capture(), ignored);
    lastAutosave_ = juce::Time::getMillisecondCounter();
}

void ProfileService::timerCallback() {
    // Hot reload when the file is edited outside REKA (text editor, sync tool, script).
    if (app_.settings().watchProfileForChanges && file_.existsAsFile()) {
        const auto modified = file_.getLastModificationTime();
        if (modified != knownModification_) {
            knownModification_ = modified;
            juce::String error;
            if (load(file_, error)) app_.log("Reloaded " + file_.getFileName() + " (changed on disk)");
            else app_.log("Could not reload " + file_.getFileName() + ": " + error);
        }
    }
    if (juce::Time::getMillisecondCounter() - lastAutosave_ > 60000) writeAutosave();
}

}  // namespace reka::app
