#pragma once
#include <lvgl.h>

// Sun & Moon screen reached from the Time page: sunrise / sunset / day length
// for the current GPS position and date, plus the moon phase and illumination.
// Sun times need a GPS fix (location); the moon phase is date-only and always
// shows. Swipe right to return to the Time grid.

void sun_moon_screen_create();
void sun_moon_screen_show();
bool sun_moon_screen_is_active();
