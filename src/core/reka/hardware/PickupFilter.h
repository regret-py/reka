#pragma once

#include <cmath>

namespace reka {

// "Soft takeover" for non-motorised faders and pots.
//
// When a value is changed from somewhere else (the UI, a profile load, MIDI), the physical fader no
// longer matches it. Applying the next physical movement directly would make the value jump. Instead
// the filter ignores the fader until it reaches (or crosses) the current value, then follows it again.
// hint() tells the device which way to move (shown as an arrow on the OLED).
class PickupFilter {
public:
    static constexpr float kTolerance = 0.015f;  // normalised position

    // A physical movement. Returns true when the new position should be applied to the parameter.
    bool onHardwareMove(float hardwarePosition, float parameterPosition) noexcept {
        if (!synced_) {
            const bool close = std::fabs(hardwarePosition - parameterPosition) <= kTolerance;
            const bool crossed = known_ && ((lastHardware_ - parameterPosition) * (hardwarePosition - parameterPosition) <= 0.0f);
            synced_ = close || crossed;
        }
        lastHardware_ = hardwarePosition;
        known_ = true;
        return synced_;
    }

    // The parameter changed from another source.
    void onExternalChange(float parameterPosition) noexcept {
        synced_ = known_ && std::fabs(lastHardware_ - parameterPosition) <= kTolerance;
    }

    // Physical position reported without user movement (snapshot after connecting).
    void setHardwarePosition(float hardwarePosition, float parameterPosition) noexcept {
        lastHardware_ = hardwarePosition;
        known_ = true;
        synced_ = std::fabs(hardwarePosition - parameterPosition) <= kTolerance;
    }

    void reset() noexcept {
        known_ = false;
        synced_ = false;
    }

    bool synced() const noexcept { return synced_; }
    bool hardwarePositionKnown() const noexcept { return known_; }
    float hardwarePosition() const noexcept { return lastHardware_; }

    // +1 = move the fader up, -1 = move it down, 0 = in control.
    int hint(float parameterPosition) const noexcept {
        if (synced_ || !known_) return 0;
        return parameterPosition > lastHardware_ ? 1 : -1;
    }

private:
    float lastHardware_ = 0.0f;
    bool known_ = false;
    bool synced_ = false;
};

}  // namespace reka
