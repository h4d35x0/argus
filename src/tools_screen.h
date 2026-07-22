#pragma once
#include <lvgl.h>

void tools_screen_create();
void tools_screen_show();
bool tools_screen_is_active();

// Show/hide the Tools tiles for the current ArgusMode (Daily hides all, Defense
// shows Daily+Defense tiles, Offense shows all). Idempotent; safe to call on any
// mode change and on Tools-screen entry. Defined in tools_screen.cpp.
void tools_apply_mode();
