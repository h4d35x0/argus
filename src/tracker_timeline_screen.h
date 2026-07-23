#pragma once
#include <lvgl.h>

// "Who's been following me" timeline (DEFENSE). A chronological, oldest-first
// view of the tracker/tail contacts the Threat Radar has correlated as co-moving
// with you, so a device that latched on early and stayed reads as a long run down
// the list. Read-only; builds entirely on the existing threat_radar store (no new
// scanning). BOOT / swipe-right returns to Tools.

void tracker_timeline_screen_create();
void tracker_timeline_screen_show();
bool tracker_timeline_screen_is_active();
