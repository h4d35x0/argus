#include "lora_analyze_screen.h"
#include "theme.h"
#include "bt_analyze_screen.h"
#include "lora_screen.h"
#include "pager.h"
#include "tpms.h"
#include "aprs.h"
#include <LilyGoLib.h>
#include <stdio.h>
#include <string.h>

// Defined in tools_screen.cpp
void tools_screen_show();

#define N_BINS          13
#define BAR_W           24
#define BAR_GAP         4
// Chart geometry tuned to fit inside the watch's ~410-diameter visible
// circle. 13 bins × 24 + 12 gaps × 4 = 360 px wide; that width fits the
// circle for any Y in [153, 349] — pulling top/bottom in keeps the chart
// corners off the bezel.
#define CHART_BOTTOM_Y  348
#define CHART_TOP_Y     154
#define HOP_MS          30
#define LABEL_EVERY     2          // draw a freq label every Nth bin

// The visualisation maps RSSI linearly between these two anchors. Outside the
// range we clamp; -130 dBm is below the SX1262 noise floor in any realistic
// setting and -30 dBm is a "very loud nearby transmitter" reading.
#define RSSI_FLOOR      (-130)
#define RSSI_CEIL       (-30)

struct Band {
    const char *label;
    const char *short_label;
    float       start_mhz;
    float       step_mhz;
};

// Three bands covering the SX1262's tuning range that match the common ISM
// allocations the watch's LoRa modem is used in. The PCB matching network is
// tuned for one band (depending on the watch variant), so off-band readings
// have lower sensitivity than the native band.
static const Band s_bands[] = {
    { "915 MHz US ISM", "915 MHz", 902.0f, 2.0f  },   // 902-926 MHz, 26 MHz wide
    { "868 MHz EU ISM", "868 MHz", 863.0f, 0.5f  },   // 863-869 MHz
    { "433 MHz ISM",    "433 MHz", 433.0f, 0.15f },   // 433.0-434.8 MHz
};
static const int N_BANDS = sizeof(s_bands) / sizeof(s_bands[0]);

// ---- state -----------------------------------------------------------------

static lv_obj_t *screen;
static lv_obj_t *title_label;
static lv_obj_t *status_label;
static lv_obj_t *legend_label;
static lv_obj_t *band_btn, *band_btn_label;
static lv_obj_t *bars[N_BINS];
static lv_obj_t *bin_labels[N_BINS];
static int       bar_x[N_BINS];

static int16_t  s_rssi[N_BINS];           // last reading per bin (dBm)
static int16_t  s_peak[N_BINS];           // running peak per bin
static int      s_band            = 0;
static int      s_cur_bin         = 0;
static int      s_strongest_bin   = 0;
static int8_t   s_strongest_rssi  = -127;
static bool     s_running         = false;
static bool     s_start_error     = false;
static int16_t  s_last_radio_err  = 0;
static uint32_t s_hop_start_ms    = 0;

// Snapshot of the prior SX1262 consumer so stop_analysis() can resume
// whoever was using the radio before the analyzer barged in. Only one of
// these can be true at a time because pager / TPMS / APRS each take the
// radio exclusively, but tracking all three independently keeps the
// restore branchless and obvious.
static bool      s_prev_pager_running   = false;
static float     s_prev_pager_freq      = 0.0f;
static PagerMode s_prev_pager_mode      = PAGER_POCSAG_1200;
static bool      s_prev_pager_scan_all  = false;
static bool      s_prev_tpms_running    = false;
static bool      s_prev_tpms_433        = true;
static bool      s_prev_aprs_running    = false;

// ---- helpers ---------------------------------------------------------------

static float freq_of_bin(int band, int bin)
{
    return s_bands[band].start_mhz + bin * s_bands[band].step_mhz;
}

static void reset_data()
{
    for (int i = 0; i < N_BINS; i++) {
        s_rssi[i] = RSSI_FLOOR;
        s_peak[i] = RSSI_FLOOR;
    }
    s_cur_bin        = 0;
    s_strongest_bin  = 0;
    s_strongest_rssi = -127;
}

static void update_bin_labels()
{
    for (int i = 0; i < N_BINS; i++) {
        if (i % LABEL_EVERY != 0) {
            lv_obj_add_flag(bin_labels[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(bin_labels[i], LV_OBJ_FLAG_HIDDEN);
        char buf[12];
        // %g picks the shorter of %f / %e and drops trailing zeros — gives
        // "902" for the 915 band, "863.5" for the 868 band, etc.
        snprintf(buf, sizeof(buf), "%g", (double)freq_of_bin(s_band, i));
        lv_label_set_text(bin_labels[i], buf);
    }
}

// ---- radio lifecycle -------------------------------------------------------

static bool start_analysis()
{
    if (s_running) return true;
    s_start_error    = false;
    s_last_radio_err = 0;

    if (lora_screen_is_powered()) {
        // Meshtastic owns the radio — we refuse rather than fight for it.
        s_start_error = true;
        return false;
    }

    // Snapshot whoever currently owns the SX1262 so stop_analysis() can
    // hand the radio back to them on exit. Has to happen before we call
    // their _stop() functions below — otherwise we'd record the cleared
    // state and never resume.
    s_prev_pager_running  = pager_is_running();
    s_prev_pager_freq     = pager_get_freq();
    s_prev_pager_mode     = pager_get_mode();
    s_prev_pager_scan_all = pager_is_scanning_all();
    s_prev_tpms_running   = tpms_is_running();
    s_prev_tpms_433       = tpms_is_freq_433();
    s_prev_aprs_running   = aprs_is_running();

    // FSK scanners and APRS share the SX1262; standby them so they don't
    // re-tune the radio out from under us.
    pager_stop();
    tpms_stop();
    aprs_stop();

    instance.powerControl(POWER_RADIO, true);

    // Wide LoRa bandwidth maximises the per-sample frequency coverage; the SF
    // and CR settings are irrelevant here because we're never receiving a
    // payload, only reading the instantaneous channel RSSI.
    int16_t rc = radio.begin(freq_of_bin(s_band, 0),
                             500.0f,    // 500 kHz bandwidth (widest LoRa BW)
                             7, 5, 0x12, 14, 8, 1.6f);
    s_last_radio_err = rc;
    if (rc != RADIOLIB_ERR_NONE) {
        s_start_error = true;
        return false;
    }

    radio.startReceive();
    reset_data();
    s_hop_start_ms = millis();
    s_running      = true;
    return true;
}

static void stop_analysis()
{
    if (!s_running) return;
    s_running = false;
    radio.standby();
    // Mirrors the APRS / pager / TPMS convention: leave POWER_RADIO on so
    // those features can still claim the radio without re-powering the rail.
    // Only the LoRa-screen "off" toggle drops the rail.

    // Restore whoever owned the SX1262 before the analyzer started. At
    // most one of these will fire because the three FSK consumers each
    // hold the radio exclusively. Order is arbitrary — none can succeed
    // while another is already holding the radio, and only one can have
    // been running pre-snapshot.
    if (s_prev_pager_running) {
        if (s_prev_pager_scan_all) pager_start_scanner(s_prev_pager_mode);
        else                       pager_start(s_prev_pager_freq, s_prev_pager_mode);
    }
    if (s_prev_tpms_running) tpms_start(s_prev_tpms_433);
    if (s_prev_aprs_running) aprs_start();

    s_prev_pager_running = false;
    s_prev_tpms_running  = false;
    s_prev_aprs_running  = false;
}

// ---- per-tick sweep --------------------------------------------------------

static int8_t read_rssi()
{
    float r = radio.getRSSI(false);    // false = instantaneous, not packet RSSI
    if (r > 0)    r = 0;
    if (r < -127) r = -127;
    return (int8_t)r;
}

// Called once per HOP_MS while the analyzer is running. Reads the RSSI at the
// currently-tuned bin, then retunes to the next one so the radio has the
// settling window between ticks.
static void hop_one()
{
    int8_t r = read_rssi();
    s_rssi[s_cur_bin] = r;
    if (r > s_peak[s_cur_bin]) s_peak[s_cur_bin] = r;
    if (r > s_strongest_rssi) {
        s_strongest_rssi = r;
        s_strongest_bin  = s_cur_bin;
    }
    s_cur_bin = (s_cur_bin + 1) % N_BINS;
    // Retune AND re-arm RX. setFrequency drops the SX1262 to standby, so without
    // a fresh startReceive() every getRSSI() after the first hop reads a stale
    // floor value (= flat chart). skipCalibration=true: the band was calibrated
    // in begin(); intra-band hops don't need it (band switches recalibrate).
    radio.setFrequency(freq_of_bin(s_band, s_cur_bin), true);
    radio.startReceive();
}

// ---- redraw ----------------------------------------------------------------

static lv_color_t rssi_color(int8_t r)
{
    if (r > -50)  return lv_color_make(0xFF, 0x44, 0x44);   // strong (red)
    if (r > -65)  return lv_color_make(0xFF, 0x88, 0x44);   // hot   (orange)
    if (r > -80)  return lv_color_make(0xFF, 0xCC, 0x00);   // busy  (yellow)
    if (r > -95)  return lv_color_make(0x44, 0xCC, 0x44);   // weak  (green)
    if (r > -110) return lv_color_make(0x00, 0xCC, 0xCC);   // quiet (cyan)
    return                lv_color_make(0x44, 0x66, 0x99);   // floor (blue)
}

static void update_bars()
{
    int chart_h = CHART_BOTTOM_Y - CHART_TOP_Y;
    for (int i = 0; i < N_BINS; i++) {
        int rssi = (int)s_rssi[i];
        int h;
        if (rssi <= RSSI_FLOOR) {
            h = 2;
        } else {
            int range = RSSI_CEIL - RSSI_FLOOR;
            h = (rssi - RSSI_FLOOR) * chart_h / range;
            if (h < 2)       h = 2;
            if (h > chart_h) h = chart_h;
        }
        lv_obj_set_pos(bars[i], bar_x[i], CHART_BOTTOM_Y - h);
        lv_obj_set_height(bars[i], h);
        if (rssi > RSSI_FLOOR) {
            lv_obj_set_style_bg_color(bars[i], rssi_color((int8_t)rssi), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(bars[i], LV_OPA_COVER, LV_PART_MAIN);
        } else {
            lv_obj_set_style_bg_color(bars[i], lv_color_make(0x33, 0x33, 0x33), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(bars[i], LV_OPA_70, LV_PART_MAIN);
        }
        bool active = (i == s_cur_bin) && s_running;
        lv_obj_set_style_border_color(bars[i], lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_border_width(bars[i], active ? 2 : 0, LV_PART_MAIN);
    }
}

static void update_status()
{
    if (s_start_error || !s_running) {
        if (lora_screen_is_powered()) {
            lv_label_set_text(status_label, "LoRa radio in use - stop Meshtastic");
            lv_obj_set_style_text_color(status_label,
                lv_color_make(0xFF, 0xAA, 0x00), LV_PART_MAIN);
        } else if (s_last_radio_err != 0) {
            lv_label_set_text_fmt(status_label, "Radio init failed (%d)",
                                  (int)s_last_radio_err);
            lv_obj_set_style_text_color(status_label,
                lv_color_make(0xFF, 0x55, 0x55), LV_PART_MAIN);
        } else {
            lv_label_set_text(status_label, "Stopped");
            lv_obj_set_style_text_color(status_label,
                ARGUS_TEXT_DIM, LV_PART_MAIN);
        }
        return;
    }
    if (s_strongest_rssi > -127) {
        lv_label_set_text_fmt(status_label,
            "%s   peak %d dBm @ %g MHz",
            s_bands[s_band].label,
            (int)s_strongest_rssi,
            (double)freq_of_bin(s_band, s_strongest_bin));
        lv_obj_set_style_text_color(status_label,
            ARGUS_TEXT, LV_PART_MAIN);
    } else {
        lv_label_set_text_fmt(status_label,
            "scanning %s", s_bands[s_band].label);
        lv_obj_set_style_text_color(status_label,
            ARGUS_TEXT, LV_PART_MAIN);
    }
}

static void update_legend()
{
    if (!s_running) { lv_label_set_text(legend_label, ""); return; }
    lv_label_set_text_fmt(legend_label,
        "current %g MHz   bin %d/%d",
        (double)freq_of_bin(s_band, s_cur_bin),
        s_cur_bin + 1, N_BINS);
}

// ---- events ----------------------------------------------------------------

static void on_timer(lv_timer_t *)
{
    if (s_running && (millis() - s_hop_start_ms >= HOP_MS)) {
        s_hop_start_ms = millis();
        hop_one();
    }
    update_bars();
    update_status();
    update_legend();
}

static void on_band(lv_event_t *)
{
    s_band = (s_band + 1) % N_BANDS;
    lv_label_set_text(band_btn_label, s_bands[s_band].short_label);
    reset_data();
    update_bin_labels();
    if (s_running) {
        radio.setFrequency(freq_of_bin(s_band, 0));   // band jump → recalibrate
        radio.startReceive();                         // re-arm RX after retune
        s_cur_bin      = 0;
        s_hop_start_ms = millis();
    }
}

// Swipe RIGHT returns to the BT analyzer (the chain's neighbour to the left).
// No LEFT handler — LoRa is the end of the analyzer chain.
static void on_gesture(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir == LV_DIR_RIGHT) {
        stop_analysis();
        bt_analyze_screen_show();
    }
}

// ---- layout ----------------------------------------------------------------

static lv_obj_t *make_band_btn()
{
    lv_obj_t *b = lv_obj_create(screen);
    lv_obj_set_size(b, 100, 38);
    lv_obj_set_style_radius(b, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(b, lv_color_make(0x22, 0x44, 0x66), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(b, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(b, 0, LV_PART_MAIN);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(b, LV_ALIGN_BOTTOM_MID, 0, -53);
    lv_obj_add_event_cb(b, on_band, LV_EVENT_CLICKED, NULL);

    band_btn_label = lv_label_create(b);
    lv_label_set_text(band_btn_label, s_bands[s_band].short_label);
    lv_obj_set_style_text_color(band_btn_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(band_btn_label, &font_argus_label_16, LV_PART_MAIN);
    lv_obj_center(band_btn_label);
    return b;
}

void lora_analyze_screen_create()
{
    screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(screen, 0, LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(screen, 0, LV_PART_MAIN);

    band_btn = make_band_btn();

    title_label = lv_label_create(screen);
    lv_obj_set_style_text_color(title_label, argus_base_accent(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title_label, &font_argus_ui, LV_PART_MAIN);
    lv_label_set_text(title_label, "LoRa");
    // Anchored at the top centre to match the PAGER / TPMS / SETTINGS
    // screens. font_48 extends to ~y=56 from this origin; status/legend
    // below stay where they are with breathing room between them.
    lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 8);

    status_label = lv_label_create(screen);
    lv_obj_set_style_text_color(status_label, ARGUS_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(status_label, &font_argus_label_14, LV_PART_MAIN);
    lv_label_set_text(status_label, "starting...");
    lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 88);

    legend_label = lv_label_create(screen);
    lv_obj_set_style_text_color(legend_label, ARGUS_TEXT_DIM, LV_PART_MAIN);
    lv_obj_set_style_text_font(legend_label, &font_argus_label_14, LV_PART_MAIN);
    lv_label_set_text(legend_label, "");
    lv_obj_align(legend_label, LV_ALIGN_TOP_MID, 0, 108);

    int total_w = N_BINS * BAR_W + (N_BINS - 1) * BAR_GAP;
    int start_x = (410 - total_w) / 2;

    for (int i = 0; i < N_BINS; i++) {
        int x = start_x + i * (BAR_W + BAR_GAP);
        bar_x[i] = x;

        bars[i] = lv_obj_create(screen);
        lv_obj_set_size(bars[i], BAR_W, 2);
        lv_obj_set_style_bg_color(bars[i], lv_color_make(0x33, 0x33, 0x33), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(bars[i], LV_OPA_70, LV_PART_MAIN);
        lv_obj_set_style_radius(bars[i], 3, LV_PART_MAIN);
        lv_obj_set_style_pad_all(bars[i], 0, LV_PART_MAIN);
        lv_obj_set_style_border_width(bars[i], 0, LV_PART_MAIN);
        lv_obj_clear_flag(bars[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(bars[i], x, CHART_BOTTOM_Y - 2);

        bin_labels[i] = lv_label_create(screen);
        lv_obj_set_style_text_color(bin_labels[i], ARGUS_TEXT, LV_PART_MAIN);
        lv_obj_set_style_text_font(bin_labels[i], &lv_font_montserrat_14, LV_PART_MAIN);
        lv_label_set_text(bin_labels[i], "");
        lv_obj_set_pos(bin_labels[i], x - 4, CHART_BOTTOM_Y + 4);
    }
    update_bin_labels();

    lv_obj_t *base = lv_obj_create(screen);
    lv_obj_set_size(base, total_w, 1);
    lv_obj_set_style_bg_color(base, lv_color_make(0x44, 0x44, 0x44), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(base, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(base, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(base, 0, LV_PART_MAIN);
    lv_obj_clear_flag(base, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(base, start_x, CHART_BOTTOM_Y);

    lv_obj_t *axis_hint = lv_label_create(screen);
    lv_obj_set_style_text_color(axis_hint, ARGUS_TEXT_DIM, LV_PART_MAIN);
    lv_obj_set_style_text_font(axis_hint, &font_argus_label_14, LV_PART_MAIN);
    lv_label_set_text(axis_hint, "MHz   -   tap band to switch");
    lv_obj_align(axis_hint, LV_ALIGN_BOTTOM_MID, 0, -100);

    lv_obj_add_event_cb(screen, on_gesture, LV_EVENT_GESTURE, NULL);
    lv_timer_create(on_timer, HOP_MS, NULL);
}

void lora_analyze_screen_show()
{
    start_analysis();    // failure leaves the screen up with a status hint
    update_status();
    update_legend();
    update_bars();
    lv_scr_load(screen);
}

bool lora_analyze_screen_is_active()
{
    return lv_screen_active() == screen;
}

bool lora_analyze_is_running()
{
    return s_running;
}

void lora_analyze_screen_stop()
{
    // No-op when the analyzer isn't running; safe for the back-button
    // handler to call unconditionally on any analyze screen.
    stop_analysis();
}
