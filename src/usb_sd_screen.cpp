#include "usb_sd_screen.h"
#include "theme.h"
#include "usb_sd.h"
#include <LilyGoLib.h>
#include <SD.h>
#include <stdio.h>

// Defined in tools_screen.cpp
void tools_screen_show();

static lv_obj_t *usb_sd_screen;
static lv_obj_t *status_label;
static lv_obj_t *card_label;
static lv_obj_t *usage_label;
static lv_obj_t *start_btn;
static lv_obj_t *start_label;

// Used-bytes is the only expensive read here — it walks the entire FAT to
// count occupied clusters (~30–100 ms on FAT32, faster on exFAT). We cache
// the last computed value and only refresh on screen-open and after the
// USB mount cycles, never on the 500 ms screen-refresh timer. That keeps
// the contention off the shared SD/SPI bus during normal background
// logging by the other detectors.
static uint64_t s_used_bytes  = 0;
static uint64_t s_total_bytes = 0;
static bool     s_usage_known = false;

// ---- refresh ---------------------------------------------------------------

static void update_card_label()
{
    uint64_t bytes = usb_sd_card_bytes();
    if (bytes == 0) {
        lv_label_set_text(card_label, "Card: none");
        return;
    }
    // Report in GB with one decimal, the way card capacities are advertised.
    double gb = (double)bytes / 1000000000.0;
    char buf[32];
    snprintf(buf, sizeof(buf), "Card: %.1f GB", gb);
    lv_label_set_text(card_label, buf);
}

// Display the cached used-bytes figure. The value comes from
// recompute_usage() — this just renders whatever's in the cache.
//
// Unit auto-scales (KB / MB / GB) so a tiny usage like 4 MB of detector
// logs on a 32 GB card doesn't truncate to "0.00 GB". Percent shows
// "<1%" instead of "0%" whenever there's *any* non-zero usage, for the
// same reason — 4 MB / 32 GB rounds to 0 % otherwise.
static void update_usage_label()
{
    if (usb_sd_card_bytes() == 0) {
        // No card → nothing meaningful to show.
        lv_label_set_text(usage_label, "Usage: --");
        return;
    }
    if (!s_usage_known || s_total_bytes == 0) {
        lv_label_set_text(usage_label, "Usage: ...");
        return;
    }

    // Pick the right unit so the visible digits are meaningful.
    char size_buf[24];
    if (s_used_bytes == 0) {
        snprintf(size_buf, sizeof(size_buf), "empty");
    } else if (s_used_bytes < 1024ULL * 1024) {
        snprintf(size_buf, sizeof(size_buf), "%llu KB",
                 (unsigned long long)(s_used_bytes / 1024ULL));
    } else if (s_used_bytes < 1000ULL * 1000 * 1000) {
        snprintf(size_buf, sizeof(size_buf), "%llu MB",
                 (unsigned long long)(s_used_bytes / (1000ULL * 1000)));
    } else {
        double used_gb = (double)s_used_bytes / 1000000000.0;
        snprintf(size_buf, sizeof(size_buf), "%.2f GB", used_gb);
    }

    // Percentage — guard against div-by-zero (s_total_bytes > 0 here per
    // the early-return above) and show "<1%" for any non-zero usage that
    // rounds to zero, so the user can tell "there's some data" apart
    // from "there's nothing".
    char pct_buf[12];
    if (s_used_bytes == 0) {
        snprintf(pct_buf, sizeof(pct_buf), "0%%");
    } else {
        double ratio = 100.0 * (double)s_used_bytes / (double)s_total_bytes;
        int pct = (int)(ratio + 0.5);
        if (pct == 0) snprintf(pct_buf, sizeof(pct_buf), "<1%%");
        else          snprintf(pct_buf, sizeof(pct_buf), "%d%%", pct);
    }

    char buf[48];
    snprintf(buf, sizeof(buf), "Usage: %s  (%s)", size_buf, pct_buf);
    lv_label_set_text(usage_label, buf);
}

// Walk the FAT once to refresh the cached used/total bytes. Skips the work
// when the card is unmounted OR currently exposed over USB MSC (host owns
// the FAT in the latter case — querying it would race with host I/O).
static void recompute_usage()
{
    if (usb_sd_is_running() || !instance.isCardReady() ||
        usb_sd_card_bytes() == 0) {
        return;                  // leave the cache alone
    }
    s_total_bytes = SD.totalBytes();
    s_used_bytes  = SD.usedBytes();
    s_usage_known = true;
    // Trace to USB-CDC serial so we can sanity-check the underlying
    // numbers if the on-screen display ever looks suspect.
    Serial.printf("[USB SD] usage: total=%llu used=%llu\n",
        (unsigned long long)s_total_bytes,
        (unsigned long long)s_used_bytes);
}

static void update_start_button()
{
    bool running = usb_sd_is_running();
    lv_label_set_text(start_label, running ? "UNMOUNT" : "MOUNT");
    lv_obj_set_style_bg_color(start_btn,
        running ? lv_color_make(0xCC, 0x00, 0x00)
                : lv_color_make(0x00, 0xAA, 0x44),
        LV_PART_MAIN);
}

static void update_status()
{
    if (!usb_sd_is_running()) {
        if (usb_sd_card_bytes() == 0) {
            lv_label_set_text(status_label, "No SD card inserted");
            lv_obj_set_style_text_color(status_label,
                lv_color_make(0xFF, 0x55, 0x55), LV_PART_MAIN);
        } else {
            lv_label_set_text(status_label, "Ready - press MOUNT");
            lv_obj_set_style_text_color(status_label,
                lv_color_make(0x88, 0x88, 0x88), LV_PART_MAIN);
        }
    } else if (usb_sd_host_active()) {
        lv_label_set_text(status_label, "Host active - do not unplug");
        lv_obj_set_style_text_color(status_label,
            ARGUS_ACCENT, LV_PART_MAIN);
    } else {
        lv_label_set_text(status_label, "Mounted - open the drive on your PC");
        lv_obj_set_style_text_color(status_label,
            lv_color_make(0xFF, 0xCC, 0x00), LV_PART_MAIN);
    }
}

static void refresh()
{
    update_card_label();
    update_usage_label();   // paints the cached value; no FAT scan here
    update_start_button();
    update_status();
}

static void on_refresh(lv_timer_t *)
{
    // Pure UI tick — no SD activity depends on it (USB-SD lifecycle is
    // edge-driven). Skip whenever the user isn't on this screen.
    if (!usb_sd_screen_is_active()) return;
    refresh();
}

// ---- event handlers --------------------------------------------------------

static void on_start_stop(lv_event_t *)
{
    bool was_running = usb_sd_is_running();
    if (was_running)
        usb_sd_stop();
    else
        usb_sd_start();   // no-ops + status shows "No SD card" if none present

    // After an unmount the host may have written or deleted files — the
    // cached usage figure is now stale. Re-scan the FAT once (the only
    // place this expensive call happens outside of screen-open).
    if (was_running) recompute_usage();

    refresh();
}

// BACK leaves USB SD running if it was mounted — the drive stays available to
// the host while you use other watch screens. Stop it explicitly with UNMOUNT.

// ---- layout ----------------------------------------------------------------

static lv_obj_t *make_button(lv_obj_t *parent, const char *text,
                             lv_coord_t w, lv_coord_t h, lv_color_t bg)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_radius(btn, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_center(lbl);
    return btn;
}

void usb_sd_screen_create()
{
    usb_sd_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(usb_sd_screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(usb_sd_screen, 0, LV_PART_MAIN);

    // Title — font_48 to match the PAGER / TPMS / SETTINGS headers.
    lv_obj_t *title = lv_label_create(usb_sd_screen);
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_label_set_text(title, "USB SD");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

    // Each non-title row sits 20 px lower than before to clear the font_48
    // header (which extends to ~y=60 from its y=12 origin) without touching
    // the next row.

    // Status line
    status_label = lv_label_create(usb_sd_screen);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(status_label, lv_color_make(0x88, 0x88, 0x88), LV_PART_MAIN);
    lv_label_set_text(status_label, "Ready");
    lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 84);

    // Card capacity
    card_label = lv_label_create(usb_sd_screen);
    lv_obj_set_style_text_font(card_label, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(card_label, lv_color_make(0xCC, 0xCC, 0xCC), LV_PART_MAIN);
    lv_label_set_text(card_label, "Card: none");
    lv_obj_align(card_label, LV_ALIGN_TOP_MID, 0, 116);

    // Usage — bytes currently in use, shown immediately below the card
    // size. Pulled from the cached value the on_refresh tick never
    // recomputes (only screen-open + after-unmount do, see
    // recompute_usage()), so this row has zero ongoing SD-I/O cost.
    usage_label = lv_label_create(usb_sd_screen);
    lv_obj_set_style_text_font(usage_label, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(usage_label, lv_color_make(0xAA, 0xAA, 0xAA), LV_PART_MAIN);
    lv_label_set_text(usage_label, "Usage: --");
    lv_obj_align(usage_label, LV_ALIGN_TOP_MID, 0, 144);

    // Mount / Unmount button (shifted down to make room for the usage row)
    start_btn = make_button(usb_sd_screen, "MOUNT", 220, 64,
                            lv_color_make(0x00, 0xAA, 0x44));
    lv_obj_align(start_btn, LV_ALIGN_TOP_MID, 0, 188);
    start_label = lv_obj_get_child(start_btn, 0);
    lv_obj_add_event_cb(start_btn, on_start_stop, LV_EVENT_CLICKED, NULL);

    // Explanatory note
    lv_obj_t *note = lv_label_create(usb_sd_screen);
    lv_obj_set_style_text_font(note, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(note, lv_color_make(0x77, 0x77, 0x77), LV_PART_MAIN);
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(note, 360);
    lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(note,
        "While mounted, the microSD card appears as a USB drive on a "
        "computer connected to the USB-C port.\n\n"
        "SD-card logging (wardriver, AirTag, Flock) is paused while mounted "
        "to keep the filesystem consistent.\n\n"
        "Press UNMOUNT (or eject from the computer) before unplugging.");
    lv_obj_align(note, LV_ALIGN_TOP_MID, 0, 278);

    lv_timer_create(on_refresh, 500, NULL);
}

void usb_sd_screen_show()
{
    // One FAT scan per screen-open — the user is actively looking at this
    // info, so it's worth paying ~50 ms once to give them a fresh value.
    recompute_usage();
    refresh();
    lv_scr_load(usb_sd_screen);
}

bool usb_sd_screen_is_active()
{
    return lv_screen_active() == usb_sd_screen;
}
