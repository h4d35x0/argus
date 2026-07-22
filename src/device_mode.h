// device_mode.h - Daily-wear vs Field-tool arbiter (Decision A).
//
// The watch's two radios (WiFi, BLE) cannot run at once, so the product splits
// into two modes:
//   - DailyWear: BLE is up mirroring phone notifications (ANCS today, Gadgetbridge
//     later). WiFi pentest tools are not usable.
//   - FieldTool: WiFi/pentest tools are usable; phone notifications are paused.
//
// This module owns that switch. The DECISION half (device_mode_plan) is pure and
// host-tested; the ACTUATION half (device_mode_set) drives ANCS and reports
// whether the switch was blocked by the other radio, so the caller can raise the
// existing radio-conflict dialog rather than silently failing.
#pragma once

enum class DeviceMode {
    FieldTool = 0,   // default: WiFi/pentest tools; notifications paused
    DailyWear,       // BLE notifications live; WiFi tools paused
};

// Which phone we mirror in DailyWear. iOS uses ANCS (no app); Android uses the
// Alert Notification Service via Gadgetbridge. Selected before enabling; changing
// it while notifications are live has no effect until the next enable.
enum class NotifyPlatform {
    iOS = 0,
    Android,
};

NotifyPlatform device_mode_platform();
void device_mode_set_platform(NotifyPlatform p);

// Re-apply the persisted notification state at boot: if the user had Daily-wear
// enabled, bring the saved platform's notifications back up. Call once from
// setup() after basic init. A no-op (leaving the preference intact) if WiFi is
// active, so a blocked restore is retried the next time the user toggles.
void device_mode_restore_boot();

// What actuation a requested switch implies. Pure, so it is unit-tested.
enum class ModeAction {
    StartNotifications,   // enter DailyWear: bring BLE/ANCS up
    StopNotifications,    // enter FieldTool: tear BLE/ANCS down
    BlockedWifiActive,    // wanted DailyWear but WiFi is up (radios exclusive)
    NoChange,             // already in the requested mode
};

// Pure decision. `current` = mode we are in, `requested` = mode asked for,
// `wifi_active` = is the WiFi radio powered right now.
ModeAction device_mode_plan(DeviceMode current, DeviceMode requested, bool wifi_active);

// Current mode.
DeviceMode device_mode_get();

// Request a mode switch. Returns the action that was taken (or the reason it was
// blocked). On BlockedWifiActive the mode is left unchanged and the caller
// should tell the user to turn WiFi off first.
ModeAction device_mode_set(DeviceMode requested);

// True while DailyWear is active (notifications mirroring).
bool device_mode_is_daily_wear();
