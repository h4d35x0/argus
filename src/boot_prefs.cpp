#include "boot_prefs.h"
#include <LilyGoLib.h>
#include <SD.h>
#include "usb_sd.h"

// Small key=value file holding the per-radio "enable at boot" opt-in flags.
// Mirrors the small-file SD pattern used by wifi_radio_screen.cpp / gps_screen.cpp.
#define BOOT_PREFS_PATH "/Settings/boot_radios.txt"

// File keys indexed by BootRadio, in enum order. Kept short and stable so the
// on-disk format stays readable and forward-compatible.
static const char *k_radio_keys[BOOT_RADIO_COUNT] = {
    "wifi",   // BOOT_RADIO_WIFI
    "ble",    // BOOT_RADIO_BLE
    "lora",   // BOOT_RADIO_LORA
    "gps",    // BOOT_RADIO_GPS
};

// Reads every flag off the SD card into out[]. Any key that is missing (or the
// whole file / card being unavailable) leaves that slot at its caller-seeded
// default, so a missing file means "all off".
static void boot_prefs_read_all(bool out[BOOT_RADIO_COUNT])
{
    if (!instance.isCardReady() || usb_sd_is_running()) return; // host owns SD when mounted
    if (!SD.exists(BOOT_PREFS_PATH)) return;

    File f = SD.open(BOOT_PREFS_PATH, FILE_READ);
    if (!f) return;

    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        int eq = line.indexOf('=');
        if (eq <= 0) continue;
        String key = line.substring(0, eq);
        bool   on  = (line.substring(eq + 1).toInt() != 0);
        for (int i = 0; i < BOOT_RADIO_COUNT; i++) {
            if (key == k_radio_keys[i]) { out[i] = on; break; }
        }
    }
    f.close();
}

bool boot_prefs_get(BootRadio radio)
{
    if ((int)radio < 0 || (int)radio >= BOOT_RADIO_COUNT) return false;
    bool flags[BOOT_RADIO_COUNT] = { false, false, false, false };
    boot_prefs_read_all(flags);
    return flags[radio];
}

void boot_prefs_set(BootRadio radio, bool enabled)
{
    if ((int)radio < 0 || (int)radio >= BOOT_RADIO_COUNT) return;
    if (!instance.isCardReady() || usb_sd_is_running()) return; // host owns SD when mounted

    // Read the current flags first so writing one radio doesn't clobber the
    // others - the file holds all four on their own lines.
    bool flags[BOOT_RADIO_COUNT] = { false, false, false, false };
    boot_prefs_read_all(flags);
    flags[radio] = enabled;

    if (!SD.exists("/Settings")) SD.mkdir("/Settings");
    File f = SD.open(BOOT_PREFS_PATH, FILE_WRITE);   // FILE_WRITE = truncate
    if (!f) return;
    for (int i = 0; i < BOOT_RADIO_COUNT; i++)
        f.printf("%s=%d\n", k_radio_keys[i], flags[i] ? 1 : 0);
    f.close();
}

// Marker file whose mere existence means "a BLE-at-boot attempt is in flight and
// has not yet been confirmed survived". See the header for the boot-loop-breaker
// contract.
#define BLE_BOOT_MARKER_PATH "/Settings/ble_boot_pending.txt"

bool boot_prefs_ble_attempt_pending()
{
    if (!instance.isCardReady() || usb_sd_is_running()) return false;
    return SD.exists(BLE_BOOT_MARKER_PATH);
}

void boot_prefs_ble_attempt_mark()
{
    if (!instance.isCardReady() || usb_sd_is_running()) return;
    if (!SD.exists("/Settings")) SD.mkdir("/Settings");
    File f = SD.open(BLE_BOOT_MARKER_PATH, FILE_WRITE);
    if (!f) return;
    f.print("1\n");
    f.close();
}

void boot_prefs_ble_attempt_clear()
{
    if (!instance.isCardReady() || usb_sd_is_running()) return;
    if (SD.exists(BLE_BOOT_MARKER_PATH)) SD.remove(BLE_BOOT_MARKER_PATH);
}
