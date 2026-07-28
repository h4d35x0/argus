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

// Secondary body/label TEXT colour. The maintainer preferred the secondary text in a
// warm white/cream over the steel-blue accent, so these carry body text while the
// accent is reserved for structure/titles. ARGUS_TEXT = bright cream, _DIM = a
// softer warm grey for captions/hints.
#define ARGUS_TEXT           lv_color_make(0xED, 0xE8, 0xDA)
#define ARGUS_TEXT_DIM       lv_color_make(0xB2, 0xAB, 0x98)

// HADES threat-red (#DB615A) — the "red eyes" alert state.
#define HADES_RED            lv_color_make(0xDB, 0x61, 0x5A)

// Offense-mode base accent — aggressive red-team red (#F02E2E). Distinct from
// the calm steel-blue (Daily/Defense). Drives the Offense border + "OFF" chip
// and the offense tool icons, so Offense reads unmistakably "red team". HADES_RED
// (a softer coral) still overlays as the live threat/alert state on top of this.
#define ARGUS_OFFENSE_ACCENT lv_color_make(0xF0, 0x2E, 0x2E)

// Runtime, state-aware accent. Returns ARGUS_ACCENT (steel-blue) at rest and
// HADES_RED when Threat Radar is flagging a tail (top level >= TR_LVL_LIKELY).
// High-visibility, frequently-repainted surfaces (clock status bar, Threat
// Radar screen) call this so the brand flips to the alert state live; static
// low-traffic screens keep using the ARGUS_ACCENT macro. Defined in theme.cpp.
lv_color_t argus_accent(void);

// Mode-aware BASE accent (no threat overlay): steel-blue in Daily/Defense, amber
// in Offense. argus_accent() layers the threat-red flip on top of this (except in
// Daily, which stays innocent and never flips). Defined in theme.cpp.
lv_color_t argus_base_accent(void);

// Persistent per-mode indicator on lv_layer_top(): an amber/red border frame in
// Offense, nothing in Daily or Defense. init() once at boot (after LVGL/clock
// build); refresh() on every mode change and on the 1s status tick so the border
// flips live under threat. Defined in theme.cpp.
//
// The "DEF" / "OFF" corner chip this used to also draw is disabled (2026-07-28)
// and left commented in theme.cpp: mode is already obvious from the wallpaper,
// tool set and accent colour, and the chip rode on every screen, not just the
// clock.
void argus_mode_indicator_init(void);
void argus_mode_indicator_refresh(void);

// ---- On-screen keyboard placement ------------------------------------------
//
// The panel is a 410x502 ROUNDED rectangle. A keyboard sized to the full 410 and
// aligned flush to LV_ALIGN_BOTTOM_MID puts its bottom row exactly where the
// corner radius cuts in, so the outer keys of that row are clipped by the bezel
// and cannot be read or reliably tapped. Every keyboard in the tree had this.
//
// argus_keyboard_fit() applies one safe geometry to all of them: narrowed to
// ARGUS_KB_SAFE_W and lifted ARGUS_KB_BOTTOM_INSET px off the bottom edge, which
// keeps the bottom row inside the flat part of the panel. The LVGL keyboard is a
// button matrix and lays its keys out within the object, so narrowing the object
// narrows the keys rather than clipping them.
//
// Height stays per-caller: keyboards here range 180..240 px depending on how much
// of the screen the field above them needs.
//
// These two numbers are the whole tuning surface. If a bottom row still clips,
// raise ARGUS_KB_BOTTOM_INSET (and/or lower ARGUS_KB_SAFE_W) here, once, rather
// than editing call sites.
#define ARGUS_KB_SAFE_W         360
#define ARGUS_KB_BOTTOM_INSET   36

void argus_keyboard_fit(lv_obj_t *kb, int height);

// Pipeline-driven threat override for the brand accent. The detect_pipeline
// (WiFi evil-twin + beacon-flood aggregator, src/detect_pipeline.*) calls this
// to flip argus_accent() to HADES_RED when its ThreatState posture reaches
// Alert or above, independently of the GPS-co-movement Threat Radar. Passing
// false clears the override, so the accent falls back to the Threat Radar state.
// Default (never called) is false, so existing behavior is unchanged until the
// pipeline drives it. Defined in theme.cpp.
void argus_set_threat(bool active);

// Full-alphabet Saira Condensed brand font for screen titles (src/font_argus_ui.c).
LV_FONT_DECLARE(font_argus_ui);

// ARGUS UI text fonts (brand system): Orbitron for labels, VT323 for numeric/
// terminal readouts, Saira Condensed for the wordmark/titles, and Montserrat for
// the digital clock. All are generated from OFL-licensed font sources.
LV_FONT_DECLARE(font_argus_label_14);   // Orbitron 14 - small labels
LV_FONT_DECLARE(font_argus_label_16);   // Orbitron 16 - body labels
LV_FONT_DECLARE(font_argus_label_20);   // Orbitron 20 - tile labels, date
LV_FONT_DECLARE(font_argus_label_28);   // Orbitron 28 - larger labels
LV_FONT_DECLARE(font_argus_mono_16);    // VT323 16 - small numeric readouts
LV_FONT_DECLARE(font_argus_mono_48);    // VT323 48 subset (digits/colon) - big readouts (alarm/stopwatch/timer)
LV_FONT_DECLARE(font_argus_label_48);   // Orbitron 48 subset (START/STOP) - wardriver button
