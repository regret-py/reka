#include "gui/components/EqEditor.h"

namespace reka::app {

namespace {
constexpr float kMinFrequency = 20.0f;
constexpr float kMaxFrequency = 20000.0f;
constexpr float kRangeDb = 18.0f;

const FilterType kTypes[] = {FilterType::Bell,    FilterType::LowShelf, FilterType::HighShelf, FilterType::LowCut,
                             FilterType::HighCut, FilterType::Notch,    FilterType::BandPass,  FilterType::AllPass};
const char* kTypeNames[] = {"Bell", "Low shelf", "High shelf", "Low cut", "High cut", "Notch", "Band pass", "All pass"};

bool usesGain(FilterType type) {
    return type == FilterType::Bell || type == FilterType::LowShelf || type == FilterType::HighShelf;
}
}  // namespace

EqEditor::EqEditor(RekaLookAndFeel& lnf, EqParameters& eq, std::shared_ptr<void> keepAlive, double sampleRate,
                   juce::String title)
    : lookAndFeel_(lnf), eq_(eq), keepAlive_(std::move(keepAlive)), sampleRate_(sampleRate), title_(std::move(title)) {
    bands_ = eq_.bands();
    seenVersion_ = eq_.version();

    enabled_.setToggleState(eq_.enabled(), juce::dontSendNotification);
    enabled_.onClick = [this] { eq_.setEnabled(enabled_.getToggleState()); };
    addAndMakeVisible(enabled_);

    bandOn_.onClick = [this] {
        bands_[static_cast<std::size_t>(selected_)].enabled = bandOn_.getToggleState();
        pushBand();
    };
    addAndMakeVisible(bandOn_);

    for (int i = 0; i < 8; ++i) type_.addItem(kTypeNames[i], i + 1);
    type_.onChange = [this] {
        bands_[static_cast<std::size_t>(selected_)].type = kTypes[juce::jlimit(0, 7, type_.getSelectedId() - 1)];
        pushBand();
    };
    addAndMakeVisible(type_);

    auto setupSlider = [this](juce::Slider& s, double min, double max, double skewCentre, const juce::String& suffix) {
        s.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        s.setRange(min, max, 0.0);
        s.setSkewFactorFromMidPoint(skewCentre);
        s.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 70, 16);
        s.setTextValueSuffix(suffix);
        s.setNumDecimalPlacesToDisplay(suffix == " Hz" ? 0 : 1);
        addAndMakeVisible(s);
    };
    setupSlider(frequency_, kMinFrequency, kMaxFrequency, 1000.0, " Hz");
    setupSlider(gain_, -kRangeDb, kRangeDb, 0.0, " dB");
    setupSlider(q_, 0.1, 18.0, 1.0, " Q");
    frequency_.onValueChange = [this] { bands_[static_cast<std::size_t>(selected_)].frequency = static_cast<float>(frequency_.getValue()); pushBand(); };
    gain_.onValueChange = [this] { bands_[static_cast<std::size_t>(selected_)].gainDb = static_cast<float>(gain_.getValue()); pushBand(); };
    q_.onValueChange = [this] { bands_[static_cast<std::size_t>(selected_)].q = static_cast<float>(q_.getValue()); pushBand(); };

    selectBand(0);
    setSize(620, 360);
    startTimerHz(15);
}

void EqEditor::timerCallback() {
    // Follow changes made elsewhere (profile reload, another editor).
    if (eq_.version() == seenVersion_) return;
    seenVersion_ = eq_.version();
    bands_ = eq_.bands();
    enabled_.setToggleState(eq_.enabled(), juce::dontSendNotification);
    selectBand(selected_);
    repaint();
}

void EqEditor::selectBand(int index) {
    selected_ = juce::jlimit(0, kNumEqBands - 1, index);
    const EqBand& band = bands_[static_cast<std::size_t>(selected_)];
    bandOn_.setToggleState(band.enabled, juce::dontSendNotification);
    for (int i = 0; i < 8; ++i)
        if (kTypes[i] == band.type) type_.setSelectedId(i + 1, juce::dontSendNotification);
    frequency_.setValue(band.frequency, juce::dontSendNotification);
    gain_.setValue(band.gainDb, juce::dontSendNotification);
    gain_.setEnabled(usesGain(band.type));
    q_.setValue(band.q, juce::dontSendNotification);
    repaint();
}

void EqEditor::pushBand() {
    eq_.setBand(selected_, bands_[static_cast<std::size_t>(selected_)]);
    seenVersion_ = eq_.version();
    gain_.setEnabled(usesGain(bands_[static_cast<std::size_t>(selected_)].type));
    repaint();
}

juce::Rectangle<float> EqEditor::plotArea() const {
    return getLocalBounds().toFloat().reduced(12.0f).withTrimmedTop(24.0f).withTrimmedBottom(96.0f);
}

float EqEditor::frequencyToX(float frequency) const {
    const auto area = plotArea();
    const float t = std::log(frequency / kMinFrequency) / std::log(kMaxFrequency / kMinFrequency);
    return area.getX() + t * area.getWidth();
}

float EqEditor::xToFrequency(float x) const {
    const auto area = plotArea();
    const float t = juce::jlimit(0.0f, 1.0f, (x - area.getX()) / area.getWidth());
    return kMinFrequency * std::pow(kMaxFrequency / kMinFrequency, t);
}

float EqEditor::gainToY(float gainDb) const {
    const auto area = plotArea();
    return area.getCentreY() - gainDb / kRangeDb * area.getHeight() * 0.5f;
}

float EqEditor::yToGain(float y) const {
    const auto area = plotArea();
    return juce::jlimit(-kRangeDb, kRangeDb, (area.getCentreY() - y) / (area.getHeight() * 0.5f) * kRangeDb);
}

int EqEditor::hitTest(juce::Point<float> position) const {
    for (int i = 0; i < kNumEqBands; ++i) {
        const EqBand& band = bands_[static_cast<std::size_t>(i)];
        const juce::Point<float> node(frequencyToX(band.frequency), gainToY(usesGain(band.type) ? band.gainDb : 0.0f));
        if (node.getDistanceFrom(position) < 10.0f) return i;
    }
    return -1;
}

void EqEditor::paint(juce::Graphics& g) {
    const Theme& theme = lookAndFeel_.theme();
    lookAndFeel_.drawPanel(g, *this, getLocalBounds().toFloat(), false);
    g.setColour(theme.colour("text"));
    g.setFont(theme.font(theme.metrics.headerFontSize, true));
    g.drawText(title_, getLocalBounds().reduced(12, 6).removeFromTop(18), juce::Justification::centredLeft);

    const auto area = plotArea();
    g.setColour(theme.colour("meterBackground"));
    g.fillRoundedRectangle(area, 4.0f);

    g.setFont(theme.font(9.0f));
    for (float f : {50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f}) {
        const float x = frequencyToX(f);
        g.setColour(theme.colour("panelBorder"));
        g.drawVerticalLine(juce::roundToInt(x), area.getY(), area.getBottom());
        g.setColour(theme.colour("textDim"));
        g.drawText(f >= 1000.0f ? juce::String(f / 1000.0f, 0) + "k" : juce::String(juce::roundToInt(f)),
                   juce::Rectangle<float>(x - 20.0f, area.getBottom() - 12.0f, 40.0f, 12.0f), juce::Justification::centred);
    }
    for (float db : {-12.0f, -6.0f, 0.0f, 6.0f, 12.0f}) {
        g.setColour(theme.colour("panelBorder").withMultipliedAlpha(std::abs(db) < 0.01f ? 2.0f : 1.0f));
        g.drawHorizontalLine(juce::roundToInt(gainToY(db)), area.getX(), area.getRight());
    }

    // Total response
    juce::Path curve;
    const bool active = eq_.enabled();
    bool first = true;
    for (float x = area.getX(); x <= area.getRight(); x += 1.5f) {
        const float db = active ? static_cast<float>(eqMagnitudeDb(bands_, xToFrequency(x), sampleRate_)) : 0.0f;
        const float y = juce::jlimit(area.getY(), area.getBottom(), gainToY(db));
        if (first) curve.startNewSubPath(x, y);
        else curve.lineTo(x, y);
        first = false;
    }
    g.setColour(theme.colour("accent").withAlpha(active ? 1.0f : 0.35f));
    g.strokePath(curve, juce::PathStrokeType(2.0f));

    // Nodes
    for (int i = 0; i < kNumEqBands; ++i) {
        const EqBand& band = bands_[static_cast<std::size_t>(i)];
        const juce::Point<float> node(frequencyToX(band.frequency), gainToY(usesGain(band.type) ? band.gainDb : 0.0f));
        const auto circle = juce::Rectangle<float>(16.0f, 16.0f).withCentre(node);
        g.setColour(band.enabled ? theme.colour("accent") : theme.colour("button"));
        g.fillEllipse(circle);
        g.setColour(i == selected_ ? theme.colour("text") : theme.colour("panelBorder"));
        g.drawEllipse(circle, i == selected_ ? 2.0f : 1.0f);
        g.setColour(band.enabled ? juce::Colours::black : theme.colour("textDim"));
        g.drawText(juce::String(i + 1), circle, juce::Justification::centred);
    }
}

void EqEditor::resized() {
    auto header = getLocalBounds().reduced(12, 6).removeFromTop(18);
    enabled_.setBounds(header.removeFromRight(80));
    auto controls = getLocalBounds().reduced(12).removeFromBottom(86);
    bandOn_.setBounds(controls.removeFromLeft(90).withSizeKeepingCentre(90, 24));
    type_.setBounds(controls.removeFromLeft(120).withSizeKeepingCentre(116, 24));
    const int knobWidth = controls.getWidth() / 3;
    frequency_.setBounds(controls.removeFromLeft(knobWidth));
    gain_.setBounds(controls.removeFromLeft(knobWidth));
    q_.setBounds(controls);
}

void EqEditor::mouseDown(const juce::MouseEvent& event) {
    const int hit = hitTest(event.position);
    if (hit >= 0) selectBand(hit);
}

void EqEditor::mouseDrag(const juce::MouseEvent& event) {
    if (!plotArea().expanded(10.0f).contains(event.mouseDownPosition)) return;
    EqBand& band = bands_[static_cast<std::size_t>(selected_)];
    band.frequency = juce::jlimit(kMinFrequency, kMaxFrequency, xToFrequency(event.position.x));
    if (usesGain(band.type)) band.gainDb = yToGain(event.position.y);
    band.enabled = true;
    selectBand(selected_);
    pushBand();
}

void EqEditor::mouseDoubleClick(const juce::MouseEvent& event) {
    const int hit = hitTest(event.position);
    if (hit < 0) return;
    bands_[static_cast<std::size_t>(hit)].enabled = !bands_[static_cast<std::size_t>(hit)].enabled;
    selectBand(hit);
    pushBand();
}

void EqEditor::mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) {
    if (!plotArea().contains(event.position)) return;
    EqBand& band = bands_[static_cast<std::size_t>(selected_)];
    band.q = juce::jlimit(0.1f, 18.0f, band.q * (wheel.deltaY > 0.0f ? 1.15f : 1.0f / 1.15f));
    selectBand(selected_);
    pushBand();
}

}  // namespace reka::app
