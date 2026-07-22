#pragma once
#include <lvgl.h>

// Spycam results screen: lists the wireless cameras the passive #9 fingerprint
// detector has seen (class, confidence, SSID, RSSI). Reached from the Spycam
// tile. Swipe right to return to Tools. Detection itself is passive and always
// running in the WiFi beacon pipeline; this screen just reads the store.

void spycam_screen_create();
void spycam_screen_show();
bool spycam_screen_is_active();
