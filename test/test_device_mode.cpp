// test_device_mode.cpp - host tests for the pure mode-arbiter decision.
#include "wl_test.h"
#include "device_mode.h"

WL_TEST(mode_same_mode_is_no_change)
{
    WL_CHECK(device_mode_plan(DeviceMode::FieldTool, DeviceMode::FieldTool, false)
             == ModeAction::NoChange);
    WL_CHECK(device_mode_plan(DeviceMode::DailyWear, DeviceMode::DailyWear, false)
             == ModeAction::NoChange);
}

WL_TEST(mode_enter_daily_wear_starts_notifications_when_wifi_off)
{
    WL_CHECK(device_mode_plan(DeviceMode::FieldTool, DeviceMode::DailyWear, false)
             == ModeAction::StartNotifications);
}

WL_TEST(mode_enter_daily_wear_blocked_when_wifi_on)
{
    WL_CHECK(device_mode_plan(DeviceMode::FieldTool, DeviceMode::DailyWear, true)
             == ModeAction::BlockedWifiActive);
}

WL_TEST(mode_enter_field_tool_stops_notifications)
{
    // Leaving DailyWear is always allowed, regardless of WiFi state.
    WL_CHECK(device_mode_plan(DeviceMode::DailyWear, DeviceMode::FieldTool, false)
             == ModeAction::StopNotifications);
    WL_CHECK(device_mode_plan(DeviceMode::DailyWear, DeviceMode::FieldTool, true)
             == ModeAction::StopNotifications);
}
