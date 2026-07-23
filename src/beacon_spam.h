#pragma once
#include <stdint.h>
#include <stdbool.h>

// Beacon / SSID flood (OFFENSE). Sprays fabricated 802.11 beacon frames across
// channels 1-13 so a swarm of junk SSIDs appears in nearby WiFi lists. Annoyance
// / spectrum-pollution class, reversible (stops the instant you stop it). Beacon
// frames pass the ESP-IDF raw-frame sanity check, so no override is needed.
//
// AUTHORIZED USE ONLY (transmits): reachable only in Offense, and started by a
// deliberate press on the warning screen. Uses the shared offense_wifi injector,
// which refuses to bring WiFi up while BLE is active (never hangs the watch).

bool beacon_spam_start();     // returns false if WiFi can't be claimed (BLE up, etc.)
void beacon_spam_stop();
bool beacon_spam_is_running();
uint32_t beacon_spam_count(); // frames transmitted this run

// The tool's screen (warning + START/STOP + live count). Offense-gated.
void beacon_spam_screen_create();
void beacon_spam_screen_show();
bool beacon_spam_screen_is_active();
