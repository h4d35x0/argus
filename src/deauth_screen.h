#pragma once
#include <lvgl.h>

// Deauth-attack detector status screen (DEFENSE). Read-only: it shows whether a
// nearby WiFi deauth/disassoc FLOOD is happening (someone trying to knock clients
// off a network / force handshakes). The detector is passive and piggybacks on
// any running WiFi scan (Evil Twin / Pwn / Flock / wardriver), so this screen is
// only "live" while one of those is active - it says so when nothing is scanning.
// Never transmits. BOOT / swipe-right returns to Tools.

void deauth_screen_create();
void deauth_screen_show();
bool deauth_screen_is_active();
