#include "hexhound.h"
#include "handshake.h"          // handshake_pwnd_count()
#include "pwnagotchi_peer.h"    // pwnagotchi_peer_count()
#include <LilyGoLib.h>          // instance.isCardReady()
#include <SD.h>
#include <Arduino.h>
#include <string.h>
#include <stdlib.h>

// ── HexHound — engine implementation ─────────────────────────────
//
// Pure game logic. No LVGL. The renderer (pet_screen.cpp) reads state through
// the accessors. Real recon feeds are polled from handshake.* and
// pwnagotchi_peer.* plus a WiFi-beacon note hook; nothing here reaches into the
// Threat Radar (that arrives through hexhound_set_threat_level()).

namespace {

// Evolution XP thresholds (from the HexHound design table). XP only accrues
// from real recon, so these are effectively real-milestone gates.
constexpr long XP_BEAST    = 200;   // Packet Pup  -> Beacon Beast
constexpr long XP_GREMLIN  = 500;   // Beacon Beast -> Gremlin Mode
constexpr long XP_SENTINEL = 1000;  // Gremlin Mode -> Sentinel

// Feed / reward tuning.
constexpr long XP_PER_PWND  = 30;   // per eaten handshake
constexpr long XP_PER_PEER  = 50;   // per Pwnagotchi met
constexpr long XP_PER_AP    = 2;    // per newly-seen distinct AP
constexpr int  FEED_PWND    = 25;   // hunger restored per handshake
constexpr int  FEED_PEER    = 12;   // hunger restored per peer
constexpr int  BOND_PWND    = 3;
constexpr int  BOND_PEER    = 5;

// Decay tuning (real time).
constexpr uint32_t DECAY_PERIOD_MS = 60000;  // one decay tick per minute
constexpr int  HUNGER_DECAY = 2;             // per tick
constexpr int  ENERGY_DECAY = 1;             // per tick
constexpr uint32_t EXCITED_MS = 5000;        // "excited" mood dwell after a feed
constexpr uint32_t SAVE_MIN_MS = 15000;      // debounce SD writes

HexHoundState s_st;
bool     s_loaded    = false;
int      s_threat    = 0;

// Feed baselines: the counters the last time we credited them, so re-opening the
// screen retro-credits recon that happened while it was closed.
int      s_basePwnd  = 0;
int      s_basePeer  = 0;
bool     s_baseInit  = false;

uint32_t s_lastDecay = 0;
uint32_t s_excitedUntil = 0;
uint32_t s_lastSave  = 0;
bool     s_dirty     = false;

// Evolution one-shot.
bool     s_evolved   = false;
uint8_t  s_prevStage = HEX_EGG;

// Small BSSID dedup ring for distinct-AP XP (session-scoped).
constexpr int RING = 16;
uint8_t  s_bssidRing[RING][6] = {};
int      s_ringCount = 0;
int      s_ringHead  = 0;

int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

const char* const STAGE_NAMES[] = {
    "Egg", "Packet Pup", "Beacon Beast", "Gremlin Mode", "DH Sentinel"
};
const char* const STAGE_ABILITY[] = {
    "INCUBATING",
    "WIFI RECON",
    "BLE BEACON SENSE",
    "MISCHIEF / CAPTURE",
    "ANOMALY DETECTION"
};

} // namespace

// ── Persistence ────────────────────────────────────────────────────────────

void hexhound_init() {
    if (s_loaded) return;
    s_loaded = true;

    if (instance.isCardReady()) {
        File f = SD.open("/HexHound/pet.txt", FILE_READ);
        if (f) {
            char line[64];
            while (f.available()) {
                int n = f.readBytesUntil('\n', line, sizeof(line) - 1);
                if (n <= 0) continue;
                line[n] = '\0';
                char* eq = strchr(line, '=');
                if (!eq) continue;
                *eq = '\0';
                const char* key = line;
                long val = atol(eq + 1);
                if      (!strcmp(key, "stage"))    s_st.stage   = (uint8_t)clampi((int)val, HEX_EGG, HEX_SENTINEL);
                else if (!strcmp(key, "xp"))       s_st.xp      = val < 0 ? 0 : val;
                else if (!strcmp(key, "hunger"))   s_st.hunger  = clampi((int)val, 0, 100);
                else if (!strcmp(key, "energy"))   s_st.energy  = clampi((int)val, 0, 100);
                else if (!strcmp(key, "bond"))     s_st.bond    = clampi((int)val, 0, 100);
                else if (!strcmp(key, "pwnd"))     s_st.pwnd    = (int)val;
                else if (!strcmp(key, "peers"))    s_st.peers   = (int)val;
                else if (!strcmp(key, "wifiSeen")) s_st.wifiSeen= (int)val;
                else if (!strcmp(key, "bleSeen"))  s_st.bleSeen = (int)val;
            }
            f.close();
        }
    }
    s_prevStage = s_st.stage;
    s_lastDecay = millis();
}

void hexhound_save() {
    if (!instance.isCardReady()) return;
    if (!SD.exists("/HexHound")) SD.mkdir("/HexHound");
    File f = SD.open("/HexHound/pet.txt", FILE_WRITE);
    if (!f) return;
    f.printf("stage=%u\n",    s_st.stage);
    f.printf("xp=%ld\n",      s_st.xp);
    f.printf("hunger=%d\n",   s_st.hunger);
    f.printf("energy=%d\n",   s_st.energy);
    f.printf("bond=%d\n",     s_st.bond);
    f.printf("pwnd=%d\n",     s_st.pwnd);
    f.printf("peers=%d\n",    s_st.peers);
    f.printf("wifiSeen=%d\n", s_st.wifiSeen);
    f.printf("bleSeen=%d\n",  s_st.bleSeen);
    f.close();
    s_dirty = false;
    s_lastSave = millis();
}

// ── Evolution ──────────────────────────────────────────────────────────────

static void check_evolution() {
    uint8_t old = s_st.stage;

    if (s_st.stage == HEX_EGG) {
        // Hatch on the first real recon event: any eaten handshake or met peer.
        if (s_st.pwnd > 0 || s_st.peers > 0 || s_st.xp > 0) {
            s_st.stage = HEX_PUP;
        }
    }
    if (s_st.stage >= HEX_PUP) {
        if      (s_st.xp >= XP_SENTINEL && s_st.stage < HEX_SENTINEL) s_st.stage = HEX_SENTINEL;
        else if (s_st.xp >= XP_GREMLIN  && s_st.stage < HEX_GREMLIN)  s_st.stage = HEX_GREMLIN;
        else if (s_st.xp >= XP_BEAST    && s_st.stage < HEX_BEAST)    s_st.stage = HEX_BEAST;
    }

    if (s_st.stage != old) {
        s_prevStage = old;
        s_evolved = true;
        s_dirty = true;
        Serial.printf("[HexHound] Evolved %s -> %s (xp=%ld)\n",
                      STAGE_NAMES[old - 1], STAGE_NAMES[s_st.stage - 1], s_st.xp);
    }
}

static void add_xp(long amount) {
    if (amount <= 0) return;
    s_st.xp += amount;
    s_dirty = true;
    check_evolution();
}

// ── Feeds ──────────────────────────────────────────────────────────────────

void hexhound_note_wifi(const uint8_t bssid[6]) {
    if (!bssid) return;
    for (int i = 0; i < s_ringCount; i++) {
        if (memcmp(s_bssidRing[i], bssid, 6) == 0) return;   // already counted
    }
    memcpy(s_bssidRing[s_ringHead], bssid, 6);
    s_ringHead = (s_ringHead + 1) % RING;
    if (s_ringCount < RING) s_ringCount++;

    s_st.wifiSeen++;
    s_st.energy = clampi(s_st.energy + 1, 0, 100);   // sniffing keeps it alert
    add_xp(XP_PER_AP);
}

void hexhound_set_threat_level(int level) {
    s_threat = level < 0 ? 0 : level;
}

int hexhound_threat_level() { return s_threat; }

// ── Main tick ──────────────────────────────────────────────────────────────

void hexhound_update() {
    if (!s_loaded) hexhound_init();
    uint32_t now = millis();

    int pwnd  = handshake_pwnd_count();
    int peers = pwnagotchi_peer_count();

    if (!s_baseInit) {
        // On first update this session, treat existing counters as already
        // credited (they were persisted); only NEW recon feeds the pet.
        s_basePwnd = pwnd;
        s_basePeer = peers;
        s_baseInit = true;
    }

    bool fed = false;

    if (pwnd > s_basePwnd) {
        int d = pwnd - s_basePwnd;
        s_basePwnd = pwnd;
        s_st.pwnd += d;
        s_st.hunger = clampi(s_st.hunger + FEED_PWND * d, 0, 100);
        s_st.bond   = clampi(s_st.bond + BOND_PWND * d, 0, 100);
        add_xp(XP_PER_PWND * d);
        fed = true;
    }
    if (peers > s_basePeer) {
        int d = peers - s_basePeer;
        s_basePeer = peers;
        s_st.peers += d;
        s_st.hunger = clampi(s_st.hunger + FEED_PEER * d, 0, 100);
        s_st.bond   = clampi(s_st.bond + BOND_PEER * d, 0, 100);
        add_xp(XP_PER_PEER * d);
        fed = true;
    }
    if (fed) s_excitedUntil = now + EXCITED_MS;

    // Time-based decay (per real minute while running).
    while (now - s_lastDecay >= DECAY_PERIOD_MS) {
        s_lastDecay += DECAY_PERIOD_MS;
        s_st.hunger = clampi(s_st.hunger - HUNGER_DECAY, 0, 100);
        // Rest when well-fed, tire when starving.
        s_st.energy = clampi(s_st.energy + (s_st.hunger > 40 ? 1 : -ENERGY_DECAY), 0, 100);
        s_dirty = true;
    }

    // Mood (priority order).
    uint8_t mood;
    if (s_threat > 0)                       mood = HEX_WARY;
    else if (now < s_excitedUntil)          mood = HEX_EXCITED;
    else if (s_st.hunger < 20)              mood = HEX_HUNGRY;
    else if (s_st.energy < 20)              mood = HEX_SLEEPY;
    else if (s_st.hunger > 60 && s_st.bond > 40) mood = HEX_HAPPY;
    else                                    mood = HEX_IDLE;
    s_st.mood = mood;

    check_evolution();

    if (s_dirty && now - s_lastSave > SAVE_MIN_MS) hexhound_save();
}

// ── One-shot evolution flag ────────────────────────────────────────────────

bool hexhound_take_evolved_flag() {
    bool e = s_evolved;
    s_evolved = false;
    return e;
}

uint8_t hexhound_prev_stage() { return s_prevStage; }

// ── Accessors ──────────────────────────────────────────────────────────────

const HexHoundState& hexhound_state() { return s_st; }

int hexhound_level() { return 1 + (int)(s_st.xp / 100); }

int hexhound_xp_into_stage() {
    long base;
    switch (s_st.stage) {
        case HEX_BEAST:    base = XP_BEAST;   break;
        case HEX_GREMLIN:  base = XP_GREMLIN; break;
        case HEX_SENTINEL: base = XP_SENTINEL;break;
        default:           base = 0;          break;   // Egg / Pup start at 0
    }
    long into = s_st.xp - base;
    return into < 0 ? 0 : (int)into;
}

int hexhound_xp_stage_span() {
    switch (s_st.stage) {
        case HEX_EGG:
        case HEX_PUP:      return (int)XP_BEAST;                 // 0   -> 200
        case HEX_BEAST:    return (int)(XP_GREMLIN - XP_BEAST);  // 200 -> 500
        case HEX_GREMLIN:  return (int)(XP_SENTINEL - XP_GREMLIN);// 500 -> 1000
        case HEX_SENTINEL:
        default:           return 100;   // Sentinel: 100-XP mastery bands
    }
}

const char* hexhound_stage_name() {
    int i = clampi(s_st.stage, HEX_EGG, HEX_SENTINEL) - 1;
    return STAGE_NAMES[i];
}

const char* hexhound_ability_name() {
    int i = clampi(s_st.stage, HEX_EGG, HEX_SENTINEL) - 1;
    return STAGE_ABILITY[i];
}

const char* hexhound_mood_speech() {
    switch (s_st.mood) {
        case HEX_WARY:    return "tail confirmed. teeth out.";
        case HEX_EXCITED: return "nom! fresh packets!";
        case HEX_HUNGRY:  return "feed me a handshake...";
        case HEX_SLEEPY:  return "low power... resting.";
        case HEX_HAPPY:   return "hunting the airwaves ~";
        case HEX_IDLE:
        default:
            if (s_st.stage == HEX_EGG) return "...tick... incubating...";
            return "sniffing for signals.";
    }
}
