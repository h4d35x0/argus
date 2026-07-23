#pragma once
#include <stdint.h>
#include <stdbool.h>

// Deauthentication attack (OFFENSE). Two automatic phases per run: a brief
// passive SURVEY (collect nearby AP BSSIDs + channels via the beacon manager),
// then INJECT broadcast deauth/disassoc frames to knock clients off those APs,
// hopping to each AP's channel. Actively disconnects third parties - the
// highest-impact WiFi tool here.
//
// AUTHORIZED USE ONLY. Reachable only in Offense; started by a deliberate press
// on the warning screen. Uses offense_wifi (refuses while BLE is up, never hangs
// the watch). Deauth frames need the raw-frame sanity-check override (below),
// which must be VERIFIED ON-HARDWARE against the pinned core version.

bool deauth_attack_start();   // false if WiFi can't be surveyed/claimed
void deauth_attack_stop();
bool deauth_attack_is_running();
int  deauth_attack_target_count();
uint32_t deauth_attack_frames();
bool deauth_attack_surveying();   // true during the initial survey phase

void deauth_attack_screen_create();
void deauth_attack_screen_show();
bool deauth_attack_screen_is_active();
