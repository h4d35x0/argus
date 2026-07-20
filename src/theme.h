#pragma once
// ARGUS theme.
//
// The 13-37 base hardcoded its matrix-green accent as lv_color_make(0x00,0xCC,0x66)
// (and a brighter "active" lv_color_make(0x00,0xFF,0x80)) at ~67 sites across the
// screens. This header centralizes the ARGUS brand palette so the accent lives
// in one place: calm steel-blue at rest, HADES threat-red for the alert state
// (brand-as-functional-state). Colors are lv_color_make(r,g,b); LVGL converts to
// the panel's native format.
#include <lvgl.h>

// Brand accent, at rest (calm) — ARGUS steel-blue (#9BBCD6).
// Drop-in replacement for the old matrix-green lv_color_make(0x00,0xCC,0x66).
#define ARGUS_ACCENT         lv_color_make(0x9B, 0xBC, 0xD6)

// Brighter "active / live" accent — drop-in for lv_color_make(0x00,0xFF,0x80).
#define ARGUS_ACCENT_ACTIVE  lv_color_make(0xC8, 0xDE, 0xF0)

// Dim accent for idle / secondary structure.
#define ARGUS_ACCENT_DIM     lv_color_make(0x5B, 0x7C, 0x96)

// HADES threat-red (#DB615A) — the "red eyes" alert state.
#define HADES_RED            lv_color_make(0xDB, 0x61, 0x5A)

// Runtime, state-aware accent. Returns ARGUS_ACCENT (steel-blue) at rest and
// HADES_RED when Threat Radar is flagging a tail (top level >= TR_LVL_LIKELY).
// High-visibility, frequently-repainted surfaces (clock status bar, Threat
// Radar screen) call this so the brand flips to the alert state live; static
// low-traffic screens keep using the ARGUS_ACCENT macro. Defined in theme.cpp.
lv_color_t argus_accent(void);

// Pipeline-driven threat override for the brand accent. The detect_pipeline
// (WiFi evil-twin + beacon-flood aggregator, src/detect_pipeline.*) calls this
// to flip argus_accent() to HADES_RED when its ThreatState posture reaches
// Alert or above, independently of the GPS-co-movement Threat Radar. Passing
// false clears the override, so the accent falls back to the Threat Radar state.
// Default (never called) is false, so existing behavior is unchanged until the
// pipeline drives it. Defined in theme.cpp.
void argus_set_threat(bool active);

// Full-alphabet Bank Gothic brand font for screen titles (src/font_argus_ui.c).
LV_FONT_DECLARE(font_argus_ui);
