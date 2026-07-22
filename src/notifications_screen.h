// notifications_screen.h - LVGL screen listing mirrored phone notifications.
#pragma once

// Build the screen once at boot (called from setup(), like the other screens).
void notifications_screen_create();

// Show it (from the Tools "Notify" tile).
void notifications_screen_show();
