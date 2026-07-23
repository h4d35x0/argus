#include "deauth_screen.h"
#include "detect_pipeline.h"
#include "wifi_beacon_manager.h"   // wifi_beacon_active()
#include "theme.h"
#include <stdio.h>

// Defined in tools_screen.cpp
void tools_screen_show();

static lv_obj_t *deauth_screen;
static lv_obj_t *banner;        // status pill (Clear / Elevated / Under attack)
static lv_obj_t *rate_label;    // big VT323 frames/min
static lv_obj_t *rate_unit;     // "/min" caption
static lv_obj_t *detail_label;  // top attacker BSSID + rate, or the empty-state hint
static lv_timer_t *refresh_timer;

static void refresh(lv_timer_t *)
{
    if (!deauth_screen_is_active()) return;

    DeauthSnapshot s;
    detect_pipeline_deauth_snapshot(&s);
    bool scanning = wifi_beacon_active();

    // Banner: colour + text by posture.
    lv_color_t col; const char *txt;
    if (s.flag >= 2)      { col = HADES_RED;               txt = LV_SYMBOL_WARNING "  UNDER DEAUTH ATTACK"; }
    else if (s.flag == 1) { col = lv_color_make(0xF0,0xA0,0x30); txt = LV_SYMBOL_WARNING "  DEAUTH ELEVATED"; }
    else if (!scanning)   { col = ARGUS_ACCENT_DIM;        txt = "IDLE - no WiFi scan running"; }
    else                  { col = ARGUS_ACCENT;            txt = LV_SYMBOL_OK "  Clear"; }
    lv_obj_set_style_bg_color(banner, col, LV_PART_MAIN);
    lv_label_set_text(lv_obj_get_child(banner, 0), txt);

    char buf[16];
    snprintf(buf, sizeof(buf), "%u", (unsigned)s.global_rate);
    lv_label_set_text(rate_label, buf);
    lv_obj_set_style_text_color(rate_label, s.flag >= 2 ? HADES_RED : ARGUS_TEXT, LV_PART_MAIN);

    if (!scanning) {
        lv_label_set_text(detail_label,
            "Deauth watch is passive: it rides an\n"
            "active WiFi scan. Start Evil Twin, Pwn,\n"
            "Flock or the wardriver to arm it.");
    } else if (s.tracked == 0) {
        lv_label_set_text(detail_label, "No disconnect frames in the air.");
    } else {
        char d[96];
        snprintf(d, sizeof(d),
            "%u AP(s) emitting disconnects\n"
            "worst: %02X:%02X:%02X:%02X:%02X:%02X\n"
            "%u/min   %d dBm",
            (unsigned)s.tracked,
            s.top_bssid[0], s.top_bssid[1], s.top_bssid[2],
            s.top_bssid[3], s.top_bssid[4], s.top_bssid[5],
            (unsigned)s.top_rate, (int)s.top_rssi);
        lv_label_set_text(detail_label, d);
    }
}

static void on_gesture(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    if (lv_indev_get_gesture_dir(indev) == LV_DIR_RIGHT) tools_screen_show();
}

void deauth_screen_create()
{
    deauth_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(deauth_screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(deauth_screen, 0, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(deauth_screen);
    lv_obj_set_style_text_color(title, ARGUS_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_argus_label_28, LV_PART_MAIN);
    lv_label_set_text(title, "DEAUTH WATCH");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);

    banner = lv_obj_create(deauth_screen);
    lv_obj_set_size(banner, 380, 46);
    lv_obj_align(banner, LV_ALIGN_TOP_MID, 0, 62);
    lv_obj_set_style_radius(banner, 10, LV_PART_MAIN);
    lv_obj_set_style_bg_color(banner, ARGUS_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_border_width(banner, 0, LV_PART_MAIN);
    lv_obj_clear_flag(banner, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *blab = lv_label_create(banner);
    lv_obj_set_style_text_color(blab, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_text_font(blab, &font_argus_label_16, LV_PART_MAIN);
    lv_label_set_text(blab, LV_SYMBOL_OK "  Clear");
    lv_obj_center(blab);

    rate_label = lv_label_create(deauth_screen);
    lv_obj_set_style_text_color(rate_label, ARGUS_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(rate_label, &font_argus_mono_48, LV_PART_MAIN);
    lv_label_set_text(rate_label, "0");
    lv_obj_align(rate_label, LV_ALIGN_TOP_MID, 0, 150);

    rate_unit = lv_label_create(deauth_screen);
    lv_obj_set_style_text_color(rate_unit, ARGUS_TEXT_DIM, LV_PART_MAIN);
    lv_obj_set_style_text_font(rate_unit, &font_argus_label_16, LV_PART_MAIN);
    lv_label_set_text(rate_unit, "deauth + disassoc / min");
    lv_obj_align(rate_unit, LV_ALIGN_TOP_MID, 0, 218);

    detail_label = lv_label_create(deauth_screen);
    lv_obj_set_style_text_color(detail_label, ARGUS_TEXT_DIM, LV_PART_MAIN);
    lv_obj_set_style_text_font(detail_label, &font_argus_label_16, LV_PART_MAIN);
    lv_obj_set_style_text_align(detail_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(detail_label, "");
    lv_obj_align(detail_label, LV_ALIGN_TOP_MID, 0, 262);

    lv_obj_add_event_cb(deauth_screen, on_gesture, LV_EVENT_GESTURE, NULL);

    refresh_timer = lv_timer_create(refresh, 1000, NULL);
    lv_timer_pause(refresh_timer);
}

void deauth_screen_show()
{
    refresh(nullptr);
    lv_timer_resume(refresh_timer);
    lv_scr_load(deauth_screen);
}

bool deauth_screen_is_active() { return lv_screen_active() == deauth_screen; }
