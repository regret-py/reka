#include "gui/components/StripComponent.h"

#include "AppServices.h"
#include "gui/RekaGuiEngine.h"
#include "gui/components/GainControl.h"
#include "gui/components/InsertRack.h"
#include "gui/components/MeterComponent.h"
#include "gui/components/Widgets.h"

namespace reka::app {

namespace {

using Widget = StripComponent::Widget;

juce::String shortLabel(const std::string& name) {
    const juce::String text(name);
    const juce::String first = text.upToFirstOccurrenceOf(" ", false, false);
    return first.length() <= 3 ? first : first.substring(0, 3).toUpperCase();
}

void styleToggle(juce::TextButton& button, juce::Colour onColour) {
    button.setClickingTogglesState(false);
    button.setColour(juce::TextButton::buttonOnColourId, onColour);
}

// ---- header ----------------------------------------------------------------------------------------------
class HeaderWidget final : public Widget {
public:
    HeaderWidget(AppServices& app, RekaGuiEngine& gui, StripId id, std::function<void()> onMenu) : app_(app), gui_(gui), id_(id) {
        name_.setJustificationType(juce::Justification::centred);
        name_.setEditable(false, true, false);
        name_.setFont(gui.theme().font(gui.theme().metrics.fontSize, true));
        name_.onTextChange = [this] { app_.mixer().renameStrip(id_, name_.getText().trim().toStdString()); };
        addAndMakeVisible(name_);
        menu_.setButtonText(juce::String::fromUTF8("\xE2\x80\xA6"));
        menu_.setTooltip("Strip options");
        menu_.onClick = std::move(onMenu);
        addAndMakeVisible(menu_);
        refresh();
    }
    int preferredHeight() const override { return 32; }
    void refresh() override {
        if (const StripInfo* info = app_.mixer().strip(id_)) {
            if (!name_.isBeingEdited() && name_.getText() != juce::String(info->name))
                name_.setText(juce::String(info->name), juce::dontSendNotification);
            colour_ = juce::Colour(info->colour);
        }
    }
    void paint(juce::Graphics& g) override {
        g.setColour(colour_);
        g.fillRoundedRectangle(getLocalBounds().removeFromTop(4).toFloat().reduced(6.0f, 0.0f), 2.0f);
    }
    void resized() override {
        auto area = getLocalBounds().withTrimmedTop(6);
        menu_.setBounds(area.removeFromRight(20).reduced(0, 3));
        name_.setBounds(area);
    }

private:
    AppServices& app_;
    RekaGuiEngine& gui_;
    StripId id_;
    juce::Label name_;
    juce::TextButton menu_;
    juce::Colour colour_;
};

// ---- source / sink selector ------------------------------------------------------------------------------
class SourceWidget final : public Widget {
public:
    SourceWidget(AppServices& app, StripId id) : app_(app), id_(id) {
        const StripInfo* info = app_.mixer().strip(id_);
        const bool isInput = info->kind == StripKind::Input;
        box_.setTooltip(isInput ? "Source: device / virtual endpoint / application channels" : "Output: device / virtual endpoint channels");

        candidates_.push_back({});
        box_.addItem(isInput ? "(no source)" : "(no output)", 1);
        auto ports = app_.mixer().ports();
        std::sort(ports.begin(), ports.end(), [](const PortInfo& a, const PortInfo& b) {
            return a.type != b.type ? a.type < b.type : a.displayName < b.displayName;
        });
        for (const PortInfo& port : ports) {
            const int channels = isInput ? port.numInputs : port.numOutputs;
            if (channels <= 0) continue;
            box_.addSectionHeading(juce::String(port.displayName));
            for (int ch = 0; ch + 1 < channels; ch += 2) add(port, ch, ch + 1, juce::String(ch + 1) + "/" + juce::String(ch + 2));
            if (channels > 1)
                for (int ch = 0; ch < channels; ++ch) add(port, ch, -1, juce::String(ch + 1) + " (mono)");
            else
                add(port, 0, -1, "mono");
        }

        // Keep showing a binding whose port is currently offline (unplugged device, app not running).
        int selected = 1;
        for (std::size_t i = 0; i < candidates_.size(); ++i)
            if (candidates_[i] == info->binding) selected = static_cast<int>(i) + 1;
        if (selected == 1 && !info->binding.port.empty()) {
            candidates_.push_back(info->binding);
            box_.addItem(juce::String(info->binding.port) + " (offline)", static_cast<int>(candidates_.size()));
            selected = static_cast<int>(candidates_.size());
        }
        box_.setSelectedId(selected, juce::dontSendNotification);
        box_.onChange = [this] {
            const int index = box_.getSelectedId() - 1;
            if (index >= 0 && index < static_cast<int>(candidates_.size()))
                app_.mixer().setBinding(id_, candidates_[static_cast<std::size_t>(index)]);
        };
        addAndMakeVisible(box_);
    }
    int preferredHeight() const override { return 26; }
    void resized() override { box_.setBounds(getLocalBounds().reduced(4, 2)); }

private:
    void add(const PortInfo& port, int left, int right, const juce::String& label) {
        candidates_.push_back({port.key, left, right});
        box_.addItem(juce::String(port.displayName).substring(0, 18) + ": " + label, static_cast<int>(candidates_.size()));
    }

    AppServices& app_;
    StripId id_;
    juce::ComboBox box_;
    std::vector<PortBinding> candidates_;
};

// ---- channel mode + polarity -----------------------------------------------------------------------------
class ModeWidget final : public Widget {
public:
    ModeWidget(RekaGuiEngine& gui, std::shared_ptr<StripParameters> params) : params_(std::move(params)) {
        mode_.setTooltip("Stereo / mono summing (click to cycle)");
        mode_.onClick = [this] {
            const int next = (static_cast<int>(params_->channelMode()) + 1) % 5;
            params_->setChannelMode(static_cast<ChannelMode>(next));
            refresh();
        };
        const auto accent = gui.theme().colour("accentAlt");
        styleToggle(phaseL_, accent);
        styleToggle(phaseR_, accent);
        phaseL_.setButtonText(juce::String::fromUTF8("\xC3\x98L"));
        phaseR_.setButtonText(juce::String::fromUTF8("\xC3\x98R"));
        phaseL_.setTooltip("Invert polarity (left)");
        phaseR_.setTooltip("Invert polarity (right)");
        phaseL_.onClick = [this] { params_->setPhaseInvertLeft(!params_->phaseInvertLeft()); refresh(); };
        phaseR_.onClick = [this] { params_->setPhaseInvertRight(!params_->phaseInvertRight()); refresh(); };
        addAndMakeVisible(mode_);
        addAndMakeVisible(phaseL_);
        addAndMakeVisible(phaseR_);
        refresh();
    }
    int preferredHeight() const override { return 24; }
    void refresh() override {
        static const char* names[] = {"ST", "MONO", "SWAP", "L", "R"};
        mode_.setButtonText(names[static_cast<int>(params_->channelMode())]);
        phaseL_.setToggleState(params_->phaseInvertLeft(), juce::dontSendNotification);
        phaseR_.setToggleState(params_->phaseInvertRight(), juce::dontSendNotification);
    }
    void resized() override {
        auto area = getLocalBounds().reduced(4, 1);
        const int w = area.getWidth() / 4;
        phaseR_.setBounds(area.removeFromRight(w).reduced(1, 0));
        phaseL_.setBounds(area.removeFromRight(w).reduced(1, 0));
        mode_.setBounds(area.reduced(1, 0));
    }

private:
    std::shared_ptr<StripParameters> params_;
    juce::TextButton mode_, phaseL_, phaseR_;
};

// ---- EQ button ---------------------------------------------------------------------------------------------
class EqWidget final : public Widget {
public:
    EqWidget(AppServices& app, RekaGuiEngine& gui, StripId id) : app_(app), gui_(gui), id_(id) {
        styleToggle(button_, gui.theme().colour("accent"));
        button_.setButtonText("EQ");
        button_.setTooltip("8-band parametric EQ (right-click: enable/disable)");
        button_.onClick = [this] {
            auto params = app_.mixer().parameters(id_);
            if (params == nullptr) return;
            if (button_.lastClickModifiers.isPopupMenu()) {
                params->eq().setEnabled(!params->eq().enabled());
                return;
            }
            gui_.showEqEditor(params->eq(), params, juce::String(app_.mixer().strip(id_)->name) + " - EQ", button_);
        };
        addAndMakeVisible(button_);
        refresh();
    }
    int preferredHeight() const override { return 24; }
    void refresh() override {
        auto params = app_.mixer().parameters(id_);
        if (params == nullptr) return;
        bool audible = false;
        for (const EqBand& band : params->eq().bands()) audible = audible || isBandAudible(band);
        button_.setToggleState(params->eq().enabled() && audible, juce::dontSendNotification);
    }
    void resized() override { button_.setBounds(getLocalBounds().reduced(4, 1)); }

private:
    AppServices& app_;
    RekaGuiEngine& gui_;
    StripId id_;
    ModsTextButton button_;
};

// ---- insert rack -------------------------------------------------------------------------------------------
class InsertsWidget final : public Widget {
public:
    InsertsWidget(AppServices& app, RekaGuiEngine& gui, StripId id) : rack_(app, gui, id) { addAndMakeVisible(rack_); }
    int preferredHeight() const override { return rack_.preferredHeight(); }
    void refresh() override { rack_.refresh(); }
    void resized() override { rack_.setBounds(getLocalBounds().reduced(4, 0)); }

private:
    InsertRack rack_;
};

// ---- quick routing (input -> bus toggles) --------------------------------------------------------------------
class SendsWidget final : public Widget {
public:
    SendsWidget(AppServices& app, RekaGuiEngine& gui, StripId id) : app_(app), gui_(gui), id_(id) {
        for (StripId bus : app_.mixer().strips(StripKind::Bus)) {
            const StripInfo* info = app_.mixer().strip(bus);
            auto* button = buttons_.add(new ModsTextButton(shortLabel(info->name)));
            styleToggle(*button, juce::Colour(info->colour));
            button->setTooltip("Route to " + juce::String(info->name) + " (right-click: send level, pan, EQ)");
            button->onClick = [this, bus, button] {
                if (button->lastClickModifiers.isPopupMenu()) {
                    app_.mixer().route(id_, bus);
                    gui_.showCrosspointEditor(id_, bus, *button);
                    return;
                }
                auto route = app_.mixer().findRoute(id_, bus);
                app_.mixer().setRouteEnabled(id_, bus, route == nullptr || !route->enabled());
                refresh();
            };
            buses_.push_back(bus);
            addAndMakeVisible(button);
        }
        refresh();
    }
    int preferredHeight() const override { return 2 + 20 * ((buttons_.size() + kPerRow - 1) / kPerRow); }
    void refresh() override {
        for (int i = 0; i < buttons_.size(); ++i) {
            auto route = app_.mixer().findRoute(id_, buses_[static_cast<std::size_t>(i)]);
            buttons_[i]->setToggleState(route != nullptr && route->enabled(), juce::dontSendNotification);
        }
    }
    void resized() override {
        auto area = getLocalBounds().reduced(4, 1);
        const int w = area.getWidth() / kPerRow;
        for (int i = 0; i < buttons_.size(); ++i)
            buttons_[i]->setBounds(area.getX() + (i % kPerRow) * w, area.getY() + (i / kPerRow) * 20, w - 2, 18);
    }

private:
    static constexpr int kPerRow = 3;
    AppServices& app_;
    RekaGuiEngine& gui_;
    StripId id_;
    juce::OwnedArray<ModsTextButton> buttons_;
    std::vector<StripId> buses_;
};

// ---- pan -------------------------------------------------------------------------------------------------------
class PanWidget final : public Widget {
public:
    PanWidget(RekaGuiEngine& gui, std::shared_ptr<StripParameters> params, juce::Colour colour)
        : params_(params), knob_([params] { return params->pan(); }, [params](float v) { params->setPan(v); }) {
        knob_.setAccentColour(colour);
        juce::ignoreUnused(gui);
        addAndMakeVisible(knob_);
    }
    int preferredHeight() const override { return 46; }
    void refresh() override { knob_.refresh(); }
    void resized() override { knob_.setBounds(getLocalBounds().withSizeKeepingCentre(44, 44)); }

private:
    std::shared_ptr<StripParameters> params_;
    PanKnob knob_;
};

// ---- gain + meter ------------------------------------------------------------------------------------------------
class GainWidget final : public Widget {
public:
    GainWidget(AppServices& app, RekaGuiEngine& gui, StripId id, GainStyle style)
        : gain_(GainControl::forStrip(app.mixer().parameters(id), style)),
          meter_(gui.lookAndFeel(), app.mixer().processor(id), [&app, id] { return app.mixer().integratedLufs(id); }) {
        gain_->setAccentColour(juce::Colour(app.mixer().strip(id)->colour));
        meter_.onResetLoudness = [&app, id] { app.mixer().resetIntegratedLoudness(id); };
        addAndMakeVisible(*gain_);
        addAndMakeVisible(meter_);
    }
    int preferredHeight() const override { return 170; }
    bool isFlexible() const override { return true; }
    void refresh() override { gain_->refresh(); }
    void resized() override {
        auto area = getLocalBounds().reduced(4, 2);
        if (gain_->style() == GainStyle::Knob) {
            // Knob on top, a tall meter underneath.
            gain_->setBounds(area.removeFromTop(juce::jmin(84, area.getHeight() / 2)));
            meter_.setBounds(area.withSizeKeepingCentre(juce::jmin(34, area.getWidth()), area.getHeight()));
        } else {
            meter_.setBounds(area.removeFromRight(juce::jmax(18, area.getWidth() / 4)));
            gain_->setBounds(area);
        }
    }

private:
    std::unique_ptr<GainControl> gain_;
    MeterComponent meter_;
};

// ---- mute / solo / fx -------------------------------------------------------------------------------------------
class ButtonsWidget final : public Widget {
public:
    ButtonsWidget(RekaGuiEngine& gui, std::shared_ptr<StripParameters> params) : params_(std::move(params)) {
        const Theme& theme = gui.theme();
        styleToggle(mute_, theme.colour("mute"));
        styleToggle(solo_, theme.colour("solo"));
        styleToggle(fx_, theme.colour("fx"));
        mute_.setButtonText("M");
        solo_.setButtonText("S");
        fx_.setButtonText("FX");
        mute_.setTooltip("Mute");
        solo_.setTooltip("Solo (solo-in-place)");
        fx_.setTooltip("Bypass the whole insert rack (latency-compensated)");
        mute_.onClick = [this] { params_->toggleMute(); refresh(); };
        solo_.onClick = [this] { params_->toggleSolo(); refresh(); };
        fx_.onClick = [this] { params_->toggleFxBypass(); refresh(); };
        addAndMakeVisible(mute_);
        addAndMakeVisible(solo_);
        addAndMakeVisible(fx_);
        refresh();
    }
    int preferredHeight() const override { return 28; }
    void refresh() override {
        mute_.setToggleState(params_->mute(), juce::dontSendNotification);
        solo_.setToggleState(params_->solo(), juce::dontSendNotification);
        fx_.setToggleState(params_->fxBypass(), juce::dontSendNotification);
    }
    void resized() override {
        auto area = getLocalBounds().reduced(4, 2);
        const int w = area.getWidth() / 3;
        mute_.setBounds(area.removeFromLeft(w).reduced(1, 0));
        solo_.setBounds(area.removeFromLeft(w).reduced(1, 0));
        fx_.setBounds(area.reduced(1, 0));
    }

private:
    std::shared_ptr<StripParameters> params_;
    juce::TextButton mute_, solo_, fx_;
};

}  // namespace

// ---- StripComponent -------------------------------------------------------------------------------------------

StripComponent::StripComponent(AppServices& app, RekaGuiEngine& gui, StripId id) : app_(app), gui_(gui), id_(id) {
    const StripInfo* info = app_.mixer().strip(id_);
    jassert(info != nullptr);
    const bool isInput = info->kind == StripKind::Input;
    const auto params = info->params;

    for (const auto& section : gui_.layout().stripTemplate) {
        std::unique_ptr<Widget> widget;
        if (section == "header") widget = std::make_unique<HeaderWidget>(app_, gui_, id_, [this] { showContextMenu(); });
        else if (section == "source") widget = std::make_unique<SourceWidget>(app_, id_);
        else if (section == "mode") widget = std::make_unique<ModeWidget>(gui_, params);
        else if (section == "eq") widget = std::make_unique<EqWidget>(app_, gui_, id_);
        else if (section == "inserts") widget = std::make_unique<InsertsWidget>(app_, gui_, id_);
        else if (section == "sends" && isInput) widget = std::make_unique<SendsWidget>(app_, gui_, id_);
        else if (section == "pan") widget = std::make_unique<PanWidget>(gui_, params, juce::Colour(info->colour));
        else if (section == "gain") widget = std::make_unique<GainWidget>(app_, gui_, id_, gui_.layout().gainStyleFor(id_));
        else if (section == "buttons") widget = std::make_unique<ButtonsWidget>(gui_, params);
        if (widget == nullptr) continue;
        addAndMakeVisible(*widget);
        widgets_.push_back(std::move(widget));
    }
}

StripComponent::~StripComponent() = default;

void StripComponent::refresh() {
    for (auto& widget : widgets_) widget->refresh();
}

void StripComponent::setSelected(bool selected) {
    if (selected_ == selected) return;
    selected_ = selected;
    repaint();
}

void StripComponent::paint(juce::Graphics& g) {
    auto& lnf = gui_.lookAndFeel();
    lnf.drawPanel(g, *this, getLocalBounds().toFloat().reduced(0.5f), true);
    if (selected_) {
        g.setColour(lnf.theme().colour("accent"));
        g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(1.0f), lnf.theme().metrics.cornerRadius, 2.0f);
    }
}

void StripComponent::resized() {
    auto area = getLocalBounds().reduced(0, 4);
    int fixed = 0;
    int flexibleCount = 0;
    for (const auto& widget : widgets_) {
        if (widget->isFlexible()) ++flexibleCount;
        else fixed += widget->preferredHeight();
    }
    const int flexibleHeight = flexibleCount > 0 ? juce::jmax(60, (area.getHeight() - fixed) / flexibleCount) : 0;
    for (auto& widget : widgets_) {
        const int h = widget->isFlexible() ? flexibleHeight : widget->preferredHeight();
        widget->setBounds(area.removeFromTop(h));
    }
}

void StripComponent::showContextMenu() {
    const StripInfo* info = app_.mixer().strip(id_);
    if (info == nullptr) return;
    juce::PopupMenu colours;
    const juce::uint32 palette[] = {0xFF4DA3FF, 0xFF41D6A0, 0xFFB47CFF, 0xFFFFC94D, 0xFFFF6B8B, 0xFF7CE0FF, 0xFFFF9F43, 0xFFE6EAF2};
    for (int i = 0; i < 8; ++i) {
        juce::PopupMenu::Item item;
        item.itemID = 100 + i;
        item.text = juce::Colour(palette[i]).toDisplayString(false);
        item.colour = juce::Colour(palette[i]);
        colours.addItem(item);
    }
    juce::PopupMenu style;
    const auto current = gui_.layout().gainStyleFor(id_);
    style.addItem(200, "Fader", true, current == GainStyle::Fader);
    style.addItem(201, "Rotary knob", true, current == GainStyle::Knob);
    juce::PopupMenu width;
    const int w = gui_.layout().widthFor(id_);
    width.addItem(300, "Narrow (88 px)", true, w == 88);
    width.addItem(301, "Normal (theme)", true, w == gui_.layout().stripWidth);
    width.addItem(302, "Wide (140 px)", true, w == 140);

    juce::PopupMenu menu;
    menu.addSectionHeader(juce::String(info->name));
    menu.addSubMenu("Colour", colours);
    menu.addSubMenu("Gain control", style);
    menu.addSubMenu("Width", width);
    menu.addSeparator();
    menu.addItem(1, "Move left");
    menu.addItem(2, "Move right");
    menu.addItem(3, "Hide strip");
    menu.addItem(4, "Reset integrated loudness");
    menu.addSeparator();
    menu.addItem(5, "Delete strip");

    menu.showMenuAsync(juce::PopupMenu::Options(), [this, palette](int result) {
        auto& mixer = app_.mixer();
        const int index = mixer.indexOf(id_);
        if (result >= 100 && result < 108) mixer.setStripColour(id_, palette[result - 100]);
        else if (result == 200 || result == 201)
            gui_.updateLayout([&](UiLayout& l) { l.overrides[id_].gainStyle = result == 201 ? GainStyle::Knob : GainStyle::Fader; });
        else if (result >= 300 && result <= 302)
            gui_.updateLayout([&](UiLayout& l) { l.overrides[id_].width = result == 300 ? 88 : (result == 302 ? 140 : 0); });
        else if (result == 1) mixer.moveStrip(id_, index - 1);
        else if (result == 2) mixer.moveStrip(id_, index + 1);
        else if (result == 3) gui_.updateLayout([&](UiLayout& l) { l.overrides[id_].hidden = true; });
        else if (result == 4) mixer.resetIntegratedLoudness(id_);
        else if (result == 5) {
            const StripId id = id_;
            auto options = juce::MessageBoxOptions::makeOptionsOkCancel(
                juce::MessageBoxIconType::QuestionIcon, "Delete strip",
                "Delete \"" + juce::String(app_.mixer().strip(id)->name) + "\" and its plugins?", "Delete", "Cancel", this);
            juce::AlertWindow::showAsync(options, [&app = app_, id](int answer) {
                if (answer == 1) app.mixer().removeStrip(id);
            });
        }
    });
}

}  // namespace reka::app
