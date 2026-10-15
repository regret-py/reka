#include "gui/components/SettingsPanel.h"

#include "AppServices.h"
#include "ProfileService.h"
#include "gui/RekaGuiEngine.h"

namespace reka::app {

namespace {

// Small layout helper: a column of "label | control" rows.
struct Rows {
    juce::Rectangle<int> area;
    int labelWidth = 150;
    juce::Rectangle<int> next(juce::Label& label, int height = 28) {
        auto row = area.removeFromTop(height);
        area.removeFromTop(4);
        label.setBounds(row.removeFromLeft(labelWidth));
        return row;
    }
    juce::Rectangle<int> full(int height = 28) {
        auto row = area.removeFromTop(height);
        area.removeFromTop(4);
        return row;
    }
};

class Tab : public juce::Component, protected juce::Timer {
public:
    Tab(AppServices& app, RekaGuiEngine& gui) : app_(app), gui_(gui) {}

protected:
    juce::Label& label(const juce::String& text) {
        auto* l = labels_.add(new juce::Label({}, text));
        l->setColour(juce::Label::textColourId, gui_.theme().colour("textDim"));
        addAndMakeVisible(l);
        return *l;
    }
    void heading(juce::Label& l) {
        l.setFont(gui_.theme().font(gui_.theme().metrics.fontSize + 1.0f, true));
        l.setColour(juce::Label::textColourId, gui_.theme().colour("text"));
    }
    void timerCallback() override {}

    AppServices& app_;
    RekaGuiEngine& gui_;
    juce::OwnedArray<juce::Label> labels_;
};

// Simple string list box.
class StringListModel final : public juce::ListBoxModel {
public:
    explicit StringListModel(RekaGuiEngine& gui) : gui_(gui) {}
    juce::StringArray items;
    int getNumRows() override { return items.size(); }
    void paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected) override {
        if (selected) g.fillAll(gui_.theme().colour("selection"));
        g.setColour(gui_.theme().colour("text"));
        g.setFont(gui_.theme().font(12.0f));
        g.drawText(items[row], 6, 0, width - 12, height, juce::Justification::centredLeft);
    }

private:
    RekaGuiEngine& gui_;
};

// ---- Audio ---------------------------------------------------------------------------------------------
class AudioTab final : public Tab {
public:
    AudioTab(AppServices& app, RekaGuiEngine& gui) : Tab(app, gui), secondaryModel_(gui) {
        heading(masterHeading_ = label("Master device (clocks the engine)"));
        auto& devices = app_.devices();
        type_.addItem("Software clock (virtual I/O only)", 1);
        int id = 2;
        for (const auto& name : devices.typeNames()) type_.addItem(name, id++);
        type_.onChange = [this] { refreshDevices(); };
        for (auto* c : std::initializer_list<juce::Component*>{&type_, &input_, &output_, &rate_, &buffer_, &apply_, &status_, &secondaryList_,
                                                                &secType_, &secDevice_, &secDirection_, &secAdd_, &secRemove_})
            addAndMakeVisible(c);
        input_.onChange = output_.onChange = [this] { refreshFormats(); };
        apply_.onClick = [this] {
            const auto error = app_.devices().setMaster(currentSetup());
            if (error.isNotEmpty()) app_.log("Audio device: " + error);
            refreshStatus();
        };

        heading(secondaryHeading_ = label("Secondary devices (drift-corrected, any sample rate)"));
        secondaryList_.setModel(&secondaryModel_);
        for (const auto& name : devices.typeNames()) secType_.addItem(name, secType_.getNumItems() + 1);
        secType_.onChange = [this] { refreshSecondaryDevices(); };
        secDirection_.addItem("Input (capture)", 1);
        secDirection_.addItem("Output (playback)", 2);
        secDirection_.setSelectedId(2, juce::dontSendNotification);
        secDirection_.onChange = [this] { refreshSecondaryDevices(); };
        secAdd_.onClick = [this] {
            SecondarySetup setup;
            setup.type = secType_.getText();
            setup.device = secDevice_.getText();
            setup.isInput = secDirection_.getSelectedId() == 1;
            if (setup.device.isEmpty()) return;
            const auto error = app_.devices().addSecondary(setup);
            if (error.isNotEmpty()) app_.log(setup.device + ": " + error);
            refreshSecondaries();
        };
        secRemove_.onClick = [this] {
            const int row = secondaryList_.getSelectedRow();
            const auto list = app_.devices().secondaries();
            if (row >= 0 && row < static_cast<int>(list.size())) app_.devices().removeSecondary(list[static_cast<std::size_t>(row)].key());
            refreshSecondaries();
        };

        const auto& master = devices.master();
        type_.setText(master.type.isEmpty() ? type_.getItemText(0) : master.type, juce::dontSendNotification);
        refreshDevices();
        input_.setText(master.input, juce::dontSendNotification);
        output_.setText(master.output, juce::dontSendNotification);
        refreshFormats();
        secType_.setSelectedItemIndex(juce::jmax(0, secType_.indexOfItemId(secType_.getItemId(0))), juce::dontSendNotification);
        if (devices.typeNames().contains("Windows Audio")) secType_.setText("Windows Audio", juce::dontSendNotification);
        refreshSecondaryDevices();
        refreshSecondaries();
        startTimerHz(2);
    }

    void resized() override {
        Rows rows{getLocalBounds().reduced(16)};
        masterHeading_.setBounds(rows.full(24));
        type_.setBounds(rows.next(label0(), 28));
        input_.setBounds(rows.next(label1()));
        output_.setBounds(rows.next(label2()));
        auto formatRow = rows.next(label3());
        rate_.setBounds(formatRow.removeFromLeft(formatRow.getWidth() / 2).reduced(0, 0).withTrimmedRight(4));
        buffer_.setBounds(formatRow);
        auto applyRow = rows.full();
        apply_.setBounds(applyRow.removeFromLeft(140));
        status_.setBounds(applyRow.withTrimmedLeft(10));
        rows.full(8);
        secondaryHeading_.setBounds(rows.full(24));
        auto addRow = rows.full();
        secType_.setBounds(addRow.removeFromLeft(170).withTrimmedRight(4));
        secDirection_.setBounds(addRow.removeFromLeft(150).withTrimmedRight(4));
        secAdd_.setBounds(addRow.removeFromRight(70));
        secDevice_.setBounds(addRow.withTrimmedRight(4));
        auto bottom = rows.area;
        secRemove_.setBounds(bottom.removeFromBottom(26).removeFromLeft(140));
        secondaryList_.setBounds(bottom.withTrimmedBottom(4));
    }

private:
    juce::Label& label0() { return fieldLabel(0, "Driver"); }
    juce::Label& label1() { return fieldLabel(1, "Input device"); }
    juce::Label& label2() { return fieldLabel(2, "Output device"); }
    juce::Label& label3() { return fieldLabel(3, "Sample rate / buffer"); }
    juce::Label& fieldLabel(int index, const juce::String& text) {
        while (fieldLabels_.size() <= index) fieldLabels_.add(&label({}));
        fieldLabels_[index]->setText(text, juce::dontSendNotification);
        return *fieldLabels_[index];
    }

    MasterSetup currentSetup() const {
        MasterSetup setup;
        setup.type = type_.getSelectedId() <= 1 ? juce::String() : type_.getText();
        setup.input = input_.getText() == "(none)" ? juce::String() : input_.getText();
        setup.output = output_.getText() == "(none)" ? juce::String() : output_.getText();
        setup.sampleRate = rate_.getText().getDoubleValue() > 0.0 ? rate_.getText().getDoubleValue() : 48000.0;
        setup.bufferSize = buffer_.getText().getIntValue() > 0 ? buffer_.getText().getIntValue() : 256;
        return setup;
    }

    void refreshDevices() {
        const bool software = type_.getSelectedId() <= 1;
        input_.clear(juce::dontSendNotification);
        output_.clear(juce::dontSendNotification);
        input_.addItem("(none)", 1);
        output_.addItem("(none)", 1);
        if (!software) {
            int id = 2;
            for (const auto& name : app_.devices().deviceNames(type_.getText(), true)) input_.addItem(name, id++);
            id = 2;
            for (const auto& name : app_.devices().deviceNames(type_.getText(), false)) output_.addItem(name, id++);
            input_.setSelectedItemIndex(input_.getNumItems() > 1 ? 1 : 0, juce::dontSendNotification);
            output_.setSelectedItemIndex(output_.getNumItems() > 1 ? 1 : 0, juce::dontSendNotification);
        } else {
            input_.setSelectedId(1, juce::dontSendNotification);
            output_.setSelectedId(1, juce::dontSendNotification);
        }
        input_.setEnabled(!software);
        output_.setEnabled(!software);
        refreshFormats();
    }

    void refreshFormats() {
        const auto setup = currentSetup();
        const auto previousRate = rate_.getText(), previousBuffer = buffer_.getText();
        rate_.clear(juce::dontSendNotification);
        buffer_.clear(juce::dontSendNotification);
        int id = 1;
        for (double r : app_.devices().sampleRates(setup)) rate_.addItem(juce::String(juce::roundToInt(r)), id++);
        id = 1;
        for (int b : app_.devices().bufferSizes(setup)) buffer_.addItem(juce::String(b), id++);
        const auto& master = app_.devices().master();
        rate_.setText(previousRate.isNotEmpty() ? previousRate : juce::String(juce::roundToInt(master.sampleRate)), juce::dontSendNotification);
        buffer_.setText(previousBuffer.isNotEmpty() ? previousBuffer : juce::String(master.bufferSize), juce::dontSendNotification);
    }

    void refreshSecondaryDevices() {
        secDevice_.clear(juce::dontSendNotification);
        int id = 1;
        for (const auto& name : app_.devices().deviceNames(secType_.getText(), secDirection_.getSelectedId() == 1)) secDevice_.addItem(name, id++);
        secDevice_.setSelectedItemIndex(0, juce::dontSendNotification);
    }

    void refreshSecondaries() {
        secondaryModel_.items.clear();
        for (const auto& s : app_.devices().secondaries()) {
            juce::String line = (s.isInput ? "IN   " : "OUT  ") + s.device + "  -  " + app_.devices().secondaryStatus(s.key());
            if (auto stats = app_.devices().secondaryStats(s.key()))
                line << "  |  drift " << juce::String(s.isInput ? stats->captureDriftPpm : stats->renderDriftPpm, 1) << " ppm, xruns "
                     << juce::String(s.isInput ? stats->captureUnderruns : stats->renderUnderruns);
            secondaryModel_.items.add(line);
        }
        secondaryList_.updateContent();
        secondaryList_.repaint();
    }

    void refreshStatus() {
        auto& devices = app_.devices();
        status_.setText(devices.masterStatus() + "   CPU " + juce::String(juce::roundToInt(app_.engine().cpuLoad() * 100.0f)) +
                            " %   out latency " + juce::String(devices.outputLatencyMs(), 1) + " ms" +
                            (devices.xruns() >= 0 ? "   xruns " + juce::String(devices.xruns()) : juce::String()),
                        juce::dontSendNotification);
    }

    void timerCallback() override {
        refreshStatus();
        refreshSecondaries();
    }

    juce::Label masterHeading_, secondaryHeading_;
    juce::Array<juce::Label*> fieldLabels_;
    juce::ComboBox type_, input_, output_, rate_, buffer_;
    juce::TextButton apply_{"Apply"};
    juce::Label status_;
    StringListModel secondaryModel_;
    juce::ListBox secondaryList_;
    juce::ComboBox secType_, secDevice_, secDirection_;
    juce::TextButton secAdd_{"Add"}, secRemove_{"Remove selected"};
};

// ---- Virtual I/O ------------------------------------------------------------------------------------------
class VirtualTab final : public Tab {
public:
    VirtualTab(AppServices& app, RekaGuiEngine& gui) : Tab(app, gui), model_(gui) {
        heading(heading_ = label("REKA virtual endpoints"));
        name_.setText("REKA Input 3", juce::dontSendNotification);
        flow_.addItem("Input - apps play into it (Windows playback device)", 1);
        flow_.addItem("Output - apps record from it (Windows recording device)", 2);
        flow_.setSelectedId(1, juce::dontSendNotification);
        create_.onClick = [this] {
            VirtualEndpointSpec spec;
            spec.name = name_.getText().trim().toStdString();
            spec.flow = flow_.getSelectedId() == 2 ? EndpointFlow::Capture : EndpointFlow::Render;
            if (spec.name.empty()) return;
            const auto error = app_.virtualIo().addVirtualEndpoint(spec);
            if (error.isNotEmpty()) app_.log(juce::String(spec.name) + ": " + error);
            refreshList();
        };
        list_.setModel(&model_);
        remove_.onClick = [this] {
            const auto entries = app_.virtualIo().entries();
            const int row = list_.getSelectedRow();
            if (row >= 0 && row < static_cast<int>(entries.size())) app_.virtualIo().remove(entries[static_cast<std::size_t>(row)].key);
            refreshList();
        };
        heading(appsHeading_ = label("Per-application capture (WASAPI process loopback)"));
        refreshApps_.onClick = [this] { refreshApps(); };
        captureApp_.onClick = [this] {
            if (apps_.getText().isEmpty()) return;
            const auto error = app_.virtualIo().addApplicationCapture(apps_.getText(), true);
            if (error.isNotEmpty()) app_.log(apps_.getText() + ": " + error);
            refreshList();
        };
        soundPanel_.onClick = [] { RekaVirtualAudio::openSoundControlPanel(); };
        for (auto* c : std::initializer_list<juce::Component*>{&driver_, &name_, &flow_, &create_, &list_, &remove_, &apps_, &refreshApps_,
                                                                &captureApp_, &cables_, &soundPanel_})
            addAndMakeVisible(c);
        cables_.setJustificationType(juce::Justification::topLeft);
        refreshDriver();
        refreshApps();
        refreshList();
        startTimerHz(1);
    }

    void resized() override {
        Rows rows{getLocalBounds().reduced(16)};
        heading_.setBounds(rows.full(24));
        driver_.setBounds(rows.full(22));
        auto create = rows.full();
        name_.setBounds(create.removeFromLeft(160).withTrimmedRight(4));
        create_.setBounds(create.removeFromRight(130));
        flow_.setBounds(create.withTrimmedRight(4));
        list_.setBounds(rows.full(130));
        remove_.setBounds(rows.full(26).removeFromLeft(150));
        rows.full(6);
        appsHeading_.setBounds(rows.full(24));
        auto appRow = rows.full();
        captureApp_.setBounds(appRow.removeFromRight(130));
        refreshApps_.setBounds(appRow.removeFromRight(90).withTrimmedRight(4));
        apps_.setBounds(appRow.withTrimmedRight(4));
        soundPanel_.setBounds(rows.area.removeFromBottom(26).removeFromLeft(220));
        cables_.setBounds(rows.area);
    }

private:
    void refreshDriver() {
        auto& backend = app_.virtualIo().backend();
        juce::String text;
        switch (backend.driverState()) {
            case DriverState::Ready: text = "Driver: REKA Virtual Audio " + juce::String(backend.driverVersion()) + " - ready"; break;
            case DriverState::NotInstalled: text = "Driver: not installed - use per-app capture or a third-party cable (see docs/VIRTUAL_AUDIO_DRIVER.md)"; break;
            case DriverState::VersionMismatch: text = "Driver: version mismatch - reinstall the REKA driver"; break;
            case DriverState::AccessDenied: text = "Driver: access denied"; break;
            case DriverState::Unsupported: text = "Driver: virtual endpoints require Windows"; break;
        }
        driver_.setText(text, juce::dontSendNotification);

        juce::StringArray cables;
        for (const auto& endpoint : backend.enumerateEndpoints())
            if (endpoint.isThirdPartyCable || endpoint.isRekaVirtual)
                cables.add(juce::String(endpoint.flow == EndpointFlow::Render ? "playback  " : "recording ") + juce::String(endpoint.name) +
                           (endpoint.isRekaVirtual ? "  (REKA)" : "  - add it under Audio > Secondary devices"));
        cables_.setText(cables.isEmpty() ? juce::String("No virtual cables detected.") : "Detected virtual cables:\n" + cables.joinIntoString("\n"),
                        juce::dontSendNotification);
    }

    void refreshApps() {
        apps_.clear(juce::dontSendNotification);
        int id = 1;
        juce::StringArray seen;
        for (const auto& process : app_.virtualIo().backend().listAudioProcesses()) {
            const juce::String exe(process.executable);
            if (seen.contains(exe)) continue;
            seen.add(exe);
            apps_.addItem(exe, id++);
        }
        if (!app_.virtualIo().backend().processLoopbackSupported()) apps_.setTextWhenNothingSelected("Requires Windows 10 build 20348+ / Windows 11");
        else apps_.setTextWhenNothingSelected("Apps currently playing audio...");
    }

    void refreshList() {
        model_.items.clear();
        for (const auto& e : app_.virtualIo().entries())
            model_.items.add((e.kind == VirtualIoManager::Kind::Endpoint ? "ENDPOINT  " : "APP       ") + juce::String(e.key) + "  -  " +
                             (e.running ? juce::String("running") : e.status));
        list_.updateContent();
        list_.repaint();
    }

    void timerCallback() override { refreshList(); }

    juce::Label heading_, appsHeading_, driver_, cables_;
    juce::TextEditor name_;
    juce::ComboBox flow_, apps_;
    juce::TextButton create_{"Create endpoint"}, remove_{"Remove selected"}, refreshApps_{"Refresh"}, captureApp_{"Capture app"},
        soundPanel_{"Open Windows Sound settings"};
    StringListModel model_;
    juce::ListBox list_;
};

// ---- Control surface -----------------------------------------------------------------------------------------
class HardwareTab final : public Tab {
public:
    HardwareTab(AppServices& app, RekaGuiEngine& gui) : Tab(app, gui), mappingModel_(gui) {
        heading(serialHeading_ = label("REKA controller (USB serial)"));
        const auto& s = app_.hardwareSettings();
        enabled_.setToggleState(s.enabled, juce::dontSendNotification);
        port_.addItem("auto", 1);
        int id = 2;
        for (const auto& p : enumerateSerialPorts()) port_.addItem(juce::String(p.name), id++);
        port_.setText(juce::String(s.port), juce::dontSendNotification);
        for (int b : {9600, 57600, 115200, 230400, 460800}) baud_.addItem(juce::String(b), b);
        baud_.setSelectedId(s.baud, juce::dontSendNotification);
        led_.setRange(0, 255, 1);
        led_.setValue(s.ledBrightness, juce::dontSendNotification);
        oled_.setRange(0, 255, 1);
        oled_.setValue(s.oledContrast, juce::dontSendNotification);
        for (auto* slider : {&led_, &oled_}) slider->setTextBoxStyle(juce::Slider::TextBoxRight, false, 50, 20);
        apply_.onClick = [this] {
            HardwareSettings settings = app_.hardwareSettings();
            settings.enabled = enabled_.getToggleState();
            settings.port = port_.getText().toStdString();
            settings.baud = baud_.getSelectedId() > 0 ? baud_.getSelectedId() : 115200;
            settings.ledBrightness = static_cast<int>(led_.getValue());
            settings.oledContrast = static_cast<int>(oled_.getValue());
            app_.applyHardwareSettings(settings);
        };

        heading(midiHeading_ = label("MIDI control surface"));
        midiIn_.addItem("(none)", 1);
        midiOut_.addItem("(none)", 1);
        id = 2;
        for (const auto& d : app_.midi().availableInputs()) {
            midiIn_.addItem(d.name, id);
            midiInIds_.set(id++, d.identifier);
        }
        id = 2;
        for (const auto& d : app_.midi().availableOutputs()) {
            midiOut_.addItem(d.name, id);
            midiOutIds_.set(id++, d.identifier);
        }
        midiIn_.onChange = [this] { app_.midi().openInput(midiInIds_[midiIn_.getSelectedId()]); };
        midiOut_.onChange = [this] { app_.midi().openOutput(midiOutIds_[midiOut_.getSelectedId()]); };

        for (StripKind kind : {StripKind::Input, StripKind::Bus})
            for (StripId sid : app_.mixer().strips(kind)) {
                strip_.addItem(juce::String(app_.mixer().strip(sid)->name), static_cast<int>(sid));
            }
        for (int t = 0; t < 5; ++t) target_.addItem(MidiControllerBridge::toString(static_cast<MidiControllerBridge::Target>(t)), t + 1);
        target_.setSelectedId(1, juce::dontSendNotification);
        learn_.onClick = [this] {
            if (strip_.getSelectedId() <= 0) return;
            app_.midi().startLearn(static_cast<StripId>(strip_.getSelectedId()),
                                   static_cast<MidiControllerBridge::Target>(target_.getSelectedId() - 1));
        };
        clearMapping_.onClick = [this] {
            auto mappings = app_.midi().mappings();
            const int row = mappings_.getSelectedRow();
            if (row >= 0 && row < static_cast<int>(mappings.size())) mappings.erase(mappings.begin() + row);
            app_.midi().setMappings(mappings);
        };
        mappings_.setModel(&mappingModel_);
        for (auto* c : std::initializer_list<juce::Component*>{&enabled_, &port_, &baud_, &led_, &oled_, &apply_, &status_, &midiIn_, &midiOut_,
                                                                &strip_, &target_, &learn_, &mappings_, &clearMapping_})
            addAndMakeVisible(c);
        timerCallback();
        startTimerHz(4);
    }

    void resized() override {
        Rows rows{getLocalBounds().reduced(16)};
        serialHeading_.setBounds(rows.full(24));
        auto r1 = rows.full();
        enabled_.setBounds(r1.removeFromLeft(110));
        port_.setBounds(r1.removeFromLeft(180).withTrimmedRight(4));
        baud_.setBounds(r1.removeFromLeft(110).withTrimmedRight(4));
        apply_.setBounds(r1.removeFromLeft(90));
        auto r2 = rows.full();
        led_.setBounds(r2.removeFromLeft(r2.getWidth() / 2).withTrimmedRight(8));
        oled_.setBounds(r2);
        status_.setBounds(rows.full(40));
        rows.full(6);
        midiHeading_.setBounds(rows.full(24));
        auto r3 = rows.full();
        midiIn_.setBounds(r3.removeFromLeft(r3.getWidth() / 2).withTrimmedRight(4));
        midiOut_.setBounds(r3);
        auto r4 = rows.full();
        strip_.setBounds(r4.removeFromLeft(180).withTrimmedRight(4));
        target_.setBounds(r4.removeFromLeft(120).withTrimmedRight(4));
        learn_.setBounds(r4.removeFromLeft(120));
        clearMapping_.setBounds(rows.area.removeFromBottom(26).removeFromLeft(160));
        mappings_.setBounds(rows.area.withTrimmedBottom(4));
    }

private:
    void timerCallback() override {
        auto& hw = app_.hardware();
        status_.setText(juce::String(hw.statusText()) + "\nframes received " + juce::String(hw.framesReceived()) + ", frame errors " +
                            juce::String(hw.frameErrors()) + "   |   LED brightness / OLED contrast",
                        juce::dontSendNotification);
        learn_.setButtonText(app_.midi().isLearning() ? "Move a control..." : "MIDI learn");
        mappingModel_.items.clear();
        for (const auto& m : app_.midi().mappings()) {
            const StripInfo* info = app_.mixer().strip(m.strip);
            mappingModel_.items.add((m.isNote ? "Note " : "CC ") + juce::String(m.number) + " ch" + juce::String(m.channel) + "  ->  " +
                                    (info != nullptr ? juce::String(info->name) : juce::String("(missing strip)")) + " / " +
                                    MidiControllerBridge::toString(m.target));
        }
        mappings_.updateContent();
        mappings_.repaint();
    }

    juce::Label serialHeading_, midiHeading_, status_;
    juce::ToggleButton enabled_{"Enabled"};
    juce::ComboBox port_, baud_, midiIn_, midiOut_, strip_, target_;
    juce::HashMap<int, juce::String> midiInIds_, midiOutIds_;
    juce::Slider led_, oled_;
    juce::TextButton apply_{"Apply"}, learn_{"MIDI learn"}, clearMapping_{"Remove mapping"};
    StringListModel mappingModel_;
    juce::ListBox mappings_;
};

// ---- Plugins + general --------------------------------------------------------------------------------------
class GeneralTab final : public Tab {
public:
    GeneralTab(AppServices& app, RekaGuiEngine& gui) : Tab(app, gui) {
        heading(pluginsHeading_ = label("Plugins"));
        paths_.setText(app_.settings().pluginSearchPath, juce::dontSendNotification);
        paths_.setTextToShowWhenEmpty("VST3 folders, ';'-separated (empty = default locations)", gui.theme().colour("textDim"));
        applyPaths_.onClick = [this] {
            app_.settings().pluginSearchPath = paths_.getText();
            app_.plugins().setSearchPath(juce::FileSearchPath(paths_.getText()));
            app_.plugins().startScan(false);
        };
        heading(generalHeading_ = label("General"));
        auto& s = app_.settings();
        startMinimised_.setToggleState(s.startMinimised, juce::dontSendNotification);
        tray_.setToggleState(s.minimiseToTray, juce::dontSendNotification);
        watch_.setToggleState(s.watchProfileForChanges, juce::dontSendNotification);
        startMinimised_.onClick = [this] { app_.settings().startMinimised = startMinimised_.getToggleState(); };
        tray_.onClick = [this] { app_.settings().minimiseToTray = tray_.getToggleState(); };
        watch_.onClick = [this] { app_.settings().watchProfileForChanges = watch_.getToggleState(); };
        openData_.onClick = [] { AppPaths::dataDirectory().startAsProcess(); };
        for (auto* c : std::initializer_list<juce::Component*>{&paths_, &applyPaths_, &startMinimised_, &tray_, &watch_, &openData_, &log_})
            addAndMakeVisible(c);
        log_.setMultiLine(true);
        log_.setReadOnly(true);
        log_.setFont(gui.theme().monoFont(11.0f));
        log_.setText(app_.recentLog().joinIntoString("\n"));
    }

    void resized() override {
        Rows rows{getLocalBounds().reduced(16)};
        pluginsHeading_.setBounds(rows.full(24));
        auto r = rows.full();
        applyPaths_.setBounds(r.removeFromRight(120));
        paths_.setBounds(r.withTrimmedRight(4));
        rows.full(6);
        generalHeading_.setBounds(rows.full(24));
        startMinimised_.setBounds(rows.full(24));
        tray_.setBounds(rows.full(24));
        watch_.setBounds(rows.full(24));
        openData_.setBounds(rows.full(26).removeFromLeft(200));
        log_.setBounds(rows.area);
    }

private:
    juce::Label pluginsHeading_, generalHeading_;
    juce::TextEditor paths_, log_;
    juce::TextButton applyPaths_{"Apply & scan"}, openData_{"Open REKA data folder"};
    juce::ToggleButton startMinimised_{"Start minimised to the tray"}, tray_{"Closing the window keeps REKA running in the tray"},
        watch_{"Hot-reload the profile when its file changes on disk"};
};

}  // namespace

SettingsPanel::SettingsPanel(AppServices& app, RekaGuiEngine& gui) : gui_(gui) {
    const auto background = juce::Colours::transparentBlack;
    tabs_.addTab("Audio", background, new AudioTab(app, gui), true);
    tabs_.addTab("Virtual I/O", background, new VirtualTab(app, gui), true);
    tabs_.addTab("Control surface", background, new HardwareTab(app, gui), true);
    tabs_.addTab("Plugins & general", background, new GeneralTab(app, gui), true);
    tabs_.setTabBarDepth(32);
    addAndMakeVisible(tabs_);
    setSize(820, 620);
}

void SettingsPanel::paint(juce::Graphics& g) { gui_.lookAndFeel().drawBackground(g, getLocalBounds().toFloat()); }

void SettingsPanel::resized() { tabs_.setBounds(getLocalBounds().reduced(8)); }

}  // namespace reka::app
