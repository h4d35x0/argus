#pragma once
#include <stdint.h>
#include <stdbool.h>

// Detect nearby Pwnagotchi units. Every Pwnagotchi advertises from the same
// source MAC DE:AD:BE:EF:DE:AD, carrying a JSON blob (name, pwnd_tot, face, …)
// in an oversized SSID information element — so peers are distinguished by the
// JSON "name", not by MAC. Fed from the WiFi beacon manager's frame parser,
// which special-cases this MAC ahead of its normal infrastructure-AP filter.

// Returns true the first time a given peer name is seen this session.
bool        pwnagotchi_check(const uint8_t *src_mac, int8_t rssi, const char *json);

int         pwnagotchi_peer_count();    // distinct names seen this session
const char *pwnagotchi_last_name();     // most-recent peer ("pwnagotchi" if unnamed)
int         pwnagotchi_last_pwnd();     // pwnd_tot of the most-recent peer, -1 if unknown
uint32_t    pwnagotchi_last_seen_ms();  // millis() of last sighting (0 = never)
void        pwnagotchi_reset();
