#pragma once
#include <lvgl.h>

// World-clock screen reached from the Time page: current time across a fixed
// set of major zones, with the watch's local zone highlighted. Updates live
// once a second while visible. Swipe right to return to the Time grid.

void world_clock_screen_create();
void world_clock_screen_show();
bool world_clock_screen_is_active();
