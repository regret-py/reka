#pragma once

#include "gui/RekaLookAndFeel.h"
#include "reka/engine/StripProcessor.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>
#include <memory>

namespace reka::app {

// Stereo level meter driven by the display's vertical blank (smooth on 60-240 Hz screens).
// Styles: Peak, RMS, Peak+RMS, VU (300 ms ballistics) and LUFS (momentary, with short-term/integrated
// read-outs). Decay speed, peak hold, zone thresholds and LED segmentation come from the theme.
class MeterComponent final : public juce::Component, public juce::SettableTooltipClient {
public:
    MeterComponent(RekaLookAndFeel& lookAndFeel, std::shared_ptr<StripProcessor> processor,
                   std::function<float()> integratedLufs = {});

    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& event) override;  // click resets peak hold, clip and LUFS

    void setStyleOverride(std::optional<MeterStyle> style) { styleOverride_ = style; }
    std::function<void()> onResetLoudness;

private:
    struct ChannelState {
        float peakDb = -100.0f;
        float holdDb = -100.0f;
        double holdUntil = 0.0;
        float rmsDb = -100.0f;
        float vuDb = -100.0f;
    };

    void tick();
    float dbToY(float db, juce::Rectangle<float> area) const;
    void drawBar(juce::Graphics& g, juce::Rectangle<float> bar, float levelDb, float overlayDb, float holdDb) const;

    RekaLookAndFeel& lookAndFeel_;
    std::shared_ptr<StripProcessor> processor_;
    std::function<float()> integratedLufs_;
    std::optional<MeterStyle> styleOverride_;
    std::array<ChannelState, 2> channels_{};
    bool clipped_ = false;
    float momentary_ = -100.0f, shortTerm_ = -100.0f, integrated_ = -100.0f;
    double lastTick_ = 0.0;
    juce::VBlankAttachment vblank_;
};

}  // namespace reka::app
