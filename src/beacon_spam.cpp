#include "beacon_spam.h"
#include "offense_wifi.h"
#include "argus_mode.h"
#include "theme.h"
#include <lvgl.h>
#include <string.h>
#include <stdio.h>
#include "esp_random.h"

// Defined in tools_screen.cpp / main.cpp
void tools_screen_show();
void low_mem_show_dialog(const char *msg);

// ---- engine ----------------------------------------------------------------

static const char *SSIDS[] = {
    "FBI Surveillance Van", "Pretty Fly for a WiFi", "Loading...",
    "Mom Use This One", "The LAN Before Time", "Bill Wi the Science Fi",
    "Drop It Like a Hotspot", "Wu-Tang LAN", "It Hurts When IP",
    "Hide Yo Kids Hide Yo WiFi", "Never Gonna Give You WiFi", "Router? I Hardly Know Her",
    "Definitely Not a Trap", "Area 51 Guest", "Silence of the LANs",
    "No Free WiFi Here",
};
static const int SSID_COUNT = sizeof(SSIDS) / sizeof(SSIDS[0]);

static lv_timer_t *s_tx_timer = nullptr;
static uint32_t    s_count    = 0;
static int         s_ssid_i   = 0;
static uint8_t     s_ch       = 1;

// Build one 802.11 beacon into buf; return length. Broadcast DA, random BSSID.
static int build_beacon(uint8_t *buf, const char *ssid, const uint8_t *bssid, uint8_t channel)
{
    int slen = (int)strlen(ssid);
    if (slen > 32) slen = 32;
    int i = 0;
    buf[i++] = 0x80; buf[i++] = 0x00;                 // FC: mgmt / beacon
    buf[i++] = 0x00; buf[i++] = 0x00;                 // duration
    for (int k = 0; k < 6; k++) buf[i++] = 0xFF;      // DA broadcast
    for (int k = 0; k < 6; k++) buf[i++] = bssid[k];  // SA
    for (int k = 0; k < 6; k++) buf[i++] = bssid[k];  // BSSID
    buf[i++] = 0x00; buf[i++] = 0x00;                 // seq
    for (int k = 0; k < 8; k++) buf[i++] = 0x00;      // timestamp
    buf[i++] = 0x64; buf[i++] = 0x00;                 // beacon interval 100 TU
    buf[i++] = 0x01; buf[i++] = 0x04;                 // capability: ESS
    buf[i++] = 0x00; buf[i++] = (uint8_t)slen;        // SSID element
    memcpy(buf + i, ssid, slen); i += slen;
    buf[i++] = 0x01; buf[i++] = 0x08;                 // supported rates
    buf[i++] = 0x82; buf[i++] = 0x84; buf[i++] = 0x8b; buf[i++] = 0x96;
    buf[i++] = 0x24; buf[i++] = 0x30; buf[i++] = 0x48; buf[i++] = 0x6c;
    buf[i++] = 0x03; buf[i++] = 0x01; buf[i++] = channel;   // DS param (channel)
    return i;
}

// How many distinct SSIDs to spray per tick. Sending a BATCH each tick means a
// scanning phone sees several junk networks at once (a beacon roughly every
// ~100ms per SSID) instead of one trickling in.
#define BS_PER_TICK 6

static void on_tx(lv_timer_t *)
{
    // Hop channel round-robin so the junk SSIDs appear on every band.
    s_ch = (uint8_t)((s_ch % 13) + 1);
    offense_wifi_set_channel(s_ch);

    for (int n = 0; n < BS_PER_TICK; n++) {
        uint8_t bssid[6];
        uint32_t r1 = esp_random(), r2 = esp_random();
        bssid[0] = 0x02;                    // locally-administered, unicast
        bssid[1] = (uint8_t)(r1 >> 8);
        bssid[2] = (uint8_t)(r1 >> 16);
        bssid[3] = (uint8_t)(r1 >> 24);
        bssid[4] = (uint8_t)(r2);
        bssid[5] = (uint8_t)(r2 >> 8);

        const char *ssid = SSIDS[s_ssid_i];
        s_ssid_i = (s_ssid_i + 1) % SSID_COUNT;

        uint8_t frame[128];
        int len = build_beacon(frame, ssid, bssid, s_ch);
        if (offense_wifi_tx(frame, (size_t)len)) s_count++;
    }
}

bool beacon_spam_start()
{
    if (s_tx_timer) return true;
    if (!offense_wifi_claim(1, "Beacon flood")) return false;
    s_count  = 0;
    s_ssid_i = 0;
    s_ch     = 1;
    s_tx_timer = lv_timer_create(on_tx, 30, nullptr);   // ~33 frames/s
    return true;
}

void beacon_spam_stop()
{
    if (s_tx_timer) { lv_timer_del(s_tx_timer); s_tx_timer = nullptr; }
    offense_wifi_release();
}

bool beacon_spam_is_running() { return s_tx_timer != nullptr; }
uint32_t beacon_spam_count()  { return s_count; }

// ---- screen ----------------------------------------------------------------

static lv_obj_t *bs_screen;
static lv_obj_t *bs_toggle_lbl;
static lv_obj_t *bs_toggle_btn;
static lv_obj_t *bs_count_lbl;
static lv_timer_t *bs_ui_timer;

static void bs_refresh(lv_timer_t *)
{
    if (!beacon_spam_screen_is_active()) return;
    bool run = beacon_spam_is_running();
    lv_label_set_text(bs_toggle_lbl, run ? "STOP" : "START");
    lv_obj_set_style_bg_color(bs_toggle_btn, run ? HADES_RED : lv_color_make(0x00, 0xAA, 0x44), LV_PART_MAIN);
    char b[40];
    snprintf(b, sizeof(b), "%lu frames", (unsigned long)beacon_spam_count());
    lv_label_set_text(bs_count_lbl, run ? b : "idle");
}

static void bs_on_toggle(lv_event_t *)
{
    if (beacon_spam_is_running()) {
        beacon_spam_stop();
    } else if (!beacon_spam_start()) {
        const char *why = offense_wifi_busy_reason();
        low_mem_show_dialog(why ? why : "Can't start the radio right now.");
        return;
    }
    bs_refresh(nullptr);
}

// Leaving the screen does NOT stop the flood - it keeps running so you can
// navigate; the single-owner guard + the busy dialog handle a second tool.
static void bs_on_gesture(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    if (lv_indev_get_gesture_dir(indev) == LV_DIR_RIGHT) tools_screen_show();
}

void beacon_spam_screen_create()
{
    bs_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(bs_screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(bs_screen, 0, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(bs_screen);
    lv_obj_set_style_text_color(title, ARGUS_OFFENSE_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_argus_label_28, LV_PART_MAIN);
    lv_label_set_text(title, "BEACON FLOOD");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

    lv_obj_t *warn = lv_label_create(bs_screen);
    lv_obj_set_style_text_color(warn, HADES_RED, LV_PART_MAIN);
    lv_obj_set_style_text_font(warn, &font_argus_label_16, LV_PART_MAIN);
    lv_obj_set_style_text_align(warn, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(warn, 380);
    lv_label_set_long_mode(warn, LV_LABEL_LONG_WRAP);
    lv_label_set_text(warn, "AUTHORIZED USE ONLY\nSprays fake SSIDs across all channels.\n"
                            "Transmits RF - use only where you are\nauthorized to do so.");
    lv_obj_align(warn, LV_ALIGN_TOP_MID, 0, 70);

    bs_count_lbl = lv_label_create(bs_screen);
    lv_obj_set_style_text_color(bs_count_lbl, lv_color_make(0x3C, 0xDC, 0x78), LV_PART_MAIN);
    lv_obj_set_style_text_font(bs_count_lbl, &font_argus_label_20, LV_PART_MAIN);
    lv_label_set_text(bs_count_lbl, "idle");
    lv_obj_align(bs_count_lbl, LV_ALIGN_CENTER, 0, 20);

    bs_toggle_btn = lv_button_create(bs_screen);
    lv_obj_set_size(bs_toggle_btn, 200, 70);
    lv_obj_align(bs_toggle_btn, LV_ALIGN_BOTTOM_MID, 0, -30);
    lv_obj_set_style_radius(bs_toggle_btn, 12, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bs_toggle_btn, lv_color_make(0x00, 0xAA, 0x44), LV_PART_MAIN);
    lv_obj_add_event_cb(bs_toggle_btn, bs_on_toggle, LV_EVENT_CLICKED, NULL);
    bs_toggle_lbl = lv_label_create(bs_toggle_btn);
    lv_obj_set_style_text_color(bs_toggle_lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(bs_toggle_lbl, &font_argus_label_28, LV_PART_MAIN);
    lv_label_set_text(bs_toggle_lbl, "START");
    lv_obj_center(bs_toggle_lbl);

    lv_obj_add_event_cb(bs_screen, bs_on_gesture, LV_EVENT_GESTURE, NULL);

    bs_ui_timer = lv_timer_create(bs_refresh, 500, NULL);
    lv_timer_pause(bs_ui_timer);
}

void beacon_spam_screen_show()
{
    if (argus_mode_current() != ArgusMode::Offense) { tools_screen_show(); return; }
    bs_refresh(nullptr);
    lv_timer_resume(bs_ui_timer);
    lv_scr_load(bs_screen);
}

bool beacon_spam_screen_is_active() { return lv_screen_active() == bs_screen; }
