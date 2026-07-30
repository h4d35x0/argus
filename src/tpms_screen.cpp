#include "tpms_screen.h"
#include "theme.h"
#include "tpms.h"
#include "lora_screen.h"
#include <LilyGoLib.h>

// Defined in main.cpp
void tools_screen_show();

// Pressure thresholds (kPa)
#define TPMS_THRESH_LOW      185.0f   // ~26.8 PSI — below normal
#define TPMS_THRESH_CRITICAL 138.0f   // ~20 PSI — flat warning

static lv_obj_t *tpms_screen;
static lv_obj_t *status_label;
static lv_obj_t *btn_433;
static lv_obj_t *btn_fmt;
static lv_obj_t *btn_fmt_label;
static lv_obj_t *start_btn;
static lv_obj_t *start_label;

// Frequency and format the user has selected (decoupled from running state).
static bool       s_sel_433    = true;
static TpmsFormat s_sel_fmt    = TPMS_FORMAT_FSK;
// Set when tpms_start() returns false for a reason other than LoRa holding
// the radio (e.g. RadioLib init error), so on_update() can report it.
static bool s_start_error = false;

// GMC_96 removed entirely (UI + backend): unsupported on this radio.
static const char *FORMAT_LABELS[] = { "FSK", "SCHR" };

// Per-wheel objects (indexed by TpmsPos: FL=0, FR=1, RL=2, RR=3)
static lv_obj_t *cell[4];
static lv_obj_t *cell_press[4];   // large pressure number
static lv_obj_t *cell_unit[4];    // "PSI" label
static lv_obj_t *cell_temp[4];    // temperature
static lv_obj_t *cell_id[4];      // sensor ID (small, below position label)
static lv_obj_t *cell_bat[4];     // battery icon
static lv_obj_t *cell_age[4];     // last-seen age

static const char *WHEEL_LABEL[4] = { "T1", "T2", "T3", "T4" };

// ---- helpers ---------------------------------------------------------------

static lv_color_t pressure_color(float kpa)
{
    if (kpa <= 0.0f)               return lv_color_make(0x44, 0x44, 0x44); // no data
    if (kpa < TPMS_THRESH_CRITICAL) return lv_color_make(0xFF, 0x22, 0x22); // red
    if (kpa < TPMS_THRESH_LOW)      return lv_color_make(0xFF, 0xCC, 0x00); // yellow
    return                                 lv_color_make(0x22, 0xBB, 0x22); // green
}

static void update_freq_buttons()
{
    // 433 MHz only -- 315 receive doesn't work on this board's front-end
    // (antenna/matching tuned for the LoRa bands; noisy at 315). btn_433 is a
    // static band indicator, always shown active.
    lv_obj_set_style_bg_color(btn_433, lv_color_make(0x00, 0x55, 0xAA), LV_PART_MAIN);
}

static void update_fmt_button()
{
    lv_label_set_text(btn_fmt_label, FORMAT_LABELS[(int)s_sel_fmt]);
    // Amber tint when in OOK mode to indicate non-default selection.
    lv_color_t col = (s_sel_fmt == TPMS_FORMAT_FSK)
                     ? lv_color_make(0x22, 0x22, 0x22)
                     : lv_color_make(0x55, 0x33, 0x00);
    lv_obj_set_style_bg_color(btn_fmt, col, LV_PART_MAIN);
}

// START button reflects scanner state: green START when idle, red STOP while
// running, greyed-out and non-clickable when the SX1262 is held by LoRa.
static void update_start_button()
{
    bool running = tpms_is_running();
    bool busy    = !running && lora_screen_is_powered();

    lv_label_set_text(start_label, running ? "STOP" : "START");

    lv_color_t col = running ? lv_color_make(0xCC, 0x00, 0x00)
                   : busy    ? lv_color_make(0x33, 0x33, 0x33)
                             : lv_color_make(0x00, 0xAA, 0x44);
    lv_obj_set_style_bg_color(start_btn, col, LV_PART_MAIN);

    if (busy) lv_obj_clear_flag(start_btn, LV_OBJ_FLAG_CLICKABLE);
    else      lv_obj_add_flag(start_btn,   LV_OBJ_FLAG_CLICKABLE);
}

static void refresh_cell(int idx)
{
    const TpmsSensor *s = tpms_get_sensor((TpmsPos)idx);

    if (!s || !s->valid) {
        lv_label_set_text(cell_press[idx], "--");
        lv_label_set_text(cell_unit[idx],  "PSI");
        lv_label_set_text(cell_temp[idx],  "--");
        lv_label_set_text(cell_id[idx],    "");
        lv_label_set_text(cell_bat[idx],   "");
        lv_label_set_text(cell_age[idx],   "");
        lv_obj_set_style_bg_color(cell[idx], lv_color_make(0x1A, 0x1A, 0x1A), LV_PART_MAIN);
        lv_obj_set_style_text_color(cell_press[idx], ARGUS_TEXT_DIM, LV_PART_MAIN);
        return;
    }

    // Pressure in PSI
    float psi = s->pressure_kpa / 6.894757f;
    char psi_buf[12];
    if (psi > 0.5f)
        snprintf(psi_buf, sizeof(psi_buf), "%.1f", psi);
    else
        snprintf(psi_buf, sizeof(psi_buf), "--");
    lv_label_set_text(cell_press[idx], psi_buf);
    lv_label_set_text(cell_unit[idx],  "PSI");

    // Temperature
    if (s->temp_c != -128) {
        char tmp[12];
        snprintf(tmp, sizeof(tmp), "%d°C", (int)s->temp_c);
        lv_label_set_text(cell_temp[idx], tmp);
    } else {
        lv_label_set_text(cell_temp[idx], "--");
    }

    // Sensor ID (abbreviated)
    char id_buf[14];
    snprintf(id_buf, sizeof(id_buf), "%08lX", (unsigned long)s->id);
    lv_label_set_text(cell_id[idx], id_buf);

    // Battery
    lv_label_set_text(cell_bat[idx], s->battery_low ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_FULL);
    lv_obj_set_style_text_color(cell_bat[idx],
        s->battery_low ? lv_color_make(0xFF, 0x44, 0x00)
                       : lv_color_make(0x44, 0x88, 0x44), LV_PART_MAIN);

    // Age in seconds
    uint32_t age_s = (millis() - s->last_seen_ms) / 1000;
    char age_buf[12];
    if (age_s < 60)
        snprintf(age_buf, sizeof(age_buf), "%lus", (unsigned long)age_s);
    else
        snprintf(age_buf, sizeof(age_buf), "%lum", (unsigned long)(age_s / 60));
    lv_label_set_text(cell_age[idx], age_buf);

    // Colour the cell and pressure label based on pressure level
    lv_color_t col = pressure_color(s->pressure_kpa);
    lv_obj_set_style_text_color(cell_press[idx], col, LV_PART_MAIN);

    // Tint cell border to match status
    lv_obj_set_style_border_color(cell[idx], col, LV_PART_MAIN);
    lv_obj_set_style_bg_color(cell[idx], lv_color_make(0x1A, 0x1A, 0x1A), LV_PART_MAIN);
}

static void on_update(lv_timer_t *)
{
    // Skip the 4-cell UI refresh + status update when the screen isn't
    // visible — tpms_bg_tick() in the main loop keeps the RX path live
    // independently, so we don't miss any sensors by gating the UI.
    if (!tpms_screen_is_active()) return;

    for (int i = 0; i < 4; i++) refresh_cell(i);

    if (tpms_is_running()) {
        // rx=burst captures, sy=sync marker found, crc=full decode passed.
        // rx>0 sy=0: preamble/sync not matched.
        // sy>0 crc=0: Manchester violation or checksum wrong.
        char buf[64];
        int rx  = tpms_get_isr_count();
        int crc = tpms_get_crc_count();
        int n   = tpms_get_total_count();
        bool ook = (tpms_get_format() != TPMS_FORMAT_FSK);
        if (n > 0) {
            snprintf(buf, sizeof(buf), "%d sensor%s  %drx/%dcrc",
                     n, n == 1 ? "" : "s", rx, crc);
        } else if (ook) {
            int sy  = tpms_get_sync_count();
            int man = tpms_get_man_count();
            int mx  = tpms_get_max_n_chips();
            int b8  = (int)tpms_get_last_chip8();
            int thr = (int)tpms_get_ook_threshold();
            // rx=bursts captured  sy=sync found  man=Manchester passed
            // c=checksum ok  mx=longest burst chips  b=first8 of last  t=threshold dBm
            snprintf(buf, sizeof(buf), "rx%d sy%d man%d c%d mx%d b%02X t%d",
                     rx, sy, man, crc, mx, b8, thr);
        } else {
            snprintf(buf, sizeof(buf), "Scanning  %drx/%dcrc", rx, crc);
        }
        lv_label_set_text(status_label, buf);
        lv_obj_set_style_text_color(status_label, ARGUS_TEXT, LV_PART_MAIN);
    } else if (lora_screen_is_powered()) {
        lv_label_set_text(status_label, "Radio in use by LoRa");
        lv_obj_set_style_text_color(status_label, lv_color_make(0xFF, 0xAA, 0x00), LV_PART_MAIN);
    } else if (s_start_error) {
        char buf[32];
        snprintf(buf, sizeof(buf), "Radio init failed (%d)", (int)tpms_last_error());
        lv_label_set_text(status_label, buf);
        lv_obj_set_style_text_color(status_label, lv_color_make(0xFF, 0x44, 0x44), LV_PART_MAIN);
    } else {
        lv_label_set_text(status_label, "Scanner off");
        lv_obj_set_style_text_color(status_label, ARGUS_TEXT_DIM, LV_PART_MAIN);
    }

    update_start_button();
}

// ---- event handlers --------------------------------------------------------

static void on_fmt(lv_event_t *)
{
    // Cycle FSK <-> SCHR only. GMC_96 cannot be captured reliably via RSSI
    // polling on the SX1262, so it was removed entirely (see README).
    s_sel_fmt = (TpmsFormat)(((int)s_sel_fmt + 1) % 2);
    update_fmt_button();
    if (tpms_is_running()) tpms_start(s_sel_433, s_sel_fmt);  // live switch
}

static void on_start_stop(lv_event_t *)
{
    if (tpms_is_running()) {
        tpms_stop();
        s_start_error = false;
    } else {
        // tpms_start() fails silently if LoRa holds the radio; on_update()
        // distinguishes that case from a genuine init error.
        s_start_error = !tpms_start(s_sel_433, s_sel_fmt) && !lora_screen_is_powered();
    }
    update_freq_buttons();
    update_start_button();
    on_update(nullptr);   // refresh the status line immediately
}

static void on_gesture(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir == LV_DIR_TOP) {
        tpms_stop();
        tools_screen_show();
    }
}

// ---- build helpers ---------------------------------------------------------

static lv_obj_t *make_btn(lv_obj_t *parent, const char *text, lv_coord_t x, lv_coord_t w,
                           lv_coord_t y = 62, lv_coord_t h = 40)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_style_radius(btn, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_make(0x22, 0x22, 0x22), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &font_argus_label_20, LV_PART_MAIN);
    lv_obj_center(lbl);

    return btn;
}

// Build one 160×145 tile for a wheel position. Shortened from 165 so both rows
// fit on the 466 px round panel (see grid layout note in tpms_screen_create);
// interior content is centre-aligned and stays inside the visible circle.
static void make_cell(lv_obj_t *parent, int idx, lv_coord_t x, lv_coord_t y)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_size(c, 160, 145);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_style_bg_color(c, lv_color_make(0x1A, 0x1A, 0x1A), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(c, lv_color_make(0x44, 0x44, 0x44), LV_PART_MAIN);
    lv_obj_set_style_border_width(c, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(c, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_all(c, 0, LV_PART_MAIN);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    cell[idx] = c;

    // Wheel position label (FL / FR / RL / RR)
    lv_obj_t *pos_lbl = lv_label_create(c);
    lv_obj_set_style_text_color(pos_lbl, ARGUS_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(pos_lbl, &font_argus_label_16, LV_PART_MAIN);
    lv_label_set_text(pos_lbl, WHEEL_LABEL[idx]);
    lv_obj_align(pos_lbl, LV_ALIGN_TOP_MID, 0, 2);

    // Sensor ID (small, below position label)
    cell_id[idx] = lv_label_create(c);
    lv_obj_set_style_text_color(cell_id[idx], ARGUS_TEXT_DIM, LV_PART_MAIN);
    lv_obj_set_style_text_font(cell_id[idx], &lv_font_montserrat_14, LV_PART_MAIN);
    lv_label_set_text(cell_id[idx], "");
    lv_obj_align(cell_id[idx], LV_ALIGN_TOP_MID, 0, 20);

    // Pressure value (large — dropped one size to fit the shorter tile).
    cell_press[idx] = lv_label_create(c);
    lv_obj_set_style_text_color(cell_press[idx], ARGUS_TEXT_DIM, LV_PART_MAIN);
    lv_obj_set_style_text_font(cell_press[idx], &lv_font_montserrat_36, LV_PART_MAIN);
    lv_label_set_text(cell_press[idx], "--");
    lv_obj_align(cell_press[idx], LV_ALIGN_TOP_MID, 0, 36);

    // PSI unit label (below pressure)
    cell_unit[idx] = lv_label_create(c);
    lv_obj_set_style_text_color(cell_unit[idx], ARGUS_TEXT_DIM, LV_PART_MAIN);
    lv_obj_set_style_text_font(cell_unit[idx], &lv_font_montserrat_14, LV_PART_MAIN);
    lv_label_set_text(cell_unit[idx], "PSI");
    lv_obj_align(cell_unit[idx], LV_ALIGN_TOP_MID, 0, 74);

    // Temperature
    cell_temp[idx] = lv_label_create(c);
    lv_obj_set_style_text_color(cell_temp[idx], ARGUS_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(cell_temp[idx], &lv_font_montserrat_16, LV_PART_MAIN);
    lv_label_set_text(cell_temp[idx], "--");
    lv_obj_align(cell_temp[idx], LV_ALIGN_TOP_MID, -22, 92);

    // Battery icon (right of temperature)
    cell_bat[idx] = lv_label_create(c);
    lv_obj_set_style_text_font(cell_bat[idx], &lv_font_montserrat_16, LV_PART_MAIN);
    lv_label_set_text(cell_bat[idx], "");
    lv_obj_align(cell_bat[idx], LV_ALIGN_TOP_MID, 22, 92);

    // Age indicator (bottom-right corner)
    cell_age[idx] = lv_label_create(c);
    lv_obj_set_style_text_color(cell_age[idx], ARGUS_TEXT_DIM, LV_PART_MAIN);
    lv_obj_set_style_text_font(cell_age[idx], &lv_font_montserrat_14, LV_PART_MAIN);
    lv_label_set_text(cell_age[idx], "");
    lv_obj_align(cell_age[idx], LV_ALIGN_BOTTOM_RIGHT, -4, -2);
}

// ---- public API ------------------------------------------------------------

void tpms_screen_create()
{
    tpms_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(tpms_screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(tpms_screen, 0, LV_PART_MAIN);

    // Title — sized to match the SETTINGS screen header so the two read
    // as siblings at the same hierarchy.
    lv_obj_t *title = lv_label_create(tpms_screen);
    lv_obj_set_style_text_color(title, argus_base_accent(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_argus_ui, LV_PART_MAIN);
    lv_label_set_text(title, "TPMS");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    // Single button row y=62: [433 MHz] [START/STOP] [FSK/SCHR].
    // The format selector takes the slot where the 315 MHz button used to be,
    // freeing the old row 2 so the wheel grid can move up and fit on-screen.
    // btn_433 is a static band indicator (433-only; 315 RX unsupported here).
    btn_433     = make_btn(tpms_screen, "433 MHz",         18,  104);
    start_btn   = make_btn(tpms_screen, "START",          140,  130);
    btn_fmt     = make_btn(tpms_screen, FORMAT_LABELS[0], 288,  104);
    start_label   = lv_obj_get_child(start_btn, 0);
    btn_fmt_label = lv_obj_get_child(btn_fmt, 0);
    lv_obj_add_event_cb(btn_fmt,   on_fmt,        LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(start_btn, on_start_stop, LV_EVENT_CLICKED, NULL);

    // Status line — sits just below the single button row (ends y=102).
    status_label = lv_label_create(tpms_screen);
    lv_obj_set_style_text_font(status_label, &font_argus_label_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(status_label, ARGUS_TEXT_DIM, LV_PART_MAIN);
    lv_label_set_text(status_label, "Scanner off");
    lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 110);

    // 2×2 wheel grid — 160×145 tiles with 4 px gaps. Pulled up (top row y=132)
    // and shortened (165 → 145) so the bottom row's bottom edge lands at y=426,
    // well inside the 466 px panel / 456 px visible circle (was y=496, which ran
    // off the bottom). Interior content is centre-aligned and stays in view; the
    // bottom-outer corners still curve behind the bezel, which is fine.
    //   T1: x=43,  y=132    T2: x=207, y=132
    //   T3: x=43,  y=281    T4: x=207, y=281
    make_cell(tpms_screen, TPMS_T1,  43, 132);
    make_cell(tpms_screen, TPMS_T2, 207, 132);
    make_cell(tpms_screen, TPMS_T3,  43, 281);
    make_cell(tpms_screen, TPMS_T4, 207, 281);

    lv_obj_add_event_cb(tpms_screen, on_gesture, LV_EVENT_GESTURE, NULL);

    update_freq_buttons();
    update_fmt_button();
    update_start_button();
    lv_timer_create(on_update, 1000, NULL);
}

void tpms_screen_show()
{
    s_start_error = false;
    update_freq_buttons();
    update_fmt_button();
    update_start_button();
    lv_scr_load(tpms_screen);
}

bool tpms_screen_is_active()
{
    return lv_screen_active() == tpms_screen;
}
