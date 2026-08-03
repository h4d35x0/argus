#include <Arduino.h>
#include "theme.h"
#include <LilyGoLib.h>
#include <LV_Helper.h>
#include <bosch/BoschSensorDataHelper.hpp>
#include "esp_wifi.h"
#include "esp_bt.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include <SD.h>
#include <time.h>
#include <math.h>
#include "gps_screen.h"
#include "lora_screen.h"
#include "meshtastic.h"
#include "meshtastic_screen.h"
#include "nodes_screen.h"
#include "send_message_screen.h"
#include "map_screen.h"
#include "configuration_screen.h"
#include "channels_screen.h"
#include "settings_screen.h"
#include "screenshot.h"
#include "tools_screen.h"
#include "notifications_screen.h"
#include "notify_popup.h"
#include "device_mode.h"
#include "pet_screen.h"
#include "handshake.h"
#include "tpms_screen.h"
#include "timezone.h"
#include "tpms.h"
#include "pager_screen.h"
#include "pager.h"
#include "mouse_screen.h"
#include "usb_sd.h"
#include "usb_sd_screen.h"
#include "aprs.h"
#include "aprs_screen.h"
#include "tesla_cp_screen.h"
#include "wifi_screen.h"
#include "wifi_radio_screen.h"
#include "bluetooth_screen.h"
#include "analyze_screen.h"
#include "bt_analyze_screen.h"
#include "lora_analyze_screen.h"
#include "pingsweep.h"
#include "portscan.h"
#include "portscan_screen.h"
#include "wardriver_screen.h"
#include "nfc_screen.h"
#include "nfc_write_screen.h"
#include "stopwatch_screen.h"
#include "timer_screen.h"
#include "alarm.h"
#include "alarm_screen.h"
#include "calendar_screen.h"
#include "world_clock_screen.h"
#include "sun_moon_screen.h"
#include "time_screen.h"
#include "flashlight_screen.h"
#include "argus_mode.h"
#include "spycam_screen.h"
#include "nfc_field_screen.h"
#include "pin_pad_screen.h"
#include "loot_screen.h"
#include "deauth_screen.h"
#include "tracker_timeline_screen.h"
#include "beacon_spam.h"
#include "deauth_attack.h"
#include "rogue_ap.h"
#include "probe_sniffer.h"
#include "offense_wipe.h"
#include "airtag.h"
#include "flipper.h"
#include "skimmer.h"
#include "evil_twin.h"
#include "flock.h"
#include "threat_radar.h"
#include "hexhound.h"
#include "ble_scan_manager.h"
#include "radio_coexist.h"
#include "boot_prefs.h"
#include "charge_state.h"   // debounced Discharging/Charging/Topped readout
#include "detect_log_sd.h"   // detection-log retention sweep
#include <esp_wifi.h>   // esp_wifi_get_mode() for live WiFi-mode reads
#include "threat_radar_screen.h"
#include "tracker_rep.h"
#include "counter_tail.h"
#include "matrix_bg.h"
#include "background.h"
#include "nfc_icon.h"
#include "detect_pipeline.h"
#include "ble_detect_pipeline.h"
// WiFi threat-detection pipeline (evil-twin + beacon-flood -> HADES-red + log).
// ON, using PIGGYBACK activation (detect_pipeline.cpp): the pipeline attaches its
// beacon consumer ONLY while another WiFi scan (Evil Twin / Flock / Pwn /
// wardriver) is already running, and detaches when none remain. So it never
// powers WiFi on by itself, never flips a connected STA into monitor mode, and
// adds nothing to the boot path - it just enriches scans the user already started
// with threat posture (HADES-red accent + HexHound) and a forensic log.
#define ARGUS_WIFI_THREAT_PIPELINE 1
// BLE anti-stalking pipeline (tracker_ident + tail_detect, piggyback on the BLE
// scan manager). Starts OFF: flip to 1 only after an on-device battery test that
// it piggybacks cleanly (toggle AirTag/Flipper ON to bring BLE up) with no
// boot-loop. See ble_detect_pipeline.h.
#define ARGUS_BLE_THREAT_PIPELINE  1

#if ARGUS_COEX_MEASURE
static void coex_log_heap(const char *phase)
{
    unsigned largest_internal = (unsigned)heap_caps_get_largest_free_block(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    unsigned free_internal = (unsigned)heap_caps_get_free_size(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    unsigned largest_dma = (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    unsigned bt_status = (unsigned)esp_bt_controller_get_status();

    Serial.printf(
        "[COEX] phase=%s ms=%lu largest_internal=%u free_internal=%u "
        "largest_dma=%u bt_status=%u\n",
        phase, (unsigned long)millis(), largest_internal, free_internal,
        largest_dma, bt_status);
    Serial.flush();

    if (!instance.isCardReady()) return;
    File f = SD.open("/Settings/coexlog.txt", FILE_APPEND);
    if (!f) return;
    f.printf(
        "phase=%s ms=%lu reset=%u largest_internal=%u free_internal=%u "
        "largest_dma=%u bt_status=%u\n",
        phase, (unsigned long)millis(), (unsigned)esp_reset_reason(),
        largest_internal, free_internal, largest_dma, bt_status);
    f.flush();
    f.close();
}
#else
static inline void coex_log_heap(const char *) {}
#endif

// Modal dialog helper; defined lower down (near the low-mem section) but called
// from setup()'s boot-radio block above it, so it needs an early prototype.
void low_mem_show_dialog(const char *msg);


static lv_obj_t *clock_screen;
static lv_obj_t *time_label;
static lv_span_t *s_span_hours;
static lv_span_t *s_span_colon;
static lv_span_t *s_span_rest;
static lv_span_t *s_span_ampm;   // AM/PM suffix in a smaller font than the digits
static lv_obj_t *date_label;
static lv_obj_t *gps_indicator;
static lv_obj_t *wardriver_container;
static lv_obj_t *wardriver_wifi_label;
static lv_obj_t *wardriver_bt_label;
static lv_obj_t *wifi_indicator;
static lv_obj_t *bt_indicator;
static lv_obj_t *sd_indicator;
static lv_obj_t *nfc_indicator;
static lv_obj_t *lora_container;
static lv_obj_t *lora_arc;
static lv_obj_t *lora_ball;
static lv_obj_t *lora_stick;
static lv_obj_t *analog_container;
static lv_obj_t *hand_hour;
static lv_obj_t *hand_min;
static lv_obj_t *hand_sec;
static bool      analog_face = false;
static uint32_t last_update_ms   = 0;
static int      clock_utc_offset = -4; // hours; default US Eastern (EDT).
                                       // The RTC holds UTC; GPS fix refines this
                                       // (incl. DST) from longitude when available.
static bool     manual_time_override = false; // user-set time; blocks GPS sync
static bool     clock_12h        = true;
static bool     clock_show_day   = true;
static bool     clock_show_date  = true;
static bool     clock_show_ampm  = true;
static bool     clock_show_secs  = true;
static bool     clock_vibrate    = false;

// Battery widget objects
static lv_obj_t *bat_fill;
static lv_obj_t *bat_label;

// Charging bolt drawn inside the battery body, left of the percentage.
// Built from lv_line strokes rather than LV_SYMBOL_CHARGE: bat_label is
// styled with font_argus_mono_16, a VT323 subset that only covers U+0020..U+007E
// and declares no .fallback, so the U+F0E7 symbol glyph resolved to nothing
// and the charge state rendered as an invisible trailing space. Same reason
// the timer/stopwatch icons above are primitives.
static lv_obj_t *bat_bolt;

// Debounced charge readout feeding bat_bolt. Fed one sample per 1 Hz tick.
static ChargeIndicator bat_charge;
// Alternates each tick while charging to breathe the bolt's opacity.
static bool bolt_pulse_dim = false;
static void update_charge_bolt();

// Bell glyph shown to the left of the battery when the alarm is enabled.
static lv_obj_t *alarm_indicator;
static lv_obj_t *bat_nub;

// Small green icons drawn next to the battery while a timer is counting
// down or the stopwatch is running. Built from primitives (ring + knob +
// hand) because LV_SYMBOL_* has no clock glyph.
static lv_obj_t *timer_indicator;
static lv_obj_t *stopwatch_indicator;

// Compact unread badge shown in the top status row to the left of the
// LoRa icon. Hidden when unread count is zero. The previous bottom-row
// envelope + count duplicated this number, so it was removed.
static lv_obj_t *mesh_top_count_label;

// AirTag sniffer indicator (above battery, shown only while scanner is active)
static lv_obj_t *airtag_indicator;
static lv_obj_t *airtag_count_label;

// Flipper Zero detector indicator (next to AirTag indicator)
static lv_obj_t *flipper_indicator;
static lv_obj_t *flipper_count_label;

// Card-skimmer detector indicator (next to Flipper indicator)
static lv_obj_t *skimmer_indicator;
static lv_obj_t *skimmer_count_label;

// Evil-twin WiFi attack indicator (next to Skimmer indicator)
static lv_obj_t *evil_twin_indicator;
static lv_obj_t *evil_twin_count_label;

// Flock/OUI surveillance-device indicator (left of AirTag indicator)
static lv_obj_t *flock_indicator;
static lv_obj_t *flock_count_label;

// Battery body geometry (pixels)
static constexpr int BAT_W      = 60;                       // halved from 120
static constexpr int BAT_H      = 20;
static constexpr int BAT_BORDER = 2;
static constexpr int BAT_INNER_W = BAT_W - 2 * BAT_BORDER;  // 56
static constexpr int BAT_INNER_H = BAT_H - 2 * BAT_BORDER;  // 16
static constexpr int BAT_NUB_W  = 10;
static constexpr int BAT_NUB_H  = 10;                       // scaled with body

static lv_color_t bat_color(int pct)
{
    // Dimmed ~50% from the previous palette so the fill doesn't dominate
    // the clock face. Same hue intent (green / yellow / red / dark red),
    // just lower luminance.
    if (pct >= 51) return lv_color_make(0x11, 0x66, 0x11); // dim green   (51-100)
    if (pct >= 35) return lv_color_make(0x99, 0x77, 0x00); // dim yellow  (35-50)
    if (pct >= 15) return lv_color_make(0x99, 0x22, 0x22); // dim red     (15-34)
    return             lv_color_make(0x55, 0x00, 0x00);    // very dark red (0-14)
}

static void build_battery_widget(lv_obj_t *screen)
{
    // Transparent container sized to hold body + nub
    lv_obj_t *container = lv_obj_create(screen);
    lv_obj_set_size(container, BAT_W + BAT_NUB_W, BAT_H);
    lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(container, 0, LV_PART_MAIN);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(container, LV_ALIGN_BOTTOM_MID, 0, -10);

    // Battery body: black fill, white border, rounded corners
    lv_obj_t *bat_body = lv_obj_create(container);
    lv_obj_set_pos(bat_body, 0, 0);
    lv_obj_set_size(bat_body, BAT_W, BAT_H);
    lv_obj_set_style_bg_color(bat_body, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bat_body, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(bat_body, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_border_width(bat_body, BAT_BORDER, LV_PART_MAIN);
    lv_obj_set_style_radius(bat_body, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bat_body, 0, LV_PART_MAIN);
    lv_obj_clear_flag(bat_body, LV_OBJ_FLAG_SCROLLABLE);

    // Fill bar — child of bat_body; positioned inside border, grows left-to-right.
    // Width is updated each second by update_battery().
    bat_fill = lv_obj_create(bat_body);
    lv_obj_set_pos(bat_fill, 0, 0);
    lv_obj_set_size(bat_fill, 0, BAT_INNER_H);
    lv_obj_set_style_bg_color(bat_fill, bat_color(100), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bat_fill, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(bat_fill, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(bat_fill, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bat_fill, 0, LV_PART_MAIN);

    // Percentage label — drawn after fill so it renders on top. Sized
    // down to Montserrat 14 to fit inside the halved (16 px inner)
    // battery body without clipping the digits.
    bat_label = lv_label_create(bat_body);
    lv_obj_set_style_text_color(bat_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(bat_label, &font_argus_mono_16, LV_PART_MAIN);   // VT323 (brand readout)
    lv_label_set_text(bat_label, "--");
    lv_obj_align(bat_label, LV_ALIGN_CENTER, 0, 0);

    // Charging bolt - a 3-segment zigzag stroked in white, parked against the
    // left inside edge of the body. Created after bat_label so it draws on
    // top of both the fill and the digits.
    //
    // It cannot actually collide with the digits: VT323 16 is monospace at
    // adv_w 96 (6 px/glyph), so the widest reading, "100%", is 24 px centred
    // in the 56 px inner width and spans x=16..40. The bolt lives in x=3..13.
    //
    // White, not ARGUS_ACCENT / status_accent_active(): those flip to
    // threat-red on a tail, and a battery bolt is chrome, not a live-threat
    // surface. It must mean the same thing in Defense and Offense.
    //
    // The line IS the widget (no wrapper container) so the pulse can drive
    // line_opa directly. Setting `opa` on a parent instead would make LVGL
    // composite the subtree through an intermediate layer on every redraw,
    // which is pure waste for four strokes.
    //
    // Points: down-left, jog right, down-left again - the standard zigzag.
    static lv_point_precise_t bolt_pts[] = { {8, 1}, {3, 7}, {7, 8}, {1, 15} };
    bat_bolt = lv_line_create(bat_body);
    lv_line_set_points(bat_bolt, bolt_pts, 4);
    lv_obj_set_pos(bat_bolt, 3, 0);
    lv_obj_set_style_line_color(bat_bolt, lv_color_white(), 0);
    lv_obj_set_style_line_width(bat_bolt, 2, 0);
    lv_obj_set_style_line_rounded(bat_bolt, true, 0);
    lv_obj_clear_flag(bat_bolt, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(bat_bolt, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_add_flag(bat_bolt, LV_OBJ_FLAG_HIDDEN);   // shown only on USB power

    // Terminal nub (positive terminal on the right)
    bat_nub = lv_obj_create(container);
    lv_obj_set_pos(bat_nub, BAT_W, (BAT_H - BAT_NUB_H) / 2);
    lv_obj_set_size(bat_nub, BAT_NUB_W, BAT_NUB_H);
    lv_obj_set_style_bg_color(bat_nub, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bat_nub, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(bat_nub, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(bat_nub, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bat_nub, 0, LV_PART_MAIN);
}

// 160×160 px analog clock. Hands are thin rectangles rotated around the
// clock center (80,80) using LVGL transform_rotation (tenths of degrees).
// build_analog_clock() creates the widget hidden; clock_screen_set_analog_face()
// toggles it and the digital time_label.
static void build_analog_clock(lv_obj_t *screen)
{
    analog_container = lv_obj_create(screen);
    lv_obj_set_size(analog_container, 320, 320);
    lv_obj_set_style_bg_opa(analog_container, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(analog_container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(analog_container, 0, LV_PART_MAIN);
    lv_obj_clear_flag(analog_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(analog_container, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_align(analog_container, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(analog_container, LV_OBJ_FLAG_HIDDEN); // hidden until analog mode is on

    // Subtle clock face ring
    lv_obj_t *face = lv_obj_create(analog_container);
    lv_obj_set_pos(face, 1, 1);
    lv_obj_set_size(face, 316, 316);
    lv_obj_set_style_radius(face, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(face, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_color(face, lv_color_make(0x44, 0x44, 0x44), LV_PART_MAIN);
    lv_obj_set_style_border_width(face, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(face, 0, LV_PART_MAIN);
    lv_obj_clear_flag(face, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(face, LV_OBJ_FLAG_CLICKABLE);

    // Hour hand: 4×60 px (tip=48, tail=12). Pivot at (2,48) → abs (80,80).
    hand_hour = lv_obj_create(analog_container);
    lv_obj_set_pos(hand_hour, 156, 64);   // 80-2=78, 80-48=32
    lv_obj_set_size(hand_hour, 8, 120);
    lv_obj_set_style_bg_color(hand_hour, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(hand_hour, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(hand_hour, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(hand_hour, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(hand_hour, 0, LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_x(hand_hour, 4, LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_y(hand_hour, 96, LV_PART_MAIN);
    lv_obj_clear_flag(hand_hour, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(hand_hour, LV_OBJ_FLAG_CLICKABLE);

    // Minute hand: 4×88 px (tip=70, tail=18). Pivot at (2,70) → abs (80,80).
    hand_min = lv_obj_create(analog_container);
    lv_obj_set_pos(hand_min, 156, 20);    // 80-2=78, 80-70=10
    lv_obj_set_size(hand_min, 8, 176);
    lv_obj_set_style_bg_color(hand_min, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(hand_min, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(hand_min, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(hand_min, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(hand_min, 0, LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_x(hand_min, 4, LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_y(hand_min, 140, LV_PART_MAIN);
    lv_obj_clear_flag(hand_min, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(hand_min, LV_OBJ_FLAG_CLICKABLE);

    // Second hand: 2×100 px (tip=78, tail=22). Pivot at (1,78) → abs (80,80). Red.
    hand_sec = lv_obj_create(analog_container);
    lv_obj_set_pos(hand_sec, 158, 4);     // 80-1=79, 80-78=2
    lv_obj_set_size(hand_sec, 4, 200);
    lv_obj_set_style_bg_color(hand_sec, lv_color_make(0xFF, 0x00, 0x00), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(hand_sec, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(hand_sec, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(hand_sec, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(hand_sec, 0, LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_x(hand_sec, 2, LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_y(hand_sec, 156, LV_PART_MAIN);
    lv_obj_clear_flag(hand_sec, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(hand_sec, LV_OBJ_FLAG_CLICKABLE);

    // Center cap drawn last so it sits on top of all hands
    lv_obj_t *dot = lv_obj_create(analog_container);
    lv_obj_set_pos(dot, 154, 154);         // 80-3=77
    lv_obj_set_size(dot, 12, 12);
    lv_obj_set_style_bg_color(dot, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(dot, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_pad_all(dot, 0, LV_PART_MAIN);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
}

static void update_analog_clock(const struct tm *t)
{
    // Angles in tenths of degrees, clockwise from 12 o'clock.
    // Hour:   30°/h  → 300 tenths/h,  + 0.5°/min  → 5 tenths/min
    // Minute:  6°/min → 60 tenths/min, + 0.1°/sec  → 1 tenth/sec
    // Second:  6°/sec → 60 tenths/sec
    int32_t h = (int32_t)(t->tm_hour % 12) * 300 + t->tm_min * 5;
    int32_t m = t->tm_min * 60 + t->tm_sec;
    int32_t s = t->tm_sec * 60;
    lv_obj_set_style_transform_rotation(hand_hour, h, LV_PART_MAIN);
    lv_obj_set_style_transform_rotation(hand_min,  m, LV_PART_MAIN);
    lv_obj_set_style_transform_rotation(hand_sec,  s, LV_PART_MAIN);
}

// Status-bar "active" accent, threat-aware (ARGUS -> HADES). Normally the
// bright steel-blue ARGUS_ACCENT_ACTIVE; the instant Threat Radar flags a tail
// (top level >= TR_LVL_LIKELY) every live status icon flips to HADES red, so a
// glance at the clock face shows the watch has "opened its red eyes". Refreshed
// each second by the status-icon update loop, so it flips live and clears on its
// own once the tail goes stale. Shares argus_accent()'s exact threshold so all
// themed surfaces agree.
static inline lv_color_t status_accent_active()
{
    return threatradar_top_level() >= TR_LVL_LIKELY ? HADES_RED : ARGUS_ACCENT_ACTIVE;
}

static void update_lora_indicator()
{
    // Green whenever the shared SX1262 radio is in use — by LoRa/meshtastic,
    // the pager scanner, the TPMS scanner, APRS, or the LoRa analyzer.
    bool in_use = lora_screen_is_powered() || pager_is_running()
               || tpms_is_running() || aprs_is_running()
               || lora_analyze_is_running();
    lv_color_t color = in_use
        ? status_accent_active()
        : lv_color_make(0x33, 0x33, 0x33);
    lv_obj_set_style_arc_color(lora_arc,  color, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(lora_ball,  color, LV_PART_MAIN);
    lv_obj_set_style_bg_color(lora_stick, color, LV_PART_MAIN);
}

// Antenna icon: half-circle (∩) above a ball on a stick.
// All three parts are children of lora_container so realign_status_icons()
// can move the whole widget as one unit.
static void build_lora_indicator(lv_obj_t *screen)
{
    lora_container = lv_obj_create(screen);
    lv_obj_set_size(lora_container, 24, 36);
    lv_obj_set_style_bg_opa(lora_container, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(lora_container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(lora_container, 0, LV_PART_MAIN);
    lv_obj_clear_flag(lora_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(lora_container, LV_ALIGN_TOP_RIGHT, -195, 54); // placeholder; realign() overrides

    // Stick: 2×12 px, centered at x=12, from y=24 down
    lora_stick = lv_obj_create(lora_container);
    lv_obj_set_pos(lora_stick, 11, 24);
    lv_obj_set_size(lora_stick, 2, 12);
    lv_obj_set_style_bg_color(lora_stick, lv_color_make(0x33,0x33,0x33), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(lora_stick, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(lora_stick, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(lora_stick, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(lora_stick, 0, LV_PART_MAIN);

    // Ball: 8×8 circle, center at (12, 20)
    lora_ball = lv_obj_create(lora_container);
    lv_obj_set_pos(lora_ball, 8, 16);
    lv_obj_set_size(lora_ball, 8, 8);
    lv_obj_set_style_bg_color(lora_ball, lv_color_make(0x33,0x33,0x33), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(lora_ball, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(lora_ball, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(lora_ball, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(lora_ball, 0, LV_PART_MAIN);

    // Arc: top half ∩, center at (12, 20), radius 11.
    // LVGL angles: 0°=east, 90°=south, 180°=west, 270°=north — clockwise.
    // 180°→360° traces west→north→east = top half ∩.
    lora_arc = lv_arc_create(lora_container);
    lv_obj_set_pos(lora_arc, 1, 9);
    lv_obj_set_size(lora_arc, 22, 22);
    lv_obj_set_style_bg_opa(lora_arc, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(lora_arc, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(lora_arc, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(lora_arc, 0, LV_PART_MAIN);
    lv_arc_set_angles(lora_arc, 180, 360);
    lv_arc_set_bg_angles(lora_arc, 0, 360);
    lv_obj_set_style_arc_color(lora_arc, lv_color_make(0x33,0x33,0x33), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(lora_arc, 2, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(lora_arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_clear_flag(lora_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(lora_arc, LV_OBJ_FLAG_SCROLLABLE);
}

// Chains all status icons left of GPS, accommodating GPS label width changes
// (e.g. when satellite count is appended). Call after updating GPS label text
// and after lv_obj_update_layout(clock_screen).
static void realign_status_icons()
{
    lv_obj_update_layout(clock_screen);
    lv_obj_align_to(wardriver_container, gps_indicator,        LV_ALIGN_OUT_LEFT_MID, -5, 0);
    lv_obj_align_to(wifi_indicator,      wardriver_container, LV_ALIGN_OUT_LEFT_MID, -5, 0);
    lv_obj_align_to(bt_indicator,        wifi_indicator,      LV_ALIGN_OUT_LEFT_MID, -5, 0);
    lv_obj_align_to(sd_indicator,        bt_indicator,        LV_ALIGN_OUT_LEFT_MID, -5, 0);
    lv_obj_align_to(nfc_indicator,       sd_indicator,        LV_ALIGN_OUT_LEFT_MID, -5, 0);
    lv_obj_align_to(lora_container,      nfc_indicator,       LV_ALIGN_OUT_LEFT_MID, -5, 0);
    if (mesh_top_count_label) {
        lv_obj_align_to(mesh_top_count_label, lora_container, LV_ALIGN_OUT_LEFT_MID, -4, 0);
    }
}

static void update_bt_indicator()
{
    bool on = btStarted();
    lv_color_t color = on
        ? status_accent_active()  // steel-blue — BT active (HADES red on a tail)
        : lv_color_make(0x33, 0x33, 0x33); // gray  — BT off
    lv_obj_set_style_text_color(bt_indicator, color, LV_PART_MAIN);
}

static void update_wifi_indicator()
{
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&mode);
    bool on = (mode != WIFI_MODE_NULL);
    lv_color_t color = on
        ? status_accent_active()  // steel-blue — radio active (HADES red on a tail)
        : lv_color_make(0x33, 0x33, 0x33); // gray  — radio off
    lv_obj_set_style_text_color(wifi_indicator, color, LV_PART_MAIN);
}

// Tracks whether the SD was successfully mounted the last time we checked.
// Initialized in setup() to match the state after instance.begin().
static bool sd_was_ready = false;

static void update_sd_indicator()
{
    // Combined "is the SD writable by us?" — both insertion and USB-SD
    // claim state matter. Tracked here (rather than just sd_was_ready)
    // so the settings nudge below also fires when the host computer
    // mounts/unmounts the card over USB-MSC.
    bool prev_usable = sd_was_ready && !usb_sd_is_running();
    bool prev_ready  = sd_was_ready;
    if (sd_was_ready) {
        // Use the physical card-detect pin on XL9555 IO10 (active-LOW) for removal.
        // This is a live I2C read, unlike SD.sectorSize() which is cached at mount time.
        if (instance.io.digitalRead(EXPANDS_SD_DET) == HIGH) {
            instance.uninstallSD();
            sd_was_ready = false;
        }
    } else {
        // Poll for insertion: installSD() reads the physical card-detect pin on
        // XL9555 IO10 before attempting SD.begin(), so it returns false fast
        // when no card is present without stressing the SPI bus.
        if (instance.installSD()) {
            sd_was_ready = true;
        }
    }
    lv_color_t color = sd_was_ready
        ? status_accent_active()  // steel-blue — card mounted (HADES red on a tail)
        : lv_color_make(0x33, 0x33, 0x33); // gray  — no card
    lv_obj_set_style_text_color(sd_indicator, color, LV_PART_MAIN);

    // On any SD-usability state edge (card insert/eject *or* USB-SD
    // claim/release), nudge the settings screen so it can grey out /
    // re-enable the "Screenshot long press" toggle accordingly.
    bool now_usable = sd_was_ready && !usb_sd_is_running();
    if (now_usable != prev_usable || sd_was_ready != prev_ready)
        settings_screen_apply_sd_state();
}

static void update_nfc_indicator()
{
    bool on = instance.pmu.isEnableDLDO1();
    lv_color_t color = on
        ? status_accent_active()
        : lv_color_make(0x33, 0x33, 0x33);
    lv_obj_set_style_image_recolor(nfc_indicator, color, LV_PART_MAIN);
    lv_obj_set_style_image_recolor_opa(nfc_indicator, LV_OPA_COVER, LV_PART_MAIN);
}

static void update_battery()
{
    int pct = instance.pmu.getBatteryPercent();
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;

    lv_color_t color = bat_color(pct);

    lv_obj_set_width(bat_fill, (int32_t)pct * BAT_INNER_W / 100);
    lv_obj_set_style_bg_color(bat_fill, color, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bat_nub, color, LV_PART_MAIN);

    char buf[16];
    snprintf(buf, sizeof(buf), "%d%%", pct);
    lv_label_set_text(bat_label, buf);

    update_charge_bolt();
}

// Render the charging bolt from the debounced PMU state. Called from
// update_battery() on the 1 Hz tick, which is also what paces the pulse.
//
//   Discharging  bolt hidden
//   Charging     bolt pulses between full and 40% opacity, one step per tick
//   Topped       bolt steady at full opacity (on the charger, cell full)
//
// Two USB states rather than one because isCharging() alone goes false the
// moment the AXP2101 terminates, so a watch sitting full on the charger would
// look identical to one running on the cell. A slow 1 Hz breathe reads as
// "energy flowing in"; it is deliberately not a hard on/off blink, which on
// this watch face means fault or alert.
static void update_charge_bolt()
{
    if (!bat_bolt) return;

    ChargeState st = bat_charge.update(instance.pmu.isVbusIn(),
                                       instance.pmu.isCharging());

    // Only touch the hidden flag on an actual edge. lv_obj_add_flag()
    // invalidates whenever LV_OBJ_FLAG_HIDDEN is in the set, so re-asserting
    // it every tick would queue a redraw of this rect once a second for the
    // entire time the watch is off the charger.
    static ChargeState prev = ChargeState::Discharging;
    bool was_visible = prev != ChargeState::Discharging;
    bool is_visible  = st  != ChargeState::Discharging;
    prev = st;

    if (!is_visible) {
        if (was_visible) lv_obj_add_flag(bat_bolt, LV_OBJ_FLAG_HIDDEN);
        bolt_pulse_dim = false;
        return;
    }

    if (!was_visible) lv_obj_clear_flag(bat_bolt, LV_OBJ_FLAG_HIDDEN);

    if (st == ChargeState::Charging)
        bolt_pulse_dim = !bolt_pulse_dim;
    else
        bolt_pulse_dim = false;   // Topped: hold steady

    // Same edge-only discipline: setting a local style prop refreshes and
    // invalidates whether or not the value actually changed, and Topped can
    // sit unchanged for hours on the dock.
    lv_opa_t want = bolt_pulse_dim ? LV_OPA_40 : LV_OPA_COVER;
    static lv_opa_t applied = LV_OPA_TRANSP;   // never a legal rendered value
    if (want != applied || !was_visible) {
        lv_obj_set_style_line_opa(bat_bolt, want, LV_PART_MAIN);
        applied = want;
    }
}

// Pack the alarm / stopwatch / timer indicators flush against the left
// edge of the battery widget, right-to-left in priority order. The
// rightmost slot is filled first, then the next slot inward, etc., so
// whichever subset is currently enabled always sits in a tidy row up
// against the battery instead of leaving gaps at fixed positions.
//
// Order from rightmost (closest to battery) outward:
//   alarm  → stopwatch  → timer
// Run from the 1 s clock tick and any time enabled state changes.
static void layout_battery_indicators()
{
    if (!alarm_indicator || !stopwatch_indicator || !timer_indicator) return;

    struct Entry { lv_obj_t *obj; bool on; };
    Entry order[] = {
        { alarm_indicator,     alarm_is_enabled()    },
        { stopwatch_indicator, stopwatch_is_running() },
        { timer_indicator,     timer_is_running()    },
    };

    // Battery container is BAT_W + BAT_NUB_W = 130 px wide, centred on
    // BOTTOM_MID, so its left edge sits at x_offset = -65. Each indicator
    // gets a 26 px-wide slot (20 px icon + 6 px gap), with the rightmost
    // slot's centre 8 px outside the battery's left edge.
    constexpr int BAT_LEFT_OFFSET = -(BAT_W + BAT_NUB_W) / 2;  // -35
    constexpr int GAP             = 8;
    constexpr int SLOT_W          = 26;
    // Match the battery's BOTTOM_MID offset (-10) so the indicator
    // row's bottom edge sits flush with the battery's bottom edge,
    // sharing a baseline at the very bottom of the visible disc.
    constexpr int Y_OFFSET        = -10;

    int filled = 0;
    for (auto &e : order) {
        if (e.on) {
            int cx = BAT_LEFT_OFFSET - GAP - SLOT_W / 2 - filled * SLOT_W;
            lv_obj_align(e.obj, LV_ALIGN_BOTTOM_MID, cx, Y_OFFSET);
            lv_obj_clear_flag(e.obj, LV_OBJ_FLAG_HIDDEN);
            filled++;
        } else {
            lv_obj_add_flag(e.obj, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// Build a tiny 20×24 stopwatch/timer-style icon from primitives. The two
// variants share a green circular ring and an angled "hand"; the
// distinguishing feature is the cap on top — `wide_cap` true draws a wide
// flat knob (timer/kitchen-clock look) instead of the narrow stem of a
// stopwatch button. `hand_rotation_deci_deg` lets each variant point its
// hand a different direction so they read as two clearly distinct icons.
static lv_obj_t *build_clock_icon(lv_obj_t *parent, bool wide_cap,
                                  int16_t hand_rotation_deci_deg)
{
    // const (not constexpr): ARGUS_ACCENT uses the runtime lv_color_make(); the
    // stock code's LV_COLOR_MAKE brace-init was constexpr, but our themed accent
    // is centralized as a function-form macro (needed for the ternary/arg sites).
    static const lv_color_t green = ARGUS_ACCENT;

    lv_obj_t *icon = lv_obj_create(parent);
    lv_obj_set_size(icon, 20, 24);
    lv_obj_set_style_bg_opa(icon, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(icon, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(icon, 0, LV_PART_MAIN);
    lv_obj_clear_flag(icon, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(icon, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    // Cap on top — stopwatch button (narrow) vs timer knob (wide flat).
    lv_obj_t *cap = lv_obj_create(icon);
    if (wide_cap) lv_obj_set_pos(cap, 3, 0), lv_obj_set_size(cap, 14, 4);
    else          lv_obj_set_pos(cap, 7, 0), lv_obj_set_size(cap,  6, 4);
    lv_obj_set_style_bg_color(cap, green, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(cap, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(cap, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(cap, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_all(cap, 0, LV_PART_MAIN);
    lv_obj_clear_flag(cap, LV_OBJ_FLAG_SCROLLABLE);

    // The face — a green ring (transparent fill, 3 px green border).
    lv_obj_t *ring = lv_obj_create(icon);
    lv_obj_set_pos(ring, 0, 4);
    lv_obj_set_size(ring, 20, 20);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_color(ring, green, LV_PART_MAIN);
    lv_obj_set_style_border_width(ring, 3, LV_PART_MAIN);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ring, 0, LV_PART_MAIN);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_SCROLLABLE);

    // Hand — a thin vertical rectangle pivoted at its bottom, rotated to
    // point in the direction the caller chose. Mounted inside the ring so
    // it gets clipped by the round face naturally if it ever goes long.
    lv_obj_t *hand = lv_obj_create(ring);
    lv_obj_set_pos(hand, 7, -2);   // bottom of hand sits at ring centre
    lv_obj_set_size(hand, 2, 9);
    lv_obj_set_style_bg_color(hand, green, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(hand, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(hand, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(hand, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_all(hand, 0, LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_x(hand, 1, LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_y(hand, 9, LV_PART_MAIN);
    lv_obj_set_style_transform_rotation(hand, hand_rotation_deci_deg, LV_PART_MAIN);
    lv_obj_clear_flag(hand, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(hand, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_add_flag(icon, LV_OBJ_FLAG_HIDDEN);
    return icon;
}

static volatile bool back_btn_pressed = false;
static void IRAM_ATTR on_back_btn_isr() { back_btn_pressed = true; }

static void update_wardriver_indicator()
{
    bool running = wardriver_is_running();
    int  wc = running ? wardriver_get_wifi_count() : 0;
    int  bc = running ? wardriver_get_bt_count()   : 0;

    if (running) {
        lv_obj_set_style_text_color(wardriver_wifi_label,
            ARGUS_ACCENT_ACTIVE, LV_PART_MAIN);
        if (wc > 0)
            lv_label_set_text_fmt(wardriver_wifi_label, LV_SYMBOL_EYE_OPEN " %d", wc);
        else
            lv_label_set_text(wardriver_wifi_label, LV_SYMBOL_EYE_OPEN);

        if (bc > 0) {
            lv_label_set_text_fmt(wardriver_bt_label, " %d", bc);
            lv_obj_clear_flag(wardriver_bt_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(wardriver_bt_label, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        lv_obj_set_style_text_color(wardriver_wifi_label,
            ARGUS_TEXT_DIM, LV_PART_MAIN);
        lv_label_set_text(wardriver_wifi_label, LV_SYMBOL_EYE_OPEN);
        lv_obj_add_flag(wardriver_bt_label, LV_OBJ_FLAG_HIDDEN);
    }

    realign_status_icons();
}

static void on_clock_gesture(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    // Daily keeps the innocent surfaces reachable. Meshtastic (mesh comms) and Time
    // are day-to-day features allowed in every mode; Wardriver (recon) and the Tools
    // grid are gated to Defense/Offense so a Daily glance/confiscation reveals nothing.
    bool daily = (argus_mode_current() == ArgusMode::Daily);
    if (dir == LV_DIR_LEFT) {
        if (!daily) wardriver_screen_show();   // recon, gated in Daily
    } else if (dir == LV_DIR_RIGHT) {
        meshtastic_mark_read();                // comms, available in every mode
        meshtastic_screen_show();
    } else if (dir == LV_DIR_BOTTOM) {   // swipe down from clock face
        if (!daily) tools_screen_show();       // Tools gated in Daily
    } else if (dir == LV_DIR_TOP) {      // swipe up from clock face
        time_screen_show();              // Time is innocent - allowed in every mode
    }
}

// Called by settings screen to switch between digital and analog face
void clock_screen_set_analog_face(bool analog)
{
    analog_face = analog;
    if (analog) {
        lv_obj_add_flag(time_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(analog_container, LV_OBJ_FLAG_HIDDEN);
        // Immediately drive hands to the current time
        struct tm t;
        instance.rtc.getDateTime(&t);
        if (clock_utc_offset != 0) { t.tm_hour += clock_utc_offset; mktime(&t); }
        update_analog_clock(&t);
    } else {
        lv_obj_clear_flag(time_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(analog_container, LV_OBJ_FLAG_HIDDEN);
    }
}

// Called by the LoRa screen when LoRa power is toggled, for an immediate icon
// refresh. The actual state is read live in update_lora_indicator() (which also
// runs once a second), so the argument is no longer needed.
void clock_screen_set_lora_active(bool active)
{
    (void)active;
    update_lora_indicator();
}

// Positions AirTag, Flipper, Skimmer, EvilTwin, and Flock indicators relative
// to each other and the mesh envelope. From right to left: mesh, AirTag,
// Flipper, Skimmer, EvilTwin, Flock. Each indicator only takes a slot when
// it's visible, so when one is hidden the others slide right to fill in.
// Spacing: 65 px per slot; mesh occupies the rightmost position (x ~ 0).
static void update_scan_indicators()
{
    if (!airtag_indicator || !flock_indicator ||
        !flipper_indicator || !skimmer_indicator ||
        !evil_twin_indicator) return;

    bool airtag_vis   = airtag_is_running()    || airtag_get_count()    > 0;
    // Flipper / Skimmer / EvilTwin indicators show only when at least one
    // detection has happened — they stay hidden while the wardriver /
    // detector is idle or running with zero hits.
    bool flipper_vis  = (flipper_get_count()   > 0);
    bool skimmer_vis  = (skimmer_get_count()   > 0);
    bool eviltwin_vis = (evil_twin_get_count() > 0);
    bool flock_vis    = (flock_get_count()     > 0);

    if (airtag_vis) {
        lv_obj_clear_flag(airtag_indicator, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text_fmt(airtag_count_label, "%d", airtag_get_count());
    } else {
        lv_obj_add_flag(airtag_indicator, LV_OBJ_FLAG_HIDDEN);
    }

    if (flipper_vis) {
        lv_obj_clear_flag(flipper_indicator, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text_fmt(flipper_count_label, "%d", flipper_get_count());
    } else {
        lv_obj_add_flag(flipper_indicator, LV_OBJ_FLAG_HIDDEN);
    }

    if (skimmer_vis) {
        lv_obj_clear_flag(skimmer_indicator, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text_fmt(skimmer_count_label, "%d", skimmer_get_count());
    } else {
        lv_obj_add_flag(skimmer_indicator, LV_OBJ_FLAG_HIDDEN);
    }

    if (eviltwin_vis) {
        lv_obj_clear_flag(evil_twin_indicator, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text_fmt(evil_twin_count_label, "%d", evil_twin_get_count());
    } else {
        lv_obj_add_flag(evil_twin_indicator, LV_OBJ_FLAG_HIDDEN);
    }

    if (flock_vis) {
        lv_obj_clear_flag(flock_indicator, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text_fmt(flock_count_label, "%d", flock_get_count());
    } else {
        lv_obj_add_flag(flock_indicator, LV_OBJ_FLAG_HIDDEN);
    }

    if (!airtag_vis && !flipper_vis && !skimmer_vis &&
        !eviltwin_vis && !flock_vis) return;

    // Row order, left -> right:
    //   flock | evil_twin | airtag | flipper | skimmer | (mesh)
    // Skimmer + Flipper sit to the right of AirTag; EvilTwin + Flock
    // stay on AirTag's left. Pack right-most slot first and step left
    // by 65 px per visible indicator so hidden ones collapse without
    // leaving a gap.
    int slot = 0;
    if (skimmer_vis) {
        lv_obj_align(skimmer_indicator, LV_ALIGN_BOTTOM_MID, slot, -60);
        slot -= 65;
    }
    if (flipper_vis) {
        lv_obj_align(flipper_indicator, LV_ALIGN_BOTTOM_MID, slot, -60);
        slot -= 65;
    }
    if (airtag_vis) {
        lv_obj_align(airtag_indicator, LV_ALIGN_BOTTOM_MID, slot, -60);
        slot -= 65;
    }
    if (eviltwin_vis) {
        lv_obj_align(evil_twin_indicator, LV_ALIGN_BOTTOM_MID, slot, -60);
        slot -= 65;
    }
    if (flock_vis) {
        lv_obj_align(flock_indicator, LV_ALIGN_BOTTOM_MID, slot, -60);
    }
}

// Called by meshtastic.cpp when a new message arrives or all are read
void clock_screen_set_mesh_count(int count)
{
    if (!mesh_top_count_label) return;
    if (count <= 0) {
        lv_obj_add_flag(mesh_top_count_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(mesh_top_count_label, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text_fmt(mesh_top_count_label, "%d", count);
        realign_status_icons();
    }
}

// Called by gps_screen when GPS power state changes
void clock_screen_set_gps_active(bool active)
{
    lv_color_t color = active
        ? ARGUS_ACCENT_ACTIVE
        : lv_color_make(0x33, 0x33, 0x33);
    lv_obj_set_style_text_color(gps_indicator, color, LV_PART_MAIN);
    if (!active) {
        lv_label_set_text(gps_indicator, LV_SYMBOL_GPS);
        realign_status_icons();
    }
}

static void update_clock(); // forward declaration

void clock_screen_set_12h(bool use_12h)
{
    clock_12h = use_12h;
    update_clock();   // refresh the displayed string + rescale the label
}

// True when the watch face shows time in 12-hour form — either the digital
// face has 12h enabled in settings, or the analog face is active (no implicit
// 24h cue on an analog dial). The alarm screen consults this to decide
// whether to show its AM/PM selector.
bool clock_screen_uses_12h()
{
    return clock_12h || analog_face;
}

void clock_screen_set_show_day(bool show)
{
    clock_show_day = show;
    update_clock();
}

void clock_screen_set_show_date(bool show)
{
    clock_show_date = show;
    update_clock();
}

void clock_screen_set_show_ampm(bool show)
{
    clock_show_ampm = show;
    update_clock();
}

void clock_screen_set_show_secs(bool show)
{
    clock_show_secs = show;
    update_clock();
}

void clock_screen_set_vibrate(bool enabled)
{
    clock_vibrate = enabled;
}

// Called by settings screen to enable/disable the Matrix rain background
void clock_screen_set_matrix(bool enabled)
{
    matrix_bg_set_enabled(enabled);
}

// Called by settings screen to enable/disable the SD-card wallpaper. Coexists
// with the matrix rain: when both are on the faint image sits behind the rain.
void clock_screen_set_wallpaper(bool enabled)
{
    background_set_enabled(enabled);
}

// Dim timer state — updated by settings screen callbacks
static uint32_t s_dim_timeout_ms   = 0;   // 0 = disabled
static uint8_t  s_dim_brightness   = DEVICE_MAX_BRIGHTNESS_LEVEL / 4;
static uint32_t s_last_activity_ms = 0;
static bool     s_is_dimmed        = false;

void clock_screen_set_dim_timeout(uint32_t ms)
{
    s_dim_timeout_ms = ms;
    // Reset activity timer and restore brightness when timeout changes
    s_last_activity_ms = millis();
    if (s_is_dimmed) {
        s_is_dimmed = false;
        instance.setBrightness(DEVICE_MAX_BRIGHTNESS_LEVEL);
    }
}

void clock_screen_set_dim_brightness(uint8_t level)
{
    s_dim_brightness = level;
    // If already dimmed, apply new level immediately
    if (s_is_dimmed) instance.setBrightness(s_dim_brightness);
}

static void dim_reset_activity()
{
    s_last_activity_ms = millis();
    if (s_is_dimmed) {
        s_is_dimmed = false;
        instance.setBrightness(DEVICE_MAX_BRIGHTNESS_LEVEL);
    }
}

// Public hook so full-screen utility screens (e.g. the Flashlight) can keep the
// display awake at active brightness for as long as they are shown.
void ui_reset_dim_activity() { dim_reset_activity(); }

// ---- Motion-wake ----------------------------------------------------------
//
// When enabled, the BHI260AP accelerometer is streamed at a low rate and any
// jump in magnitude greater than MOTION_DELTA_G is treated like a tap: the
// dim timer is reset and (if currently dimmed) the screen is brought back to
// full brightness. Default ON to match smartwatch wrist-raise behaviour;
// settings can switch it off if the user wants the dim timer to run even
// while the watch is being worn.
static SensorXYZ s_motion_accel(SensorBHI260AP::ACCEL_PASSTHROUGH, instance.sensor);
static bool      s_motion_wake_enabled    = true;
static bool      s_motion_accel_started   = false;
static float     s_motion_last_mag        = 0.0f;
// 10 Hz is enough to catch a wrist tilt without burning power; the BHI260
// fuses its own samples internally and only fires its interrupt when a
// sample is ready.
#define MOTION_SAMPLE_RATE_HZ   10.0f
// Threshold in g for "this counts as motion". Stationary sample-to-sample
// noise is well under 0.05 g; any noticeable wrist movement easily exceeds
// 0.2 g.
#define MOTION_DELTA_G          0.20f

void clock_screen_set_motion_wake(bool enabled)
{
    s_motion_wake_enabled = enabled;
    if (enabled && !s_motion_accel_started) {
        // The BHI260AP firmware needs to be up before configuring virtual
        // sensors; this setter is called from the settings load + the UI
        // toggle, both of which run after instance.begin().
        s_motion_accel.enable(MOTION_SAMPLE_RATE_HZ, 0);
        s_motion_accel_started = true;
        // Drop the cached previous-magnitude so the first sample after a
        // restart isn't compared against a stale baseline.
        s_motion_last_mag = 0.0f;
    } else if (!enabled) {
        if (s_motion_accel_started) {
            s_motion_accel.disable();
            s_motion_accel_started = false;
        }
    }
}

// Pumped from the main loop after instance.loop() has drained the BHI260's
// sample queue. No-op when motion-wake is off or no new sample is in.
static void motion_wake_poll()
{
    if (!s_motion_wake_enabled || !s_motion_accel_started) return;
    if (!s_motion_accel.hasUpdated()) return;

    float x = s_motion_accel.getX();
    float y = s_motion_accel.getY();
    float z = s_motion_accel.getZ();
    float mag = sqrtf(x * x + y * y + z * z);

    // First sample after enable — no prior baseline, just seed and return.
    if (s_motion_last_mag == 0.0f) {
        s_motion_last_mag = mag;
        return;
    }

    float delta = fabsf(mag - s_motion_last_mag);
    s_motion_last_mag = mag;
    if (delta >= MOTION_DELTA_G) {
        dim_reset_activity();
    }
}

// Called by gps_screen after a quality fix to set the longitude-derived UTC offset
void clock_screen_set_utc_offset(int offset_hours)
{
    clock_utc_offset = offset_hours;
}

int clock_screen_get_utc_offset()
{
    return clock_utc_offset;
}

// Public repaint hook for the timezone module after it restores/refreshes the
// UTC offset (or re-syncs the RTC over WiFi).
void clock_screen_refresh()
{
    update_clock();
}

// True while the user has set the clock by hand — GPS time sync defers to it.
bool clock_screen_manual_time_active()
{
    return manual_time_override;
}

// Settings "Manual Time" switch: enable/disable the override without changing
// the clock. Turning it off lets the next GPS lock re-sync the RTC.
void clock_screen_set_manual_override(bool on)
{
    manual_time_override = on;
}

// Fill *out with the wall-clock local time currently shown on the face
// (the RTC holds UTC; clock_utc_offset shifts it to local).
void clock_screen_get_local_time(struct tm *out)
{
    instance.rtc.getDateTime(out);
    if (clock_utc_offset != 0) {
        out->tm_hour += clock_utc_offset;
        mktime(out);
    }
}

// Apply a user-entered local date/time: write it straight to the RTC, drop the
// GPS-derived UTC offset (the RTC now holds local time directly), enable the
// manual override so GPS sync stops touching the clock, and refresh the face.
void clock_screen_apply_manual_time(int year, int mon, int day, int hour, int min)
{
    instance.rtc.setDateTime(year, mon, day, hour, min, 0);
    instance.rtc.hwClockRead();
    clock_utc_offset     = 0;
    manual_time_override = true;
    update_clock();
}

// Called by gps_screen each second with the current satellite count
void clock_screen_set_sat_count(uint32_t count)
{
    if (count > 0)
        lv_label_set_text_fmt(gps_indicator, LV_SYMBOL_GPS " %lu", (unsigned long)count);
    else
        lv_label_set_text(gps_indicator, LV_SYMBOL_GPS);
    realign_status_icons();
}

// Skip bg_ticks for this many loop iterations after a screen transition
// is requested. Set by *_show() helpers; main loop drains this on each
// iteration. Gives LVGL guaranteed render budget when the wardriver +
// detector pipeline would otherwise stall the redraw.
static int s_lvgl_priority_cycles = 0;
void main_loop_request_lvgl_priority(int cycles) {
    if (cycles > s_lvgl_priority_cycles) s_lvgl_priority_cycles = cycles;
}

void clock_screen_show()
{
    // Pause matrix-rain so the synchronous refresh below doesn't have to
    // re-render 22 recolored labels every refresh tick. The main loop
    // also pauses during the priority window, but pausing here covers
    // the window in between (load + refresh + return).
    bool paused_matrix = matrix_bg_is_enabled();
    if (paused_matrix) matrix_bg_set_paused(true);

    lv_scr_load(clock_screen);
    lv_obj_invalidate(clock_screen);
    // Synchronous full refresh. lv_scr_load defers rendering to the next
    // lv_task_handler, and in partial mode with a full-screen buffer the
    // *buffer* only gets the dirty pixels written - non-dirty areas keep
    // whatever was rendered last, which is the previous screen's content.
    // Calling lv_refr_now after the invalidate guarantees the entire
    // screen's background + widgets land in the buffer in one shot
    // before we return to the busy main loop.
    lv_refr_now(NULL);

    if (paused_matrix) matrix_bg_set_paused(false);
    main_loop_request_lvgl_priority(12);
}

// Pad each side by this many px so the clock has breathing room from the
// screen edge. Kept small so the 116 px digits fill nearly the full width.
// NOTE: do NOT add negative letter-spacing here - the clock is a FIXED-mode
// spangroup and letter-spacing on it corrupts glyph layout (clipped/missing
// digits). Size the clock purely by picking the largest font that fits.
#define CLOCK_TEXT_PAD_X     4
// Cap the up-scale so the rendered glyphs don't get too soft. The base
// font is now the custom 96 px Montserrat subset (digits / colon / AM /
// PM / space), so 1.5× = ~144 px tall is the practical visual ceiling
// before the time overlaps with the date label below.
#define CLOCK_TEXT_MAX_SCALE (LV_SCALE_NONE * 3 / 2)
// Allow the scaler to shrink down to 50 % so long formats ("00:00:00 PM"
// in 12 h mode) still fit on a 410-wide screen. Without this floor the
// "never shrink" guard would clip the seconds off the right edge.
#define CLOCK_TEXT_MIN_SCALE (LV_SCALE_NONE / 2)

// Defined in lv_font_montserrat_clock_96.c — bigger base font generated
// from the project's bundled Montserrat-Medium.ttf via tools/gen_clock_font.py.
// Only contains 15 glyphs (0-9, ':', ' ', 'A', 'M', 'P') so the flash
// cost is ~24 KB of glyph data instead of the ~120 KB a full-character
// 96 px font would cost.
extern "C" const lv_font_t lv_font_montserrat_clock_96;
// Two smaller subset sizes for the adaptive-font clock (same 15 glyphs), so the
// widest formats fit WITHOUT a runtime transform. See tools/gen_clock_font.py.
extern "C" const lv_font_t lv_font_montserrat_clock_72;
extern "C" const lv_font_t lv_font_montserrat_clock_56;
// Larger 110 px Montserrat size so the "00:00" formats fill more of the 378 px
// usable width. resize_clock_text() only picks it when its worst-case string fits,
// so it is a pure "go bigger when there is room" option with no overflow risk.
extern "C" const lv_font_t lv_font_montserrat_clock_110;
// 116 px - the largest Montserrat size whose "00:00" fits the ~402 px usable
// width with no letter-spacing tricks. Selected only when it actually fits.
extern "C" const lv_font_t lv_font_montserrat_clock_116;

// Size the home-screen digital clock to fill the width, by SELECTING the largest
// pre-generated font whose worst-case string fits - NOT by transform-scaling one
// font. A runtime transform_scale fragmented/jumped the clock under LVGL
// partial-refresh whenever an overlapping layer (Matrix rain, SD wallpaper) or a
// busy radio invalidated part of it; a plain font has no transform, so partial
// redraws are always pixel-correct and it is glitch-free in every display mode.
// Memoised on (12h, ampm, secs, width) so the per-second tick is a no-op.
static void resize_clock_text()
{
    if (!time_label || !clock_screen) return;

    int screen_w = lv_obj_get_width(clock_screen);
    int usable_w = screen_w - 2 * CLOCK_TEXT_PAD_X;
    if (usable_w <= 0) return;

    uint32_t key = ((uint32_t)usable_w << 3)
                 | (clock_12h        ? 0x4 : 0)
                 | (clock_show_ampm  ? 0x2 : 0)
                 | (clock_show_secs  ? 0x1 : 0);
    static uint32_t s_cached_key = 0xFFFFFFFFu;   // force a pick on first call
    if (key == s_cached_key) return;              // same format+width -> no-op
    s_cached_key = key;

    // Worst-case digit string for the current format ("00…" wide glyphs + the
    // longest suffix). Selecting off the format, not the live text, keeps the
    // chosen size stable as the seconds tick.
    const char *ref;
    if (clock_12h) {
        if      (clock_show_secs && clock_show_ampm) ref = "00:00:00 PM";
        else if (clock_show_secs)                    ref = "00:00:00";
        else if (clock_show_ampm)                    ref = "00:00 PM";
        else                                         ref = "00:00";
    } else {
        ref = clock_show_secs ? "00:00:00" : "00:00";
    }

    // Largest font (widest -> narrowest) whose worst-case string fits usable_w.
    // The 56 px fallback fits every supported format, so a font is always chosen.
    const lv_font_t *fonts[] = {
        &lv_font_montserrat_clock_116,
        &lv_font_montserrat_clock_110,
        &lv_font_montserrat_clock_96,
        &lv_font_montserrat_clock_72,
        &lv_font_montserrat_clock_56,
    };
    const lv_font_t *chosen = fonts[4];
    for (unsigned i = 0; i < sizeof(fonts) / sizeof(fonts[0]); i++) {
        lv_point_t sz;
        lv_text_get_size(&sz, ref, fonts[i], 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (sz.x <= usable_w) { chosen = fonts[i]; break; }
    }
    lv_obj_set_style_text_font(time_label, chosen, LV_PART_MAIN);
    // No transform: guarantee any leftover scale from a prior build is cleared.
    lv_obj_set_style_transform_scale(time_label, LV_SCALE_NONE, LV_PART_MAIN);
    // Group font just changed; recompute FIXED-mode line geometry so the digit
    // spans and the smaller AM/PM span stay consistently laid out.
    lv_spangroup_refr_mode(time_label);
}

static void update_clock()
{
    struct tm t;
    instance.rtc.getDateTime(&t); // RTC stores UTC

    // Apply longitude-derived offset to get local time.
    // mktime() normalises any hour/day/month overflow after the addition.
    if (clock_utc_offset != 0) {
        t.tm_hour += clock_utc_offset;
        mktime(&t);
    }

    if (analog_face) {
        update_analog_clock(&t);
    } else {
        char hours_buf[4];
        char rest_buf[12];
        char ampm_buf[6];
        ampm_buf[0] = '\0';   // AM/PM span stays empty (invisible) unless 12h + show_ampm
        if (clock_12h) {
            int h = t.tm_hour % 12;
            if (h == 0) h = 12;
            snprintf(hours_buf, sizeof(hours_buf), "%d", h);
            if (clock_show_secs)
                snprintf(rest_buf, sizeof(rest_buf), "%02d:%02d", t.tm_min, t.tm_sec);
            else
                snprintf(rest_buf, sizeof(rest_buf), "%02d", t.tm_min);
            if (clock_show_ampm)
                snprintf(ampm_buf, sizeof(ampm_buf), " %s", t.tm_hour < 12 ? "AM" : "PM");
        } else {
            snprintf(hours_buf, sizeof(hours_buf), "%02d", t.tm_hour);
            if (clock_show_secs)
                snprintf(rest_buf, sizeof(rest_buf), "%02d:%02d", t.tm_min, t.tm_sec);
            else
                snprintf(rest_buf, sizeof(rest_buf), "%02d", t.tm_min);
        }
        lv_span_set_text(s_span_hours, hours_buf);
        lv_span_set_text(s_span_rest,  rest_buf);
        lv_span_set_text(s_span_ampm,  ampm_buf);   // smaller font; "" when unused
        // Blink by toggling the colon span's color — the character stays in
        // the string so the total width never changes and the digits stay put.
        static bool s_colon_on = true;
        if (!clock_show_secs) s_colon_on = !s_colon_on;
        lv_style_set_text_color(&s_span_colon->style,
                                (clock_show_secs || s_colon_on) ? lv_color_white() : lv_color_black());
        resize_clock_text();
    }

    char date_buf[32];
    if (clock_show_day && clock_show_date)
        strftime(date_buf, sizeof(date_buf), "%A\n%B %d, %Y", &t);
    else if (clock_show_day)
        strftime(date_buf, sizeof(date_buf), "%A", &t);
    else if (clock_show_date)
        strftime(date_buf, sizeof(date_buf), "%B %d, %Y", &t);
    else
        date_buf[0] = '\0';
    lv_label_set_text(date_label, date_buf);

    // Invalidate ONLY the clock's own area, not the whole screen. The earlier
    // full-screen-per-tick invalidate was a workaround for the transform_scale
    // (a scaled label drew past a label-only clear); that transform is gone now
    // (adaptive font selection), so the fixed-width time_label's own bounds cover
    // the whole time line and clear it cleanly. Just as important: a full-screen
    // flush every second maximised the window for WiFi to stall the PSRAM
    // framebuffer mid-flush -> the intermittent ghost. A tiny clock-area flush
    // (what base 13-37 does) barely exposes PSRAM, so the WiFi ghost goes away.
    lv_obj_invalidate(time_label);
}

// Firmware name + version surfaced in the boot banner so support tickets carry
// a fixed anchor. Bump FW_VERSION on each cut.
#define FW_NAME    "ARGUS"
#define FW_VERSION "0.1.0"   // ARGUS fork of r3dfish/13-37 (base 1.0.0)

// Saira Condensed boot-splash fonts generated via lv_font_conv; see
// src/font_argus_argus.c and src/font_argus_wordmark.c.
LV_FONT_DECLARE(font_argus_argus);
LV_FONT_DECLARE(font_argus_wordmark);

void setup()
{
    Serial.begin(115200);
    delay(50);   // let the USB-CDC link settle so the banner isn't truncated
    Serial.printf("\n%s firmware v%s  (T-Watch Ultra)  build %s %s\n",
                  FW_NAME, FW_VERSION, __DATE__, __TIME__);

    instance.begin();
    coex_log_heap("after-instance-begin");
    instance.powerControl(POWER_NFC, false); // ensure NFC is off on boot
    beginLvglHelper(instance);
    // Raise the scroll threshold so a tap that drifts a few pixels still registers
    // as a click, not a scroll. Fixes finicky buttons inside scrollable pages (e.g.
    // Exit Offense at the bottom of Settings). LVGL default is ~10 px.
    for (lv_indev_t *id = lv_indev_get_next(NULL); id; id = lv_indev_get_next(id))
        if (lv_indev_get_type(id) == LV_INDEV_TYPE_POINTER)
            lv_indev_set_scroll_limit(id, 20);
    // NOTE: bringing the BLE controller up here (ble_scan_boot_keepalive) crashed
    // boot — too early / unsafe at this point in init. Reverted. The keep-alive
    // approach is still right, but it must be invoked LATER (after full init) and
    // verified. For now the BT toggle brings the controller up on demand.

    // Time fallback: with no GPS fix and no WiFi/NTP, the RTC can come up unset and
    // the clock reads wildly wrong. If the RTC year is implausible, seed it from the
    // firmware BUILD time (__DATE__/__TIME__) as local wall-clock so the face shows
    // it directly (clock_utc_offset stays 0). Approximate (build time, not flash
    // time); a later GPS fix / NTP sync overrides it precisely. Placed after
    // beginLvglHelper so USB/console are up first.
    {
        struct tm rn;
        instance.rtc.getDateTime(&rn);
        if (rn.tm_year + 1900 < 2025) {
            const char *D = __DATE__;   // "Mmm dd yyyy" (day may be space-padded)
            const char *T = __TIME__;   // "hh:mm:ss"
            const char *M = "JanFebMarAprMayJunJulAugSepOctNovDec";
            int mon = 1;
            for (int i = 0; i < 12; i++)
                if (D[0]==M[i*3] && D[1]==M[i*3+1] && D[2]==M[i*3+2]) { mon = i+1; break; }
            int day = (D[4]==' ' ? 0 : (D[4]-'0')*10) + (D[5]-'0');
            int yr  = (D[7]-'0')*1000 + (D[8]-'0')*100 + (D[9]-'0')*10 + (D[10]-'0');
            int hh  = (T[0]-'0')*10 + (T[1]-'0');
            int mm  = (T[3]-'0')*10 + (T[4]-'0');
            int ss  = (T[6]-'0')*10 + (T[7]-'0');
            instance.rtc.setDateTime(yr, mon, day, hh, mm, ss);
            instance.rtc.hwClockRead();
        }
    }

    // Boot splash — ARGUS lockup on the panel before the clock comes up.
    // Backlight on now so it's visible; the splash stays up through the rest of
    // setup (screen construction) and is swapped for the clock below, held to a
    // minimum visible time. Brand typeface is Saira Condensed (src/font_argus_*.c),
    // filled steel-blue (#9BBCD6) on black — the sanctioned dark-surface treatment.
    instance.setBrightness(DEVICE_MAX_BRIGHTNESS_LEVEL);
    lv_obj_t *boot_splash = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(boot_splash, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(boot_splash, 0, LV_PART_MAIN);

    // Small "HADES" wordmark above the ARGUS hero.
    lv_obj_t *boot_wm = lv_label_create(boot_splash);
    lv_label_set_text(boot_wm, "HADES");
    lv_obj_set_style_text_color(boot_wm, ARGUS_ACCENT, LV_PART_MAIN);   // ARGUS steel-blue
    lv_obj_set_style_text_font(boot_wm, &font_argus_wordmark, LV_PART_MAIN);
    lv_obj_align(boot_wm, LV_ALIGN_CENTER, 0, -46);

    // ARGUS hero.
    lv_obj_t *boot_brand = lv_label_create(boot_splash);
    lv_label_set_text(boot_brand, FW_NAME);   // "ARGUS"
    lv_obj_set_style_text_color(boot_brand, ARGUS_ACCENT, LV_PART_MAIN);   // ARGUS steel-blue
    lv_obj_set_style_text_font(boot_brand, &font_argus_argus, LV_PART_MAIN);
    lv_obj_align(boot_brand, LV_ALIGN_CENTER, 0, 8);

    lv_scr_load(boot_splash);
    lv_refr_now(NULL);                       // paint now; no timer handler in setup yet
    uint32_t boot_splash_ms = millis();

    // Register the USB Mass Storage interface and start the USB stack. Must run
    // after instance.begin() mounts the SD card; the card stays hidden from the
    // host until the USB SD screen mounts it.
    usb_sd_init();

    // Load the saved APRS callsign from the SD card (if present).
    aprs_init();

    // Load the saved alarm-clock configuration from the SD card (if present).
    alarm_init();

    // Expire stale detection records. The per-detector writers also enforce
    // retention as they append, but that only fires when something is DETECTED.
    // A watch that sits unused for a month would otherwise keep last month's
    // records indefinitely, so the window is also swept once at boot.
    detect_log_sweep_all();

    // Build the clock screen
    clock_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(clock_screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(clock_screen, 0, LV_PART_MAIN);

    // Lowest z-order: the SD-card wallpaper renders behind everything,
    // including the matrix rain. Created before matrix_bg_create so it is
    // the first child. Hidden until the "Wallpaper" setting turns it on.
    background_create(clock_screen);

    // First child → renders behind every other widget on the clock screen
    matrix_bg_create(clock_screen);

    // GPS indicator — anchored to top-right; others chain off it via realign_status_icons()
    gps_indicator = lv_label_create(clock_screen);
    lv_obj_set_style_text_font(gps_indicator, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(gps_indicator, ARGUS_TEXT_DIM, LV_PART_MAIN);
    lv_label_set_text(gps_indicator, LV_SYMBOL_GPS);
    lv_obj_align(gps_indicator, LV_ALIGN_TOP_RIGHT, -70, 20);

    // Wardriver indicator — flex container with green WiFi count and blue BT count
    wardriver_container = lv_obj_create(clock_screen);
    lv_obj_set_size(wardriver_container, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(wardriver_container, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(wardriver_container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(wardriver_container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(wardriver_container, 2, LV_PART_MAIN);
    lv_obj_clear_flag(wardriver_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(wardriver_container, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(wardriver_container, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(wardriver_container,
        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    wardriver_wifi_label = lv_label_create(wardriver_container);
    lv_obj_set_style_text_font(wardriver_wifi_label, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(wardriver_wifi_label, ARGUS_TEXT_DIM, LV_PART_MAIN);
    lv_label_set_text(wardriver_wifi_label, LV_SYMBOL_EYE_OPEN);

    wardriver_bt_label = lv_label_create(wardriver_container);
    lv_obj_set_style_text_font(wardriver_bt_label, &font_argus_label_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(wardriver_bt_label, lv_color_make(0x55, 0x99, 0xFF), LV_PART_MAIN);
    lv_label_set_text(wardriver_bt_label, "");
    lv_obj_add_flag(wardriver_bt_label, LV_OBJ_FLAG_HIDDEN);

    // WiFi indicator
    wifi_indicator = lv_label_create(clock_screen);
    lv_obj_set_style_text_font(wifi_indicator, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(wifi_indicator, ARGUS_TEXT_DIM, LV_PART_MAIN);
    lv_label_set_text(wifi_indicator, LV_SYMBOL_WIFI);

    // Bluetooth indicator
    bt_indicator = lv_label_create(clock_screen);
    lv_obj_set_style_text_font(bt_indicator, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(bt_indicator, ARGUS_TEXT_DIM, LV_PART_MAIN);
    lv_label_set_text(bt_indicator, LV_SYMBOL_BLUETOOTH);

    // SD card indicator
    sd_indicator = lv_label_create(clock_screen);
    lv_obj_set_style_text_font(sd_indicator, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(sd_indicator, ARGUS_TEXT_DIM, LV_PART_MAIN);
    lv_label_set_text(sd_indicator, LV_SYMBOL_SD_CARD);

    // NFC indicator (between SD and LoRa) — arc logo image, recolored for on/off state
    nfc_indicator = lv_image_create(clock_screen);
    lv_image_set_src(nfc_indicator, &nfc_icon_dsc);
    lv_obj_set_style_image_recolor(nfc_indicator, lv_color_make(0x33, 0x33, 0x33), LV_PART_MAIN);
    lv_obj_set_style_image_recolor_opa(nfc_indicator, LV_OPA_COVER, LV_PART_MAIN);

    // LoRa antenna indicator (leftmost) — composite widget built separately
    build_lora_indicator(clock_screen);

    // Unread-Meshtastic-message badge, sits to the immediate left of the
    // LoRa icon. White count on a red pill, hidden when count == 0.
    mesh_top_count_label = lv_label_create(clock_screen);
    lv_obj_set_style_text_color(mesh_top_count_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(mesh_top_count_label, &font_argus_mono_16, LV_PART_MAIN);   // VT323 (brand readout)
    lv_obj_set_style_bg_color(mesh_top_count_label, lv_color_make(0xC0, 0x20, 0x20), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mesh_top_count_label, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(mesh_top_count_label, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(mesh_top_count_label, 5, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(mesh_top_count_label, 1, LV_PART_MAIN);
    lv_label_set_text(mesh_top_count_label, "0");
    lv_obj_add_flag(mesh_top_count_label, LV_OBJ_FLAG_HIDDEN);

    // Spangroup so the blinking colon can change color without changing
    // the string width — keeping hours and minutes pixel-stable.
    time_label = lv_spangroup_create(clock_screen);
    lv_obj_set_style_text_font(time_label, &lv_font_montserrat_clock_96, LV_PART_MAIN);
    lv_obj_set_style_text_color(time_label, lv_color_white(), LV_PART_MAIN);
    lv_spangroup_set_align(time_label, LV_TEXT_ALIGN_CENTER);
    lv_spangroup_set_mode(time_label, LV_SPAN_MODE_FIXED);
    lv_obj_set_width(time_label, lv_pct(100));
    lv_obj_align(time_label, LV_ALIGN_CENTER, 0, -40);

    s_span_hours = lv_spangroup_new_span(time_label);
    lv_span_set_text(s_span_hours, "00");

    s_span_colon = lv_spangroup_new_span(time_label);
    lv_span_set_text(s_span_colon, ":");

    s_span_rest = lv_spangroup_new_span(time_label);
    lv_span_set_text(s_span_rest, "00");

    // AM/PM in its own span at a smaller fixed font, so it reads as a suffix
    // rather than a full-height part of the time. Stays empty (invisible) unless
    // the format is 12h with AM/PM shown. Montserrat 32 is roughly half the
    // adaptive digit font (56-96 px), so it's a clear, readable suffix.
    s_span_ampm = lv_spangroup_new_span(time_label);
    lv_span_set_text(s_span_ampm, "");
    lv_style_set_text_font(&s_span_ampm->style, &lv_font_montserrat_32);
    lv_style_set_text_color(&s_span_ampm->style, lv_color_white());
    // The digit spans get their font from the group (swapped adaptively by
    // resize_clock_text). This span carries its OWN smaller font, so the group's
    // cached FIXED-mode line geometry must be recomputed to account for the
    // mixed font metrics - without this refresh the group renders stale/invalid
    // geometry (the earlier crash). Recompute once here; the adaptive resize
    // re-triggers its own refresh whenever it swaps the group font.
    lv_spangroup_refr_mode(time_label);

    date_label = lv_label_create(clock_screen);
    lv_obj_set_style_text_color(date_label, ARGUS_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(date_label, &font_argus_label_20, LV_PART_MAIN);   // Orbitron (brand label)
    lv_obj_set_style_text_align(date_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(date_label, "");
    lv_obj_align(date_label, LV_ALIGN_CENTER, 0, 60);

    build_battery_widget(clock_screen);

    // Alarm-enabled indicator — bell glyph positioned to the left of the
    // battery widget, vertically centred with it. Hidden by default; shown
    // when the alarm module reports the alarm enabled.
    alarm_indicator = lv_label_create(clock_screen);
    lv_obj_set_style_text_font(alarm_indicator, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(alarm_indicator, ARGUS_TEXT, LV_PART_MAIN);
    lv_label_set_text(alarm_indicator, LV_SYMBOL_BELL);
    lv_obj_align(alarm_indicator, LV_ALIGN_BOTTOM_MID, -95, -10);
    lv_obj_add_flag(alarm_indicator, LV_OBJ_FLAG_HIDDEN);

    // Timer + stopwatch indicators — sit further left of the alarm bell.
    // Each is a small custom ring icon; only shown while their respective
    // module reports actively running. The two icons differ in cap shape
    // (wide knob vs narrow stem) and hand angle so they're distinguishable
    // at a glance even at 20 px wide.
    stopwatch_indicator = build_clock_icon(clock_screen,
                                           /*wide_cap=*/false,
                                           /*hand_rotation_deci_deg=*/450);   // 1:30
    lv_obj_align(stopwatch_indicator, LV_ALIGN_BOTTOM_MID, -125, -10);

    timer_indicator     = build_clock_icon(clock_screen,
                                           /*wide_cap=*/true,
                                           /*hand_rotation_deci_deg=*/-450);  // 10:30
    lv_obj_align(timer_indicator,     LV_ALIGN_BOTTOM_MID, -158, -10);
    build_analog_clock(clock_screen);

    // AirTag scanner indicator — flex row of disc-icon + count. Hidden until
    // airtag_is_running(); update_airtag_indicator() positions it left of the
    // mesh icon when both are visible, or centered when only this one is.
    airtag_indicator = lv_obj_create(clock_screen);
    lv_obj_set_size(airtag_indicator, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(airtag_indicator, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(airtag_indicator, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(airtag_indicator, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(airtag_indicator, 4, LV_PART_MAIN);
    lv_obj_clear_flag(airtag_indicator, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(airtag_indicator, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(airtag_indicator, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(airtag_indicator,
        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(airtag_indicator, LV_OBJ_FLAG_HIDDEN);

    // Disc body
    lv_obj_t *airtag_disc = lv_obj_create(airtag_indicator);
    lv_obj_set_size(airtag_disc, 22, 22);
    lv_obj_set_style_radius(airtag_disc, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(airtag_disc, lv_color_make(0xEE, 0xEE, 0xEE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(airtag_disc, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(airtag_disc, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(airtag_disc, 0, LV_PART_MAIN);
    lv_obj_clear_flag(airtag_disc, LV_OBJ_FLAG_SCROLLABLE);

    // Inner dot inside the disc (AirTag's Apple-logo placement)
    lv_obj_t *airtag_dot = lv_obj_create(airtag_disc);
    lv_obj_set_size(airtag_dot, 6, 6);
    lv_obj_set_style_radius(airtag_dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(airtag_dot, lv_color_make(0x99, 0x99, 0x99), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(airtag_dot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(airtag_dot, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(airtag_dot, 0, LV_PART_MAIN);
    lv_obj_clear_flag(airtag_dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(airtag_dot);

    // Discovery count
    airtag_count_label = lv_label_create(airtag_indicator);
    lv_obj_set_style_text_font(airtag_count_label, &font_argus_label_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(airtag_count_label, lv_color_white(), LV_PART_MAIN);
    lv_label_set_text(airtag_count_label, "0");

    // Flipper Zero indicator — tiny cyan dolphin-pill + count. Same layout
    // pattern as the AirTag indicator next to it. Hidden until first
    // detection; update_scan_indicators() positions it left of AirTag.
    flipper_indicator = lv_obj_create(clock_screen);
    lv_obj_set_size(flipper_indicator, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(flipper_indicator, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(flipper_indicator, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(flipper_indicator, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(flipper_indicator, 4, LV_PART_MAIN);
    lv_obj_clear_flag(flipper_indicator, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(flipper_indicator, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(flipper_indicator, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(flipper_indicator,
        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(flipper_indicator, LV_OBJ_FLAG_HIDDEN);

    // Orange dolphin-pill — at this size it reads as a small dolphin silhouette.
    lv_obj_t *flipper_body = lv_obj_create(flipper_indicator);
    lv_obj_set_size(flipper_body, 26, 14);
    lv_obj_set_style_radius(flipper_body, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(flipper_body, lv_color_make(0xFF, 0x88, 0x00), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(flipper_body, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(flipper_body, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(flipper_body, 0, LV_PART_MAIN);
    lv_obj_clear_flag(flipper_body, LV_OBJ_FLAG_SCROLLABLE);

    // Tiny dark eye, anchored inside the pill near the "front"
    lv_obj_t *flipper_eye = lv_obj_create(flipper_body);
    lv_obj_set_size(flipper_eye, 3, 3);
    lv_obj_set_style_radius(flipper_eye, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(flipper_eye, lv_color_make(0x11, 0x11, 0x11), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(flipper_eye, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(flipper_eye, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(flipper_eye, 0, LV_PART_MAIN);
    lv_obj_clear_flag(flipper_eye, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(flipper_eye, LV_ALIGN_LEFT_MID, 4, -1);

    flipper_count_label = lv_label_create(flipper_indicator);
    lv_obj_set_style_text_font(flipper_count_label, &font_argus_label_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(flipper_count_label, lv_color_make(0xFF, 0x88, 0x00), LV_PART_MAIN);
    lv_label_set_text(flipper_count_label, "0");

    // Skimmer indicator — red "SK" badge + count. Hidden until the
    // wardriver / standalone scanner flags an HC-0x device. Sits between
    // the Flipper indicator and the EvilTwin indicator.
    skimmer_indicator = lv_obj_create(clock_screen);
    lv_obj_set_size(skimmer_indicator, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(skimmer_indicator, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(skimmer_indicator, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(skimmer_indicator, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(skimmer_indicator, 4, LV_PART_MAIN);
    lv_obj_clear_flag(skimmer_indicator, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(skimmer_indicator, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(skimmer_indicator, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(skimmer_indicator,
        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(skimmer_indicator, LV_OBJ_FLAG_HIDDEN);

    // Red badge with "SK" letters — reads as "skimmer alert" at indicator scale.
    lv_obj_t *sk_badge = lv_obj_create(skimmer_indicator);
    lv_obj_set_size(sk_badge, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(sk_badge, 4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sk_badge, lv_color_make(0xCC, 0x22, 0x22), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sk_badge, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(sk_badge, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(sk_badge, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(sk_badge, 1, LV_PART_MAIN);
    lv_obj_clear_flag(sk_badge, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *sk_lbl = lv_label_create(sk_badge);
    lv_obj_set_style_text_font(sk_lbl, &font_argus_label_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(sk_lbl, lv_color_white(), LV_PART_MAIN);
    lv_label_set_text(sk_lbl, "SK");

    skimmer_count_label = lv_label_create(skimmer_indicator);
    lv_obj_set_style_text_font(skimmer_count_label, &font_argus_label_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(skimmer_count_label, lv_color_make(0xFF, 0x66, 0x66), LV_PART_MAIN);
    lv_label_set_text(skimmer_count_label, "0");

    // Evil-twin indicator — "ET" tag in alert orange + count. Hidden until
    // the wardriver flags at least one same-SSID/different-auth conflict;
    // sits between the Skimmer indicator and the Flock indicator in the
    // home-screen status row.
    evil_twin_indicator = lv_obj_create(clock_screen);
    lv_obj_set_size(evil_twin_indicator, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(evil_twin_indicator, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(evil_twin_indicator, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(evil_twin_indicator, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(evil_twin_indicator, 4, LV_PART_MAIN);
    lv_obj_clear_flag(evil_twin_indicator, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(evil_twin_indicator, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(evil_twin_indicator, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(evil_twin_indicator,
        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(evil_twin_indicator, LV_OBJ_FLAG_HIDDEN);

    // "ET" badge — small orange pill with white letters reads as
    // "evil twin alert" at the indicator's tiny size.
    lv_obj_t *et_badge = lv_obj_create(evil_twin_indicator);
    lv_obj_set_size(et_badge, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(et_badge, 4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(et_badge, lv_color_make(0xFF, 0x66, 0x00), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(et_badge, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(et_badge, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(et_badge, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(et_badge, 1, LV_PART_MAIN);
    lv_obj_clear_flag(et_badge, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *et_lbl = lv_label_create(et_badge);
    lv_obj_set_style_text_font(et_lbl, &font_argus_label_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(et_lbl, lv_color_white(), LV_PART_MAIN);
    lv_label_set_text(et_lbl, "ET");

    evil_twin_count_label = lv_label_create(evil_twin_indicator);
    lv_obj_set_style_text_font(evil_twin_count_label, &font_argus_label_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(evil_twin_count_label, lv_color_make(0xFF, 0x88, 0x00), LV_PART_MAIN);
    lv_label_set_text(evil_twin_count_label, "0");

    // Flock/OUI indicator — warning icon + count, hidden until first detection.
    // Shown to the LEFT of the AirTag indicator.
    flock_indicator = lv_obj_create(clock_screen);
    lv_obj_set_size(flock_indicator, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(flock_indicator, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(flock_indicator, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(flock_indicator, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(flock_indicator, 4, LV_PART_MAIN);
    lv_obj_clear_flag(flock_indicator, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(flock_indicator, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(flock_indicator, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(flock_indicator,
        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(flock_indicator, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *flock_icon = lv_label_create(flock_indicator);
    lv_obj_set_style_text_font(flock_icon, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(flock_icon, lv_color_make(0xFF, 0x88, 0x00), LV_PART_MAIN);
    lv_label_set_text(flock_icon, LV_SYMBOL_WARNING);

    flock_count_label = lv_label_create(flock_indicator);
    lv_obj_set_style_text_font(flock_count_label, &font_argus_label_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(flock_count_label, lv_color_make(0xFF, 0x88, 0x00), LV_PART_MAIN);
    lv_label_set_text(flock_count_label, "0");

    // ARGUS mode state machine (Daily/Defense/Offense). Init BEFORE any screen is
    // created so gating/theming can read the mode as screens build. Forces Daily
    // at boot (or Defense only if the user enabled Defense-persistence).
    argus_mode_init();

    gps_screen_create();
    lora_screen_create();
    nfc_screen_create();
    nfc_write_screen_create();
    meshtastic_screen_create();
    nodes_screen_create();
    send_message_screen_create();
    map_screen_create();
    configuration_screen_create();
    channels_screen_create();
    settings_screen_create();
    tools_screen_create();
    notifications_screen_create();
    notify_popup_init();
    threat_radar_screen_create();
    pet_screen_create();
    tpms_screen_create();
    pager_screen_create();
    mouse_screen_create();
    usb_sd_screen_create();
    aprs_screen_create();
    tesla_cp_screen_create();
    wifi_screen_create();
    wifi_radio_screen_create();
    bluetooth_screen_create();
    portscan_screen_create();
    analyze_screen_create();
    bt_analyze_screen_create();
    lora_analyze_screen_create();
    stopwatch_screen_create();
    timer_screen_create();
    alarm_screen_create();
    calendar_screen_create();
    world_clock_screen_create();
    sun_moon_screen_create();
    spycam_screen_create();
    nfc_field_screen_create();
    pin_pad_screen_create();
    loot_screen_create();
    deauth_screen_create();
    tracker_timeline_screen_create();
    beacon_spam_screen_create();
    deauth_attack_screen_create();
    rogue_ap_screen_create();
    probe_sniffer_screen_create();
    offense_wipe_register();   // arm the duress-shred Tier-1 wipe hook
    time_screen_create();
    flashlight_screen_create();
    wardriver_screen_create();
    lv_obj_add_event_cb(clock_screen, on_clock_gesture, LV_EVENT_GESTURE, NULL);
    // Hold the boot splash to a minimum ~1.5 s, then reveal the clock.
    while (millis() - boot_splash_ms < 1500) delay(10);
    lv_scr_load(clock_screen);
    lv_obj_del(boot_splash);
    s_last_activity_ms = millis();
    coex_log_heap("before-ble");

#if ARGUS_RADIO_COEXIST || ARGUS_COEX_MEASURE
    // Bring the BLE (Bluedroid) controller up ONCE now - late in setup, after the
    // display/LVGL and clock are up, and while WiFi is still OFF. This is the
    // coexistence-safe order (BLE before WiFi); the controller then stays alive so
    // WiFi coming up later COEXISTS instead of hanging. Matches upstream r3dfish;
    // the mutual-exclusion guards are compiled out via radio_coexist.h. If this
    // ever hangs the watch, flip ARGUS_RADIO_COEXIST to 0 and reflash.
    ble_scan_boot_keepalive();
    coex_log_heap("after-ble");
#endif

    // Power button short press cycles forward through screens:
    //   - clock -> GPS -> LoRa -> WiFi -> Bluetooth -> NFC
    //   - meshtastic -> nodes -> send_message -> map (when tiles exist)
    //     -> configuration (the same chain swipe-RIGHT follows on the
    //     Meshtastic family of screens, so the user can navigate via
    //     buttons or gestures interchangeably)
    //   - settings -> clock
    instance.onEvent([](DeviceEvent_t event, void *params, void *user_data) {
        if (instance.getPMUEventType(params) == PMU_EVENT_KEY_CLICKED) {
            dim_reset_activity();
            if (clock_vibrate) instance.vibrator();
            if (lv_screen_active() == clock_screen)
                gps_screen_show();
            else if (gps_screen_is_active())
                lora_screen_show();
            else if (lora_screen_is_active())
                bluetooth_screen_show();      // BT before WiFi: bring BLE up before
            else if (bluetooth_screen_is_active())
                wifi_radio_screen_show();      // WiFi comes up second (coexistence order)
            else if (wifi_radio_screen_is_active())
                nfc_screen_show();
            else if (meshtastic_screen_is_active())
                nodes_screen_show();
            else if (nodes_screen_is_active())
                send_message_screen_show();
            else if (send_message_screen_is_active()) {
                if (map_screen_available()) map_screen_show();
                else                        configuration_screen_show();
            } else if (map_screen_is_active())
                configuration_screen_show();
            // configuration_screen: swipe-RIGHT is unbound, so power
            // button is intentionally a no-op there too.
            else if (settings_screen_is_active())
                clock_screen_show();
        }
    }, POWER_EVENT, NULL);

    // Back button (GPIO0): GPS → clock
    pinMode(0, INPUT_PULLUP);
    attachInterrupt(0, on_back_btn_isr, FALLING);

    sd_was_ready = instance.isCardReady(); // sync with whatever instance.begin() mounted
    // Pull persisted meshtastic channel state off the SD card now
    // that it's mounted. Safe before LoRa is enabled - channels are
    // just data; meshtastic_set_active() reads from s_channels when
    // it starts transmitting.
    meshtastic_load_channels_from_sd();
    // NOTE: the user's "Enable at boot" radios are brought up LOWER DOWN, AFTER
    // settings_screen_load() has enabled the wallpaper and we have forced it to
    // decode. Bringing a radio up here (before the wallpaper decodes) leaves too
    // little free internal SRAM for the PNG decode -> OOM boot-loop when both a
    // wallpaper AND a boot radio (esp. WiFi ~45 KB) are enabled. See below.
    realign_status_icons();
    layout_battery_indicators(); // seed packing so a boot-enabled alarm renders immediately
    update_clock();
    // Seed the charge readout from the live PMU so a watch booted on the
    // charger shows the bolt on the first frame rather than after the first
    // state change.
    bat_charge.prime(instance.pmu.isVbusIn(), instance.pmu.isCharging());
    update_battery();
    update_lora_indicator();
    update_bt_indicator();
    update_wifi_indicator();
    update_sd_indicator();
    update_nfc_indicator();
    update_wardriver_indicator();

    // Per-mode indicator overlay (Offense border frame only; the "DEF" / "OFF"
    // corner chip is disabled - see theme.cpp). Init now that LVGL + argus_mode
    // are up; refresh on every mode change. The 1 Hz loop tick also refreshes it
    // so the Offense border flips to threat-red live.
    argus_mode_indicator_init();
    argus_mode_on_change([](ArgusMode) { argus_mode_indicator_refresh(); });

    instance.setBrightness(DEVICE_MAX_BRIGHTNESS_LEVEL);

    // Bring the motion-wake accelerometer up to its default state before
    // loading settings — settings_screen_load() will flip it off again if
    // the user has it disabled in /Settings/settings.txt. Doing this here
    // (rather than at the static-init / declaration site) means it happens
    // after instance.begin() has finished bringing the BHI260 firmware up.
    clock_screen_set_motion_wake(true);

    // Restore persisted settings from the SD card (if mounted and file exists).
    // Called after the default setBrightness so a saved brightness wins.
    settings_screen_load();

    // Timezone: restore the last GPS/WiFi-detected UTC offset so the face shows
    // correct local time immediately (must run after settings_screen_load() so
    // the Manual Time flag is known — Manual Time suppresses it). Then register
    // the WiFi auto-sync hook + background worker.
    timezone_load_on_boot();
    timezone_init();

    // Decode the wallpaper NOW, while internal SRAM is still fully free, BEFORE
    // any boot radio takes its ~45 KB (WiFi) share. settings_screen_load() above
    // enabled the wallpaper; lv_refr_now() renders the clock screen, which forces
    // the PNG decode and caches the result in PSRAM. A later WiFi bring-up then
    // cannot collide with the decode. Without this ordering, wallpaper + WiFi-at-
    // boot together OOM the decode and boot-loop the watch.
    lv_refr_now(NULL);

    // Now bring up the user's "Enable at boot" radios (Settings > Enable at boot).
    // Everything is OFF at boot by default; a radio powers on here ONLY if opted in
    // via boot_prefs (/Settings/boot_radios.txt). WiFi and BLE are mutually
    // exclusive in the chooser, so at most one of the two is ever set. If BLE-at-
    // boot boot-loops, recover by setting ble=0 in boot_radios.txt (card reader).
    if (boot_prefs_get(BOOT_RADIO_GPS))  gps_screen_restore_power();
    if (boot_prefs_get(BOOT_RADIO_WIFI)) wifi_radio_screen_restore_power();
    if (boot_prefs_get(BOOT_RADIO_BLE))  bluetooth_screen_restore_power();
    if (boot_prefs_get(BOOT_RADIO_LORA)) lora_screen_restore_power();
    // Repaint the radio indicators now that the boot radios are up (the earlier
    // update_*_indicator() calls ran before this and would show them off).
    update_lora_indicator();
    update_bt_indicator();
    update_wifi_indicator();

    // Re-apply persisted phone-notification state (Daily-wear). Done LAST, after
    // the boot radios, so it correctly no-ops if WiFi-at-boot or a BLE scanner is
    // already holding the radio (and keeps the preference for next time).
    device_mode_restore_boot();
    coex_log_heap("setup-done");
}

// ── Low-memory warning ────────────────────────────────────────────────────
// Internal RAM (not PSRAM) is the scarce resource: WiFi + BLE + the display and
// detection state all draw from it, and when it runs low a radio can fail to
// start or the display can glitch. Rather than fail silently, pop a dismissable
// HADES-red toast so the user knows to shed load (turn off wallpaper / matrix /
// a radio). Created LAZILY on the LVGL top layer only when memory is low - never
// at setup - so it cannot affect boot. Rate-limited so it never spams.
#define LOW_MEM_WARN_BYTES   18432u   // ~18 KB internal free (conservative: only
                                      // fire when genuinely critical, to avoid
                                      // false nags; tune once normal free is known)
#define LOW_MEM_WARN_GAP_MS  60000u   // at most once a minute

// The warning is a MODAL dialog the user must acknowledge (OK button), not a
// timed toast: the earlier auto-dismiss toast flashed for a fraction of a second
// during a radio-toggle failure and could not be read. One dialog at a time -
// s_low_mem_dialog is the live instance (nullptr when none is open).
static lv_obj_t *s_low_mem_dialog = nullptr;

void low_mem_show_dialog(const char *msg);   // defined below; called by the 1 Hz check

static void low_mem_dialog_ok(lv_event_t *e)
{
    (void)e;
    if (s_low_mem_dialog) {
        lv_obj_del(s_low_mem_dialog);   // deletes the modal + its children
        s_low_mem_dialog = nullptr;
    }
}

static void low_mem_check()
{
    static uint32_t s_last_warn_ms = 0;
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

    if (free_internal >= LOW_MEM_WARN_BYTES) return;

    // Never stack a second dialog on top of an unacknowledged one.
    if (s_low_mem_dialog) return;

    uint32_t now = millis();
    if (s_last_warn_ms != 0 && (now - s_last_warn_ms) < LOW_MEM_WARN_GAP_MS) return;
    s_last_warn_ms = now;

    low_mem_show_dialog(
        "#ff5555 LOW MEMORY#\n\n"
        "Not enough free memory to\n"
        "start another radio.\n\n"
        "Turn off Bluetooth, Wallpaper,\n"
        "or Matrix, then try again.");
}

// Build the modal low-memory dialog: a full-screen dim scrim on the top layer so
// nothing behind it is interactive, a HADES-red-bordered card, the message, and
// an OK button that tears the whole modal down. Stays up until the user taps OK.
// Placed on lv_layer_top so it floats above every screen. Safe to call from the
// WiFi/BLE toggle-failure path as well as the 1 Hz background check.
void low_mem_show_dialog(const char *msg)
{
    if (s_low_mem_dialog) return;   // one at a time

    // Modal scrim: covers the whole display, eats touches, dims the background.
    lv_obj_t *scrim = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(scrim);
    lv_obj_set_size(scrim, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(scrim, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scrim, LV_OPA_60, LV_PART_MAIN);
    lv_obj_clear_flag(scrim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_CLICKABLE);   // swallow taps behind the card
    s_low_mem_dialog = scrim;

    lv_obj_t *card = lv_obj_create(scrim);
    lv_obj_set_size(card, 360, 260);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_make(0x18, 0x0A, 0x0A), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(card, HADES_RED, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 3, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 12, LV_PART_MAIN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *lbl = lv_label_create(card);
    lv_label_set_text(lbl, msg);
    lv_label_set_recolor(lbl, true);
    lv_obj_set_style_text_color(lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    lv_obj_t *btn = lv_button_create(card);
    lv_obj_set_size(btn, 160, 60);                  // large, finger-friendly target
    lv_obj_set_ext_click_area(btn, 24);             // extra forgiving hit area
    lv_obj_set_style_bg_color(btn, HADES_RED, LV_PART_MAIN);
    lv_obj_set_style_margin_top(btn, 16, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 8, LV_PART_MAIN);
    lv_obj_add_event_cb(btn, low_mem_dialog_ok, LV_EVENT_CLICKED, NULL);
    lv_obj_t *btn_lbl = lv_label_create(btn);
    lv_label_set_text(btn_lbl, "OK");
    lv_obj_set_style_text_font(btn_lbl, &font_argus_label_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(btn_lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_center(btn_lbl);
}

#ifdef SCREENSHOT_AUTO
// Screen entry points used by the auto-capture (redundant forward declarations
// are harmless if a header already provides them).
void threat_radar_screen_show();
void pet_screen_show();
void alarm_screen_show();
void stopwatch_screen_show();
void timer_screen_show();
void calendar_screen_show();
void meshtastic_screen_show();
void configuration_screen_show();

// Recursively find the descendant with the largest vertical scroll range - the
// scroll container may be nested (screen > wrapper > list), not a direct child.
static lv_obj_t *s_best_sc;
static int32_t   s_best_range;
static void scan_scrollable(lv_obj_t *obj)
{
    uint32_t cnt = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < cnt; i++) {
        lv_obj_t *c = lv_obj_get_child(obj, i);
        if (lv_obj_has_flag(c, LV_OBJ_FLAG_SCROLLABLE)) {
            int32_t range = lv_obj_get_scroll_top(c) + lv_obj_get_scroll_bottom(c);
            if (range > s_best_range) { s_best_range = range; s_best_sc = c; }
        }
        scan_scrollable(c);
    }
}

// Capture the active screen, scrolling its main container so long screens (Tools
// grid, Settings list) are captured in full across several frames named
// <name>-a, <name>-b, ... A screen that fits on one viewport gets a single
// <name>.bmp.
static void capture_screen_scrolled(const char *name)
{
    lv_obj_t *scr = lv_screen_active();

    s_best_sc = NULL;
    s_best_range = 0;
    // The screen ITSELF may be the scroll container (items are direct children,
    // as on Settings), so measure it too - not just its descendants.
    if (lv_obj_has_flag(scr, LV_OBJ_FLAG_SCROLLABLE)) {
        s_best_range = lv_obj_get_scroll_top(scr) + lv_obj_get_scroll_bottom(scr);
        if (s_best_range > 0) s_best_sc = scr;
    }
    scan_scrollable(scr);
    lv_obj_t *sc = s_best_sc;
    int32_t best = s_best_range;
    Serial.printf("[shots]   %s scroll range = %d\n", name, (int)best);

    if (!sc || best <= 8) {                 // fits on one screen
        screenshot_capture_named(name);
        return;
    }

    lv_obj_scroll_to_y(sc, 0, LV_ANIM_OFF); // start at the top
    char fname[48];
    for (int frame = 0; frame < 8; frame++) {
        lv_refr_now(NULL);
        delay(250);
        snprintf(fname, sizeof(fname), "%s-%c", name, (char)('a' + frame));
        bool ok = screenshot_capture_named(fname);
        Serial.printf("[shots]   %s -> %s\n", fname, ok ? "OK" : "FAIL");
        if (lv_obj_get_scroll_bottom(sc) <= 0) break;   // reached the end
        int32_t step = lv_obj_get_height(sc) - 40;      // ~one viewport, slight overlap
        lv_obj_scroll_by(sc, 0, -step, LV_ANIM_OFF);
    }
}

// Dev-only (the `screenshots` build env): walk a curated set of side-effect-free
// screens, render each, and snapshot it to /Screenshots/<name>.bmp on the SD so
// the README can be illustrated from real UI. Requires a microSD card inserted.
// Radio/data screens (WiFi/BT/GPS/NFC/map/wardriver) are intentionally omitted -
// showing them starts hardware; capture those by hand with the long-press tool.
static void screenshot_auto_run()
{
    struct Item { const char *name; void (*show)(); };
    static const Item items[] = {
        { "01-clock",         clock_screen_show         },
        { "02-tools",         tools_screen_show         },
        { "03-radar",         threat_radar_screen_show  },
        { "04-notify",        notifications_screen_show },
        { "05-hexhound",      pet_screen_show           },
        { "06-settings",      settings_screen_show      },
        { "07-meshtastic",    meshtastic_screen_show    },
        { "08-configuration", configuration_screen_show },
        { "09-alarm",         alarm_screen_show         },
        { "10-stopwatch",     stopwatch_screen_show     },
        { "11-timer",         timer_screen_show         },
        { "12-calendar",      calendar_screen_show      },
    };
    Serial.println("[shots] auto-capture starting (need microSD inserted)");
    for (auto &it : items) {
        it.show();
        lv_refr_now(NULL);
        delay(500);            // let the screen's own timers/animations settle
        lv_refr_now(NULL);
        Serial.printf("[shots] %s\n", it.name);
        capture_screen_scrolled(it.name);   // multi-frame for scrollable screens
        delay(150);
    }
    clock_screen_show();
    lv_refr_now(NULL);
    Serial.println("[shots] auto-capture DONE - files in /Screenshots on the SD");
}
#endif

// The BOOT-button SHORT-press "back" chain: step to the previous screen for
// whatever is active (and open Settings from the clock, which the user likes).
// Factored out of loop() so the short-press path calls it while the new
// long-press path jumps home. Behaviour here is UNCHANGED from before.
static void do_boot_back_action()
{
    if (settings_screen_is_active()) {
        clock_screen_show();
    } else if (nfc_write_screen_is_active()) {
        nfc_screen_show();
    } else if (nfc_screen_is_active()) {
        wifi_radio_screen_show();
    } else if (wifi_radio_screen_is_active()) {
        bluetooth_screen_show();      // BT before WiFi (see forward chain)
    } else if (bluetooth_screen_is_active()) {
        lora_screen_show();
    } else if (lora_screen_is_active()) {
        gps_screen_show();
    } else if (configuration_screen_is_active()) {
        // BOOT mirrors swipe-LEFT: commit edits then step back through the chain
        // Config -> Map (if tiles present) -> Send Message.
        configuration_screen_commit();
        if (map_screen_available()) map_screen_show();
        else                        send_message_screen_show();
    } else if (map_screen_is_active()) {
        send_message_screen_show();
    } else if (send_message_screen_is_active()) {
        nodes_screen_show();
    } else if (nodes_screen_is_active()) {
        meshtastic_screen_show();
    } else if (meshtastic_screen_is_active()) {
        clock_screen_show();
    } else if (loot_screen_is_active()) {
        tools_screen_show();          // back to the Offense grid
    } else if (deauth_screen_is_active()) {
        tools_screen_show();          // back to the Defense grid
    } else if (tracker_timeline_screen_is_active()) {
        tools_screen_show();          // back to the Defense grid
    } else if (beacon_spam_screen_is_active()) {
        tools_screen_show();          // back to the Offense grid (tool keeps running)
    } else if (deauth_attack_screen_is_active()) {
        tools_screen_show();          // back to the Offense grid (tool keeps running)
    } else if (rogue_ap_screen_is_active()) {
        tools_screen_show();          // back to the Offense grid (AP keeps running)
    } else if (probe_sniffer_screen_is_active()) {
        tools_screen_show();          // back to the Offense grid (sniffer keeps running)
    } else if (spycam_screen_is_active()) {
        tools_screen_show();          // back to the Defense grid
    } else if (nfc_field_screen_is_active()) {
        tools_screen_show();          // its own tick powers NFC back down on exit
    } else if (pin_pad_screen_is_active()) {
        clock_screen_show();          // cancel the unlock
    } else if (analyze_screen_is_active()
            || bt_analyze_screen_is_active()
            || lora_analyze_screen_is_active()) {
        // Each *_stop() is a no-op when its analyzer isn't running, so calling all
        // three keeps the exit path the same regardless of which one is active.
        analyze_screen_stop();
        bt_analyze_screen_stop();
        lora_analyze_screen_stop();
        if (argus_mode_current() != ArgusMode::Daily) tools_screen_show();
        else clock_screen_show();     // Daily gates Tools; fall back to the clock
    } else if (lv_screen_active() == clock_screen) {
        settings_screen_show();       // from the clock, BOOT opens Settings
    } else {
        clock_screen_show();
    }
}

// ---- Offense unlock "knock": a Long-Short-Long on the BOOT button -------------
//
// The offensive tools have no visible entry point. The only way in is a private
// gesture on the BOOT button (GPIO0): LONG, SHORT, LONG, each beat within
// KNOCK_GAP_MS of the last. A match opens the PIN pad; a correct PIN then enters
// Offense (-> enter_offense()).
//
// KEY LESSON (this failed 3x before): a silent multi-beat timing gesture is
// unperformable - the user cannot tell whether a beat registered, so they cannot
// find the rhythm. So every ACCEPTED beat now gives a short haptic buzz (feel the
// L, S, L land), and a completed knock gives a double buzz. Timing is generous.
//
// Because a knock starts with a LONG, the lone-LONG "home" can't fire on release
// (it might be beat one): a LONG is buffered and its "home" is deferred by the
// gap (boot_knock_poll fires it if no SHORT follows). A SHORT that isn't
// continuing a knock fires its normal "back" immediately.
enum BootPress { BP_SHORT, BP_LONG };
static const uint32_t KNOCK_GAP_MS = 1500;  // generous window to START the next beat
static BootPress s_knock_seq[3];
static int       s_knock_len             = 0;
static uint32_t  s_knock_last_release_ms = 0;

// Short haptic tick so the user can FEEL each accepted knock beat.
static void knock_buzz(int n)
{
    for (int i = 0; i < n; i++) { instance.vibrator(); delay(60); }
}

static void boot_run_action(BootPress p)
{
    if (p == BP_LONG) clock_screen_show();     // home
    else              do_boot_back_action();   // back / Settings-from-clock
}

static void boot_flush_knock()
{
    for (int i = 0; i < s_knock_len; i++) boot_run_action(s_knock_seq[i]);
    s_knock_len = 0;
}

// Fire a deferred beat once its gap goes quiet (measured from the last RELEASE).
static void boot_knock_poll(uint32_t now)
{
    if (s_knock_len > 0 && (now - s_knock_last_release_ms) > KNOCK_GAP_MS)
        boot_flush_knock();
}

// Feed one completed press (already classified). The inter-beat gap is measured
// from the previous beat's RELEASE to THIS beat's PRESS-DOWN (a long beat holds
// ~600ms, so release-to-release made L-S-L impossible). Every accepted knock beat
// buzzes so the gesture is actually performable.
static void boot_knock_feed(BootPress p, uint32_t press_down_ms, uint32_t release_ms)
{
    if (s_knock_len > 0 && (press_down_ms - s_knock_last_release_ms) > KNOCK_GAP_MS)
        boot_flush_knock();

    s_knock_last_release_ms = release_ms;

    if (s_knock_len == 0) {                     // fresh buffer
        if (p == BP_SHORT) { boot_run_action(BP_SHORT); return; }  // knock never starts short
        s_knock_seq[s_knock_len++] = BP_LONG;   // buffer the opening LONG (defer home)
        knock_buzz(1);                          // beat 1 (L) registered
        return;
    }

    if (s_knock_len == 1) {                     // have [L]
        if (p == BP_SHORT) { s_knock_seq[s_knock_len++] = BP_SHORT; knock_buzz(1); return; }  // beat 2 (S)
        boot_run_action(BP_LONG);               // [L,L]: first L was just "home"...
        s_knock_seq[0] = BP_LONG; s_knock_len = 1;  // ...restart the knock at this L
        knock_buzz(1);
        return;
    }

    // Buffer holds [L,S].
    if (p == BP_LONG) {                         // [L,S,L] -> KNOCK
        s_knock_len = 0;
        knock_buzz(2);                          // completed: double buzz
        pin_pad_screen_show();
        return;
    }
    boot_flush_knock();                         // [L,S,S]: not a knock; run [L,S]...
    boot_run_action(BP_SHORT);                  // ...then this trailing SHORT now
}

// Dispatch one classified BOOT press. The Offense knock (L-S-L) is armed ONLY on
// the clock screen - its natural, private entry point. Everywhere else a press
// runs its ordinary back/home action IMMEDIATELY, so an accidental long-ish tap
// can't buffer a stray [L] that then eats the next press (the "BOOT didn't go
// back from the Time screen / had to press twice" bug). down_ms/up_ms carry the
// real press timestamps for knock-gap timing (equal for a synthesized tap).
static void boot_dispatch(BootPress p, uint32_t down_ms, uint32_t up_ms)
{
    dim_reset_activity();
    main_loop_request_lvgl_priority(20);
    bool armed = (argus_mode_current() != ArgusMode::Offense)
              && (s_low_mem_dialog == nullptr)
              && (lv_screen_active() == clock_screen);
    if (armed) {
        boot_knock_feed(p, down_ms, up_ms);     // buzzes each accepted beat
    } else {
        if (s_knock_len > 0) boot_flush_knock();
        if (clock_vibrate) instance.vibrator();
        boot_run_action(p);
    }
}

void loop()
{
    instance.loop(); // required for power button and PMU event dispatch

#ifdef SCREENSHOT_AUTO
    // Fire once, ~6 s after boot, so the UI and SD mount have settled.
    static bool s_shots_done = false;
    if (!s_shots_done && millis() > 6000) {
        s_shots_done = true;
        screenshot_auto_run();
    }
#endif

    // NOTE: auto-bringing the BLE controller up here (ble_scan_boot_keepalive at
    // ~4 s) boot-looped/froze the watch - the BLE bring-up is unstable regardless
    // of timing on this setup. Reverted. The Bluetooth toggle stays on its
    // on-demand bring-up (works when BT is toggled BEFORE WiFi is up); a robust
    // proper fix needs an async/off-thread bring-up, tracked separately.

    motion_wake_poll();   // accel-driven wake; no-op when toggle is off
    timezone_bg_tick();   // apply background WiFi NTP/geolocation results
    // Cheap on every iteration (an indev_state read + a millis() compare);
    // only crosses into the heavy capture+SD-write path on the 3 s edge.
    screenshot_poll();

    // BOOT button (GPIO0, INPUT_PULLUP -> pressed = LOW). Polled duration state
    // machine: SHORT press = back / Settings-from-clock (do_boot_back_action);
    // LONG press (>= BOOT_LONG_MS held) = jump HOME to the clock from anywhere.
    // The Offense knock (L-S-L) layers onto these edges via boot_knock_feed and
    // buzzes each accepted beat so it is actually performable. The FALLING ISR flag
    // is not used for actions (we poll for press DURATION), but attachInterrupt is
    // kept as a possible wake source; clear the flag so it can't linger.
    // Read the FALLING-edge ISR latch BEFORE clearing it: it catches a press the
    // duration-poll below misses when a background tick stalls the loop long enough
    // that the button is pressed AND released between two digitalRead() samples.
    bool boot_isr = back_btn_pressed;
    back_btn_pressed = false;
    static const uint32_t BOOT_LONG_MS     = 600;   // >= this held = long press
    static const uint32_t BOOT_DEBOUNCE_MS = 40;    // shorter = contact bounce
    static const uint32_t BOOT_COOLDOWN_MS = 150;   // swallow release bounce
    static bool     s_boot_down     = false;
    static uint32_t s_boot_down_ms  = 0;
    static uint32_t s_boot_cooldown = 0;
    bool     boot_down = (digitalRead(0) == LOW);
    uint32_t boot_ms   = millis();
    if (boot_down && !s_boot_down && (boot_ms - s_boot_cooldown) > BOOT_COOLDOWN_MS) {
        s_boot_down    = true;
        s_boot_down_ms = boot_ms;
    } else if (!boot_down && s_boot_down) {
        s_boot_down     = false;
        s_boot_cooldown = boot_ms;
        uint32_t held   = boot_ms - s_boot_down_ms;
        if (held >= BOOT_DEBOUNCE_MS)                          // else: bounce, ignore
            boot_dispatch((held >= BOOT_LONG_MS) ? BP_LONG : BP_SHORT, s_boot_down_ms, boot_ms);
    } else if (boot_isr && !boot_down && !s_boot_down && (boot_ms - s_boot_cooldown) > BOOT_COOLDOWN_MS) {
        // ISR latched a falling edge the poll never registered (button already back
        // HIGH, no press in flight) -> a fast tap was dropped to loop latency.
        // Recover it as a SHORT press so quick taps aren't silently lost.
        s_boot_cooldown = boot_ms;
        boot_dispatch(BP_SHORT, boot_ms, boot_ms);
    }
    boot_knock_poll(boot_ms);   // fire a deferred beat once its gap goes quiet

    // Feed NMEA bytes to TinyGPSPlus while the GPS radio is on
    if (gps_screen_is_powered()) {
        instance.gps.loop(false);
    }

    // When a screen transition was just requested, skip all the heavy
    // bg_ticks for a few iterations. With wardriver running, a single
    // loop iteration can take hundreds of ms (SD writes per detector
    // hit, drain_queue, etc) - enough that the LVGL refresh can't keep
    // up with the screen change and the user sees the old screen
    // "frozen" until the next loop iteration finally renders the new
    // one. This window gives LVGL ~12 fast iterations to fully redraw.
    // Queues continue to fill in the callbacks; we just defer the
    // draining for a few ms while the UI catches up.
    // While a modal dialog is open, give LVGL full cadence so its OK button
    // responds instantly. Otherwise the per-iteration background work (bg ticks,
    // 1 Hz detector flushes) runs between lv_task_handler() calls and the touch
    // feels laggy / hard to press, especially on a radio screen that never
    // requested priority itself.
    bool lvgl_priority = s_lvgl_priority_cycles > 0 || s_low_mem_dialog != nullptr;
    {
        // Pause the matrix-rain animation while LVGL is trying to complete
        // a screen transition. Without this, the 120 ms rain timer keeps
        // invalidating 22 recolored labels, forcing a full clock-screen
        // re-render every refresh tick.
        static bool s_matrix_paused_by_us = false;
        if (lvgl_priority && !s_matrix_paused_by_us) {
            matrix_bg_set_paused(true);
            s_matrix_paused_by_us = true;
        } else if (!lvgl_priority && s_matrix_paused_by_us) {
            matrix_bg_set_paused(false);
            s_matrix_paused_by_us = false;
        }
    }
    if (lvgl_priority) s_lvgl_priority_cycles--;


    if (!lvgl_priority) {
        meshtastic_bg_tick();
        // While the SD card is mounted over USB, the host owns the filesystem —
        // suspend the background loggers so the two sides never write at once.
        if (!usb_sd_is_running()) {
            airtag_bg_tick();
            flipper_bg_tick();
            skimmer_bg_tick();
            evil_twin_bg_tick();
            flock_bg_tick();
            threatradar_bg_tick();  // correlate detector hits into follow-scores
            // Bridge: a Likely+ tail flips the HexHound to its wary/HADES-red mood,
            // matching the status-bar/radar HADES flip. (Team-decoupled hook.)
            hexhound_set_threat_level(threatradar_top_level() >= TR_LVL_LIKELY ? 1 : 0);
            handshake_bg_tick();    // drain captured EAPOL frames to /pwn/*.pcap
        }
    }
    // Yield to LVGL between SD-heavy batches. The display uses partial
    // refresh with ~6 tiles per screen, one tile per lv_task_handler call;
    // without these extra passes, a single loop iteration with several
    // detector SD writes can stall screen transitions visibly (new screen
    // draws tile-by-tile on top of the old one for hundreds of ms).
    lv_task_handler();
    if (!lvgl_priority) {
        tpms_bg_tick();
        pager_bg_tick();
        aprs_bg_tick();   // RX drain + queued TX; SD logging self-gates on USB SD
        pingsweep_poll(); // writes /PingSweeps/ once a sweep finishes
        portscan_poll();  // writes /PingSweeps/portscan_* once a scan finishes
        nfc_screen_worker();
        nfc_write_screen_worker();
    }
    lv_task_handler();

    // Any touchscreen press resets the dim timer; rising edge also triggers vibration
    {
        static lv_indev_state_t prev_touch = LV_INDEV_STATE_RELEASED;
        lv_indev_t *indev = lv_indev_get_next(NULL);
        while (indev) {
            if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
                lv_indev_state_t state = lv_indev_get_state(indev);
                if (state == LV_INDEV_STATE_PRESSED) {
                    dim_reset_activity();
                    // Keep LVGL on its fast cadence while the user is interacting
                    // so the next taps (buttons, Exit Offense, etc.) aren't starved
                    // by the heavy per-iteration background work.
                    main_loop_request_lvgl_priority(20);
                    if (clock_vibrate && prev_touch == LV_INDEV_STATE_RELEASED)
                        instance.vibrator();
                }
                prev_touch = state;
                break;
            }
            indev = lv_indev_get_next(indev);
        }
    }

    // Dim timer: check every loop iteration for low latency
    if (s_dim_timeout_ms > 0 && !s_is_dimmed) {
        if (millis() - s_last_activity_ms >= s_dim_timeout_ms) {
            s_is_dimmed = true;
            instance.setBrightness(s_dim_brightness);
        }
    }

    // 1Hz block also skipped during LVGL priority window - it contains
    // I2C-heavy status updates plus wardriver_bg_tick (drain_queue +
    // periodic flush_to_sd, the latter iterating all 32768 ap_table
    // buckets which can block for seconds with a populated session).
    // Missing one 1Hz tick during a screen transition is invisible.
    if (millis() - last_update_ms >= 1000 && !lvgl_priority) {
        last_update_ms = millis();
        update_clock();
        argus_mode_indicator_refresh();   // Offense border flips to threat-red live
        alarm_tick();              // fires the alarm at the set time
        layout_battery_indicators(); // pack alarm/stopwatch/timer icons R→L
        update_battery();
        update_lora_indicator();
        update_bt_indicator();
        update_wifi_indicator();
        update_sd_indicator();
        update_nfc_indicator();
        update_scan_indicators();
        if (!usb_sd_is_running())   // host owns the SD card while mounted
            wardriver_bg_tick();
        update_wardriver_indicator();
        if (wardriver_screen_is_active())
            wardriver_screen_update();
        if (configuration_screen_is_active())
            configuration_screen_update();
        low_mem_check();   // warn (once/min) if internal RAM is running low
        // Fold the live WiFi beacon stream through the pure evil-twin +
        // beacon-flood detectors -> ThreatState -> forensic log + HADES accent /
        // HexHound. millis()/1000 is a monotonic seconds base (never rewinds),
        // which the detectors' sliding windows + decay require.
#if ARGUS_WIFI_THREAT_PIPELINE
        detect_pipeline_tick(millis() / 1000);
#endif
#if ARGUS_BLE_THREAT_PIPELINE
        ble_detect_pipeline_tick(millis() / 1000);
#endif
    }
    // Multiple LVGL passes per loop iteration. Each lv_task_handler call
    // renders at most one partial-refresh tile, and the watch panel needs
    // ~6 tiles for a full screen. When wardriver is dumping detector hits
    // into SD writes between iterations, having only one pass per loop
    // means a single screen transition takes 6+ loop iterations to fully
    // redraw - long enough that the user sees the old screen "freeze".
    lv_task_handler();
    delay(2);
    lv_task_handler();
    delay(2);
    lv_task_handler();
}
