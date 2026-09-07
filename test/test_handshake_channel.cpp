// test_handshake_channel.cpp - which channel handshake capture parks on.
//
// Context: the shared WiFi scan hops 1-13 every 200 ms, so capture sits on any
// one channel ~7.7% of the time while a WPA 4-way handshake completes in tens
// of milliseconds. Capture therefore surveys for DATA traffic and then PINS.
// handshake.cpp pulls in Arduino/SD/FreeRTOS and so can never be in the host
// suite's MODULES (see argus-host-suite-blind-spot); handshake_channel.h exists
// to make this decision reachable from here.
//
// THE CASE THAT MATTERS, and the reason these are cross-products rather than
// one illustrative example: an all-zero census must yield "do not pin" (0), NOT
// the argmax, because a plain argmax over an empty array is channel 1. Pinning
// a dead channel is strictly worse than hopping, since hopping would at least
// eventually cross a live one. A test that only checked "the busiest channel
// wins" passes against that bug.

#include "wl_test.h"
#include "handshake_channel.h"

WL_TEST(hschan_empty_census_does_not_pin)
{
    HsChannelCensus c;
    WL_CHECK(c.best() == 0);
    WL_CHECK(c.silent());
}

WL_TEST(hschan_every_single_channel_is_pickable)
{
    // Cross-product: whichever lone channel is live must be the one chosen.
    for (uint8_t ch = HS_CH_MIN; ch <= HS_CH_MAX; ch++) {
        HsChannelCensus c;
        c.observe(ch);
        WL_CHECK(c.best() == ch);
        WL_CHECK(!c.silent());
    }
}

WL_TEST(hschan_busiest_channel_wins_over_every_other)
{
    // For each candidate winner, give it more traffic than all the rest put
    // together is irrelevant - it only has to beat each of them individually.
    for (uint8_t win = HS_CH_MIN; win <= HS_CH_MAX; win++) {
        HsChannelCensus c;
        for (uint8_t ch = HS_CH_MIN; ch <= HS_CH_MAX; ch++)
            for (int n = 0; n < (ch == win ? 5 : 2); n++) c.observe(ch);
        WL_CHECK(c.best() == win);
    }
}

WL_TEST(hschan_ties_resolve_to_the_lowest_channel)
{
    // Deterministic for a given census, rather than depending on scan order.
    for (uint8_t a = HS_CH_MIN; a < HS_CH_MAX; a++) {
        for (uint8_t b = (uint8_t)(a + 1); b <= HS_CH_MAX; b++) {
            HsChannelCensus c;
            c.observe(a); c.observe(a);
            c.observe(b); c.observe(b);
            WL_CHECK(c.best() == a);
        }
    }
}

WL_TEST(hschan_best_never_returns_a_silent_channel)
{
    // The invariant, over a spread of populated/unpopulated shapes: whatever
    // best() returns, it either is 0 or has actually been heard from.
    for (uint8_t live = HS_CH_MIN; live <= HS_CH_MAX; live++) {
        HsChannelCensus c;
        c.observe(live);
        uint8_t pick = c.best();
        WL_CHECK(pick != 0);
        WL_CHECK(c.at(pick) > 0);
    }
    HsChannelCensus empty;
    WL_CHECK(empty.best() == 0);      // the 0 case is the exception, by design
}

WL_TEST(hschan_out_of_range_observations_are_ignored)
{
    // A bogus channel must not inflate a real one or invent a pick.
    HsChannelCensus c;
    c.observe(0);
    c.observe(HS_CH_MAX + 1);
    c.observe(200);
    c.observe(255);
    WL_CHECK(c.silent());
    WL_CHECK(c.best() == 0);
    for (uint8_t ch = HS_CH_MIN; ch <= HS_CH_MAX; ch++) WL_CHECK(c.at(ch) == 0);
}

WL_TEST(hschan_out_of_range_never_outvotes_a_real_channel)
{
    HsChannelCensus c;
    c.observe(7);
    for (int i = 0; i < 100; i++) { c.observe(0); c.observe(14); }
    WL_CHECK(c.best() == 7);
}

WL_TEST(hschan_at_is_bounds_safe)
{
    HsChannelCensus c;
    c.observe(3);
    WL_CHECK(c.at(0) == 0);
    WL_CHECK(c.at(HS_CH_MAX + 1) == 0);
    WL_CHECK(c.at(255) == 0);
    WL_CHECK(c.at(3) == 1);
}

WL_TEST(hschan_counter_saturates_and_does_not_wrap)
{
    // A wrap would hand the argmax to a quiet channel after a long busy
    // session, which is the kind of bug that only shows up in the field.
    HsChannelCensus c;
    for (long i = 0; i < 70000; i++) c.observe(6);
    WL_CHECK(c.at(6) == 0xFFFF);
    c.observe(11);
    WL_CHECK(c.best() == 6);
}

WL_TEST(hschan_clear_restores_the_do_not_pin_state)
{
    HsChannelCensus c;
    for (uint8_t ch = HS_CH_MIN; ch <= HS_CH_MAX; ch++) c.observe(ch);
    WL_CHECK(c.best() != 0);
    c.clear();
    WL_CHECK(c.silent());
    WL_CHECK(c.best() == 0);
    for (uint8_t ch = HS_CH_MIN; ch <= HS_CH_MAX; ch++) WL_CHECK(c.at(ch) == 0);
}
