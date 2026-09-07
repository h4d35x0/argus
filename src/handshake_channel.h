#pragma once
//
// handshake_channel.h - pure channel-selection logic for WPA handshake capture.
//
// Split out of handshake.cpp so the decision is testable on the host
// (test/test_handshake_channel.cpp) without an ESP32 on the bench. No Arduino,
// no SD, no FreeRTOS, no LVGL: keep it that way. Same pattern as
// charge_state.h and hexhound_threat.h.
//
// THE PROBLEM THIS SOLVES
//
// The shared WiFi scan hops channels 1-13 on a 200 ms timer, so a full sweep is
// 2.6 s and the radio sits on any one channel about 7.7% of the time. Surveying
// tolerates that (an AP beacons every ~100 ms, so it is seen within a sweep),
// but a WPA 4-way handshake completes in tens of milliseconds. Hop off and it
// is gone. Passive capture was therefore missing most handshakes even when one
// happened directly in front of the watch.
//
// So: SURVEY while hopping, counting DATA frames per channel, then PIN to the
// channel actually carrying client traffic.
//
// WHY DATA FRAMES AND NOT AP COUNT: we want the channel where CLIENTS are
// talking, because that is where a (re)association and its handshake happen.
// The channel with the most APs can easily carry no client traffic at all.
//
// C++11 only: the ESP32 Arduino core builds this at -std=gnu++11 even though
// the host suite is C++17. No `if constexpr`, no multi-statement constexpr.

#include <stdint.h>

// 2.4 GHz channels are 1..13. Index 0 is unused so a channel number indexes
// directly and no caller has to remember an off-by-one.
#define HS_CH_MIN 1
#define HS_CH_MAX 13
#define HS_CH_SLOTS (HS_CH_MAX + 1)

// Per-channel DATA-frame census.
struct HsChannelCensus {
    uint16_t hits[HS_CH_SLOTS] = {};

    void clear()
    {
        for (int c = 0; c < HS_CH_SLOTS; c++) hits[c] = 0;
    }

    // Record one observed DATA frame on `ch`. Out-of-range channels are ignored
    // rather than clamped: a bogus channel must not inflate a real one's count
    // and swing the pick. Saturating, so a long busy session cannot wrap the
    // counter and silently hand the argmax to a quiet channel.
    void observe(uint8_t ch)
    {
        if (ch < HS_CH_MIN || ch > HS_CH_MAX) return;
        if (hits[ch] != 0xFFFF) hits[ch]++;
    }

    uint16_t at(uint8_t ch) const
    {
        return (ch < HS_CH_MIN || ch > HS_CH_MAX) ? 0 : hits[ch];
    }

    // The channel to park on, or 0 for "do not pin".
    //
    // Returning 0 on an all-zero census is the whole point of this function.
    // A plain argmax over an empty census yields channel 1, which means
    // nothing: pinning there parks capture on an arbitrary dead channel and is
    // strictly WORSE than continuing to hop, because hopping would at least
    // eventually cross a live one. Callers must treat 0 as "stay hopping and
    // survey again", never as a channel.
    //
    // Ties resolve to the LOWEST channel (strict >), so the pick is
    // deterministic for a given census rather than depending on scan order.
    uint8_t best() const
    {
        uint8_t  best_ch = 0;
        uint16_t best_n  = 0;
        for (uint8_t c = HS_CH_MIN; c <= HS_CH_MAX; c++) {
            if (hits[c] > best_n) { best_n = hits[c]; best_ch = c; }
        }
        return best_ch;
    }

    // True when nothing at all was heard. Kept separate from best()==0 so a
    // caller can distinguish the two if the rule ever changes.
    bool silent() const
    {
        for (uint8_t c = HS_CH_MIN; c <= HS_CH_MAX; c++) if (hits[c]) return false;
        return true;
    }
};
