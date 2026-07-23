#pragma once
#include <stdint.h>
#include <stdbool.h>

// Rogue AP / evil twin (OFFENSE). Broadcasts an OPEN WiFi access point named as a
// common auto-join network (xfinitywifi, attwifi, ...) so nearby devices set to
// remember those SSIDs associate to the watch. Pairs with Deauther: kick clients
// off the real AP and they land here. No captive portal in this build (no cred
// capture) - just the lure + a live associated-client count.
//
// AUTHORIZED USE ONLY. Offense-gated; started from the tool screen. Uses the
// shared offense_wifi owner in AP mode, so it can't run at the same time as the
// deauth/beacon injectors (single radio).

bool rogue_ap_start(const char *ssid);
void rogue_ap_stop();
bool rogue_ap_is_running();
const char *rogue_ap_ssid();
int  rogue_ap_clients();

void rogue_ap_screen_create();
void rogue_ap_screen_show();
bool rogue_ap_screen_is_active();
