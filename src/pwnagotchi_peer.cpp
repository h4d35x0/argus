#include "pwnagotchi_peer.h"
#include <Arduino.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define PWN_MAX_NAMES 16

static char     s_names[PWN_MAX_NAMES][24];
static int      s_count     = 0;
static char     s_last[24]  = "";
static int      s_last_pwnd = -1;
static uint32_t s_last_ms   = 0;

// Pull a "key":"value" string out of the advert JSON (best-effort, no real
// parser — the blob is short and flat).
static bool json_str(const char *json, const char *key, char *out, int outsz)
{
    char pat[24];
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p += strlen(pat);
    int i = 0;
    while (p[i] && p[i] != '"' && i < outsz - 1) { out[i] = p[i]; i++; }
    out[i] = '\0';
    return i > 0;
}

static int json_int(const char *json, const char *key)
{
    char pat[24];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(json, pat);
    if (!p) return -1;
    p += strlen(pat);
    while (*p == ' ') p++;
    if (*p < '0' || *p > '9') return -1;
    return (int)strtol(p, nullptr, 10);
}

bool pwnagotchi_check(const uint8_t *src_mac, int8_t rssi, const char *json)
{
    (void)src_mac; (void)rssi;

    char name[24] = "pwnagotchi";
    if (json && json[0]) {
        char tmp[24];
        if (json_str(json, "name", tmp, sizeof(tmp))) {
            strncpy(name, tmp, sizeof(name) - 1);
            name[sizeof(name) - 1] = '\0';
        }
        int pt = json_int(json, "pwnd_tot");
        if (pt >= 0) s_last_pwnd = pt;
    }

    s_last_ms = millis();
    strncpy(s_last, name, sizeof(s_last) - 1);
    s_last[sizeof(s_last) - 1] = '\0';

    for (int i = 0; i < s_count; i++)
        if (strcmp(s_names[i], name) == 0) return false;   // already met

    if (s_count < PWN_MAX_NAMES) {
        strncpy(s_names[s_count], name, 23);
        s_names[s_count][23] = '\0';
        s_count++;
    }
    return true;   // a new friend
}

int         pwnagotchi_peer_count()   { return s_count; }
const char *pwnagotchi_last_name()    { return s_last[0] ? s_last : "pwnagotchi"; }
int         pwnagotchi_last_pwnd()     { return s_last_pwnd; }
uint32_t    pwnagotchi_last_seen_ms() { return s_last_ms; }

void pwnagotchi_reset()
{
    s_count = 0; s_last[0] = '\0'; s_last_pwnd = -1; s_last_ms = 0;
}
