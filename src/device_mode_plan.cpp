// device_mode_plan.cpp - the PURE decision half of the mode arbiter.
//
// Kept in its own translation unit (no Arduino/BLE includes) so it compiles and
// unit-tests on the host. device_mode.cpp holds the device-side actuation.
#include "device_mode.h"

ModeAction device_mode_plan(DeviceMode current, DeviceMode requested, bool wifi_active)
{
    if (current == requested) {
        return ModeAction::NoChange;
    }
    if (requested == DeviceMode::DailyWear) {
        // Entering DailyWear brings BLE up. If WiFi is still powered the two
        // radios would collide (and esp_bt_controller_enable() would hang), so
        // refuse until WiFi is off.
        if (wifi_active) return ModeAction::BlockedWifiActive;
        return ModeAction::StartNotifications;
    }
    // requested == FieldTool: always allowed; frees BLE for WiFi/pentest use.
    return ModeAction::StopNotifications;
}
