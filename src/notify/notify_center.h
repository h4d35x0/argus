// notify_center.h - process-wide notification hub (device side).
//
// One shared NotificationStore that every source (iOS ANCS, Android/Gadgetbridge)
// publishes into and the UI reads from. This is the device-side glue around the
// pure NotificationStore: it stamps arrival time, drives the arrival haptic, and
// mirrors each event to Serial (useful for the ANCS bench spike). The pure store
// stays free of Arduino/hardware so it remains host-testable on its own.
#pragma once
#include <cstdint>
#include "notification.h"
#include "notification_store.h"

namespace notify {

// The shared store. Valid for the whole process lifetime.
NotificationStore& center();

// Publish an incoming notification: stamps epoch (from millis), adds to the
// store (insert or update-in-place), fires the arrival haptic, and logs to
// Serial. Sources fill uid/category/app/title/body; epoch is set here.
void publish(Notification n);

// Remove a notification the phone retracted (ANCS "Removed" event) or the user
// dismissed. No-op if absent.
void retract(uint32_t uid);

// Drop everything (UI "clear all").
void clear_all();

// Hand-off for the on-screen banner. publish() runs on the BLE task, but LVGL
// must be driven from the UI thread, so publish() stashes the newest arrival and
// the UI-thread popup timer drains it here. Returns true and fills `out` if a new
// arrival is waiting; false otherwise. Thread-safe.
bool take_pending(Notification& out);

}  // namespace notify
