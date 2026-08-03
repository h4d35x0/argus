// test_charge_state.cpp - charge readout that drives the clock-face bolt.
//
// The bug this module exists to prevent is not "wrong state" but "strobing
// state": near termination the AXP2101 hunts CC -> CV -> DONE -> CC and flips
// isCharging() every couple of seconds. Rendering that raw would flicker the
// bolt between pulsing and steady, the same defect class as the tracker
// threat-level strobe. Most of these cases pin the debounce asymmetry.

#include "wl_test.h"
#include "charge_state.h"

// Feed the same sample n times and return the final published state.
static ChargeState feed(ChargeIndicator &ci, bool vbus, bool chg, int n)
{
    ChargeState st = ci.state();
    for (int i = 0; i < n; i++) st = ci.update(vbus, chg);
    return st;
}

WL_TEST(charge_raw_three_states)
{
    WL_CHECK(charge_state_raw(false, false) == ChargeState::Discharging);
    WL_CHECK(charge_state_raw(true,  true)  == ChargeState::Charging);
    WL_CHECK(charge_state_raw(true,  false) == ChargeState::Topped);

    // charging wins over a dropped VBUS bit: current is flowing, so power is
    // present regardless of what STATUS2[3] reads.
    WL_CHECK(charge_state_raw(false, true) == ChargeState::Charging);
}

WL_TEST(charge_defaults_to_discharging)
{
    ChargeIndicator ci;
    WL_CHECK(ci.state() == ChargeState::Discharging);
}

WL_TEST(charge_plug_in_is_immediate)
{
    // The whole point of the widget: plugging in must confirm on the very next
    // tick, not after a settle window.
    ChargeIndicator ci;
    WL_CHECK(ci.update(true, true) == ChargeState::Charging);

    // Same for plugging in a watch that is already full.
    ChargeIndicator ci2;
    WL_CHECK(ci2.update(true, false) == ChargeState::Topped);
}

WL_TEST(charge_to_topped_is_debounced)
{
    ChargeIndicator ci;
    ci.update(true, true);
    WL_CHECK(ci.state() == ChargeState::Charging);

    // Termination reported, but not yet long enough to publish.
    for (int i = 1; i < CHARGE_SETTLE_TICKS; i++)
        WL_CHECK(ci.update(true, false) == ChargeState::Charging);

    // The CHARGE_SETTLE_TICKS-th consecutive sample flips it.
    WL_CHECK(ci.update(true, false) == ChargeState::Topped);
}

WL_TEST(charge_cc_cv_done_hunt_does_not_strobe)
{
    // Reproduce the oscillation: charger reports DONE for a few seconds, picks
    // back up, drops again. The bolt must stay on "Charging" throughout.
    ChargeIndicator ci;
    ci.update(true, true);

    for (int cycle = 0; cycle < 8; cycle++) {
        // Not-charging for one tick short of the settle window...
        for (int i = 1; i < CHARGE_SETTLE_TICKS; i++)
            WL_CHECK(ci.update(true, false) == ChargeState::Charging);
        // ...then current resumes, resetting the count.
        WL_CHECK(ci.update(true, true) == ChargeState::Charging);
    }
    WL_CHECK(ci.state() == ChargeState::Charging);
}

WL_TEST(charge_interrupted_settle_restarts_count)
{
    ChargeIndicator ci;
    ci.update(true, true);

    feed(ci, true, false, CHARGE_SETTLE_TICKS - 1);
    WL_CHECK(ci.state() == ChargeState::Charging);

    ci.update(true, true);   // one contradicting sample resets the run

    // A fresh partial run still must not publish.
    feed(ci, true, false, CHARGE_SETTLE_TICKS - 1);
    WL_CHECK(ci.state() == ChargeState::Charging);

    WL_CHECK(ci.update(true, false) == ChargeState::Topped);
}

WL_TEST(charge_unplug_is_debounced_but_short)
{
    ChargeIndicator ci;
    ci.update(true, true);

    for (int i = 1; i < UNPLUG_SETTLE_TICKS; i++)
        WL_CHECK(ci.update(false, false) == ChargeState::Charging);

    WL_CHECK(ci.update(false, false) == ChargeState::Discharging);
}

WL_TEST(charge_cable_chatter_holds_the_bolt)
{
    // Loose pogo pins: VBUS blinks out for a single tick at a time. The bolt
    // must not flicker off, because UNPLUG_SETTLE_TICKS > 1.
    ChargeIndicator ci;
    ci.update(true, true);

    for (int i = 0; i < 20; i++) {
        WL_CHECK(ci.update(false, false) == ChargeState::Charging);
        WL_CHECK(ci.update(true,  true)  == ChargeState::Charging);
    }
}

WL_TEST(charge_unplug_from_topped)
{
    ChargeIndicator ci;
    ci.update(true, false);
    WL_CHECK(ci.state() == ChargeState::Topped);

    feed(ci, false, false, UNPLUG_SETTLE_TICKS);
    WL_CHECK(ci.state() == ChargeState::Discharging);

    // And re-plugging is immediate again.
    WL_CHECK(ci.update(true, true) == ChargeState::Charging);
}

WL_TEST(charge_prime_skips_debounce)
{
    // Booting on the charger: the first rendered frame must already show it.
    ChargeIndicator ci;
    ci.prime(true, true);
    WL_CHECK(ci.state() == ChargeState::Charging);

    ChargeIndicator ci2;
    ci2.prime(true, false);
    WL_CHECK(ci2.state() == ChargeState::Topped);

    // prime() must not leave a half-finished run behind that lets the next
    // contradicting sample publish early.
    ChargeIndicator ci3;
    ci3.prime(true, true);
    feed(ci3, true, false, CHARGE_SETTLE_TICKS - 1);
    WL_CHECK(ci3.state() == ChargeState::Charging);
}

WL_TEST(charge_steady_state_is_stable)
{
    ChargeIndicator ci;
    ci.prime(false, false);
    feed(ci, false, false, 100);
    WL_CHECK(ci.state() == ChargeState::Discharging);

    ci.prime(true, true);
    feed(ci, true, true, 100);
    WL_CHECK(ci.state() == ChargeState::Charging);
}
