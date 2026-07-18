#include "tracker_rep.h"
#include "meshtastic.h"
#include <Arduino.h>     // millis()
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// One-character category code carried on the wire, indexed by the Threat Radar
// TrCategory enum (AirTag=0, Flipper=1, Skimmer=2, Flock=3, Evil-Twin=4).
static const char kCatChar[6] = { 'A', 'F', 'S', 'K', 'E', 'V' };
static char cat_to_char(uint8_t c) { return c < 6 ? kCatChar[c] : 'A'; }
static uint8_t char_to_cat(char ch) {
    for (uint8_t i = 0; i < 6; i++) if (kCatChar[i] == ch) return i;
    return 0;
}

uint32_t tracker_rep_hash(const uint8_t *mac6)
{
    uint32_t h = 2166136261u;            // FNV-1a, 32-bit
    for (int i = 0; i < 6; i++) { h ^= mac6[i]; h *= 16777619u; }
    return h;
}

// Reputation store. Small fixed pool keyed by hash; entries expire so a tracker
// that has long since left the area stops shouting. Touched from the main task
// (mesh RX drain and Threat Radar bg_tick both run there) — no locking needed.
#define TR_REP_MAX  64
static const uint32_t TR_REP_TTL_MS = 6UL * 3600 * 1000;   // 6 hours

struct RepEntry {
    bool     used;
    uint32_t hash;
    uint8_t  cat;
    uint32_t from;     // 0 = flagged locally, else peer node id
    uint32_t ts;       // millis() of last refresh
    bool     remote;   // ever heard from a peer
};
static RepEntry s_rep[TR_REP_MAX];

static bool expired(const RepEntry *e, uint32_t now)
{
    return now - e->ts >= TR_REP_TTL_MS;
}

// Insert or refresh a flag. Returns true only when it is a genuinely new hash
// (used by the local path to decide whether to broadcast — refreshes are quiet).
static bool rep_add(uint32_t hash, uint8_t cat, uint32_t from, bool remote)
{
    uint32_t now = millis();
    int free_idx = -1;
    for (int i = 0; i < TR_REP_MAX; i++) {
        RepEntry &e = s_rep[i];
        if (e.used && !expired(&e, now)) {
            if (e.hash == hash) {           // already known — refresh
                e.ts = now;
                if (remote) { e.remote = true; if (e.from == 0) e.from = from; }
                return false;
            }
        } else if (free_idx < 0) {
            free_idx = i;                    // reuse empty or expired slot
        }
    }
    if (free_idx < 0) {                      // pool full of live flags — evict oldest
        free_idx = 0;
        for (int i = 1; i < TR_REP_MAX; i++)
            if (s_rep[i].ts < s_rep[free_idx].ts) free_idx = i;
    }
    RepEntry &e = s_rep[free_idx];
    e.used = true; e.hash = hash; e.cat = cat;
    e.from = from; e.ts = now; e.remote = remote;
    return true;
}

void tracker_rep_flag_local(const uint8_t *mac6, uint8_t category)
{
    uint32_t h = tracker_rep_hash(mac6);
    bool is_new = rep_add(h, category, 0, false);

    // Only announce a freshly confirmed tail, and only if the mesh is up.
    if (is_new && meshtastic_is_active()) {
        char buf[24];
        snprintf(buf, sizeof(buf), "TRFLAG|%08lX|%c",
                 (unsigned long)h, cat_to_char(category));
        meshtastic_send_text(buf);
    }
}

bool tracker_rep_handle_text(uint32_t from_node, const char *text, uint32_t len)
{
    // Format: "TRFLAG|" + 8 hex + "|" + 1 cat char  -> 17 chars minimum.
    if (len < 17 || memcmp(text, "TRFLAG|", 7) != 0) return false;

    char hex[9];
    memcpy(hex, text + 7, 8);
    hex[8] = '\0';
    if (text[15] != '|') return false;

    char *end = nullptr;
    uint32_t hash = (uint32_t)strtoul(hex, &end, 16);
    if (end != hex + 8) return false;        // not 8 valid hex digits

    uint8_t cat = char_to_cat(text[16]);
    rep_add(hash, cat, from_node, true);
    return true;                              // handled — caller skips chat store
}

bool tracker_rep_is_flagged(const uint8_t *mac6, uint8_t *out_cat, uint32_t *out_from)
{
    uint32_t h = tracker_rep_hash(mac6);
    uint32_t now = millis();
    for (int i = 0; i < TR_REP_MAX; i++) {
        RepEntry &e = s_rep[i];
        if (e.used && !expired(&e, now) && e.hash == h) {
            if (out_cat)  *out_cat  = e.cat;
            if (out_from) *out_from = e.from;
            return true;
        }
    }
    return false;
}

int tracker_rep_count()
{
    uint32_t now = millis();
    int n = 0;
    for (int i = 0; i < TR_REP_MAX; i++)
        if (s_rep[i].used && !expired(&s_rep[i], now)) n++;
    return n;
}

int tracker_rep_remote_count()
{
    uint32_t now = millis();
    int n = 0;
    for (int i = 0; i < TR_REP_MAX; i++)
        if (s_rep[i].used && !expired(&s_rep[i], now) && s_rep[i].remote) n++;
    return n;
}
