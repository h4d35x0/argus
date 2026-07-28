#pragma once
#include <lvgl.h>

void configuration_screen_create();
void configuration_screen_show();
bool configuration_screen_is_active();
void configuration_screen_commit(); // push field values to meshtastic

// Refresh the GPS-lock status icon and enable/disable the broadcast toggle.
// Called from the 1-second tick in main.cpp while the screen is active.
void configuration_screen_update();

// User-intent flag for the "Broadcast Location" toggle. This is the wearer's
// consent to disclose their position over the mesh at all; it defaults to OFF
// and persists in /Meshtastic/config.txt. meshtastic.cpp gates the automatic
// reply to a peer's directed position request on it. Callers that actually
// broadcast should still gate on gps_screen_has_lock() themselves.
bool configuration_screen_get_broadcast_location();

// Selected broadcast interval. Returns 0 for the "Once" option — callers
// should treat 0 as "broadcast a single position then stop".
uint32_t configuration_screen_get_broadcast_interval_ms();

// "Rebroadcast Packets" toggle. When true, meshtastic.cpp retransmits each
// unique received LoRa packet with hop_limit decremented, helping multi-hop
// mesh delivery. Defaults off to save battery and airtime.
bool configuration_screen_get_rebroadcast_enabled();

// Haptic-on-incoming-TEXT toggles. Polled by meshtastic.cpp from the
// TEXT RX path so a new message buzzes the wrist - split DM vs
// broadcast so users can leave DMs on while muting the channel chatter.
// Both default ON.
bool configuration_screen_get_vibrate_dm();
bool configuration_screen_get_vibrate_broadcast();
