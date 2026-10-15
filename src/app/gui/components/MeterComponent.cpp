#include "gui/components/MeterComponent.h"

#include "reka/dsp/Decibels.h"

namespace reka::app {

MeterComponent::MeterComponent(RekaLookAndFeel& lookAndFeel, std::shared_ptr<StripProcessor> processor,
                               std::function<float()> integratedLufs)
    : lookAndFeel_(lookAndFeel),
      processor_(std::move(processor)),
      integratedLufs_(std::move(integratedLufs)),
      vblank_(this, [this] { tick(); }) {
    setOpaque(false);
    setTooltip("Click to reset peak hold, clip and loudness");
}

void MeterComponent::tick() {
    if (processor_ == nullptr) return;
    const double now = juce::Time::getMillisecondCounterHiRes();
    const float dt = lastTick_ > 0.0 ? static_cast<float>((now - lastTick_) * 0.001) : 0.016f;
    lastTick_ = now;

    const Theme& theme = lookAndFeel_.theme();
    const MeterReading reading = processor_->meter.read(MeterReader::Ui);
    const float decay = theme.meters.decayDbPerSecond * dt;
    const float vuCoefficient = 1.0f - std::exp(-dt / 0.3f);

    bool changed = false;
    for (std::size_t ch = 0; ch < channels_.size(); ++ch) {
        ChannelState& c = channels_[ch];
        const float peak = gainToDb(reading.peak[ch]);
        const float previous = c.peakDb;
        c.peakDb = juce::jmax(peak, c.peakDb - decay);
        if (c.peakDb >= c.holdDb) {
            c.holdDb = c.peakDb;
            c.holdUntil = now + theme.meters.peakHoldMs;
        } else if (now > c.holdUntil) {
            c.holdDb = juce::jmax(c.peakDb, c.holdDb - decay);
        }
        const float rms = gainToDb(reading.rms[ch]);
        c.rmsDb = rms;
        c.vuDb += vuCoefficient * (rms - c.vuDb);
        changed = changed || std::abs(previous - c.peakDb) > 0.05f || c.peakDb > theme.meters.floorDb;
    }
    clipped_ = clipped_ || reading.clipped;
    momentary_ = processor_->loudness.momentaryLufs();
    shortTerm_ = processor_->loudness.shortTermLufs();
    if (integratedLufs_) integrated_ = integratedLufs_();
    if (changed || clipped_) repaint();
}

float MeterComponent::dbToY(float db, juce::Rectangle<float> area) const {
    const float floor = lookAndFeel_.theme().meters.floorDb;
    constexpr float ceiling = 3.0f;
    const float proportion = juce::jlimit(0.0f, 1.0f, (db - floor) / (ceiling - floor));
    return area.getBottom() - proportion * area.getHeight();
}

void MeterComponent::drawBar(juce::Graphics& g, juce::Rectangle<float> bar, float levelDb, float overlayDb, float holdDb) const {
    const Theme& theme = lookAndFeel_.theme();
    g.setColour(theme.colour("meterBackground"));
    g.fillRoundedRectangle(bar, 2.0f);

    const auto low = theme.colour("meterLow");
    const auto mid = theme.colour("meterMid");
    const auto high = theme.colour("meterHigh");
    const float topY = dbToY(levelDb, bar);

    if (theme.meters.gradient) {
        // Bottom (floor) -> top (+3 dB): low colour, then mid from midDb, high from highDb.
        juce::ColourGradient gradient(low, bar.getX(), bar.getBottom(), high, bar.getX(), bar.getY(), false);
        const auto proportionOf = [&](float db) {
            return juce::jlimit(0.0, 1.0, static_cast<double>((bar.getBottom() - dbToY(db, bar)) / bar.getHeight()));
        };
        gradient.addColour(juce::jmax(0.0, proportionOf(theme.meters.midDb) - 0.05), low.interpolatedWith(mid, 0.5f));
        gradient.addColour(proportionOf(theme.meters.midDb), mid);
        gradient.addColour(proportionOf(theme.meters.highDb), high);
        g.setGradientFill(gradient);
    } else {
        g.setColour(levelDb >= theme.meters.highDb ? high : (levelDb >= theme.meters.midDb ? mid : low));
    }

    const auto lit = bar.withTop(topY);
    if (theme.meters.segments > 0) {
        const float segment = bar.getHeight() / static_cast<float>(theme.meters.segments);
        for (int i = 0; i < theme.meters.segments; ++i) {
            const auto cell = juce::Rectangle<float>(bar.getX(), bar.getBottom() - (i + 1) * segment, bar.getWidth(), segment - 1.0f);
            if (cell.getBottom() <= topY) break;
            g.fillRect(cell);
        }
    } else {
        g.fillRoundedRectangle(lit, 2.0f);
    }

    if (overlayDb > theme.meters.floorDb) {  // RMS line over a peak bar
        g.setColour(theme.colour("meterRms"));
        g.fillRect(bar.getX(), dbToY(overlayDb, bar) - 1.0f, bar.getWidth(), 2.0f);
    }
    if (holdDb > theme.meters.floorDb) {
        g.setColour(theme.colour("meterPeak"));
        g.fillRect(bar.getX(), dbToY(holdDb, bar), bar.getWidth(), 1.5f);
    }
}

void MeterComponent::paint(juce::Graphics& g) {
    const Theme& theme = lookAndFeel_.theme();
    const MeterStyle style = styleOverride_.value_or(theme.meters.style);
    auto area = getLocalBounds().toFloat();
    const float readoutHeight = 14.0f;
    auto readout = area.removeFromBottom(readoutHeight);
    auto clipLed = area.removeFromTop(6.0f).reduced(1.0f, 0.0f);
    area.removeFromTop(2.0f);

    g.setColour(clipped_ ? theme.colour("clip") : theme.colour("meterBackground"));
    g.fillRoundedRectangle(clipLed, 2.0f);

    if (style == MeterStyle::Lufs) {
        drawBar(g, area.reduced(1.0f, 0.0f), momentary_, shortTerm_, integrated_);
    } else {
        const float gap = 2.0f;
        const float barWidth = (area.getWidth() - gap) * 0.5f;
        for (std::size_t ch = 0; ch < channels_.size(); ++ch) {
            const auto& c = channels_[ch];
            const auto bar = juce::Rectangle<float>(area.getX() + ch * (barWidth + gap), area.getY(), barWidth, area.getHeight());
            switch (style) {
                case MeterStyle::Peak: drawBar(g, bar, c.peakDb, -100.0f, c.holdDb); break;
                case MeterStyle::Rms: drawBar(g, bar, c.rmsDb, -100.0f, -100.0f); break;
                case MeterStyle::Vu: drawBar(g, bar, c.vuDb, -100.0f, -100.0f); break;
                case MeterStyle::PeakRms:
                case MeterStyle::Lufs: drawBar(g, bar, c.peakDb, c.rmsDb, c.holdDb); break;
            }
        }
    }

    // Numeric read-out: peak hold, or loudness when the meter is in LUFS mode.
    juce::String text;
    if (style == MeterStyle::Lufs || (theme.meters.showLufs && readout.getWidth() > 60.0f)) {
        text = momentary_ <= -99.0f ? juce::String("-inf") : juce::String(momentary_, 1);
        if (style == MeterStyle::Lufs) text << " M";
    } else {
        const float hold = juce::jmax(channels_[0].holdDb, channels_[1].holdDb);
        text = hold <= theme.meters.floorDb ? juce::String("-inf") : juce::String(hold, 1);
    }
    g.setColour(clipped_ ? theme.colour("clip") : theme.colour("textDim"));
    g.setFont(theme.monoFont(10.0f));
    g.drawFittedText(text, readout.toNearestInt(), juce::Justification::centred, 1);
}

void MeterComponent::mouseDown(const juce::MouseEvent&) {
    clipped_ = false;
    for (auto& c : channels_) c.holdDb = -100.0f;
    if (onResetLoudness) onResetLoudness();
    repaint();
}

}  // namespace reka::app
