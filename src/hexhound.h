#pragma once
#include <stdint.h>
#include <stdbool.h>

// ── HexHound — watch-native cyber-recon pet engine ────────────────
//
// A watch-native reimagining of the HexHound (Labs/HexHound), the
// USB-dongle recon pet, ported to the ARGUS watch (ESP32-S3, 502x410 AMOLED,
// LVGL 9.5). This module is the *game engine only* — pure state, no LVGL. The
// rendering lives in pet_screen.cpp, which keeps the pet_screen_*() API so the
// Tools "HexHound" tile keeps working.
//
// The pet evolves through five stages by doing REAL reconnaissance on the
// watch's radios, earning XP, growing a bond, and burning hunger/energy that
// decay over time:
//
//   1 Egg              incubating — no ability
//   2 Packet Pup       WiFi recon           (hatches on first real recon event)
//   3 Beacon Beast     BLE / beacon sense   (200 XP)
//   4 Gremlin Mode     mischief / capture   (500 XP)
//   5 Sentinel  full-spectrum     (1000 XP)
//
// XP only comes from real signals, so the XP thresholds ARE real-milestone
// gates. It is fed by:
//   * eaten WPA handshakes  — handshake_pwnd_count()   (+30 XP, +hunger each)
//   * met Pwnagotchi peers  — pwnagotchi_peer_count()  (+50 XP, +hunger each)
//   * distinct nearby APs   — WiFi beacon frames        (+2 XP each new BSSID)
//
// Persisted to SD /HexHound/pet.txt (simple key=value lines).

enum HexStage : uint8_t {
    HEX_EGG = 1,
    HEX_PUP,        // Packet Pup
    HEX_BEAST,      // Beacon Beast
    HEX_GREMLIN,    // Gremlin Mode
    HEX_SENTINEL    // Sentinel
};

enum HexMood : uint8_t {
    HEX_IDLE = 0,   // calm at rest
    HEX_HAPPY,      // well-fed and bonded
    HEX_EXCITED,    // just ate a handshake / met a peer
    HEX_HUNGRY,     // hunger low
    HEX_SLEEPY,     // energy low
    HEX_WARY        // threat present (HADES red) — driven by integrator hook
};

struct HexHoundState {
    uint8_t  stage    = HEX_EGG;
    long     xp       = 0;
    int      hunger   = 60;   // 0..100, decays, restored by feeding
    int      energy   = 80;   // 0..100, decays slowly, rests when idle
    int      bond     = 0;    // 0..100, grows with interaction, no decay
    uint8_t  mood     = HEX_IDLE;

    // Lifetime recon milestone counters (persisted).
    int      pwnd     = 0;    // handshakes eaten
    int      peers    = 0;    // Pwnagotchi peers met
    int      wifiSeen = 0;    // distinct APs catalogued
    int      bleSeen  = 0;    // reserved for BLE integrator wiring
};

// Load persisted state from SD (idempotent — safe to call repeatedly).
void hexhound_init();

// Poll real recon feeds, apply time-based decay, recompute mood + evolution.
// Call ~1 Hz while the pet screen is visible.
void hexhound_update();

// Feed a WiFi beacon frame's BSSID for distinct-AP XP. Cheap; dedups against a
// small session ring. Wire this to the shared WiFi beacon manager while the pet
// screen is open ("sniffing the airwaves" trickle).
void hexhound_note_wifi(const uint8_t bssid[6]);

// ── INTEGRATOR HOOK — threat level ─────────────────────────────────────────
// Team-owned Threat Radar (threat_radar.h / threatradar_*) is intentionally NOT
// included here to keep this module decoupled. When the integrator lands the
// Threat Radar bundle, wire a confirmed tail / active threat to this setter:
//   0  = calm         (pet relaxes to its normal mood)
//   >0 = threat level (pet snaps to HEX_WARY, HADES-red, "someone's tailing us")
// e.g. in the radar's on-confirm path:  hexhound_set_threat_level(1);
void hexhound_set_threat_level(int level);
int  hexhound_threat_level();

// Persist current state to SD /HexHound/pet.txt.
void hexhound_save();

// One-shot: true exactly once after an evolution, so the UI can flash a banner.
bool hexhound_take_evolved_flag();
uint8_t hexhound_prev_stage();  // stage before the most recent evolution

// ── Accessors (for the renderer) ───────────────────────────────────────────
const HexHoundState& hexhound_state();
int         hexhound_level();            // 1 + xp/100
int         hexhound_xp_into_stage();    // XP earned within the current stage
int         hexhound_xp_stage_span();    // XP width of the current stage band
const char* hexhound_stage_name();       // "Egg" / "Packet Pup" / ...
const char* hexhound_ability_name();     // ability unlocked at current stage
const char* hexhound_mood_speech();      // ASCII speech line for current mood
