#pragma once
#include <stdbool.h>

// Per-radio "enable at boot" preferences, persisted to the SD card at
// /Settings/boot_radios.txt (one key=value line per radio). This is a separate
// concept from each radio screen's own last-on/off file (e.g. /Settings/wifi.txt):
// this file records ONLY whether the user opted a radio into auto-enabling when
// the watch powers on. The default for every radio is OFF - nothing auto-enables
// unless the user opts in via Settings > "Enable at boot".
//
// WiFi and BLE cannot both auto-enable: the two radios cannot run at the same
// time on this board (shared internal SRAM), so the Settings UI enforces that
// only one of them is opted into boot at a time. Bringing BLE up at boot has
// boot-looped before, so BLE-at-boot is an opt-in the user enables knowingly.

enum BootRadio {
    BOOT_RADIO_WIFI = 0,
    BOOT_RADIO_BLE,
    BOOT_RADIO_LORA,
    BOOT_RADIO_GPS,
    BOOT_RADIO_COUNT
};

// Returns the saved "enable at boot" flag for one radio. Defaults to false
// (off) when the SD card is unavailable, the file is missing, or the key isn't
// present - so a radio only ever auto-enables when the user has explicitly
// opted in.
bool boot_prefs_get(BootRadio radio);

// Persist the "enable at boot" flag for one radio. Rewrites the whole file so
// the other radios' flags are preserved. No-op when the SD card is not mounted
// or a USB-MSC host owns it (mirrors the radio screens' save-power guard).
void boot_prefs_set(BootRadio radio, bool enabled);

// Boot-loop breaker for BLE-at-boot (which has boot-looped this watch before).
// The boot path marks an attempt PENDING before bringing BLE up, and clears it
// only once the watch has run for a few seconds. If a boot finds the marker
// still set, the previous attempt did not survive (it crashed / boot-looped),
// so the caller skips BLE and disables the opt-in to break the loop. Backed by
// a marker file /Settings/ble_boot_pending.txt (existence == pending).
bool boot_prefs_ble_attempt_pending();
void boot_prefs_ble_attempt_mark();
void boot_prefs_ble_attempt_clear();
