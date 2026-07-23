#include "rogue_ap.h"
#include "offense_wifi.h"
#include "argus_mode.h"
#include "theme.h"
#include <lvgl.h>
#include <string.h>
#include <stdio.h>

// Defined in tools_screen.cpp / main.cpp
void tools_screen_show();
void low_mem_show_dialog(const char *msg);

// ---- engine ----------------------------------------------------------------
// Common open SSIDs that phones/laptops routinely auto-join, plus a couple of
// generic lures. An open AP with one of these names catches devices configured
// to reconnect to it.
static const char *SSIDS[] = {
    "xfinitywifi", "XFINITY", "attwifi", "Google Starbucks", "Boingo Hotspot",
    "GuestWiFi", "Guest", "Free WiFi", "Public WiFi", "Airport Free WiFi",
    "Hotel Guest", "Starbucks WiFi", "McDonalds Free WiFi", "eduroam-guest",
    "linksys", "NETGEAR",
};
static const int SSID_COUNT = sizeof(SSIDS) / sizeof(SSIDS[0]);

static bool s_running = false;
static char s_ssid[33] = {0};

bool rogue_ap_start(const char *ssid)
{
    if (s_running) return true;
    if (!offense_wifi_claim_ap(ssid, "Rogue AP")) return false;
    strncpy(s_ssid, ssid ? ssid : "Free WiFi", sizeof(s_ssid) - 1);
    s_ssid[sizeof(s_ssid) - 1] = '\0';
    s_running = true;
    return true;
}

void rogue_ap_stop()
{
    offense_wifi_release();
    s_running = false;
    s_ssid[0] = '\0';
}

bool rogue_ap_is_running() { return s_running; }
const char *rogue_ap_ssid() { return s_ssid; }
int  rogue_ap_clients()     { return offense_wifi_ap_clients(); }

// ---- screen ----------------------------------------------------------------
static lv_obj_t *ra_screen;
static lv_obj_t *ra_list;       // SSID picker (shown when not broadcasting)
static lv_obj_t *ra_status;     // "Broadcasting <ssid> - N clients" (when running)
static lv_obj_t *ra_stop_btn;
static lv_timer_t *ra_ui;

static void ra_start_ssid(const char *ssid)
{
    if (!rogue_ap_start(ssid)) {
        const char *why = offense_wifi_busy_reason();
        low_mem_show_dialog(why ? why : "Can't start the AP right now.");
    }
}

static void ra_build_list()
{
    lv_obj_clean(ra_list);
    for (int i = 0; i < SSID_COUNT; i++) {
        lv_obj_t *row = lv_button_create(ra_list);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, 44);
        lv_obj_set_style_radius(row, 8, LV_PART_MAIN);
        lv_obj_set_style_bg_color(row, lv_color_make(0x22, 0x18, 0x0C), LV_PART_MAIN);
        lv_obj_set_style_border_color(row, ARGUS_OFFENSE_ACCENT, LV_PART_MAIN);
        lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
        lv_obj_set_user_data(row, (void *)SSIDS[i]);
        lv_obj_add_event_cb(row, [](lv_event_t *e) {
            const char *ssid = (const char *)lv_obj_get_user_data((lv_obj_t *)lv_event_get_target(e));
            ra_start_ssid(ssid);
        }, LV_EVENT_CLICKED, NULL);
        lv_obj_t *l = lv_label_create(row);
        lv_obj_set_style_text_font(l, &font_argus_label_16, LV_PART_MAIN);
        lv_obj_set_style_text_color(l, ARGUS_TEXT, LV_PART_MAIN);
        lv_label_set_text(l, SSIDS[i]);
        lv_obj_center(l);
    }
}

static void ra_refresh(lv_timer_t *)
{
    if (!rogue_ap_screen_is_active()) return;
    bool run = rogue_ap_is_running();

    if (run) lv_obj_add_flag(ra_list, LV_OBJ_FLAG_HIDDEN);
    else     lv_obj_clear_flag(ra_list, LV_OBJ_FLAG_HIDDEN);

    if (run) lv_obj_clear_flag(ra_stop_btn, LV_OBJ_FLAG_HIDDEN);
    else     lv_obj_add_flag(ra_stop_btn, LV_OBJ_FLAG_HIDDEN);

    if (run) {
        char b[80];
        snprintf(b, sizeof(b), "Broadcasting:\n%s\n\n%d client(s)", rogue_ap_ssid(), rogue_ap_clients());
        lv_label_set_text(ra_status, b);
        lv_obj_clear_flag(ra_status, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ra_status, LV_OBJ_FLAG_HIDDEN);
    }
}

static void ra_on_stop(lv_event_t *) { rogue_ap_stop(); ra_refresh(nullptr); }

// Leaving keeps the AP up (single-owner guard + busy dialog cover a second tool).
static void ra_on_gesture(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    if (lv_indev_get_gesture_dir(indev) == LV_DIR_RIGHT) tools_screen_show();
}

void rogue_ap_screen_create()
{
    ra_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(ra_screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(ra_screen, 0, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(ra_screen);
    lv_obj_set_style_text_color(title, ARGUS_OFFENSE_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_argus_label_28, LV_PART_MAIN);
    lv_label_set_text(title, "ROGUE AP");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);

    lv_obj_t *warn = lv_label_create(ra_screen);
    lv_obj_set_style_text_color(warn, HADES_RED, LV_PART_MAIN);
    lv_obj_set_style_text_font(warn, &font_argus_label_14, LV_PART_MAIN);
    lv_obj_set_style_text_align(warn, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(warn, 380);
    lv_label_set_long_mode(warn, LV_LABEL_LONG_WRAP);
    lv_label_set_text(warn, "AUTHORIZED USE ONLY - broadcasts a fake open network. Tap an SSID to lure devices.");
    lv_obj_align(warn, LV_ALIGN_TOP_MID, 0, 54);

    ra_list = lv_obj_create(ra_screen);
    lv_obj_set_size(ra_list, 404, 340);
    lv_obj_align(ra_list, LV_ALIGN_TOP_MID, 0, 96);
    lv_obj_set_style_bg_color(ra_list, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(ra_list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ra_list, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_row(ra_list, 6, LV_PART_MAIN);
    lv_obj_set_layout(ra_list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(ra_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(ra_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(ra_list, LV_SCROLLBAR_MODE_AUTO);
    ra_build_list();

    ra_status = lv_label_create(ra_screen);
    lv_obj_set_style_text_color(ra_status, lv_color_make(0x3C, 0xDC, 0x78), LV_PART_MAIN);
    lv_obj_set_style_text_font(ra_status, &font_argus_label_20, LV_PART_MAIN);
    lv_obj_set_style_text_align(ra_status, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(ra_status, "");
    lv_obj_align(ra_status, LV_ALIGN_CENTER, 0, -10);
    lv_obj_add_flag(ra_status, LV_OBJ_FLAG_HIDDEN);

    ra_stop_btn = lv_button_create(ra_screen);
    lv_obj_set_size(ra_stop_btn, 200, 66);
    lv_obj_align(ra_stop_btn, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_radius(ra_stop_btn, 12, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ra_stop_btn, HADES_RED, LV_PART_MAIN);
    lv_obj_add_event_cb(ra_stop_btn, ra_on_stop, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sl = lv_label_create(ra_stop_btn);
    lv_obj_set_style_text_color(sl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(sl, &font_argus_label_28, LV_PART_MAIN);
    lv_label_set_text(sl, "STOP");
    lv_obj_center(sl);
    lv_obj_add_flag(ra_stop_btn, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_event_cb(ra_screen, ra_on_gesture, LV_EVENT_GESTURE, NULL);

    ra_ui = lv_timer_create(ra_refresh, 700, NULL);
    lv_timer_pause(ra_ui);
}

void rogue_ap_screen_show()
{
    if (argus_mode_current() != ArgusMode::Offense) { tools_screen_show(); return; }
    ra_refresh(nullptr);
    lv_timer_resume(ra_ui);
    lv_scr_load(ra_screen);
}

bool rogue_ap_screen_is_active() { return lv_screen_active() == ra_screen; }
