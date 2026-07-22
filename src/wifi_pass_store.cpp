// wifi_pass_store.cpp - see wifi_pass_store.h.
#include "wifi_pass_store.h"
#include "usb_sd.h"
#include <LilyGoLib.h>
#include <SD.h>
#include <Arduino.h>

#define WIFI_PASS_PATH  "/Settings/wifi_passwords.txt"

// The host owns the SD while USB mass-storage is active; never touch it then.
static bool sd_usable()
{
    return instance.isCardReady() && !usb_sd_is_running();
}

bool wifi_pass_lookup(const char *ssid, char *out, size_t out_len)
{
    if (!ssid || !ssid[0] || !out || out_len == 0) return false;
    if (!sd_usable() || !SD.exists(WIFI_PASS_PATH)) return false;

    File f = SD.open(WIFI_PASS_PATH, FILE_READ);
    if (!f) return false;

    bool found = false;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() == 0 || line[0] == '#') continue;
        int eq = line.indexOf('=');
        if (eq <= 0) continue;                       // no key, or empty key
        if (line.substring(0, eq) == ssid) {
            String val = line.substring(eq + 1);
            strncpy(out, val.c_str(), out_len - 1);
            out[out_len - 1] = '\0';
            found = true;                            // last match wins
        }
    }
    f.close();
    return found;
}

void wifi_pass_save(const char *ssid, const char *password)
{
    if (!ssid || !ssid[0] || !password) return;
    if (!sd_usable()) return;

    // Read the existing file (if any) into memory first, replacing the line for
    // this SSID. FILE_WRITE truncates on open, so we must capture the old
    // contents before reopening for write.
    String rebuilt;
    bool replaced = false;
    if (SD.exists(WIFI_PASS_PATH)) {
        File f = SD.open(WIFI_PASS_PATH, FILE_READ);
        if (f) {
            while (f.available()) {
                String line = f.readStringUntil('\n');
                String trimmed = line;
                trimmed.trim();
                int eq = trimmed.indexOf('=');
                if (eq > 0 && trimmed.substring(0, eq) == ssid) {
                    rebuilt += String(ssid) + "=" + password + "\n";
                    replaced = true;
                } else if (trimmed.length() > 0) {
                    rebuilt += trimmed + "\n";        // keep other entries verbatim
                }
            }
            f.close();
        }
    }
    if (!replaced) rebuilt += String(ssid) + "=" + password + "\n";

    if (!SD.exists("/Settings")) SD.mkdir("/Settings");
    File w = SD.open(WIFI_PASS_PATH, FILE_WRITE);     // truncates
    if (!w) return;
    w.print(rebuilt);
    w.close();
}
