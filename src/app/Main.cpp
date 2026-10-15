// REKA.exe entry point.
#include "AppServices.h"
#include "ProfileService.h"
#include "gui/RekaGuiEngine.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace reka::app {

class RekaApplication final : public juce::JUCEApplication {
public:
    const juce::String getApplicationName() override { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override { return false; }  // one engine owns the devices

    void initialise(const juce::String& commandLine) override {
        services_ = std::make_unique<AppServices>();
        // The GUI engine installs the UI hooks before the first profile is applied, so the profile's
        // layout and theme are honoured at startup.
        gui_ = std::make_unique<RekaGuiEngine>(*services_);
        services_->initialise(commandLine);
        const bool minimised = services_->settings().startMinimised || commandLine.contains("--minimised");
        gui_->createMainWindow(minimised);

        // --smoke-test <seconds>: start everything, run, then quit through the normal shutdown path.
        // Used by CI (under Xvfb) to catch startup / teardown-order crashes.
        if (const int index = commandLine.indexOf("--smoke-test"); index >= 0) {
            const int seconds = juce::jmax(1, commandLine.substring(index + 12).trim().getIntValue());
            juce::Timer::callAfterDelay(seconds * 1000, [] {
                if (auto* app = juce::JUCEApplicationBase::getInstance()) app->systemRequestedQuit();
            });
        }
    }

    void shutdown() override {
        gui_.reset();
        if (services_ != nullptr) services_->shutdown();
        services_.reset();
    }

    void systemRequestedQuit() override { quit(); }

    void anotherInstanceStarted(const juce::String& commandLine) override {
        // Second launch: surface the running instance and load any profile passed on its command line.
        if (gui_ != nullptr) gui_->bringToFront();
        for (const auto& token : juce::StringArray::fromTokens(commandLine, true)) {
            const juce::File file(token.unquoted());
            juce::String error;
            if (file.hasFileExtension(".reka") && file.existsAsFile() && services_ != nullptr) services_->profiles().load(file, error);
        }
    }

private:
    std::unique_ptr<AppServices> services_;
    std::unique_ptr<RekaGuiEngine> gui_;
};

}  // namespace reka::app

START_JUCE_APPLICATION(reka::app::RekaApplication)
