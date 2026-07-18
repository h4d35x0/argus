#include "alarm_screen.h"
#include "theme.h"
#include "alarm.h"
#include <LilyGoLib.h>
#include <time.h>
#include <stdio.h>

// Defined elsewhere.
void tools_screen_show();
void clock_screen_show();
void clock_screen_get_local_time(struct tm *out);
bool clock_screen_uses_12h();    // true when face is analog or digital-12h

// ---- settings screen widgets ----------------------------------------------

static lv_obj_t *settings_scr;
static lv_obj_t *current_time_label;
static lv_obj_t *enable_switch;
static lv_obj_t *hour_roller;
static lv_obj_t *minute_roller;
static lv_obj_t *colon_label;        // the ":" between the rollers
static lv_obj_t *am_btn,  *am_label;
static lv_obj_t *pm_btn,  *pm_label;
// Shown in place of the rollers when the alarm is enabled — the alarm time
// stops being editable until the user disables it again.
static lv_obj_t *big_time_label;

// Alert-behaviour settings (lower portion of the scrolling settings screen)
static lv_obj_t *vibrate_switch;
static lv_obj_t *audio_switch;
static lv_obj_t *volume_label;
static lv_obj_t *volume_slider;
static lv_obj_t *snooze_dropdown;

static bool s_ui_is_pm = false;     // tracks the AM/PM toggle (12h mode only)

// ---- ringing overlay widgets ----------------------------------------------

static lv_obj_t *ring_scr;
static lv_obj_t *ring_time_label;
static lv_obj_t *snooze_btn_label;   // updated to "SNOOZE <N> min" on show

// ---- shared button helper -------------------------------------------------

static lv_obj_t *make_button(lv_obj_t *parent, const char *text,
                             lv_coord_t w, lv_coord_t h,
                             lv_color_t bg, const lv_font_t *font,
                             lv_obj_t **label_out = nullptr)
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

    lv_obj_t *l = lv_label_create(btn);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(l, font ? font : &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_center(l);
    if (label_out) *label_out = l;
    return btn;
}

// Builds "00\n01\n…\nNN" into buf for an lv_roller.
static void build_num_opts(char *buf, size_t bufsz, int lo, int hi)
{
    size_t pos = 0;
    buf[0] = '\0';
    for (int v = lo; v <= hi && pos < bufsz; v++) {
        int n = snprintf(buf + pos, bufsz - pos, "%02d", v);
        if (n < 0) break;
        pos += (size_t)n;
        if (v < hi && pos < bufsz - 1) buf[pos++] = '\n';
    }
    buf[pos < bufsz ? pos : bufsz - 1] = '\0';
}

// Format an alarm time for display, respecting the watch's 12h / 24h mode.
static void format_alarm_time(char *buf, size_t bufsz, int hour24, int minute)
{
    if (clock_screen_uses_12h()) {
        int h12 = hour24 % 12;
        if (h12 == 0) h12 = 12;
        snprintf(buf, bufsz, "%d:%02d %s",
                 h12, minute, hour24 < 12 ? "AM" : "PM");
    } else {
        snprintf(buf, bufsz, "%02d:%02d", hour24, minute);
    }
}

// ---- settings screen logic -------------------------------------------------

// Repaints the AM/PM segmented control to reflect s_ui_is_pm.
static void apply_am_pm_visual()
{
    lv_color_t active   = lv_color_make(0xCC, 0x77, 0x00);
    lv_color_t inactive = lv_color_make(0x33, 0x33, 0x33);
    lv_obj_set_style_bg_color(am_btn, s_ui_is_pm ? inactive : active, LV_PART_MAIN);
    lv_obj_set_style_bg_color(pm_btn, s_ui_is_pm ? active   : inactive, LV_PART_MAIN);
}

// Reads the rollers + the AM/PM toggle as a single 24h hour + minute.
static void get_displayed_time(int *h_out, int *m_out)
{
    int roller_sel = (int)lv_roller_get_selected(hour_roller);
    int h24;
    if (clock_screen_uses_12h()) {
        int h12 = roller_sel + 1;                 // roller index 0..11 -> hour 1..12
        if (s_ui_is_pm) h24 = (h12 == 12) ? 12 : (h12 + 12);
        else            h24 = (h12 == 12) ?  0 :  h12;
    } else {
        h24 = roller_sel;
    }
    if (h_out) *h_out = h24;
    if (m_out) *m_out = (int)lv_roller_get_selected(minute_roller);
}

// Positions the rollers + AM/PM toggle to display the given 24h time.
static void set_rollers_visual(int hour24, int minute)
{
    int roller_sel;
    if (clock_screen_uses_12h()) {
        s_ui_is_pm = (hour24 >= 12);
        int h12 = hour24 % 12;
        if (h12 == 0) h12 = 12;
        roller_sel = h12 - 1;                     // 1..12 -> 0..11
        apply_am_pm_visual();
    } else {
        s_ui_is_pm = false;
        roller_sel = hour24;
    }
    lv_roller_set_selected(hour_roller,   (uint32_t)roller_sel, LV_ANIM_OFF);
    lv_roller_set_selected(minute_roller, (uint32_t)minute,     LV_ANIM_OFF);
}

// When the alarm is enabled, the time is locked in — we hide the rollers
// (and colon + AM/PM control) and put a large read-only display in their
// place. When disabled, the editors come back so the user can pick a new
// time.
static void apply_editing_mode()
{
    bool enabled = alarm_is_enabled();
    bool use_12h = clock_screen_uses_12h();

    if (enabled) {
        lv_obj_add_flag(hour_roller,   LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(minute_roller, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(colon_label,   LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(am_btn,        LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(pm_btn,        LV_OBJ_FLAG_HIDDEN);

        // While a snooze is pending, alarm_get_next_fire_time() returns the
        // snooze deadline; otherwise the saved alarm time. The user opening
        // the screen after hitting SNOOZE sees the projected fire time.
        int next_h, next_m;
        alarm_get_next_fire_time(&next_h, &next_m);
        char buf[16];
        format_alarm_time(buf, sizeof(buf), next_h, next_m);
        lv_label_set_text(big_time_label, buf);
        lv_obj_clear_flag(big_time_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(hour_roller,   LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(minute_roller, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(colon_label,   LV_OBJ_FLAG_HIDDEN);
        if (use_12h) {
            lv_obj_clear_flag(am_btn, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(pm_btn, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(am_btn, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(pm_btn, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_add_flag(big_time_label, LV_OBJ_FLAG_HIDDEN);
    }
}

// Commits whatever the rollers / AM/PM are currently showing. Used for the
// roller-scroll and AM/PM-tap events.
static void commit_from_editors()
{
    int h24, m;
    get_displayed_time(&h24, &m);
    bool enabled = lv_obj_has_state(enable_switch, LV_STATE_CHECKED);
    alarm_set(h24, m, enabled);
}

static void on_value_changed(lv_event_t *) { commit_from_editors(); }

static void on_am(lv_event_t *) { s_ui_is_pm = false; apply_am_pm_visual(); commit_from_editors(); }
static void on_pm(lv_event_t *) { s_ui_is_pm = true;  apply_am_pm_visual(); commit_from_editors(); }

// Turning the alarm ON commits whatever the rollers currently show and hides
// the editors. Turning it OFF preserves the alarm time that just got disabled
// — the rollers re-appear pre-set to that value so the user can re-enable
// with one tap, or adjust from there.
static void on_enable_changed(lv_event_t *)
{
    bool enabled = lv_obj_has_state(enable_switch, LV_STATE_CHECKED);
    if (enabled) {
        commit_from_editors();
    } else {
        int h24 = alarm_get_hour();
        int m   = alarm_get_minute();
        set_rollers_visual(h24, m);
        alarm_set(h24, m, false);   // same time, just flip enabled off
    }
    apply_editing_mode();
}

// Live current-time updater. Runs every second; only repaints when the
// settings screen is the active LVGL screen.
static void update_current_time()
{
    struct tm t;
    clock_screen_get_local_time(&t);

    char buf[24];
    if (clock_screen_uses_12h()) {
        int h12 = t.tm_hour % 12;
        if (h12 == 0) h12 = 12;
        snprintf(buf, sizeof(buf), "Now %d:%02d %s",
                 h12, t.tm_min, t.tm_hour < 12 ? "AM" : "PM");
    } else {
        snprintf(buf, sizeof(buf), "Now %02d:%02d", t.tm_hour, t.tm_min);
    }
    lv_label_set_text(current_time_label, buf);
}

// Live updater for the ringing screen — the time displayed is the current
// RTC clock, ticking forward while the alarm is going off rather than the
// fire time itself. Format follows the watch's 12h/24h preference.
static void update_ring_time()
{
    struct tm t;
    clock_screen_get_local_time(&t);
    char buf[16];
    format_alarm_time(buf, sizeof(buf), t.tm_hour, t.tm_min);
    lv_label_set_text(ring_time_label, buf);
}

static void on_time_tick(lv_timer_t *)
{
    lv_obj_t *active = lv_screen_active();
    if      (active == settings_scr) update_current_time();
    else if (active == ring_scr)     update_ring_time();
}

// ---- settings-row event handlers ------------------------------------------

// Hides/shows the Volume label + slider based on the Audio toggle — turning
// audio off there's nothing meaningful to set a volume for, so the control
// disappears entirely instead of being grayed.
static void apply_audio_visibility()
{
    if (alarm_get_audio()) {
        lv_obj_clear_flag(volume_label,  LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(volume_slider, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(volume_label,  LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(volume_slider, LV_OBJ_FLAG_HIDDEN);
    }
}

static void on_vibrate_changed(lv_event_t *)
{
    alarm_set_vibrate(lv_obj_has_state(vibrate_switch, LV_STATE_CHECKED));
}

static void on_audio_changed(lv_event_t *)
{
    alarm_set_audio(lv_obj_has_state(audio_switch, LV_STATE_CHECKED));
    apply_audio_visibility();
}

static void on_volume_changed(lv_event_t *)
{
    int v = (int)lv_slider_get_value(volume_slider);
    alarm_set_volume((uint8_t)v);
    lv_label_set_text_fmt(volume_label, "Volume  %d%%", v);
}

static void on_snooze_changed(lv_event_t *)
{
    uint32_t sel = lv_dropdown_get_selected(snooze_dropdown);
    int mins = (sel == 0) ? 5 : (sel == 1) ? 10 : 20;
    alarm_set_snooze_minutes(mins);
}

// ---- ringing overlay logic -------------------------------------------------

static void on_dismiss(lv_event_t *)
{
    alarm_dismiss();
    clock_screen_show();
}

static void on_snooze(lv_event_t *)
{
    alarm_snooze(alarm_get_snooze_minutes());
    clock_screen_show();
}

// ---- layout ----------------------------------------------------------------

static void create_settings_screen()
{
    settings_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(settings_scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(settings_scr, 0, LV_PART_MAIN);
    // Settings section below the time picker pushes total content height
    // beyond the visible viewport, so the screen scrolls vertically. The
    // BACK button scrolls with the rest (same pattern as settings_screen.cpp).
    lv_obj_set_scroll_dir(settings_scr, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(settings_scr, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_pad_all(settings_scr, 0, LV_PART_MAIN);


    lv_obj_t *title = lv_label_create(settings_scr);
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_label_set_text(title, "ALARM");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    // Live "Now ..." display so the user can compare against the alarm time.
    current_time_label = lv_label_create(settings_scr);
    lv_obj_set_style_text_color(current_time_label, lv_color_make(0xCC, 0xCC, 0xCC), LV_PART_MAIN);
    lv_obj_set_style_text_font(current_time_label, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_label_set_text(current_time_label, "Now --:--");
    lv_obj_align(current_time_label, LV_ALIGN_TOP_MID, 0, 58);

    // Enable toggle row
    lv_obj_t *enable_lbl = lv_label_create(settings_scr);
    lv_obj_set_style_text_color(enable_lbl, lv_color_make(0xAA, 0xAA, 0xAA), LV_PART_MAIN);
    lv_obj_set_style_text_font(enable_lbl, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_label_set_text(enable_lbl, "Enabled");
    lv_obj_align(enable_lbl, LV_ALIGN_TOP_LEFT, 30, 120);

    enable_switch = lv_switch_create(settings_scr);
    lv_obj_set_size(enable_switch, 80, 40);
    lv_obj_set_style_bg_color(enable_switch, lv_color_make(0x44, 0x44, 0x44), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(enable_switch, ARGUS_ACCENT, LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_align(enable_switch, LV_ALIGN_TOP_RIGHT, -30, 115);
    lv_obj_add_event_cb(enable_switch, on_enable_changed, LV_EVENT_VALUE_CHANGED, NULL);

    // Hour + minute rollers. Options are filled in alarm_screen_show()
    // because the hour range depends on the watch's 12h / 24h mode.
    auto style_roller = [](lv_obj_t *r) {
        lv_obj_set_style_text_font(r, &lv_font_montserrat_28, LV_PART_MAIN);
        lv_obj_set_style_bg_color(r, lv_color_make(0x22, 0x22, 0x22), LV_PART_MAIN);
        lv_obj_set_style_text_color(r, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_border_color(r, lv_color_make(0x55, 0x55, 0x55), LV_PART_MAIN);
        lv_obj_set_style_border_width(r, 1, LV_PART_MAIN);
        lv_obj_set_style_bg_color(r, lv_color_make(0x00, 0x55, 0x33), LV_PART_SELECTED);
        lv_obj_set_style_text_color(r, lv_color_white(), LV_PART_SELECTED);
    };

    hour_roller = lv_roller_create(settings_scr);
    lv_roller_set_visible_row_count(hour_roller, 3);
    lv_obj_set_width(hour_roller, 110);
    style_roller(hour_roller);
    lv_obj_align(hour_roller, LV_ALIGN_TOP_MID, -70, 180);
    lv_obj_add_event_cb(hour_roller, on_value_changed, LV_EVENT_VALUE_CHANGED, NULL);

    // Big colon between rollers
    colon_label = lv_label_create(settings_scr);
    lv_obj_set_style_text_color(colon_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(colon_label, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_label_set_text(colon_label, ":");
    lv_obj_align(colon_label, LV_ALIGN_TOP_MID, 0, 220);

    minute_roller = lv_roller_create(settings_scr);
    lv_roller_set_visible_row_count(minute_roller, 3);
    lv_obj_set_width(minute_roller, 110);
    style_roller(minute_roller);
    lv_obj_align(minute_roller, LV_ALIGN_TOP_MID, 70, 180);
    lv_obj_add_event_cb(minute_roller, on_value_changed, LV_EVENT_VALUE_CHANGED, NULL);

    // AM/PM segmented control (visible only in 12h mode + disabled)
    am_btn = make_button(settings_scr, "AM", 100, 44,
                         lv_color_make(0x33, 0x33, 0x33),
                         &lv_font_montserrat_20, &am_label);
    lv_obj_align(am_btn, LV_ALIGN_TOP_MID, -60, 304);
    lv_obj_add_event_cb(am_btn, on_am, LV_EVENT_CLICKED, NULL);

    pm_btn = make_button(settings_scr, "PM", 100, 44,
                         lv_color_make(0x33, 0x33, 0x33),
                         &lv_font_montserrat_20, &pm_label);
    lv_obj_align(pm_btn, LV_ALIGN_TOP_MID, 60, 304);
    lv_obj_add_event_cb(pm_btn, on_pm, LV_EVENT_CLICKED, NULL);

    // Big locked-in time display — shown in place of the rollers when the
    // alarm is enabled.
    big_time_label = lv_label_create(settings_scr);
    lv_obj_set_style_text_color(big_time_label, ARGUS_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_text_font(big_time_label, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_label_set_text(big_time_label, "00:00");
    lv_obj_align(big_time_label, LV_ALIGN_TOP_MID, 0, 215);
    lv_obj_add_flag(big_time_label, LV_OBJ_FLAG_HIDDEN);

    // ---- Settings section (alert behaviour) ----
    //
    // Lives below the AM/PM row at y ~360+. The screen scrolls to reveal it
    // when the time-picker area at the top is in view. Each row is built
    // directly on settings_scr at a fixed Y so the layout is easy to read
    // here and matches the row spacing used by the time-picker controls.

    // Section header so the visual separation between time-picker and
    // alert-behaviour controls is obvious when scrolled to.
    lv_obj_t *section_hdr = lv_label_create(settings_scr);
    lv_obj_set_style_text_color(section_hdr, lv_color_make(0x88, 0x88, 0x88), LV_PART_MAIN);
    lv_obj_set_style_text_font(section_hdr, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_label_set_text(section_hdr, "ALERT");
    lv_obj_align(section_hdr, LV_ALIGN_TOP_MID, 0, 362);

    // Vibration toggle
    lv_obj_t *vib_lbl = lv_label_create(settings_scr);
    lv_obj_set_style_text_color(vib_lbl, lv_color_make(0xAA, 0xAA, 0xAA), LV_PART_MAIN);
    lv_obj_set_style_text_font(vib_lbl, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_label_set_text(vib_lbl, "Vibration");
    lv_obj_align(vib_lbl, LV_ALIGN_TOP_LEFT, 30, 408);

    vibrate_switch = lv_switch_create(settings_scr);
    lv_obj_set_size(vibrate_switch, 80, 40);
    lv_obj_set_style_bg_color(vibrate_switch, lv_color_make(0x44, 0x44, 0x44), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(vibrate_switch, ARGUS_ACCENT, LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_align(vibrate_switch, LV_ALIGN_TOP_RIGHT, -30, 403);
    lv_obj_add_event_cb(vibrate_switch, on_vibrate_changed, LV_EVENT_VALUE_CHANGED, NULL);

    // Audio toggle
    lv_obj_t *aud_lbl = lv_label_create(settings_scr);
    lv_obj_set_style_text_color(aud_lbl, lv_color_make(0xAA, 0xAA, 0xAA), LV_PART_MAIN);
    lv_obj_set_style_text_font(aud_lbl, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_label_set_text(aud_lbl, "Audio");
    lv_obj_align(aud_lbl, LV_ALIGN_TOP_LEFT, 30, 462);

    audio_switch = lv_switch_create(settings_scr);
    lv_obj_set_size(audio_switch, 80, 40);
    lv_obj_set_style_bg_color(audio_switch, lv_color_make(0x44, 0x44, 0x44), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(audio_switch, ARGUS_ACCENT, LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_align(audio_switch, LV_ALIGN_TOP_RIGHT, -30, 457);
    lv_obj_add_event_cb(audio_switch, on_audio_changed, LV_EVENT_VALUE_CHANGED, NULL);

    // Volume slider — label on the left shows "Volume  NN%", drag bar below.
    // Both are hidden when the Audio toggle is off (see apply_audio_visibility).
    volume_label = lv_label_create(settings_scr);
    lv_obj_set_style_text_color(volume_label, lv_color_make(0xAA, 0xAA, 0xAA), LV_PART_MAIN);
    lv_obj_set_style_text_font(volume_label, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_label_set_text(volume_label, "Volume  100%");
    lv_obj_align(volume_label, LV_ALIGN_TOP_LEFT, 30, 515);

    volume_slider = lv_slider_create(settings_scr);
    lv_obj_set_size(volume_slider, 320, 30);
    lv_slider_set_range(volume_slider, 0, 100);
    lv_slider_set_value(volume_slider, 100, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(volume_slider, lv_color_make(0x44, 0x44, 0x44), LV_PART_MAIN);
    lv_obj_set_style_radius(volume_slider, 8, LV_PART_MAIN);
    lv_obj_set_style_border_width(volume_slider, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(volume_slider, ARGUS_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_radius(volume_slider, 8, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(volume_slider, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_radius(volume_slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_pad_all(volume_slider, 10, LV_PART_KNOB);
    lv_obj_set_style_border_width(volume_slider, 0, LV_PART_KNOB);
    lv_obj_align(volume_slider, LV_ALIGN_TOP_MID, 0, 552);
    lv_obj_add_event_cb(volume_slider, on_volume_changed, LV_EVENT_VALUE_CHANGED, NULL);

    // Snooze duration dropdown
    lv_obj_t *snz_lbl = lv_label_create(settings_scr);
    lv_obj_set_style_text_color(snz_lbl, lv_color_make(0xAA, 0xAA, 0xAA), LV_PART_MAIN);
    lv_obj_set_style_text_font(snz_lbl, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_label_set_text(snz_lbl, "Snooze time");
    lv_obj_align(snz_lbl, LV_ALIGN_TOP_LEFT, 30, 610);

    snooze_dropdown = lv_dropdown_create(settings_scr);
    lv_dropdown_set_options(snooze_dropdown, "5 mins\n10 mins\n20 mins");
    lv_dropdown_set_selected(snooze_dropdown, 0);
    lv_obj_set_size(snooze_dropdown, 160, 40);
    lv_obj_set_style_text_font(snooze_dropdown, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_bg_color(snooze_dropdown, lv_color_make(0x22, 0x22, 0x22), LV_PART_MAIN);
    lv_obj_set_style_text_color(snooze_dropdown, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_border_color(snooze_dropdown, lv_color_make(0x55, 0x55, 0x55), LV_PART_MAIN);
    lv_obj_set_style_border_width(snooze_dropdown, 1, LV_PART_MAIN);
    lv_obj_align(snooze_dropdown, LV_ALIGN_TOP_RIGHT, -30, 605);
    // Match the popup list styling to the closed dropdown so the dark-mode
    // theme stays consistent when the user taps it open.
    lv_obj_t *dd_list = lv_dropdown_get_list(snooze_dropdown);
    lv_obj_set_style_bg_color(dd_list, lv_color_make(0x22, 0x22, 0x22), LV_PART_MAIN);
    lv_obj_set_style_text_color(dd_list, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(dd_list, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_border_color(dd_list, lv_color_make(0x55, 0x55, 0x55), LV_PART_MAIN);
    lv_obj_add_event_cb(snooze_dropdown, on_snooze_changed, LV_EVENT_VALUE_CHANGED, NULL);

    // Bottom hint — lives below the snooze row so it doubles as a visual end
    // marker for the scrollable content.
    lv_obj_t *hint = lv_label_create(settings_scr);
    lv_obj_set_style_text_color(hint, lv_color_make(0x77, 0x77, 0x77), LV_PART_MAIN);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_label_set_text(hint, "rings once a day at the chosen time");
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 670);
}

static void create_ringing_screen()
{
    ring_scr = lv_obj_create(NULL);
    // Dark red background so the alert screen is unmistakable
    lv_obj_set_style_bg_color(ring_scr, lv_color_make(0x22, 0x00, 0x00), LV_PART_MAIN);
    lv_obj_set_style_border_width(ring_scr, 0, LV_PART_MAIN);
    lv_obj_clear_flag(ring_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(ring_scr, 0, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(ring_scr);
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_label_set_text(title, "ALARM");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 60);

    ring_time_label = lv_label_create(ring_scr);
    lv_obj_set_style_text_color(ring_time_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(ring_time_label, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_label_set_text(ring_time_label, "00:00");
    lv_obj_align(ring_time_label, LV_ALIGN_CENTER, 0, -20);

    lv_obj_t *snooze = make_button(ring_scr, "SNOOZE 5 min", 290, 70,
                                   lv_color_make(0xCC, 0x77, 0x00),
                                   &lv_font_montserrat_28, &snooze_btn_label);
    lv_obj_align(snooze, LV_ALIGN_BOTTOM_MID, 0, -110);
    lv_obj_add_event_cb(snooze, on_snooze, LV_EVENT_CLICKED, NULL);

    lv_obj_t *dismiss = make_button(ring_scr, "DISMISS", 290, 70,
                                    lv_color_make(0xCC, 0x22, 0x22),
                                    &lv_font_montserrat_28);
    lv_obj_align(dismiss, LV_ALIGN_BOTTOM_MID, 0, -30);
    lv_obj_add_event_cb(dismiss, on_dismiss, LV_EVENT_CLICKED, NULL);
}

void alarm_screen_create()
{
    create_settings_screen();
    create_ringing_screen();
    // 1 s timer keeps the "Now …" label live while the settings screen is up.
    lv_timer_create(on_time_tick, 1000, NULL);
}

void alarm_screen_show()
{
    bool use_12h = clock_screen_uses_12h();

    // Refresh roller options for the current mode. lv_roller_set_options
    // copies the string so the static buffers can be local in scope.
    static char hour_opts_12[12 * 3 + 1];
    static char hour_opts_24[24 * 3 + 1];
    static char min_opts[60 * 3 + 1];
    build_num_opts(hour_opts_12, sizeof(hour_opts_12), 1, 12);
    build_num_opts(hour_opts_24, sizeof(hour_opts_24), 0, 23);
    build_num_opts(min_opts,     sizeof(min_opts),     0, 59);

    lv_roller_set_options(hour_roller,
        use_12h ? hour_opts_12 : hour_opts_24,
        LV_ROLLER_MODE_NORMAL);
    lv_roller_set_options(minute_roller, min_opts, LV_ROLLER_MODE_NORMAL);

    // Choose the roller starting position. When the alarm is currently
    // enabled, the rollers stay hidden behind the big locked-in time read-
    // out, but we still seed them with the saved time so disabling later
    // reveals them at that value. When the alarm is disabled on open,
    // always start from a fresh "now + 1 hour" suggestion — the user is
    // most likely opening the screen to set a new alarm, not to inspect
    // the last one. (Toggling enabled→disabled while on the screen takes
    // a separate path in on_enable_changed() that preserves the just-
    // disabled time, so that flow isn't affected by this default.)
    int h24, init_min;
    if (alarm_is_enabled()) {
        h24      = alarm_get_hour();
        init_min = alarm_get_minute();
    } else {
        struct tm now;
        clock_screen_get_local_time(&now);
        mktime(&now);
        h24      = (now.tm_hour + 1) % 24;
        init_min = now.tm_min;
    }
    set_rollers_visual(h24, init_min);

    if (alarm_is_enabled()) lv_obj_add_state(enable_switch, LV_STATE_CHECKED);
    else                    lv_obj_clear_state(enable_switch, LV_STATE_CHECKED);

    apply_editing_mode();
    update_current_time();

    // Seed the alert-behaviour widgets from the persisted values. Set the
    // states BEFORE adding back the event-listener side-effects — switch
    // state changes here would otherwise re-write the same value through
    // alarm_set_*() and bounce save_to_sd() back on init.
    if (alarm_get_vibrate()) lv_obj_add_state(vibrate_switch, LV_STATE_CHECKED);
    else                     lv_obj_clear_state(vibrate_switch, LV_STATE_CHECKED);

    if (alarm_get_audio())   lv_obj_add_state(audio_switch, LV_STATE_CHECKED);
    else                     lv_obj_clear_state(audio_switch, LV_STATE_CHECKED);

    int vol = alarm_get_volume();
    lv_slider_set_value(volume_slider, vol, LV_ANIM_OFF);
    lv_label_set_text_fmt(volume_label, "Volume  %d%%", vol);

    int snz = alarm_get_snooze_minutes();
    uint32_t sel = (snz == 10) ? 1 : (snz == 20) ? 2 : 0;
    lv_dropdown_set_selected(snooze_dropdown, sel);

    apply_audio_visibility();

    // Always open scrolled to the top so the time-picker is in view first.
    lv_obj_scroll_to_y(settings_scr, 0, LV_ANIM_OFF);
    lv_scr_load(settings_scr);
}

void alarm_screen_show_ringing()
{
    // Display the CURRENT RTC time when the alarm fires — the value the user
    // sees is "what time is it right now?" rather than "what time was the
    // alarm set for?". update_ring_time() in the 1 s tick keeps it ticking.
    update_ring_time();
    // Reflect the configured snooze duration on the snooze button so the
    // label matches the action it actually performs.
    lv_label_set_text_fmt(snooze_btn_label, "SNOOZE %d min",
                          alarm_get_snooze_minutes());
    lv_scr_load(ring_scr);
}

bool alarm_screen_is_active()
{
    return lv_screen_active() == settings_scr ||
           lv_screen_active() == ring_scr;
}
