#include "timer_screen.h"
#include "theme.h"
#include "alarm.h"     // alarm_play_chime_loop / alarm_stop_chime_loop
#include <LilyGoLib.h>
#include <stdio.h>

// Defined elsewhere.
void time_screen_show();

// ---- State machine --------------------------------------------------------
// Three modes plus an expired-overlay state. The setting UI (rollers +
// presets) is visible only in IDLE; the running UI (arc + countdown +
// PAUSE/RESET) takes over in RUNNING/PAUSED; the expired overlay pops on
// expiry and lays a full-screen red panel over everything until DISMISS.

enum TimerState { TS_IDLE = 0, TS_RUNNING, TS_PAUSED, TS_EXPIRED };

static TimerState s_state = TS_IDLE;

// ---- Time math ------------------------------------------------------------
// Pause-aware: s_target_remaining_ms is what we count down from, anchored at
// s_started_at_ms. Pause snapshots the current remaining into the same
// field; resume re-anchors started_at and keeps counting from there.
static uint32_t s_total_ms              = 60 * 1000;   // default 1 min
static uint32_t s_target_remaining_ms   = 0;
static uint32_t s_started_at_ms         = 0;
static uint32_t s_last_expired_buzz_ms  = 0;

// ---- Widgets --------------------------------------------------------------

static lv_obj_t *timer_screen;
// Idle (setting) UI
static lv_obj_t *idle_panel;
static lv_obj_t *hour_roller, *minute_roller, *second_roller;
static lv_obj_t *preset_row;
// Running / paused UI
static lv_obj_t *run_panel;
static lv_obj_t *progress_arc;
static lv_obj_t *countdown_label;
static lv_obj_t *total_hint_label;
// Expired overlay
static lv_obj_t *expired_panel;
// Bottom-row action buttons (shared across IDLE / RUNNING / PAUSED)
static lv_obj_t *start_btn, *start_btn_label;
static lv_obj_t *reset_btn;

// ---- Helpers --------------------------------------------------------------

// Compose "HH:MM:SS" (omitting the hour field when total < 1 h so a short
// 5-minute timer doesn't look like a 5-hour timer at a glance).
static void format_ms(char *buf, size_t n, uint32_t ms)
{
    uint32_t h = ms / 3600000;
    uint32_t m = (ms % 3600000) / 60000;
    uint32_t s = (ms % 60000) / 1000;
    if (h > 0)
        snprintf(buf, n, "%lu:%02lu:%02lu",
                 (unsigned long)h, (unsigned long)m, (unsigned long)s);
    else
        snprintf(buf, n, "%lu:%02lu", (unsigned long)m, (unsigned long)s);
}

// Live remaining ms; pulls from s_started_at_ms while RUNNING, otherwise
// returns whatever was snapshotted last.
static uint32_t current_remaining()
{
    if (s_state == TS_RUNNING) {
        uint32_t elapsed = millis() - s_started_at_ms;
        if (elapsed >= s_target_remaining_ms) return 0;
        return s_target_remaining_ms - elapsed;
    }
    if (s_state == TS_PAUSED) return s_target_remaining_ms;
    return 0;
}

// Build "00\n01\n…\nNN" into buf for an lv_roller (alarm-screen pattern).
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

// Convert ms duration → roller positions.
static void set_rollers_from_ms(uint32_t ms)
{
    uint32_t h = ms / 3600000;
    uint32_t m = (ms % 3600000) / 60000;
    uint32_t s = (ms % 60000) / 1000;
    if (h > 23) h = 23;
    if (m > 59) m = 59;
    if (s > 59) s = 59;
    lv_roller_set_selected(hour_roller,   h, LV_ANIM_OFF);
    lv_roller_set_selected(minute_roller, m, LV_ANIM_OFF);
    lv_roller_set_selected(second_roller, s, LV_ANIM_OFF);
}

static uint32_t ms_from_rollers()
{
    uint32_t h = lv_roller_get_selected(hour_roller);
    uint32_t m = lv_roller_get_selected(minute_roller);
    uint32_t s = lv_roller_get_selected(second_roller);
    return h * 3600000 + m * 60000 + s * 1000;
}

// Recolour the arc indicator based on time remaining — cyan most of the
// time, yellow under 30 s, orange under 10 s. Matches the way most
// physical kitchen timers escalate visual urgency near zero.
static lv_color_t arc_colour_for(uint32_t ms_remaining)
{
    if (ms_remaining <= 10000) return lv_color_make(0xFF, 0x66, 0x00);
    if (ms_remaining <= 30000) return lv_color_make(0xFF, 0xCC, 0x00);
    return lv_color_make(0x00, 0xCC, 0xFF);
}

static void update_running_ui()
{
    uint32_t rem = current_remaining();

    // Arc value as 0..1000 = remaining/total fraction. The visual depletes
    // from a full ring at start to an empty ring at zero.
    int value = 0;
    if (s_total_ms > 0)
        value = (int)((uint64_t)rem * 1000ull / s_total_ms);
    if (value < 0)      value = 0;
    if (value > 1000)   value = 1000;
    lv_arc_set_value(progress_arc, value);
    lv_obj_set_style_arc_color(progress_arc, arc_colour_for(rem), LV_PART_INDICATOR);

    char buf[16];
    format_ms(buf, sizeof(buf), rem);
    lv_label_set_text(countdown_label, buf);

    // Subtle "of total" hint under the big countdown so the user can see
    // their original setting while the timer ticks.
    char total[16];
    format_ms(total, sizeof(total), s_total_ms);
    char hint[24];
    snprintf(hint, sizeof(hint), "of %s", total);
    lv_label_set_text(total_hint_label, hint);
}

// ---- State transitions ----------------------------------------------------

static void apply_state(TimerState st)
{
    s_state = st;

    // Container visibility per mode.
    if (st == TS_IDLE) {
        lv_obj_clear_flag(idle_panel,    LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(run_panel,       LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(expired_panel,   LV_OBJ_FLAG_HIDDEN);
    } else if (st == TS_RUNNING || st == TS_PAUSED) {
        lv_obj_add_flag(idle_panel,      LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(run_panel,     LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(expired_panel,   LV_OBJ_FLAG_HIDDEN);
    } else {            // TS_EXPIRED
        lv_obj_add_flag(idle_panel,      LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(run_panel,       LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(expired_panel, LV_OBJ_FLAG_HIDDEN);
    }

    // Start/Pause/Resume label + colour + reset visibility + position.
    // In IDLE the single START button stays full-width and centred. In
    // RUNNING/PAUSED both buttons appear together — the originals were
    // 200/160 wide with LEFT/RIGHT alignment, which pushed them past the
    // 410-diameter visible circle. They're now 120×60 each, packed
    // symmetrically about the centre and lifted up to y=-50 so the
    // tappable area (and the label) stays inside the bezel.
    switch (st) {
    case TS_IDLE:
        lv_obj_set_size(start_btn, 200, 60);
        lv_label_set_text(start_btn_label, "START");
        lv_obj_set_style_bg_color(start_btn,
            lv_color_make(0x00, 0xAA, 0x44), LV_PART_MAIN);
        lv_obj_align(start_btn, LV_ALIGN_BOTTOM_MID, 0, -20);
        lv_obj_add_flag(reset_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(start_btn, LV_OBJ_FLAG_HIDDEN);
        break;
    case TS_RUNNING:
        lv_obj_set_size(start_btn, 120, 60);
        lv_obj_set_size(reset_btn, 120, 60);
        lv_label_set_text(start_btn_label, "PAUSE");
        lv_obj_set_style_bg_color(start_btn,
            lv_color_make(0xCC, 0x77, 0x00), LV_PART_MAIN);
        lv_obj_align(start_btn, LV_ALIGN_BOTTOM_MID, -66, -50);
        lv_obj_align(reset_btn, LV_ALIGN_BOTTOM_MID,  66, -50);
        lv_obj_clear_flag(reset_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(start_btn, LV_OBJ_FLAG_HIDDEN);
        break;
    case TS_PAUSED:
        lv_obj_set_size(start_btn, 120, 60);
        lv_obj_set_size(reset_btn, 120, 60);
        lv_label_set_text(start_btn_label, "RESUME");
        lv_obj_set_style_bg_color(start_btn,
            lv_color_make(0x00, 0xAA, 0x44), LV_PART_MAIN);
        lv_obj_align(start_btn, LV_ALIGN_BOTTOM_MID, -66, -50);
        lv_obj_align(reset_btn, LV_ALIGN_BOTTOM_MID,  66, -50);
        lv_obj_clear_flag(reset_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(start_btn, LV_OBJ_FLAG_HIDDEN);
        break;
    case TS_EXPIRED:
        // Hide the start/reset row entirely while the expired panel owns
        // the screen — the DISMISS button on the panel is the only way out.
        lv_obj_add_flag(start_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(reset_btn, LV_OBJ_FLAG_HIDDEN);
        break;
    }
}

// ---- Event handlers -------------------------------------------------------

static void on_start_btn(lv_event_t *)
{
    switch (s_state) {
    case TS_IDLE: {
        uint32_t ms = ms_from_rollers();
        if (ms == 0) ms = 1000;
        s_total_ms             = ms;
        s_target_remaining_ms  = ms;
        s_started_at_ms        = millis();
        apply_state(TS_RUNNING);
        update_running_ui();
        break;
    }
    case TS_RUNNING:
        // Pause — snapshot remaining into target so resume picks up cleanly.
        s_target_remaining_ms = current_remaining();
        apply_state(TS_PAUSED);
        update_running_ui();
        break;
    case TS_PAUSED:
        // Resume — re-anchor the clock to now; target_remaining is the
        // snapshot from pause time, untouched.
        s_started_at_ms = millis();
        apply_state(TS_RUNNING);
        update_running_ui();
        break;
    case TS_EXPIRED:
        break;          // dismissed via the overlay button instead
    }
}

static void on_reset_btn(lv_event_t *)
{
    s_target_remaining_ms = 0;
    apply_state(TS_IDLE);
}

static void on_dismiss(lv_event_t *)
{
    // Silence the chime first so the I2S task winds down before LVGL tears
    // the screen apart and reloads the idle panel.
    alarm_stop_chime_loop();
    s_target_remaining_ms = 0;
    apply_state(TS_IDLE);
}

static void on_preset_btn(lv_event_t *e)
{
    uint32_t seconds = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    set_rollers_from_ms(seconds * 1000);
}

// Swipe-RIGHT goes back to the TIME screen (Timer's parent in the
// navigation tree). Deliberately uses a horizontal direction — the H/M/S
// rollers consume vertical swipes for value scrolling, so any LV_DIR_TOP /
// LV_DIR_BOTTOM gesture handler would fire when the user tries to scroll
// a roller and accidentally navigate away. Other directions (up / down /
// left) are intentionally no-ops here.
static void on_gesture(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir == LV_DIR_RIGHT) time_screen_show();
}

// Periodic tick — drives the running countdown and the expired pulse.
static void on_tick(lv_timer_t *)
{
    if (s_state == TS_RUNNING) {
        uint32_t rem = current_remaining();
        if (rem == 0) {
            s_target_remaining_ms  = 0;
            apply_state(TS_EXPIRED);
            // Force the screen to the front so the user sees the overlay
            // even if they navigated away while the timer was running —
            // same approach the alarm uses on fire.
            if (lv_screen_active() != timer_screen)
                lv_scr_load(timer_screen);
            // Borrow the alarm's doorbell-loop chime at full volume — the
            // user explicitly wants the timer to be as loud and as urgent
            // as the morning alarm, regardless of what they've set the
            // alarm volume to. The chime keeps looping until DISMISS.
            alarm_play_chime_loop(100);
            instance.vibrator();
            s_last_expired_buzz_ms = millis();
        } else {
            update_running_ui();
        }
    } else if (s_state == TS_EXPIRED) {
        // Pulse the haptic every 1.5 s until DISMISS — same cadence the
        // alarm uses for the ringing screen.
        if (millis() - s_last_expired_buzz_ms >= 1500) {
            s_last_expired_buzz_ms = millis();
            instance.vibrator();
        }
    }
}

// ---- Layout helpers -------------------------------------------------------

static lv_obj_t *make_button(lv_obj_t *parent, const char *text,
                              lv_coord_t w, lv_coord_t h,
                              lv_color_t bg, const lv_font_t *font,
                              lv_obj_t **label_out)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_radius(btn, 10, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, font ? font : &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_center(lbl);
    if (label_out) *label_out = lbl;
    return btn;
}

// Preset chip — 120×68 pill (doubled from the original 60×34) with a
// centred label in 28 pt. Tapping it calls on_preset_btn() with the
// seconds value packed into the user_data slot.
static lv_obj_t *make_preset_chip(lv_obj_t *parent, const char *label,
                                  uint32_t seconds)
{
    lv_obj_t *chip = lv_obj_create(parent);
    lv_obj_set_size(chip, 120, 68);
    lv_obj_set_style_radius(chip, 34, LV_PART_MAIN);
    lv_obj_set_style_bg_color(chip, lv_color_make(0x22, 0x44, 0x66), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(chip, lv_color_make(0x44, 0x88, 0xCC), LV_PART_MAIN);
    lv_obj_set_style_border_width(chip, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_all(chip, 0, LV_PART_MAIN);
    lv_obj_clear_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(chip, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *lbl = lv_label_create(chip);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_color(lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &font_argus_label_28, LV_PART_MAIN);
    lv_obj_center(lbl);

    lv_obj_add_event_cb(chip, on_preset_btn, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)seconds);
    return chip;
}

// Reusable roller styling — same look as the alarm screen's rollers so
// the two timepiece screens feel like siblings.
static void style_roller(lv_obj_t *r)
{
    lv_obj_set_style_text_font(r, &font_argus_label_28, LV_PART_MAIN);
    lv_obj_set_style_bg_color(r, lv_color_make(0x22, 0x22, 0x22), LV_PART_MAIN);
    lv_obj_set_style_text_color(r, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_border_color(r, lv_color_make(0x55, 0x55, 0x55), LV_PART_MAIN);
    lv_obj_set_style_border_width(r, 1, LV_PART_MAIN);
    lv_obj_set_style_bg_color(r, lv_color_make(0x00, 0x55, 0x33), LV_PART_SELECTED);
    lv_obj_set_style_text_color(r, lv_color_white(), LV_PART_SELECTED);
}

// ---- Idle panel -----------------------------------------------------------
//
// Three rollers laid out as HH : MM : SS with small unit-labels beneath
// each one, then a row of preset chips, then the START button.

static void build_idle_panel()
{
    idle_panel = lv_obj_create(timer_screen);
    // Tall enough to contain the doubled-size preset grid (rollers at
    // y=50..160, preset grid at y=175..325).
    lv_obj_set_size(idle_panel, lv_pct(100), 360);
    lv_obj_align(idle_panel, LV_ALIGN_TOP_MID, 0, 56);
    lv_obj_set_style_bg_opa(idle_panel, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(idle_panel, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(idle_panel, 0, LV_PART_MAIN);
    lv_obj_clear_flag(idle_panel, LV_OBJ_FLAG_SCROLLABLE);

    // Roller options. Each is built once into a static buffer — lv_roller
    // copies the string at set_options time, so the buffer can be reused
    // for all three rollers and doesn't need to live past this function.
    static char opts_hr[24 * 3 + 1];   // "00\n01\n…\n23"
    static char opts_60[60 * 3 + 1];   // "00\n01\n…\n59"
    build_num_opts(opts_hr,  sizeof(opts_hr), 0, 23);
    build_num_opts(opts_60,  sizeof(opts_60), 0, 59);

    // H / M / S labels — uppercase, sit above each roller as column
    // headers. White (not grey) + 28 pt so they read as labels in their
    // own right rather than as a faint footnote.
    static const char *units[3] = { "H", "M", "S" };
    int xs[3] = { -110, 0, 110 };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *u = lv_label_create(idle_panel);
        lv_obj_set_style_text_color(u, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_text_font(u, &font_argus_label_28, LV_PART_MAIN);
        lv_label_set_text(u, units[i]);
        lv_obj_align(u, LV_ALIGN_TOP_MID, xs[i], 6);
    }

    // Three rollers spaced symmetrically around the panel centre, anchored
    // below the H/M/S headers.
    hour_roller = lv_roller_create(idle_panel);
    lv_roller_set_options(hour_roller, opts_hr, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(hour_roller, 3);
    lv_obj_set_width(hour_roller, 96);
    style_roller(hour_roller);
    lv_obj_align(hour_roller, LV_ALIGN_TOP_MID, -110, 50);

    minute_roller = lv_roller_create(idle_panel);
    lv_roller_set_options(minute_roller, opts_60, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(minute_roller, 3);
    lv_obj_set_width(minute_roller, 96);
    style_roller(minute_roller);
    lv_obj_align(minute_roller, LV_ALIGN_TOP_MID, 0, 50);

    second_roller = lv_roller_create(idle_panel);
    lv_roller_set_options(second_roller, opts_60, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(second_roller, 3);
    lv_obj_set_width(second_roller, 96);
    style_roller(second_roller);
    lv_obj_align(second_roller, LV_ALIGN_TOP_MID, 110, 50);

    // Preset chip grid — 3 columns × 2 rows of doubled-size chips. With
    // 120 px chips + 12 px column gap + 8 px row gap, the grid is
    // 3*120+2*12 = 384 wide and 2*68+8 = 144 tall. Insertion order maps
    // to row-major (3 per row):
    //   [1m]  [5m]  [10m]
    //   [30m] [1h]  [5h]
    preset_row = lv_obj_create(idle_panel);
    lv_obj_set_size(preset_row, 396, 150);
    lv_obj_align(preset_row, LV_ALIGN_TOP_MID, 0, 175);
    lv_obj_set_style_bg_opa(preset_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(preset_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(preset_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(preset_row, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_row(preset_row, 8, LV_PART_MAIN);
    lv_obj_clear_flag(preset_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(preset_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(preset_row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(preset_row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    make_preset_chip(preset_row, "1m",   60);
    make_preset_chip(preset_row, "5m",   5  * 60);
    make_preset_chip(preset_row, "10m",  10 * 60);
    make_preset_chip(preset_row, "30m",  30 * 60);
    make_preset_chip(preset_row, "1h",   60 * 60);
    make_preset_chip(preset_row, "5h",   5  * 60 * 60);

    // Seed the rollers with the default duration so the screen always
    // opens to a usable starting value.
    set_rollers_from_ms(s_total_ms);
}

// ---- Running panel --------------------------------------------------------
//
// One big LVGL arc — full ring background, thick indicator that depletes
// clockwise as the countdown runs. The countdown digits sit centred inside
// it; a "of HH:MM:SS" hint reminds the user of the original total.

static void build_run_panel()
{
    run_panel = lv_obj_create(timer_screen);
    lv_obj_set_size(run_panel, lv_pct(100), 320);
    lv_obj_align(run_panel, LV_ALIGN_TOP_MID, 0, 56);
    lv_obj_set_style_bg_opa(run_panel, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(run_panel, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(run_panel, 0, LV_PART_MAIN);
    lv_obj_clear_flag(run_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(run_panel, LV_OBJ_FLAG_HIDDEN);

    progress_arc = lv_arc_create(run_panel);
    lv_obj_set_size(progress_arc, 260, 260);
    lv_obj_align(progress_arc, LV_ALIGN_TOP_MID, 0, 10);
    lv_arc_set_rotation(progress_arc, 270);          // 0° at top
    lv_arc_set_bg_angles(progress_arc, 0, 360);      // full ring background
    lv_arc_set_range(progress_arc, 0, 1000);
    lv_arc_set_value(progress_arc, 1000);
    lv_obj_remove_style(progress_arc, NULL, LV_PART_KNOB);     // no thumb
    lv_obj_clear_flag(progress_arc, LV_OBJ_FLAG_CLICKABLE);    // not draggable
    lv_obj_set_style_arc_width(progress_arc, 18, LV_PART_MAIN);
    lv_obj_set_style_arc_color(progress_arc,
        lv_color_make(0x22, 0x22, 0x22), LV_PART_MAIN);
    lv_obj_set_style_arc_width(progress_arc, 18, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(progress_arc,
        lv_color_make(0x00, 0xCC, 0xFF), LV_PART_INDICATOR);

    countdown_label = lv_label_create(run_panel);
    lv_obj_set_style_text_color(countdown_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(countdown_label, &font_argus_mono_48, LV_PART_MAIN);
    lv_label_set_text(countdown_label, "0:00");
    lv_obj_align(countdown_label, LV_ALIGN_TOP_MID, 0, 110);

    total_hint_label = lv_label_create(run_panel);
    lv_obj_set_style_text_color(total_hint_label,
        ARGUS_TEXT_DIM, LV_PART_MAIN);
    lv_obj_set_style_text_font(total_hint_label, &font_argus_label_16, LV_PART_MAIN);
    lv_label_set_text(total_hint_label, "");
    lv_obj_align(total_hint_label, LV_ALIGN_TOP_MID, 0, 170);
}

// ---- Expired overlay -------------------------------------------------------
//
// Full-screen red panel that pops over everything when the timer hits zero.
// Visible until the user taps DISMISS — gives them a chance to actually
// notice if they were on a different screen when the timer fired.

static void build_expired_panel()
{
    expired_panel = lv_obj_create(timer_screen);
    lv_obj_set_size(expired_panel, lv_pct(100), lv_pct(100));
    lv_obj_align(expired_panel, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(expired_panel, lv_color_make(0x22, 0x00, 0x00), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(expired_panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(expired_panel, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(expired_panel, 0, LV_PART_MAIN);
    lv_obj_clear_flag(expired_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(expired_panel, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *title = lv_label_create(expired_panel);
    lv_obj_set_style_text_color(title, argus_base_accent(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_argus_ui, LV_PART_MAIN);
    lv_label_set_text(title, "TIME'S UP");
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -40);

    lv_obj_t *dismiss = make_button(expired_panel, "DISMISS", 240, 70,
                                    lv_color_make(0xCC, 0x22, 0x22),
                                    &lv_font_montserrat_28, NULL);
    lv_obj_align(dismiss, LV_ALIGN_BOTTOM_MID, 0, -60);
    lv_obj_add_event_cb(dismiss, on_dismiss, LV_EVENT_CLICKED, NULL);
}

// ---- Public API -----------------------------------------------------------

void timer_screen_create()
{
    timer_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(timer_screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(timer_screen, 0, LV_PART_MAIN);
    lv_obj_clear_flag(timer_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(timer_screen, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(timer_screen, on_gesture, LV_EVENT_GESTURE, NULL);

    // Title
    lv_obj_t *title = lv_label_create(timer_screen);
    lv_obj_set_style_text_color(title, argus_base_accent(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_argus_ui, LV_PART_MAIN);
    lv_label_set_text(title, "TIMER");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

    // Build the three mode panels. Idle is visible by default; the other
    // two start hidden and get swapped in on state transitions.
    build_idle_panel();
    build_run_panel();
    build_expired_panel();

    // Bottom action row — START / PAUSE / RESUME on the left, RESET on
    // the right. Both buttons are reused across modes; apply_state()
    // controls their labels + colours + visibility.
    start_btn = make_button(timer_screen, "START", 200, 60,
                            lv_color_make(0x00, 0xAA, 0x44),
                            &lv_font_montserrat_28, &start_btn_label);
    lv_obj_align(start_btn, LV_ALIGN_BOTTOM_LEFT, 16, -20);
    lv_obj_add_event_cb(start_btn, on_start_btn, LV_EVENT_CLICKED, NULL);

    reset_btn = make_button(timer_screen, "RESET", 160, 60,
                            lv_color_make(0x55, 0x22, 0x22),
                            &lv_font_montserrat_28, NULL);
    lv_obj_align(reset_btn, LV_ALIGN_BOTTOM_RIGHT, -16, -20);
    lv_obj_add_event_cb(reset_btn, on_reset_btn, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(reset_btn, LV_OBJ_FLAG_HIDDEN);

    apply_state(TS_IDLE);

    // 10 Hz tick is plenty for a one-second display — keeps the arc
    // animation smooth without burning CPU.
    lv_timer_create(on_tick, 100, NULL);
}

void timer_screen_show()
{
    // If a timer's already running, drop the user back into the running
    // view rather than the setting view (it'd be confusing to land on
    // the rollers while a timer is counting down behind them).
    if (s_state == TS_RUNNING || s_state == TS_PAUSED)
        update_running_ui();
    lv_scr_load(timer_screen);
}

bool timer_screen_is_active()
{
    return lv_screen_active() == timer_screen;
}

bool timer_is_running()
{
    return s_state == TS_RUNNING;
}
