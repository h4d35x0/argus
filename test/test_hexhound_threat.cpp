// test_hexhound_threat.cpp - the pet's threat posture combines by MAX.
//
// The defect this pins was found on 2026-09-04 and shipped green: both threat
// sources wrote ONE shared integer, so the pet's mood was decided by main-loop
// call ordering rather than by the threat. main.cpp's radar path ran after
// detect_pipeline's, wrote its own 0 over the flag the pipeline had just set,
// and did it every iteration, so a pipeline-only threat could never raise the
// pet at all. CI was green throughout because hexhound.cpp pulls in SD.h,
// Arduino.h and FreeRTOS and therefore was not, and cannot be, in the host
// suite's MODULES. Splitting the combine into the hardware-free
// hexhound_threat.h is what makes it reachable from here.
//
// The invariant, and the reason most of these cases are cross-products rather
// than one illustrative example: the effective level is the MAX across every
// source, so a source clearing its OWN slot must never cancel another's live
// threat, at any level, in any order. A test that only checked "pipeline raises
// the pet" would have passed against the broken code whenever the pipeline
// happened to tick last.

#include "wl_test.h"
#include "hexhound_threat.h"

WL_TEST(hexthreat_starts_calm)
{
    HexThreatSources s;
    WL_CHECK(s.combined() == 0);
    for (int i = 0; i < HEX_THREAT_SOURCE_COUNT; i++)
        WL_CHECK(s.get((uint8_t)i) == 0);
}

WL_TEST(hexthreat_every_source_can_raise_alone)
{
    // Not "the radar can raise it" - EVERY source, including one added later.
    for (int i = 0; i < HEX_THREAT_SOURCE_COUNT; i++) {
        HexThreatSources s;
        WL_CHECK(s.set((uint8_t)i, 1));
        WL_CHECK(s.combined() == 1);
    }
}

WL_TEST(hexthreat_a_clearing_source_never_cancels_another)
{
    // THE REGRESSION. One source holds a live threat while a different source
    // reports calm. Every ordered pair, both orders, so this cannot pass by
    // accident of which caller ticks last.
    for (int hot = 0; hot < HEX_THREAT_SOURCE_COUNT; hot++) {
        for (int calm = 0; calm < HEX_THREAT_SOURCE_COUNT; calm++) {
            if (hot == calm) continue;

            // Raise first, then let the other source report calm.
            HexThreatSources a;
            a.set((uint8_t)hot, 1);
            a.set((uint8_t)calm, 0);
            WL_CHECK(a.combined() == 1);

            // Reverse order: the calm report lands first.
            HexThreatSources b;
            b.set((uint8_t)calm, 0);
            b.set((uint8_t)hot, 1);
            WL_CHECK(b.combined() == 1);

            // And repeatedly, because the broken code re-cleared every tick.
            for (int t = 0; t < 5; t++) {
                a.set((uint8_t)calm, 0);
                WL_CHECK(a.combined() == 1);
            }
        }
    }
}

WL_TEST(hexthreat_relaxes_only_when_every_source_is_clear)
{
    HexThreatSources s;
    for (int i = 0; i < HEX_THREAT_SOURCE_COUNT; i++) s.set((uint8_t)i, 1);
    WL_CHECK(s.combined() == 1);

    // Clear them one at a time. The pet stays wary until the last one drops.
    for (int i = 0; i < HEX_THREAT_SOURCE_COUNT; i++) {
        s.set((uint8_t)i, 0);
        bool last = (i == HEX_THREAT_SOURCE_COUNT - 1);
        WL_CHECK(s.combined() == (last ? 0 : 1));
    }
}

WL_TEST(hexthreat_is_max_not_last_wins)
{
    // Cross-product over levels: whatever each source reports, the effective
    // posture is the highest of them, independent of write order.
    const int levels[] = { 0, 1, 2, 7 };
    const int n = (int)(sizeof(levels) / sizeof(levels[0]));

    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            int expect = levels[i] > levels[j] ? levels[i] : levels[j];

            HexThreatSources a;
            a.set(HEX_THREAT_RADAR,    levels[i]);
            a.set(HEX_THREAT_PIPELINE, levels[j]);
            WL_CHECK(a.combined() == expect);

            HexThreatSources b;
            b.set(HEX_THREAT_PIPELINE, levels[j]);
            b.set(HEX_THREAT_RADAR,    levels[i]);
            WL_CHECK(b.combined() == expect);
        }
    }
}

WL_TEST(hexthreat_negative_clamps_to_calm)
{
    // A negative level must store 0, not a value that would lose a MAX against
    // a real threat on another source.
    HexThreatSources s;
    s.set(HEX_THREAT_RADAR, -5);
    WL_CHECK(s.get(HEX_THREAT_RADAR) == 0);
    WL_CHECK(s.combined() == 0);

    s.set(HEX_THREAT_PIPELINE, 1);
    s.set(HEX_THREAT_RADAR, -1);
    WL_CHECK(s.combined() == 1);
}

WL_TEST(hexthreat_out_of_range_source_changes_nothing)
{
    // A bad source id must be inert, not corrupt a live slot. HEX_THREAT_SOURCE_COUNT
    // is itself out of range - it is the count, not a contributor.
    HexThreatSources s;
    s.set(HEX_THREAT_PIPELINE, 3);

    WL_CHECK(!s.set(HEX_THREAT_SOURCE_COUNT, 9));
    WL_CHECK(!s.set(200, 9));
    WL_CHECK(s.combined() == 3);
    WL_CHECK(s.get(HEX_THREAT_SOURCE_COUNT) == 0);
    WL_CHECK(s.get(200) == 0);

    // Clearing through a bad id must not relax a real threat either.
    WL_CHECK(!s.set(HEX_THREAT_SOURCE_COUNT, 0));
    WL_CHECK(s.combined() == 3);
}
