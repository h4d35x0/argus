#pragma once

// Full-screen view for Threat Radar: a banner that calls out whether anything
// is co-moving with you, then a list of ranked contacts (category, follow
// level, distance travelled alongside you, dwell time, first-seen time, RSSI).
// A CLEAR button wipes the store for a fresh trip. Swipe up returns to Tools.
//
// Built once at boot by threat_radar_screen_create(); the list rebuilds itself
// on show() and on a slow timer while the screen is in front, reading the live
// store via the threat_radar.h query API.

void threat_radar_screen_create();
void threat_radar_screen_show();
bool threat_radar_screen_is_active();
