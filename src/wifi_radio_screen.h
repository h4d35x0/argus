#pragma once
#include <lvgl.h>

// Status / power-control screen for the WiFi radio. Sits between the LoRa
// and NFC screens in the power-button rotation, mirroring the layout of
// the GPS and LoRa screens (title + power toggle + live data table).
//
// Note: this is *not* the WiFi tools screen — that's `wifi_screen` (the
// scanner, ping-sweep, port-scan, etc., reached from the Tools menu).
// This screen only governs the radio's power state and surfaces a quick
// read of its current configuration.

void wifi_radio_screen_create();
void wifi_radio_screen_restore_power();
void wifi_radio_screen_show();
bool wifi_radio_screen_is_active();
bool wifi_radio_screen_is_powered();
