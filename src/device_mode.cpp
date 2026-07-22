// device_mode.cpp - device-side actuation for the mode arbiter. See device_mode.h.
#include "device_mode.h"
#include "ancs.h"
#include "ans.h"
#include "ble_scan_manager.h"

#include <WiFi.h>
#include <Preferences.h>

static DeviceMode     s_mode     = DeviceMode::FieldTool;
static NotifyPlatform s_platform = NotifyPlatform::iOS;

static bool wifi_active() { return WiFi.getMode() != WIFI_MODE_NULL; }

// Persist the current enabled-state + platform to NVS. Called only on genuine,
// successful transitions so a WiFi-blocked attempt can never clobber the saved
// preference.
static void persist()
{
    Preferences p;
    if (!p.begin("argusnotify", false)) return;
    p.putBool("en", s_mode == DeviceMode::DailyWear);
    p.putUChar("plat", (uint8_t)s_platform);
    p.end();
}

DeviceMode device_mode_get() { return s_mode; }

bool device_mode_is_daily_wear() { return s_mode == DeviceMode::DailyWear; }

NotifyPlatform device_mode_platform() { return s_platform; }
void device_mode_set_platform(NotifyPlatform p) { s_platform = p; persist(); }

// Bring up the notification source for the selected platform. iOS -> ANCS
// (watch is GATT client), Android -> ANS/Gadgetbridge (watch is GATT server).
// Exactly one is ever up, so they never contend for the radio.
static bool start_notifications()
{
    return (s_platform == NotifyPlatform::iOS) ? ancs::start() : ans::start();
}

static void stop_notifications()
{
    ancs::stop();
    ans::stop();
}

ModeAction device_mode_set(DeviceMode requested)
{
    ModeAction action = device_mode_plan(s_mode, requested, wifi_active());
    switch (action) {
    case ModeAction::StartNotifications:
        // start_*() re-check WiFi and return false if they somehow raced on;
        // treat that as still blocked (and do NOT persist, so the preference
        // survives a boot where WiFi happened to be up).
        if (!start_notifications()) return ModeAction::BlockedWifiActive;
        s_mode = DeviceMode::DailyWear;
        persist();
        break;
    case ModeAction::StopNotifications:
        stop_notifications();
        s_mode = DeviceMode::FieldTool;
        persist();
        break;
    case ModeAction::BlockedWifiActive:
    case ModeAction::NoChange:
        break;   // mode unchanged, nothing to persist
    }
    return action;
}

void device_mode_restore_boot()
{
    Preferences p;
    if (!p.begin("argusnotify", true)) return;
    bool    en   = p.getBool("en", false);
    uint8_t plat = p.getUChar("plat", (uint8_t)NotifyPlatform::iOS);
    p.end();

    if (!en) return;                         // was off; nothing to restore
    // If a BLE scanner (AirTag/Flipper/etc., e.g. a boot radio) already owns the
    // single GAP callback slot, don't fight it; keep the preference and let the
    // user re-enable from a clean state.
    if (ble_scan_active()) return;
    s_platform = (NotifyPlatform)plat;
    device_mode_set(DeviceMode::DailyWear);  // no-op + preference kept if WiFi is up
}
