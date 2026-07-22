// ans.h - Android notification mirroring via the BLE Alert Notification Service.
//
// Android has no ANCS equivalent, so we ride Gadgetbridge (open-source, F-Droid).
// The watch presents itself as an InfiniTime/PineTime (a device Gadgetbridge
// already supports) by exposing the standard Alert Notification Service (0x1811)
// as a GATT SERVER. Gadgetbridge writes each phone notification to the New Alert
// characteristic (0x2A46); we parse it and push it into notify::center().
//
// No ARGUS app to build or maintain - the user installs Gadgetbridge and it
// forwards notifications to us over the standard service.
//
// This is the mirror image of ancs.h: there the watch is a GATT client to iOS;
// here the watch is a GATT server to the Android phone. The Daily-wear/Field-tool
// mode owner starts exactly one of {ancs, ans} at a time (never both), so they do
// not contend for the radio.
#pragma once

namespace ans {

// Bring up BLE (guarded against WiFi), expose the Alert Notification Service, and
// advertise as an InfiniTime so Gadgetbridge detects and drives us. Returns false
// if WiFi is up or the stack fails. Idempotent.
bool start();

// Tear the server + stack down. Idempotent.
void stop();

bool is_running();

// True once a phone (Gadgetbridge) has connected.
bool is_connected();

}  // namespace ans
