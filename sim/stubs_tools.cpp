// Link-time stubs that let tools_screen.cpp and threat_radar_screen.cpp build
// in the simulator. SIM ONLY.
//
// WHY THESE ARE SAFE. tools_screen.cpp is the money shot - 27 tiles, and
// tools_apply_mode() genuinely changes which are visible per mode - but it only
// ever ASKS the detector modules two things: "are you running" and "start/stop".
// It never reads detection data. So a stub that answers "not running" puts the
// grid in exactly the state a watch at rest is in. Nothing is invented.
//
// The detector ENGINES cannot be built here (WiFi/BLE/ESP-IDF), and that is the
// point of the boundary: the simulator renders the real UI, it does not pretend
// to detect anything.
#include "Arduino.h"
#include "threat_radar.h"

// ---- detector run-state ----------------------------------------------------
// All report "not running". Toggling a tile in a live sim window will call
// start() and then still read back false, so a tile cannot latch into a lying
// "armed" state during a capture.
bool airtag_start(void)             { return false; }
void airtag_stop(void)              {}
bool airtag_is_running(void)        { return false; }
bool flipper_start(void)            { return false; }
void flipper_stop(void)             {}
bool flipper_is_running(void)       { return false; }
bool skimmer_start(void)            { return false; }
void skimmer_stop(void)             {}
bool skimmer_is_running(void)       { return false; }
bool evil_twin_start(void)          { return false; }
void evil_twin_stop(void)           {}
bool evil_twin_is_running(void)     { return false; }
bool flock_start(void)              { return false; }
void flock_stop(void)               {}
bool flock_is_running(void)         { return false; }
bool flock_wifi_active(void)        { return false; }
bool flock_ble_active(void)         { return false; }
bool handshake_start(void)          { return false; }
void handshake_stop(void)           {}
bool handshake_is_running(void)     { return false; }
bool tracker_sweep_start(void)      { return false; }
void tracker_sweep_stop(void)       {}
bool tracker_sweep_is_running(void) { return false; }

// ---- Threat Radar ----------------------------------------------------------
// Rendered in its CLEAR state on purpose. Fabricating co-moving contacts would
// put a tail verdict on screen that never happened, on the one feature whose
// public claim is already carefully scoped (see the README Field status: Likely
// is field-proven, Confirmed has never been observed). The clear state is both
// honest and what the screen actually shows at rest.
//
// The engine lives in threat_radar.cpp, which needs freertos/queue.h.
int  threatradar_threat_count(void)              { return 0; }
int  threatradar_get_threats(TrThreat *, int)    { return 0; }
void threatradar_reset(void)                     {}

// These two are pure lookup tables in the real module. Copied VERBATIM from
// src/threat_radar.cpp (kCatNames / kLvlNames) so the simulator cannot drift
// from the device wording; if those tables change, change these too.
const char *threatradar_category_name(uint8_t c)
{
    static const char *kCatNames[] = {
        "AirTag", "Flipper", "Skimmer", "Flock", "Evil-Twin", "Vehicle", "Tracker"
    };
    return c < (sizeof(kCatNames) / sizeof(kCatNames[0])) ? kCatNames[c] : "?";
}
const char *threatradar_level_name(uint8_t l)
{
    static const char *kLvlNames[4] = { "-", "POSSIBLE", "LIKELY", "CONFIRMED" };
    return l < 4 ? kLvlNames[l] : "?";
}

// ---- navigation targets reached from the Tools grid ------------------------
// The grid's job in the reel is to SHOW the 27 tiles and how the mode gates
// them; the harness selects screens directly, so these stay inert.
void analyze_screen_show(void)          {}
void aprs_screen_show(void)             {}
void beacon_spam_screen_show(void)      {}
void deauth_attack_screen_show(void)    {}
void deauth_screen_show(void)           {}
void loot_screen_show(void)             {}
void mouse_screen_show(void)            {}
void nfc_field_screen_show(void)        {}
void pager_screen_show(void)            {}
void pet_screen_show(void)              {}
void probe_sniffer_screen_show(void)    {}
void rogue_ap_screen_show(void)         {}
void spycam_screen_show(void)           {}
void tesla_cp_screen_show(void)         {}
void tpms_screen_show(void)             {}
void tracker_timeline_screen_show(void) {}
void usb_sd_screen_show(void)           {}
void wifi_screen_show(void)             {}

// ---- misc ------------------------------------------------------------------
// tools_screen.cpp raises LVGL's share of the main loop before painting; there
// is no competing main loop here.
// main.cpp owns this on the device; there is no competing main loop here.
// tools_screen.cpp defines tools_screen_show/apply_mode/attach_jump_gesture
// itself, so none of those are stubbed.
void main_loop_request_lvgl_priority(int) {}

// main.cpp's low-memory warning dialog. Unreachable here: it fires from the
// 1 Hz heap check and from a failed radio start, and no tile ever starts a
// radio in the simulator.
void low_mem_show_dialog(const char *) {}
