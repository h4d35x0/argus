#pragma once
//
// hexhound_threat.h - pure combining logic for the pet's threat posture.
//
// Split out of hexhound.cpp so the combine rule is testable on the host
// (test/test_hexhound_threat.cpp) without an ESP32 on the bench. No Arduino,
// no SD, no FreeRTOS, no LVGL: keep it that way.
//
// TWO INDEPENDENT SOURCES feed the pet's wary mood, and they tick at different
// points in the main loop:
//   HEX_THREAT_RADAR    - main.cpp, threatradar_top_level() >= TR_LVL_LIKELY
//   HEX_THREAT_PIPELINE - detect_pipeline.cpp, overall posture >= Alert
//
// They used to share ONE integer, so whichever ran last won and the pet's mood
// was decided by call ordering rather than by the threat: the radar path wrote
// its 0 over the flag the pipeline had just set, every single iteration, so a
// pipeline-only threat could never raise the pet at all. Found 2026-09-04.
//
// The rule that fixes it, and the invariant this header exists to hold:
//
//     the effective level is the MAX across sources, never last-wins.
//
// So either source can raise the pet, and it only relaxes once EVERY source is
// clear. A source clearing its own slot must never cancel another's live
// threat. That is the property test/test_hexhound_threat.cpp pins.
//
// NOTE: neither source means a CONFIRMED tail. Both are boolean postures that
// trip a full rung below TR_LVL_CONFIRMED, so anything user-visible driven off
// this must not claim a confirmed tail. See hexhound_mood_speech().
//
// C++11 only: the ESP32 Arduino core builds this at -std=gnu++11 even though
// the host test suite is C++17. No `if constexpr`, no multi-statement constexpr
// bodies, no inline variables.

#include <stdint.h>

// Contributors to the pet's threat posture. The `source` argument on the setter
// is deliberately NOT defaulted: a new contributor has to declare itself rather
// than silently colliding with an existing one. Add its enumerator here, above
// HEX_THREAT_SOURCE_COUNT, and the combine picks it up with no other change.
enum HexThreatSource : uint8_t {
    HEX_THREAT_RADAR = 0,
    HEX_THREAT_PIPELINE,
    HEX_THREAT_SOURCE_COUNT
};

// One slot per source, combined by MAX. Zero-initialised, i.e. calm.
struct HexThreatSources {
    int level[HEX_THREAT_SOURCE_COUNT];

    HexThreatSources()
    {
        for (int i = 0; i < HEX_THREAT_SOURCE_COUNT; i++) level[i] = 0;
    }

    // Record `lvl` for `source`. A negative level clamps to 0 (calm) rather
    // than storing a value that would lose a MAX against a real threat.
    // Returns false and changes NOTHING for an out-of-range source, so a
    // miscompiled or future enumerator cannot corrupt a live slot.
    bool set(uint8_t source, int lvl)
    {
        if (source >= HEX_THREAT_SOURCE_COUNT) return false;
        level[source] = lvl < 0 ? 0 : lvl;
        return true;
    }

    int get(uint8_t source) const
    {
        return source < HEX_THREAT_SOURCE_COUNT ? level[source] : 0;
    }

    // Effective posture: the highest level any source is currently reporting.
    int combined() const
    {
        int top = 0;
        for (int i = 0; i < HEX_THREAT_SOURCE_COUNT; i++)
            if (level[i] > top) top = level[i];
        return top;
    }
};
