#pragma once
#include <stdint.h>
#include <stdbool.h>

// ── ARGUS HexHound — watch-native cyber-recon pet engine ────────────────
//
// A watch-native reimagining of the ARGUS HexHound (Labs/HexHound), the
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
    int      bleSeen  = 0;    // distinct ambient BLE devices sniffed
    int      detSeen  = 0;    // confirmed detector hits eaten (AirTag/Flipper/...)
    int      nfcSeen  = 0;    // NFC tags read (hand-fed "treats")
    int      cellsSeen= 0;    // distinct coarse GPS cells explored
};

// ── Detector categories that "feed" HexHound ───────────────────────────────
// Kept independent of threat_radar.h so this engine stays decoupled from the
// Threat Radar bundle. The detector call sites already include threat_radar.h
// and simply pass the matching HEX_DET_* constant one line below their existing
// threatradar_observe() confirm call.
enum HexDetector : uint8_t {
    HEX_DET_AIRTAG = 0,
    HEX_DET_FLIPPER,
    HEX_DET_SKIMMER,
    HEX_DET_FLOCK,
    HEX_DET_EVILTWIN,
    HEX_DET_COUNT
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

// ── Richer diet feeds — exercise the watch's other radios/sensors ───────────
// These four are all SAFE TO CALL FROM ANY TASK/CONTEXT (the BLE controller
// callback, the WiFi beacon task, or the LVGL/main task). Each only defers a
// tiny reward into a spinlock-guarded pending accumulator that hexhound_update()
// drains on the main task, so no feed ever touches game state or SD from a
// foreign task. Rewards are retro-credited the next time the pet screen ticks.

// A confirmed detector hit — the pet's "big meal" (more XP + hunger than an
// ambient contact). The detectors already dedup, so every call is a fresh,
// confirmed hit; `category` is a HexDetector value.
void hexhound_note_detector(uint8_t category);

// A BLE device seen on the airwaves — small XP trickle. Dedups against a
// session MAC ring so one chatty beacon can't spam XP.
void hexhound_note_ble(const uint8_t mac[6]);

// An NFC tag was read — a hand-fed "treat" (bond-forward: more bond than XP).
void hexhound_note_nfc();

// Entered new territory — a coarse GPS cell not visited this session earns
// exploration XP. Rounds lat/lon to a coarse grid and dedups against a small
// ring of recently-visited cells; safe to call every fix.
void hexhound_note_cell(double lat, double lon);

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
