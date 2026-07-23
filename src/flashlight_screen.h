#pragma once
#include <stdbool.h>

// Flashlight: a full-screen white page for use as a torch. Benign daily-wear
// utility, reachable from the Time hub. Tap anywhere to exit. Keeps the display
// awake at active brightness while shown (no idle-dim).

void flashlight_screen_create();
void flashlight_screen_show();
bool flashlight_screen_is_active();
