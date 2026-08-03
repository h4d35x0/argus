#pragma once
//
// charge_state.h - pure charge-readout logic for the battery widget.
//
// Split out of main.cpp so the debounce rules are testable on the host
// (test/test_charge_state.cpp) without an AXP2101 on the bench. No Arduino,
// no LVGL, no hardware includes: keep it that way.
//
// The AXP2101 exposes two independent facts through XPowersLib:
//   isVbusIn()    - USB power is present at the port
//   isCharging()  - STATUS2[7:5] == 0x01, i.e. current is actually flowing
//                   into the cell right now
//
// Those are NOT the same thing, and conflating them is what makes a charge
// indicator lie. Sitting on the charger at 100%, the PMU parks in DONE/STOP
// and isCharging() reads false while USB is still plugged in. A widget driven
// off isCharging() alone therefore goes dark exactly when the user glances
// down to confirm the watch is on the charger. So we report three states, not
// two.
//
// C++11 only: the ESP32 Arduino core builds this at -std=gnu++11 even though
// the host test suite is C++17. No `if constexpr`, no multi-statement
// constexpr bodies, no inline variables.

#include <stdint.h>

enum class ChargeState : uint8_t {
    Discharging,   // no USB - running off the cell
    Charging,      // USB in, current flowing into the cell
    Topped,        // USB in, charger idle (full / termination / thermal hold)
};

// Instantaneous reading, no smoothing. `charging` wins over `vbus_in`: if the
// PMU says current is flowing then power is obviously present, so a dropped
// VBUS bit can never downgrade an active charge to Discharging.
inline ChargeState charge_state_raw(bool vbus_in, bool charging)
{
    if (charging) return ChargeState::Charging;
    if (vbus_in)  return ChargeState::Topped;
    return ChargeState::Discharging;
}

// Ticks a candidate state must persist before it is published. Fed at 1 Hz
// from the main-loop status block, so these are seconds.
//
// Near termination the AXP2101 oscillates CC -> CV -> DONE -> CC as the cell
// relaxes, which flips isCharging() every couple of seconds. Publishing that
// raw would strobe the bolt between pulsing and steady - the same class of
// defect as the tracker threat-level strobe (b98cea8, e60a24f). Five seconds
// of agreement is far longer than that oscillation and still well under human
// patience for a state that only matters at a glance.
static const uint8_t CHARGE_SETTLE_TICKS = 5;

// Unplugging is a clean hardware edge, not an oscillation, so it needs only
// enough hold to ride out cable chatter on a loose pogo connection.
static const uint8_t UNPLUG_SETTLE_TICKS = 2;

// Debounced charge readout. One instance per consumer; feed it one sample per
// 1 Hz tick and render whatever it returns.
//
// Asymmetric on purpose:
//   * Discharging -> anything   publishes immediately. Plugging in is the one
//                               edge where the user is actively waiting for
//                               feedback; a 5 s lag there reads as "the
//                               charger isn't seated" and is the exact
//                               complaint this widget exists to answer.
//   * anything    -> Discharging  holds UNPLUG_SETTLE_TICKS (cable chatter).
//   * Charging   <-> Topped       holds CHARGE_SETTLE_TICKS (CC/CV/DONE hunt).
class ChargeIndicator {
public:
    // Returns the state that should be rendered after folding in this sample.
    ChargeState update(bool vbus_in, bool charging)
    {
        ChargeState raw = charge_state_raw(vbus_in, charging);

        if (raw == stable_) {
            pending_count_ = 0;
            return stable_;
        }

        // Plug-in edge: publish now, no settling.
        if (stable_ == ChargeState::Discharging) {
            stable_        = raw;
            pending_count_ = 0;
            return stable_;
        }

        uint8_t needed = (raw == ChargeState::Discharging)
                       ? UNPLUG_SETTLE_TICKS
                       : CHARGE_SETTLE_TICKS;

        if (raw != pending_) {
            pending_       = raw;
            pending_count_ = 1;
        } else if (pending_count_ < 255) {
            pending_count_++;
        }

        if (pending_count_ >= needed) {
            stable_        = raw;
            pending_count_ = 0;
        }
        return stable_;
    }

    ChargeState state() const { return stable_; }

    // Seed from a first reading without waiting out any debounce. Call once at
    // boot so a watch powered up already on the charger shows the bolt on the
    // very first frame instead of after the first transition.
    void prime(bool vbus_in, bool charging)
    {
        stable_        = charge_state_raw(vbus_in, charging);
        pending_       = stable_;
        pending_count_ = 0;
    }

private:
    ChargeState stable_        = ChargeState::Discharging;
    ChargeState pending_       = ChargeState::Discharging;
    uint8_t     pending_count_ = 0;
};
