#include "screenshot.h"
#include "settings_screen.h"
#include "usb_sd.h"
#include <LilyGoLib.h>
#include <SD.h>
#include <Arduino.h>
#include <lvgl.h>
#include <time.h>

// Defined in main.cpp — current displayed local time. Used to make each
// screenshot filename unique without keeping an in-memory counter that
// would reset on reboot. Same forward-declared C++ symbol the alarm and
// settings modules use, so no extern "C" wrapper here.
void clock_screen_get_local_time(struct tm *out);

#define SCREENSHOT_DIR  "/Screenshots"

// ---- BMP writer ------------------------------------------------------------
//
// 16-bit BI_BITFIELDS BMP: pixels stored as native RGB565 with explicit
// channel masks. Avoids any per-pixel format conversion — the LVGL
// snapshot already gives us packed little-endian RGB565, which matches
// what a 16-bit BMP with these masks expects.
//
// File layout:
//   [ 0..13 ]  BITMAPFILEHEADER (14 bytes)
//   [14..53 ]  BITMAPINFOHEADER (40 bytes)
//   [54..65 ]  Color masks (3 × 4 bytes, used when biCompression = BI_BITFIELDS)
//   [66..   ]  Pixel rows, each padded to 4-byte multiple
//
// The DIB header's height field is negative so rows are stored top-down,
// matching the order LVGL hands them out — no row reversal needed.

#define BMP_HEADER_SIZE   66

static void write_u16_le(uint8_t *p, uint16_t v)
{
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
}
static void write_u32_le(uint8_t *p, uint32_t v)
{
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF;
    p[3] = (v >> 24) & 0xFF;
}
static void write_s32_le(uint8_t *p, int32_t v) { write_u32_le(p, (uint32_t)v); }

// Pack the BITMAPFILEHEADER + BITMAPINFOHEADER + masks into `hdr`. The
// pixel-data byte count includes any per-row padding.
static void build_bmp_header(uint8_t *hdr, int w, int h, uint32_t data_size)
{
    memset(hdr, 0, BMP_HEADER_SIZE);

    // BITMAPFILEHEADER
    hdr[0] = 'B'; hdr[1] = 'M';
    write_u32_le(hdr + 2,  BMP_HEADER_SIZE + data_size);   // total file size
    write_u32_le(hdr + 10, BMP_HEADER_SIZE);               // pixel-data offset

    // BITMAPINFOHEADER — note biSize=40 but we extend it with 3 masks below;
    // BI_BITFIELDS readers stop at the documented end of the v3 header and
    // pick up the masks immediately after, which matches our 54 → 66 byte
    // layout exactly.
    write_u32_le(hdr + 14, 40);                            // biSize
    write_s32_le(hdr + 18, w);                             // biWidth
    write_s32_le(hdr + 22, -h);                            // negative → top-down
    write_u16_le(hdr + 26, 1);                             // biPlanes
    write_u16_le(hdr + 28, 16);                            // biBitCount
    write_u32_le(hdr + 30, 3);                             // biCompression = BI_BITFIELDS
    write_u32_le(hdr + 34, data_size);                     // biSizeImage
    write_s32_le(hdr + 38, 2835);                          // biXPelsPerMeter (72 dpi)
    write_s32_le(hdr + 42, 2835);                          // biYPelsPerMeter
    // biClrUsed / biClrImportant left at 0.

    // Channel masks for RGB565 — red in the top 5 bits, green in the
    // middle 6, blue in the bottom 5. Order is R, G, B (little-endian
    // 16-bit pixels read in the obvious way).
    write_u32_le(hdr + 54, 0xF800);
    write_u32_le(hdr + 58, 0x07E0);
    write_u32_le(hdr + 62, 0x001F);
}

// ---- Capture --------------------------------------------------------------

static bool ensure_dir()
{
    if (SD.exists(SCREENSHOT_DIR)) return true;
    return SD.mkdir(SCREENSHOT_DIR);
}

static void make_filename(char *out, size_t n)
{
    struct tm t;
    clock_screen_get_local_time(&t);
    // ISO-style timestamp — sorts chronologically in the host filesystem
    // browser and is unambiguous across regions.
    snprintf(out, n, SCREENSHOT_DIR "/%04d%02d%02d-%02d%02d%02d.bmp",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec);
}

// Snapshot the active screen and write it as a 16-bit RGB565 BMP to `path`.
// Shared by both the timestamped capture and the named auto-capture.
static bool capture_active_to(const char *path)
{
    lv_obj_t *scr = lv_screen_active();
    if (!scr) return false;

    // LV_USE_SNAPSHOT is enabled in the project's lv_conf.h override.
    // lv_snapshot_take returns a draw buffer in display order with the
    // chosen color format — RGB565 here so the BMP write is a direct
    // memcpy without per-pixel conversion.
    lv_draw_buf_t *snap = lv_snapshot_take(scr, LV_COLOR_FORMAT_RGB565);
    if (!snap) return false;

    int w = (int)snap->header.w;
    int h = (int)snap->header.h;
    uint32_t stride = snap->header.stride;            // bytes per row in the snapshot
    int row_bytes = w * 2;                            // RGB565 = 2 bytes per pixel
    int row_pad = (4 - (row_bytes % 4)) % 4;          // BMP rows align to 4 bytes
    uint32_t data_size = (uint32_t)(row_bytes + row_pad) * (uint32_t)h;

    File f = SD.open(path, FILE_WRITE);
    if (!f) {
        lv_draw_buf_destroy(snap);
        return false;
    }

    uint8_t hdr[BMP_HEADER_SIZE];
    build_bmp_header(hdr, w, h, data_size);
    f.write(hdr, BMP_HEADER_SIZE);

    // Write one row at a time so we honour the snapshot's `stride` (which
    // may exceed w*2 for alignment) and pad each BMP row out to the 4-byte
    // boundary the format requires.
    const uint8_t *pixels = (const uint8_t *)snap->data;
    static const uint8_t pad[3] = { 0, 0, 0 };
    for (int y = 0; y < h; y++) {
        f.write(pixels + (size_t)y * stride, row_bytes);
        if (row_pad > 0) f.write(pad, row_pad);
    }

    f.close();
    lv_draw_buf_destroy(snap);
    return true;
}

bool screenshot_capture()
{
    // Gate the same way the settings screen does — if USB-SD has the card,
    // or the card simply isn't there, we can't write anything.
    if (usb_sd_is_running() || !instance.isCardReady()) return false;
    if (!ensure_dir()) return false;

    char filename[64];
    make_filename(filename, sizeof(filename));
    return capture_active_to(filename);
}

bool screenshot_capture_named(const char *name)
{
    if (usb_sd_is_running() || !instance.isCardReady()) return false;
    if (!ensure_dir()) return false;

    char path[80];
    snprintf(path, sizeof(path), SCREENSHOT_DIR "/%s.bmp", name);
    return capture_active_to(path);
}

// ---- Long-press poll -------------------------------------------------------
//
// Polled from main.cpp's loop(). Watches the touchscreen indev's state
// directly so we see the press regardless of whether LVGL routes it to a
// gesture, scroll, or click. Once SCREENSHOT_LONG_PRESS_MS elapses we
// fire exactly one capture for this press; the user has to release and
// re-press to take another. This way a held finger doesn't spam the SD.

static uint32_t s_press_started_ms = 0;
static bool     s_pressing         = false;
static bool     s_already_fired    = false;

void screenshot_poll()
{
    // Cheap early-out — the setting is the gating intent, so don't even
    // walk the indev list when the user has it off.
    if (!settings_get_screenshot_long_press()) {
        s_pressing      = false;
        s_already_fired = false;
        return;
    }

    // Self-throttle to ~10 Hz. The main loop runs at ~200 Hz (delay(5))
    // so without this gate we'd walk the indev linked list and read its
    // state 200×/s for a 3 s threshold that's measured in seconds — 30×
    // more samples than we need. 100 ms granularity is still 30× finer
    // than the threshold, so the fired-once edge is detected promptly.
    uint32_t now = millis();
    static uint32_t s_last_poll_ms = 0;
    if (now - s_last_poll_ms < 100) return;
    s_last_poll_ms = now;

    // Find the pointer (touch) indev. There's usually exactly one on this
    // hardware; iterate just in case some other input got registered.
    lv_indev_t *indev = NULL;
    for (lv_indev_t *it = lv_indev_get_next(NULL); it; it = lv_indev_get_next(it)) {
        if (lv_indev_get_type(it) == LV_INDEV_TYPE_POINTER) { indev = it; break; }
    }
    if (!indev) return;

    bool down = (lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED);

    if (down) {
        if (!s_pressing) {
            // Start of a new press — reset the timer and the one-shot guard.
            s_pressing        = true;
            s_already_fired   = false;
            s_press_started_ms = now;
        } else if (!s_already_fired &&
                   (now - s_press_started_ms) >= SCREENSHOT_LONG_PRESS_MS) {
            // Held past the threshold for the first time this press.
            s_already_fired = true;
            if (screenshot_capture()) {
                // Short haptic blip so the user knows the capture happened
                // — otherwise long-press is silent and they might wonder
                // if anything occurred.
                instance.vibrator();
            }
        }
    } else {
        // Released (or never pressed since last poll) — clear so the next
        // touch starts a fresh timer.
        s_pressing      = false;
        s_already_fired = false;
    }
}
