#include "settings_screen.h"
#include "theme.h"
#include "usb_sd.h"
#include "usb_sd_screen.h"   // card reader, reachable in EVERY mode (see below)
#include "boot_prefs.h"
#include "argus_mode.h"
#include "pin_pad_screen.h"
#include "charge_state.h"
#include "detect_log_sd.h"
#include "detect/log_retention.h"   // kMaxAgeDays, for the readout
#include <LilyGoLib.h>
#include <SD.h>
#include <time.h>
#include <Arduino.h>          // millis()
#include <esp_heap_caps.h>    // heap_caps_get_free_size / _total_size

// Defined in main.cpp
void main_loop_request_lvgl_priority(int cycles);
void low_mem_show_dialog(const char *msg);   // modal dialog defined in main.cpp
void clock_screen_show();                    // go home to the watch face
void clock_screen_set_analog_face(bool analog);
void clock_screen_set_12h(bool use_12h);
void clock_screen_set_matrix(bool enabled);
void clock_screen_set_wallpaper(bool enabled);
void clock_screen_set_dim_timeout(uint32_t ms);
void clock_screen_set_dim_brightness(uint8_t level);
void clock_screen_set_show_day(bool show);
void clock_screen_set_show_date(bool show);
void clock_screen_set_show_ampm(bool show);
void clock_screen_set_show_secs(bool show);
void clock_screen_set_vibrate(bool enabled);
void clock_screen_set_motion_wake(bool enabled);
void clock_screen_set_manual_override(bool on);
void clock_screen_apply_manual_time(int year, int mon, int day, int hour, int min);
void clock_screen_get_local_time(struct tm *out);

// Manual-time roller range: years selectable in the date picker.
#define MANUAL_YEAR_BASE  2020
#define MANUAL_YEAR_COUNT 41   // 2020 … 2060

static lv_obj_t *settings_screen;
static lv_obj_t *s_ofs_btn = nullptr;   // one Offense button; label+action follow the mode
static lv_obj_t *s_ofs_lbl = nullptr;
static lv_obj_t *brightness_slider;
static lv_obj_t *brightness_val_label;
static lv_obj_t *sysinfo_label = nullptr;   // System Info readout (bottom of screen)
static lv_obj_t *s_settings_title = nullptr;   // "SETTINGS" heading; repainted per mode on show

// Fill the System Info label with live heap / PSRAM / battery / uptime. Cheap;
// called on settings_screen_show() so it snapshots each time the screen opens.
static void settings_update_sysinfo()
{
    if (!sysinfo_label) return;
    unsigned heap_kb  = (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
    unsigned ps_free  = (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
    unsigned ps_total = (unsigned)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024);
    int  batt = instance.pmu.getBatteryPercent();
    // Same three-state split the clock-face bolt uses, so the two readouts
    // never disagree. Snapshot (no debounce): this label is rebuilt on show,
    // not ticked, so there is nothing here to strobe.
    ChargeState cs = charge_state_raw(instance.pmu.isVbusIn(),
                                      instance.pmu.isCharging());
    const char *chg = (cs == ChargeState::Charging) ? " chg"
                    : (cs == ChargeState::Topped)   ? " full"
                                                    : "";
    uint32_t up = millis() / 1000;
    // Detection-record count. This walks the logs on SD, so it is deliberately
    // sampled here (settings_screen_show) rather than on a tick.
    int det = detect_log_total_records();
    lv_label_set_text_fmt(sysinfo_label,
        "Heap : %u KB free\n"
        "PSRAM: %u / %u KB\n"
        "Batt : %d%%%s\n"
        "Up   : %luh %02lum %02lus\n"
        "Det  : %d record(s), %dd max",
        heap_kb, ps_free, ps_total,
        batt, chg,
        (unsigned long)(up / 3600), (unsigned long)((up % 3600) / 60),
        (unsigned long)(up % 60),
        det, detlog::kMaxAgeDays);
}
static int32_t   s_brightness = DEVICE_MAX_BRIGHTNESS_LEVEL;
static lv_obj_t *face_switch;
static lv_obj_t *face_val_label;
static lv_obj_t *hour_format_row;
static lv_obj_t *hour_format_switch;
static lv_obj_t *hour_format_val_label;
static lv_obj_t *matrix_switch;
static lv_obj_t *matrix_val_label;
static lv_obj_t *wallpaper_switch;
static lv_obj_t *wallpaper_val_label;
static lv_obj_t *dim_dropdown;
static lv_obj_t *dim_brightness_slider;
static lv_obj_t *dim_brightness_val_label;
static int32_t   s_dim_brightness = DEVICE_MAX_BRIGHTNESS_LEVEL / 4;
static lv_obj_t *ampm_row;
static lv_obj_t *ampm_switch;
static lv_obj_t *secs_row;
static lv_obj_t *secs_switch;
static lv_obj_t *show_day_switch;
static lv_obj_t *show_date_switch;
static lv_obj_t *vibrate_switch;
static lv_obj_t *motion_wake_switch;
static lv_obj_t *motion_wake_val_label;
static lv_obj_t *manual_time_switch;
static lv_obj_t *manual_time_val_label;
// Screenshot long-press toggle — bottom of the settings list. Disabled
// (greyed + forced off) whenever SD isn't mounted, since there's nowhere
// to write the capture without a card.
static lv_obj_t *screenshot_row;
static lv_obj_t *screenshot_switch;
static lv_obj_t *screenshot_val_label;
static lv_obj_t *screenshot_hint;
// "Enable at boot" per-radio opt-in toggles. Each writes its flag to
// /Settings/boot_radios.txt via boot_prefs (NOT to settings.txt). No Bluetooth
// toggle: the BLE controller must never be brought up at boot (it boot-loops
// the watch), so BT is manual-only and has no boot pref.
static lv_obj_t *boot_wifi_switch;
static lv_obj_t *boot_wifi_val_label;
static lv_obj_t *boot_ble_switch;
static lv_obj_t *boot_ble_val_label;
static lv_obj_t *boot_lora_switch;
static lv_obj_t *boot_lora_val_label;
static lv_obj_t *boot_gps_switch;
static lv_obj_t *boot_gps_val_label;
static lv_obj_t *year_roller;
static lv_obj_t *month_roller;
static lv_obj_t *day_roller;
static lv_obj_t *hour_roller;
static lv_obj_t *minute_roller;

// Suppresses settings_save_to_sd() during settings_screen_load() so applying
// loaded values via lv_obj_add_state / lv_dropdown_set_selected — which fire
// LV_EVENT_VALUE_CHANGED — doesn't redundantly rewrite the file we just read.
static bool g_loading = false;

// ARGUS Daily/Defense mode controls (appended to the bottom of the list).
static lv_obj_t *defense_switch;
static lv_obj_t *defense_val_label;
static lv_obj_t *defpersist_switch;
static lv_obj_t *defpersist_val_label;

// ---- Shiftable-row registry ------------------------------------------------
//
// Every absolutely-positioned object that sits BELOW the three face-specific
// rows (12h format / AM-PM / Show seconds) gets registered here with its
// base (digital-mode) Y. When the user flips the face toggle to analog, all
// three rows disappear and apply_layout() shifts each registered row up
// by 3 × 48 = 144 px so the rows below close the gap rather than leaving
// blank space. Toggling back to digital re-applies the base Y.
#define FACE_HIDDEN_SHIFT  (3 * 48)
// Manual-time-only content (hint + 5 rollers + SET TIME button) spans
// y=900..y=1106 in design coords. When the Manual Time switch is off the
// rows below — the screenshot section separator + toggle + hint — close
// the gap by shifting up this much, leaving a normal row-to-row gap below
// manual_row (the switch row itself stays visible at y=852).
#define MANUAL_HIDDEN_SHIFT (230)
// First Y offset that belongs to the manual-time editor (the hint line).
// register_shiftable entries with base_y >= this also pick up the
// MANUAL_HIDDEN_SHIFT when the switch is off.
#define MANUAL_SECTION_TOP  (900)
// Must exceed the TOTAL number of register_shiftable*() entries created at
// runtime. Note the 5 manual-time rollers each register twice (header + roller),
// so the runtime count (~35+) is higher than the call-site count. If this cap is
// too low the LAST rows registered (the "Enable at boot" section, added last)
// silently fail to register and miss the manual-time -230px shift, leaving a big
// dead gap between WiFi and the rows below it. 64 gives generous headroom.
#define MAX_SHIFTABLE      64

struct ShiftableRow { lv_obj_t *obj; int base_x; int base_y; };
static ShiftableRow s_shiftable[MAX_SHIFTABLE];
static int          s_shiftable_count = 0;

// Most rows are LV_ALIGN_TOP_MID with x=0; the helper defaults to that.
static void register_shiftable(lv_obj_t *obj, int base_y)
{
    if (s_shiftable_count < MAX_SHIFTABLE)
        s_shiftable[s_shiftable_count++] = { obj, 0, base_y };
}

// Variant for objects that were aligned with a non-zero X offset (e.g.
// the manual-time rollers, each pinned to its own column). Preserves the
// X across the Y shift so they don't snap to centre on a reflow.
static void register_shiftable_xy(lv_obj_t *obj, int base_x, int base_y)
{
    if (s_shiftable_count < MAX_SHIFTABLE)
        s_shiftable[s_shiftable_count++] = { obj, base_x, base_y };
}

// ---- Manual-time-only registry --------------------------------------------
//
// The manual-time hint line, five rollers + their headers, and the SET TIME
// button only make sense when the manual-time switch is on — they're the
// editor for the override value. apply_layout() shows or hides them as a
// group based on the switch state, and additionally shifts the rows below
// up by MANUAL_HIDDEN_SHIFT so no empty space is left behind.
#define MAX_MANUAL_TIME_OBJS 16
static lv_obj_t *s_manual_objs[MAX_MANUAL_TIME_OBJS];
static int       s_manual_count = 0;

static void register_manual_obj(lv_obj_t *obj)
{
    if (s_manual_count < MAX_MANUAL_TIME_OBJS)
        s_manual_objs[s_manual_count++] = obj;
}

// Single layout pass that owns both shifts: the face shift (analog hides
// hour_format / ampm / show_secs rows above) and the manual-time shift
// (off hides the editor controls below). Reads the live switch states so
// callers don't have to pass them in. Hidden rows still get a position,
// just one that's off-screen above their normal slot — harmless because
// they're invisible.
static void apply_layout()
{
    bool analog    = lv_obj_has_state(face_switch,        LV_STATE_CHECKED);
    bool manual_on = lv_obj_has_state(manual_time_switch, LV_STATE_CHECKED);

    if (analog) {
        lv_obj_add_flag(hour_format_row, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ampm_row,        LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(secs_row,        LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(hour_format_row, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ampm_row,        LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(secs_row,        LV_OBJ_FLAG_HIDDEN);
    }

    for (int i = 0; i < s_manual_count; i++) {
        if (manual_on) lv_obj_clear_flag(s_manual_objs[i], LV_OBJ_FLAG_HIDDEN);
        else           lv_obj_add_flag  (s_manual_objs[i], LV_OBJ_FLAG_HIDDEN);
    }

    int face_offset = analog ? -FACE_HIDDEN_SHIFT : 0;
    for (int i = 0; i < s_shiftable_count; i++) {
        int base_y = s_shiftable[i].base_y;
        int offset = face_offset;
        if (!manual_on && base_y >= MANUAL_SECTION_TOP)
            offset -= MANUAL_HIDDEN_SHIFT;
        lv_obj_align(s_shiftable[i].obj, LV_ALIGN_TOP_MID,
                     s_shiftable[i].base_x, base_y + offset);
    }
}

static const char *SETTINGS_PATH = "/Settings/settings.txt";

// Forward declaration so the change callbacks can call it
static void settings_save_to_sd();

static void on_face_changed(lv_event_t *e)
{
    bool analog = lv_obj_has_state(face_switch, LV_STATE_CHECKED);
    lv_label_set_text(face_val_label, analog ? "Analog" : "Digital");
    clock_screen_set_analog_face(analog);
    // 12h / AM-PM / Show-seconds rows are only relevant on the digital
    // face. apply_layout hides them AND closes the resulting gap by
    // shifting every row below up by 144 px.
    apply_layout();
    settings_save_to_sd();
}

static void on_hour_format_changed(lv_event_t *e)
{
    bool use_12h = lv_obj_has_state(hour_format_switch, LV_STATE_CHECKED);
    lv_label_set_text(hour_format_val_label, use_12h ? "12h" : "24h");
    clock_screen_set_12h(use_12h);
    settings_save_to_sd();
}

static void on_matrix_changed(lv_event_t *e)
{
    bool on = lv_obj_has_state(matrix_switch, LV_STATE_CHECKED);
    lv_label_set_text(matrix_val_label, on ? "On" : "Off");
    clock_screen_set_matrix(on);
    settings_save_to_sd();
}

static void on_wallpaper_changed(lv_event_t *e)
{
    bool on = lv_obj_has_state(wallpaper_switch, LV_STATE_CHECKED);
    lv_label_set_text(wallpaper_val_label, on ? "On" : "Off");
    clock_screen_set_wallpaper(on);
    settings_save_to_sd();
}

static const uint32_t DIM_TIMEOUT_MS[] = { 0, 5000, 30000, 60000, 300000 };

static void on_dim_timeout_changed(lv_event_t *e)
{
    uint32_t idx = lv_dropdown_get_selected(dim_dropdown);
    clock_screen_set_dim_timeout(DIM_TIMEOUT_MS[idx]);
    settings_save_to_sd();
}

static void on_dim_brightness_changed(lv_event_t *e)
{
    s_dim_brightness = lv_slider_get_value(dim_brightness_slider);
    clock_screen_set_dim_brightness((uint8_t)s_dim_brightness);
    int pct = (int)s_dim_brightness * 100 / (int)DEVICE_MAX_BRIGHTNESS_LEVEL;
    lv_label_set_text_fmt(dim_brightness_val_label, "%d%%", pct);
    // No save here — a drag fires VALUE_CHANGED on every pixel.
    // The save runs once on LV_EVENT_RELEASED via on_slider_released.
}

// Slider drags fire VALUE_CHANGED many times per drag. Hooking the save into
// LV_EVENT_RELEASED instead means we write the SD once when the user lifts
// their finger. Taps on the track also fire RELEASED, so single-tap changes
// still persist.
static void on_slider_released(lv_event_t *e)
{
    settings_save_to_sd();
}

static void on_ampm_changed(lv_event_t *e)
{
    clock_screen_set_show_ampm(lv_obj_has_state(ampm_switch, LV_STATE_CHECKED));
    settings_save_to_sd();
}

static void on_secs_changed(lv_event_t *e)
{
    clock_screen_set_show_secs(lv_obj_has_state(secs_switch, LV_STATE_CHECKED));
    settings_save_to_sd();
}

static void on_vibrate_changed(lv_event_t *e)
{
    clock_screen_set_vibrate(lv_obj_has_state(vibrate_switch, LV_STATE_CHECKED));
    settings_save_to_sd();
}

static void on_motion_wake_changed(lv_event_t *e)
{
    bool on = lv_obj_has_state(motion_wake_switch, LV_STATE_CHECKED);
    lv_label_set_text(motion_wake_val_label, on ? "On" : "Off");
    clock_screen_set_motion_wake(on);
    settings_save_to_sd();
}

static void on_screenshot_changed(lv_event_t *)
{
    bool on = lv_obj_has_state(screenshot_switch, LV_STATE_CHECKED);
    lv_label_set_text(screenshot_val_label, on ? "On" : "Off");
    settings_save_to_sd();
}

static void on_show_day_changed(lv_event_t *e)
{
    clock_screen_set_show_day(lv_obj_has_state(show_day_switch, LV_STATE_CHECKED));
    settings_save_to_sd();
}

static void on_show_date_changed(lv_event_t *e)
{
    clock_screen_set_show_date(lv_obj_has_state(show_date_switch, LV_STATE_CHECKED));
    settings_save_to_sd();
}

static void on_brightness_changed(lv_event_t *e)
{
    s_brightness = lv_slider_get_value(brightness_slider);
    instance.setBrightness((uint8_t)s_brightness);
    int pct = (int)s_brightness * 100 / (int)DEVICE_MAX_BRIGHTNESS_LEVEL;
    lv_label_set_text_fmt(brightness_val_label, "%d%%", pct);
    // Save deferred to LV_EVENT_RELEASED — see on_slider_released.
}

// Builds a newline-joined option string for a numeric roller, e.g. lo=0 hi=23
// width=2 → "00\n01\n…\n23". The roller copies the string, so a temp buffer is
// fine.
static void build_num_opts(char *buf, size_t bufsz, int lo, int hi, int width)
{
    size_t pos = 0;
    buf[0] = '\0';
    for (int v = lo; v <= hi && pos < bufsz; v++) {
        int n = snprintf(buf + pos, bufsz - pos, "%0*d", width, v);
        if (n < 0) break;
        pos += (size_t)n;
        if (v < hi && pos < bufsz - 1) buf[pos++] = '\n';
    }
    buf[pos < bufsz ? pos : bufsz - 1] = '\0';
}

// Number of days in a given month, accounting for leap years.
static int days_in_month(int month, int year)
{
    static const int dim[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (month < 1 || month > 12) return 31;
    if (month == 2) {
        bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
        return leap ? 29 : 28;
    }
    return dim[month - 1];
}

// Creates a header label + roller pair as children of settings_screen, both
// horizontally centred at x_off. Returns the roller. Both children are
// registered as shiftable so the analog-face reflow moves them as a unit.
static lv_obj_t *make_time_roller(const char *header, const char *opts,
                                  lv_coord_t x_off, lv_coord_t y, lv_coord_t w)
{
    lv_obj_t *hl = lv_label_create(settings_screen);
    lv_obj_set_style_text_color(hl, ARGUS_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(hl, &font_argus_label_16, LV_PART_MAIN);
    lv_label_set_text(hl, header);
    lv_obj_align(hl, LV_ALIGN_TOP_MID, x_off, y);
    register_shiftable_xy(hl, x_off, y);
    register_manual_obj(hl);

    lv_obj_t *r = lv_roller_create(settings_screen);
    lv_roller_set_options(r, opts, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(r, 3);
    lv_obj_set_width(r, w);
    lv_obj_set_style_bg_color(r, lv_color_make(0x22, 0x22, 0x22), LV_PART_MAIN);
    lv_obj_set_style_text_color(r, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(r, &font_argus_label_20, LV_PART_MAIN);
    lv_obj_set_style_border_color(r, lv_color_make(0x55, 0x55, 0x55), LV_PART_MAIN);
    lv_obj_set_style_border_width(r, 1, LV_PART_MAIN);
    lv_obj_set_style_bg_color(r, lv_color_make(0x00, 0x55, 0x33), LV_PART_SELECTED);
    lv_obj_set_style_text_color(r, lv_color_white(), LV_PART_SELECTED);
    lv_obj_align(r, LV_ALIGN_TOP_MID, x_off, y + 22);
    register_shiftable_xy(r, x_off, y + 22);
    register_manual_obj(r);
    return r;
}

static void on_manual_time_changed(lv_event_t *e)
{
    bool on = lv_obj_has_state(manual_time_switch, LV_STATE_CHECKED);
    lv_label_set_text(manual_time_val_label, on ? "On" : "Off");
    clock_screen_set_manual_override(on);
    // Hide the editor controls + collapse the gap so the screenshot
    // section below sits flush against the manual-time switch.
    apply_layout();
    settings_save_to_sd();
}

// SET TIME: write the rollers to the RTC and switch the watch into manual mode.
static void on_set_time_clicked(lv_event_t *e)
{
    int year   = MANUAL_YEAR_BASE + (int)lv_roller_get_selected(year_roller);
    int month  = 1 + (int)lv_roller_get_selected(month_roller);
    int day    = 1 + (int)lv_roller_get_selected(day_roller);
    int hour   =     (int)lv_roller_get_selected(hour_roller);
    int minute =     (int)lv_roller_get_selected(minute_roller);

    // Clamp the day to the selected month so e.g. "Feb 31" can't be written.
    int dim = days_in_month(month, year);
    if (day > dim) day = dim;

    clock_screen_apply_manual_time(year, month, day, hour, minute);

    // Setting the time by hand switches the watch into manual mode.
    lv_obj_add_state(manual_time_switch, LV_STATE_CHECKED);
    lv_label_set_text(manual_time_val_label, "On");
    settings_save_to_sd();
}

// ---- "Enable at boot" toggle callbacks ------------------------------------
// Each toggle persists its per-radio opt-in flag to /Settings/boot_radios.txt
// via boot_prefs. These do NOT touch settings.txt, so they intentionally do not
// call settings_save_to_sd().

static void on_boot_wifi_changed(lv_event_t *e)
{
    bool on = lv_obj_has_state(boot_wifi_switch, LV_STATE_CHECKED);
    // WiFi and BLE cannot both auto-enable (they can't run together). If BLE is
    // already the chosen boot radio, refuse and tell the user rather than
    // silently overriding their earlier choice.
    if (on && boot_prefs_get(BOOT_RADIO_BLE)) {
        lv_obj_clear_state(boot_wifi_switch, LV_STATE_CHECKED);
        lv_label_set_text(boot_wifi_val_label, "Off");
        low_mem_show_dialog(
            "#ffcc33 ONE RADIO AT BOOT#\n\n"
            "WiFi and Bluetooth cannot\n"
            "both auto-enable at boot.\n\n"
            "Turn Bluetooth off here\n"
            "first, then enable WiFi.");
        return;
    }
    lv_label_set_text(boot_wifi_val_label, on ? "On" : "Off");
    boot_prefs_set(BOOT_RADIO_WIFI, on);
}

static void on_boot_ble_changed(lv_event_t *e)
{
    bool on = lv_obj_has_state(boot_ble_switch, LV_STATE_CHECKED);
    // Symmetric mutual exclusion with WiFi (see on_boot_wifi_changed).
    if (on && boot_prefs_get(BOOT_RADIO_WIFI)) {
        lv_obj_clear_state(boot_ble_switch, LV_STATE_CHECKED);
        lv_label_set_text(boot_ble_val_label, "Off");
        low_mem_show_dialog(
            "#ffcc33 ONE RADIO AT BOOT#\n\n"
            "WiFi and Bluetooth cannot\n"
            "both auto-enable at boot.\n\n"
            "Turn WiFi off here first,\n"
            "then enable Bluetooth.");
        return;
    }
    lv_label_set_text(boot_ble_val_label, on ? "On" : "Off");
    boot_prefs_set(BOOT_RADIO_BLE, on);
}

static void on_boot_lora_changed(lv_event_t *e)
{
    bool on = lv_obj_has_state(boot_lora_switch, LV_STATE_CHECKED);
    lv_label_set_text(boot_lora_val_label, on ? "On" : "Off");
    boot_prefs_set(BOOT_RADIO_LORA, on);
}

static void on_boot_gps_changed(lv_event_t *e)
{
    bool on = lv_obj_has_state(boot_gps_switch, LV_STATE_CHECKED);
    lv_label_set_text(boot_gps_val_label, on ? "On" : "Off");
    boot_prefs_set(BOOT_RADIO_GPS, on);
}

// Creates one "Enable at boot" row (left label + On/Off state label + toggle),
// styled exactly like the Matrix BG row. The checked state is seeded BEFORE the
// callback is wired so seeding never fires a spurious save. The row is
// registered shiftable so the face / manual-time reflows move it as a unit.
// Returns the switch; the state label is written to *val_out.
static lv_obj_t *make_boot_row(const char *label, int y, bool checked,
                               lv_event_cb_t cb, lv_obj_t **val_out)
{
    lv_obj_t *row = lv_obj_create(settings_screen);
    lv_obj_set_size(row, 380, 40);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, y);
    register_shiftable(row, y);

    lv_obj_t *lbl = lv_label_create(row);
    lv_obj_set_style_text_color(lbl, ARGUS_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &font_argus_label_20, LV_PART_MAIN);
    lv_label_set_text(lbl, label);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *val = lv_label_create(row);
    lv_obj_set_style_text_color(val, ARGUS_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(val, &font_argus_label_20, LV_PART_MAIN);
    lv_label_set_text(val, checked ? "On" : "Off");
    lv_obj_align(val, LV_ALIGN_RIGHT_MID, -80, 0);

    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, 70, 34);
    lv_obj_set_style_bg_color(sw, lv_color_make(0x44, 0x44, 0x44), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(sw, ARGUS_ACCENT, LV_PART_MAIN | LV_STATE_CHECKED);
    // Seed state before wiring the callback so the seed doesn't re-save the
    // value we just read out of boot_prefs.
    if (checked) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);

    *val_out = val;
    return sw;
}

// Daily<->Defense toggle. Offense is never reachable from here (hidden knock+PIN
// only), so this switch is a plain two-state Daily(off)/Defense(on) control.
static void on_defense_mode_changed(lv_event_t *)
{
    bool on = lv_obj_has_state(defense_switch, LV_STATE_CHECKED);
    lv_label_set_text(defense_val_label, on ? "On" : "Off");
    argus_mode_set(on ? ArgusMode::Defense : ArgusMode::Daily);
}

// "Keep on boot": persist Defense across reboots (default off = always boot Daily).
static void on_defpersist_changed(lv_event_t *)
{
    bool on = lv_obj_has_state(defpersist_switch, LV_STATE_CHECKED);
    lv_label_set_text(defpersist_val_label, on ? "On" : "Off");
    argus_mode_set_defense_persist(on);
}

void settings_screen_create()
{
    settings_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(settings_screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(settings_screen, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(settings_screen, 80, LV_PART_MAIN);
    lv_obj_set_scroll_dir(settings_screen, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(settings_screen, LV_SCROLLBAR_MODE_AUTO);

    // Title
    lv_obj_t *title = lv_label_create(settings_screen);
    s_settings_title = title;   // repainted per mode in settings_screen_show()
    lv_obj_set_style_text_color(title, argus_base_accent(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_argus_ui, LV_PART_MAIN);
    lv_label_set_text(title, "SETTINGS");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 5);

    // Row: "Brightness" label on the left, percentage on the right
    lv_obj_t *row = lv_obj_create(settings_screen);
    lv_obj_set_size(row, 380, 30);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 110);

    lv_obj_t *lbl = lv_label_create(row);
    lv_obj_set_style_text_color(lbl, ARGUS_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &font_argus_label_20, LV_PART_MAIN);
    lv_label_set_text(lbl, "Brightness");
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

    brightness_val_label = lv_label_create(row);
    lv_obj_set_style_text_color(brightness_val_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(brightness_val_label, &font_argus_label_20, LV_PART_MAIN);
    lv_label_set_text(brightness_val_label, "100%");
    lv_obj_align(brightness_val_label, LV_ALIGN_RIGHT_MID, 0, 0);

    // Horizontal brightness slider
    brightness_slider = lv_slider_create(settings_screen);
    lv_obj_set_size(brightness_slider, 320, 50);
    lv_obj_align(brightness_slider, LV_ALIGN_TOP_MID, 0, 155);
    lv_slider_set_range(brightness_slider, 1, DEVICE_MAX_BRIGHTNESS_LEVEL);
    lv_slider_set_value(brightness_slider, s_brightness, LV_ANIM_OFF);

    // Track styling
    lv_obj_set_style_bg_color(brightness_slider, lv_color_make(0x44, 0x44, 0x44), LV_PART_MAIN);
    lv_obj_set_style_radius(brightness_slider, 8, LV_PART_MAIN);
    lv_obj_set_style_border_width(brightness_slider, 0, LV_PART_MAIN);

    // Filled (indicator) portion
    lv_obj_set_style_bg_color(brightness_slider, lv_color_make(0xFF, 0xFF, 0xFF), LV_PART_INDICATOR);
    lv_obj_set_style_radius(brightness_slider, 8, LV_PART_INDICATOR);

    // Knob: large enough for comfortable touch
    lv_obj_set_style_bg_color(brightness_slider, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_radius(brightness_slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_pad_all(brightness_slider, 10, LV_PART_KNOB);
    lv_obj_set_style_border_width(brightness_slider, 0, LV_PART_KNOB);

    lv_obj_add_event_cb(brightness_slider, on_brightness_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(brightness_slider, on_slider_released,    LV_EVENT_RELEASED,      NULL);

    // Separator
    lv_obj_t *sep = lv_obj_create(settings_screen);
    lv_obj_set_size(sep, 380, 1);
    lv_obj_set_style_bg_color(sep, lv_color_make(0x33, 0x33, 0x33), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(sep, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(sep, 0, LV_PART_MAIN);
    lv_obj_align(sep, LV_ALIGN_TOP_MID, 0, 222);

    // Watch Face row: label left, state label + toggle right
    lv_obj_t *face_row = lv_obj_create(settings_screen);
    lv_obj_set_size(face_row, 380, 40);
    lv_obj_set_style_bg_opa(face_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(face_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(face_row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(face_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(face_row, LV_ALIGN_TOP_MID, 0, 232);

    lv_obj_t *face_lbl = lv_label_create(face_row);
    lv_obj_set_style_text_color(face_lbl, ARGUS_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(face_lbl, &font_argus_label_20, LV_PART_MAIN);
    lv_label_set_text(face_lbl, "Watch Face");
    lv_obj_align(face_lbl, LV_ALIGN_LEFT_MID, 0, 0);

    face_val_label = lv_label_create(face_row);
    lv_obj_set_style_text_color(face_val_label, ARGUS_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(face_val_label, &font_argus_label_20, LV_PART_MAIN);
    lv_label_set_text(face_val_label, "Digital");
    lv_obj_align(face_val_label, LV_ALIGN_RIGHT_MID, -80, 0);

    face_switch = lv_switch_create(face_row);
    lv_obj_set_size(face_switch, 70, 34);
    lv_obj_set_style_bg_color(face_switch, lv_color_make(0x44, 0x44, 0x44), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(face_switch, ARGUS_ACCENT, LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_add_event_cb(face_switch, on_face_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_align(face_switch, LV_ALIGN_RIGHT_MID, 0, 0);

    // Time format row — visible only when Digital face is active
    hour_format_row = lv_obj_create(settings_screen);
    lv_obj_set_size(hour_format_row, 380, 40);
    lv_obj_set_style_bg_opa(hour_format_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(hour_format_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(hour_format_row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(hour_format_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(hour_format_row, LV_ALIGN_TOP_MID, 0, 280);

    lv_obj_t *hf_lbl = lv_label_create(hour_format_row);
    lv_obj_set_style_text_color(hf_lbl, ARGUS_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(hf_lbl, &font_argus_label_20, LV_PART_MAIN);
    lv_label_set_text(hf_lbl, "Time Format");
    lv_obj_align(hf_lbl, LV_ALIGN_LEFT_MID, 0, 0);

    hour_format_val_label = lv_label_create(hour_format_row);
    lv_obj_set_style_text_color(hour_format_val_label, ARGUS_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(hour_format_val_label, &font_argus_label_20, LV_PART_MAIN);
    lv_label_set_text(hour_format_val_label, "12h");
    lv_obj_align(hour_format_val_label, LV_ALIGN_RIGHT_MID, -80, 0);

    hour_format_switch = lv_switch_create(hour_format_row);
    lv_obj_set_size(hour_format_switch, 70, 34);
    lv_obj_set_style_bg_color(hour_format_switch, lv_color_make(0x44, 0x44, 0x44), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(hour_format_switch, ARGUS_ACCENT, LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_add_state(hour_format_switch, LV_STATE_CHECKED);  // default 12h
    lv_obj_add_event_cb(hour_format_switch, on_hour_format_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_align(hour_format_switch, LV_ALIGN_RIGHT_MID, 0, 0);

    // Display AM/PM row — visible only when Digital face is active
    ampm_row = lv_obj_create(settings_screen);
    lv_obj_set_size(ampm_row, 380, 40);
    lv_obj_set_style_bg_opa(ampm_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(ampm_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ampm_row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(ampm_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(ampm_row, LV_ALIGN_TOP_MID, 0, 328);

    lv_obj_t *ampm_lbl = lv_label_create(ampm_row);
    lv_obj_set_style_text_color(ampm_lbl, ARGUS_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(ampm_lbl, &font_argus_label_20, LV_PART_MAIN);
    lv_label_set_text(ampm_lbl, "Display AM/PM");
    lv_obj_align(ampm_lbl, LV_ALIGN_LEFT_MID, 0, 0);

    ampm_switch = lv_switch_create(ampm_row);
    lv_obj_set_size(ampm_switch, 70, 34);
    lv_obj_set_style_bg_color(ampm_switch, lv_color_make(0x44, 0x44, 0x44), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ampm_switch, ARGUS_ACCENT, LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_add_state(ampm_switch, LV_STATE_CHECKED);  // default on
    lv_obj_add_event_cb(ampm_switch, on_ampm_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_align(ampm_switch, LV_ALIGN_RIGHT_MID, 0, 0);

    // Display Seconds row — visible only when Digital face is active
    secs_row = lv_obj_create(settings_screen);
    lv_obj_set_size(secs_row, 380, 40);
    lv_obj_set_style_bg_opa(secs_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(secs_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(secs_row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(secs_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(secs_row, LV_ALIGN_TOP_MID, 0, 376);

    lv_obj_t *secs_lbl = lv_label_create(secs_row);
    lv_obj_set_style_text_color(secs_lbl, ARGUS_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(secs_lbl, &font_argus_label_20, LV_PART_MAIN);
    lv_label_set_text(secs_lbl, "Display Seconds");
    lv_obj_align(secs_lbl, LV_ALIGN_LEFT_MID, 0, 0);

    secs_switch = lv_switch_create(secs_row);
    lv_obj_set_size(secs_switch, 70, 34);
    lv_obj_set_style_bg_color(secs_switch, lv_color_make(0x44, 0x44, 0x44), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(secs_switch, ARGUS_ACCENT, LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_add_state(secs_switch, LV_STATE_CHECKED);  // default on
    lv_obj_add_event_cb(secs_switch, on_secs_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_align(secs_switch, LV_ALIGN_RIGHT_MID, 0, 0);

    // Matrix background row — always visible, independent of watch face
    lv_obj_t *matrix_row = lv_obj_create(settings_screen);
    lv_obj_set_size(matrix_row, 380, 40);
    lv_obj_set_style_bg_opa(matrix_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(matrix_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(matrix_row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(matrix_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(matrix_row, LV_ALIGN_TOP_MID, 0, 424);
    register_shiftable(matrix_row, 424);

    lv_obj_t *mx_lbl = lv_label_create(matrix_row);
    lv_obj_set_style_text_color(mx_lbl, ARGUS_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(mx_lbl, &font_argus_label_20, LV_PART_MAIN);
    lv_label_set_text(mx_lbl, "Matrix BG");
    lv_obj_align(mx_lbl, LV_ALIGN_LEFT_MID, 0, 0);

    matrix_val_label = lv_label_create(matrix_row);
    lv_obj_set_style_text_color(matrix_val_label, ARGUS_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(matrix_val_label, &font_argus_label_20, LV_PART_MAIN);
    lv_label_set_text(matrix_val_label, "Off");
    lv_obj_align(matrix_val_label, LV_ALIGN_RIGHT_MID, -80, 0);

    matrix_switch = lv_switch_create(matrix_row);
    lv_obj_set_size(matrix_switch, 70, 34);
    lv_obj_set_style_bg_color(matrix_switch, lv_color_make(0x44, 0x44, 0x44), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(matrix_switch, ARGUS_ACCENT, LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_add_event_cb(matrix_switch, on_matrix_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_align(matrix_switch, LV_ALIGN_RIGHT_MID, 0, 0);

    // Show Day row
    lv_obj_t *show_day_row = lv_obj_create(settings_screen);
    lv_obj_set_size(show_day_row, 380, 40);
    lv_obj_set_style_bg_opa(show_day_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(show_day_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(show_day_row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(show_day_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(show_day_row, LV_ALIGN_TOP_MID, 0, 472);
    register_shiftable(show_day_row, 472);

    lv_obj_t *show_day_lbl = lv_label_create(show_day_row);
    lv_obj_set_style_text_color(show_day_lbl, ARGUS_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(show_day_lbl, &font_argus_label_20, LV_PART_MAIN);
    lv_label_set_text(show_day_lbl, "Show Day");
    lv_obj_align(show_day_lbl, LV_ALIGN_LEFT_MID, 0, 0);

    show_day_switch = lv_switch_create(show_day_row);
    lv_obj_set_size(show_day_switch, 70, 34);
    lv_obj_set_style_bg_color(show_day_switch, lv_color_make(0x44, 0x44, 0x44), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(show_day_switch, ARGUS_ACCENT, LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_add_state(show_day_switch, LV_STATE_CHECKED);  // default on
    lv_obj_add_event_cb(show_day_switch, on_show_day_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_align(show_day_switch, LV_ALIGN_RIGHT_MID, 0, 0);

    // Show Date row
    lv_obj_t *show_date_row = lv_obj_create(settings_screen);
    lv_obj_set_size(show_date_row, 380, 40);
    lv_obj_set_style_bg_opa(show_date_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(show_date_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(show_date_row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(show_date_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(show_date_row, LV_ALIGN_TOP_MID, 0, 520);
    register_shiftable(show_date_row, 520);

    lv_obj_t *show_date_lbl = lv_label_create(show_date_row);
    lv_obj_set_style_text_color(show_date_lbl, ARGUS_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(show_date_lbl, &font_argus_label_20, LV_PART_MAIN);
    lv_label_set_text(show_date_lbl, "Show Date");
    lv_obj_align(show_date_lbl, LV_ALIGN_LEFT_MID, 0, 0);

    show_date_switch = lv_switch_create(show_date_row);
    lv_obj_set_size(show_date_switch, 70, 34);
    lv_obj_set_style_bg_color(show_date_switch, lv_color_make(0x44, 0x44, 0x44), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(show_date_switch, ARGUS_ACCENT, LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_add_state(show_date_switch, LV_STATE_CHECKED);  // default on
    lv_obj_add_event_cb(show_date_switch, on_show_date_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_align(show_date_switch, LV_ALIGN_RIGHT_MID, 0, 0);

    // Vibrate row
    lv_obj_t *vibrate_row = lv_obj_create(settings_screen);
    lv_obj_set_size(vibrate_row, 380, 40);
    lv_obj_set_style_bg_opa(vibrate_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(vibrate_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(vibrate_row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(vibrate_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(vibrate_row, LV_ALIGN_TOP_MID, 0, 568);
    register_shiftable(vibrate_row, 568);

    lv_obj_t *vibrate_lbl = lv_label_create(vibrate_row);
    lv_obj_set_style_text_color(vibrate_lbl, ARGUS_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(vibrate_lbl, &font_argus_label_20, LV_PART_MAIN);
    lv_label_set_text(vibrate_lbl, "Vibrate");
    lv_obj_align(vibrate_lbl, LV_ALIGN_LEFT_MID, 0, 0);

    vibrate_switch = lv_switch_create(vibrate_row);
    lv_obj_set_size(vibrate_switch, 70, 34);
    lv_obj_set_style_bg_color(vibrate_switch, lv_color_make(0x44, 0x44, 0x44), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(vibrate_switch, ARGUS_ACCENT, LV_PART_MAIN | LV_STATE_CHECKED);
    // default off — do not add LV_STATE_CHECKED
    lv_obj_add_event_cb(vibrate_switch, on_vibrate_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_align(vibrate_switch, LV_ALIGN_RIGHT_MID, 0, 0);

    // Separator before dim timer section
    lv_obj_t *sep2 = lv_obj_create(settings_screen);
    lv_obj_set_size(sep2, 380, 1);
    lv_obj_set_style_bg_color(sep2, lv_color_make(0x33, 0x33, 0x33), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sep2, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(sep2, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(sep2, 0, LV_PART_MAIN);
    lv_obj_align(sep2, LV_ALIGN_TOP_MID, 0, 616);
    register_shiftable(sep2, 616);

    // Dim Timer row: label on left, dropdown on right
    lv_obj_t *dim_row = lv_obj_create(settings_screen);
    lv_obj_set_size(dim_row, 380, 40);
    lv_obj_set_style_bg_opa(dim_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(dim_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(dim_row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(dim_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(dim_row, LV_ALIGN_TOP_MID, 0, 626);
    register_shiftable(dim_row, 626);

    lv_obj_t *dim_lbl = lv_label_create(dim_row);
    lv_obj_set_style_text_color(dim_lbl, ARGUS_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(dim_lbl, &font_argus_label_20, LV_PART_MAIN);
    lv_label_set_text(dim_lbl, "Dim Timer");
    lv_obj_align(dim_lbl, LV_ALIGN_LEFT_MID, 0, 0);

    dim_dropdown = lv_dropdown_create(dim_row);
    lv_dropdown_set_options(dim_dropdown, "OFF\n5 Seconds\n30 Seconds\n1 Minute\n5 Minutes");
    lv_dropdown_set_selected(dim_dropdown, 0);
    lv_obj_set_size(dim_dropdown, 185, 34);
    lv_obj_set_style_text_font(dim_dropdown, &font_argus_label_16, LV_PART_MAIN);
    lv_obj_set_style_bg_color(dim_dropdown, lv_color_make(0x22, 0x22, 0x22), LV_PART_MAIN);
    lv_obj_set_style_text_color(dim_dropdown, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_border_color(dim_dropdown, lv_color_make(0x55, 0x55, 0x55), LV_PART_MAIN);
    lv_obj_set_style_border_width(dim_dropdown, 1, LV_PART_MAIN);
    // Style the drop-down list
    lv_obj_t *dd_list = lv_dropdown_get_list(dim_dropdown);
    lv_obj_set_style_bg_color(dd_list, lv_color_make(0x22, 0x22, 0x22), LV_PART_MAIN);
    lv_obj_set_style_text_color(dd_list, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(dd_list, &font_argus_label_16, LV_PART_MAIN);
    lv_obj_set_style_border_color(dd_list, lv_color_make(0x55, 0x55, 0x55), LV_PART_MAIN);
    lv_obj_add_event_cb(dim_dropdown, on_dim_timeout_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_align(dim_dropdown, LV_ALIGN_RIGHT_MID, 0, 0);

    // Dimmed Brightness row: label on left, percentage on right
    lv_obj_t *dbr_row = lv_obj_create(settings_screen);
    lv_obj_set_size(dbr_row, 380, 30);
    lv_obj_set_style_bg_opa(dbr_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(dbr_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(dbr_row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(dbr_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(dbr_row, LV_ALIGN_TOP_MID, 0, 678);
    register_shiftable(dbr_row, 678);

    lv_obj_t *dbr_lbl = lv_label_create(dbr_row);
    lv_obj_set_style_text_color(dbr_lbl, ARGUS_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(dbr_lbl, &font_argus_label_20, LV_PART_MAIN);
    lv_label_set_text(dbr_lbl, "Dimmed");
    lv_obj_align(dbr_lbl, LV_ALIGN_LEFT_MID, 0, 0);

    dim_brightness_val_label = lv_label_create(dbr_row);
    lv_obj_set_style_text_color(dim_brightness_val_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(dim_brightness_val_label, &font_argus_label_20, LV_PART_MAIN);
    int init_pct = (int)s_dim_brightness * 100 / (int)DEVICE_MAX_BRIGHTNESS_LEVEL;
    lv_label_set_text_fmt(dim_brightness_val_label, "%d%%", init_pct);
    lv_obj_align(dim_brightness_val_label, LV_ALIGN_RIGHT_MID, 0, 0);

    // Dimmed Brightness slider
    dim_brightness_slider = lv_slider_create(settings_screen);
    lv_obj_set_size(dim_brightness_slider, 320, 50);
    lv_obj_align(dim_brightness_slider, LV_ALIGN_TOP_MID, 0, 720);
    register_shiftable(dim_brightness_slider, 720);
    lv_slider_set_range(dim_brightness_slider, 1, DEVICE_MAX_BRIGHTNESS_LEVEL);
    lv_slider_set_value(dim_brightness_slider, s_dim_brightness, LV_ANIM_OFF);

    lv_obj_set_style_bg_color(dim_brightness_slider, lv_color_make(0x44, 0x44, 0x44), LV_PART_MAIN);
    lv_obj_set_style_radius(dim_brightness_slider, 8, LV_PART_MAIN);
    lv_obj_set_style_border_width(dim_brightness_slider, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(dim_brightness_slider, lv_color_make(0x66, 0x66, 0xFF), LV_PART_INDICATOR);
    lv_obj_set_style_radius(dim_brightness_slider, 8, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(dim_brightness_slider, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_radius(dim_brightness_slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_pad_all(dim_brightness_slider, 10, LV_PART_KNOB);
    lv_obj_set_style_border_width(dim_brightness_slider, 0, LV_PART_KNOB);

    lv_obj_add_event_cb(dim_brightness_slider, on_dim_brightness_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(dim_brightness_slider, on_slider_released,         LV_EVENT_RELEASED,      NULL);

    // Motion-wake row — sits between the Dimmed slider and the Manual Time
    // section. When on, any wrist movement detected by the BHI260AP resets
    // the dim timer (same effect as a tap or button press).
    lv_obj_t *motion_row = lv_obj_create(settings_screen);
    lv_obj_set_size(motion_row, 380, 40);
    lv_obj_set_style_bg_opa(motion_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(motion_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(motion_row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(motion_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(motion_row, LV_ALIGN_TOP_MID, 0, 778);
    register_shiftable(motion_row, 778);

    lv_obj_t *motion_lbl = lv_label_create(motion_row);
    lv_obj_set_style_text_color(motion_lbl, ARGUS_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(motion_lbl, &font_argus_label_20, LV_PART_MAIN);
    lv_label_set_text(motion_lbl, "Motion brightens screen");
    lv_obj_align(motion_lbl, LV_ALIGN_LEFT_MID, 0, 0);

    motion_wake_val_label = lv_label_create(motion_row);
    lv_obj_set_style_text_color(motion_wake_val_label, ARGUS_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(motion_wake_val_label, &font_argus_label_20, LV_PART_MAIN);
    lv_label_set_text(motion_wake_val_label, "On");
    lv_obj_align(motion_wake_val_label, LV_ALIGN_RIGHT_MID, -80, 0);

    motion_wake_switch = lv_switch_create(motion_row);
    lv_obj_set_size(motion_wake_switch, 70, 34);
    lv_obj_set_style_bg_color(motion_wake_switch, lv_color_make(0x44, 0x44, 0x44), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(motion_wake_switch, ARGUS_ACCENT, LV_PART_MAIN | LV_STATE_CHECKED);
    // Default ON so wrist-raise brightens out of the box — matches the
    // smartwatch behaviour most users expect.
    lv_obj_add_state(motion_wake_switch, LV_STATE_CHECKED);
    lv_obj_add_event_cb(motion_wake_switch, on_motion_wake_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_align(motion_wake_switch, LV_ALIGN_RIGHT_MID, 0, 0);

    // ---- Manual Time section ------------------------------------------------
    // Lets the user set the clock by hand; doing so overrides the automatic
    // GPS time sync until the override switch is turned back off.

    // Separator
    lv_obj_t *sep3 = lv_obj_create(settings_screen);
    lv_obj_set_size(sep3, 380, 1);
    lv_obj_set_style_bg_color(sep3, lv_color_make(0x33, 0x33, 0x33), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sep3, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(sep3, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(sep3, 0, LV_PART_MAIN);
    lv_obj_align(sep3, LV_ALIGN_TOP_MID, 0, 838);
    register_shiftable(sep3, 838);

    // Manual Time row: label left, state label + override toggle right
    lv_obj_t *manual_row = lv_obj_create(settings_screen);
    lv_obj_set_size(manual_row, 380, 40);
    lv_obj_set_style_bg_opa(manual_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(manual_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(manual_row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(manual_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(manual_row, LV_ALIGN_TOP_MID, 0, 852);
    register_shiftable(manual_row, 852);

    lv_obj_t *manual_lbl = lv_label_create(manual_row);
    lv_obj_set_style_text_color(manual_lbl, ARGUS_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(manual_lbl, &font_argus_label_20, LV_PART_MAIN);
    lv_label_set_text(manual_lbl, "Manual Time");
    lv_obj_align(manual_lbl, LV_ALIGN_LEFT_MID, 0, 0);

    manual_time_val_label = lv_label_create(manual_row);
    lv_obj_set_style_text_color(manual_time_val_label, ARGUS_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(manual_time_val_label, &font_argus_label_20, LV_PART_MAIN);
    lv_label_set_text(manual_time_val_label, "Off");
    lv_obj_align(manual_time_val_label, LV_ALIGN_RIGHT_MID, -80, 0);

    manual_time_switch = lv_switch_create(manual_row);
    lv_obj_set_size(manual_time_switch, 70, 34);
    lv_obj_set_style_bg_color(manual_time_switch, lv_color_make(0x44, 0x44, 0x44), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(manual_time_switch, ARGUS_ACCENT, LV_PART_MAIN | LV_STATE_CHECKED);
    // default off — automatic GPS time sync stays in control until a manual set
    lv_obj_add_event_cb(manual_time_switch, on_manual_time_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_align(manual_time_switch, LV_ALIGN_RIGHT_MID, 0, 0);

    // Instruction line
    lv_obj_t *manual_hint = lv_label_create(settings_screen);
    lv_obj_set_style_text_color(manual_hint, ARGUS_TEXT_DIM, LV_PART_MAIN);
    lv_obj_set_style_text_font(manual_hint, &font_argus_label_16, LV_PART_MAIN);
    lv_label_set_text(manual_hint, "Set the date & time below, then press SET");
    lv_obj_align(manual_hint, LV_ALIGN_TOP_MID, 0, 900);
    register_shiftable(manual_hint, 900);
    register_manual_obj(manual_hint);

    // Date / time rollers — Year, Month, Day, Hour, Minute.
    // Buffer sized for the largest list: 41 years × "YYYY\n" ≈ 205 bytes.
    char opts[256];
    build_num_opts(opts, sizeof(opts), MANUAL_YEAR_BASE,
                   MANUAL_YEAR_BASE + MANUAL_YEAR_COUNT - 1, 4);
    year_roller   = make_time_roller("Year",  opts, -148, 924, 84);
    build_num_opts(opts, sizeof(opts), 1, 12, 2);
    month_roller  = make_time_roller("Month", opts,  -64, 924, 64);
    build_num_opts(opts, sizeof(opts), 1, 31, 2);
    day_roller    = make_time_roller("Day",   opts,   10, 924, 64);
    build_num_opts(opts, sizeof(opts), 0, 23, 2);
    hour_roller   = make_time_roller("Hour",  opts,   84, 924, 64);
    build_num_opts(opts, sizeof(opts), 0, 59, 2);
    minute_roller = make_time_roller("Min",   opts,  158, 924, 64);

    // SET TIME button
    lv_obj_t *set_btn = lv_obj_create(settings_screen);
    lv_obj_set_size(set_btn, 200, 52);
    lv_obj_set_style_radius(set_btn, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(set_btn, lv_color_make(0x00, 0xAA, 0x44), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(set_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(set_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(set_btn, 0, LV_PART_MAIN);
    lv_obj_clear_flag(set_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(set_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(set_btn, LV_ALIGN_TOP_MID, 0, 1054);
    register_shiftable(set_btn, 1054);
    register_manual_obj(set_btn);

    // Provisional layout pass for the rows registered SO FAR. The sections
    // below (screenshot, wallpaper, boot, system, mode) have not been built
    // yet, so this call cannot position them - the authoritative pass is the
    // apply_layout() at the very end of this function. Kept only so the manual
    // rollers collapse before the rest of the list is measured.
    apply_layout();

    lv_obj_t *set_lbl = lv_label_create(set_btn);
    lv_label_set_text(set_lbl, "SET TIME");
    lv_obj_set_style_text_color(set_lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(set_lbl, &font_argus_label_20, LV_PART_MAIN);
    lv_obj_center(set_lbl);
    lv_obj_add_event_cb(set_btn, on_set_time_clicked, LV_EVENT_CLICKED, NULL);

    // ---- Screenshot section -------------------------------------------------
    // Long-press anywhere captures the active screen to /Screenshots on SD.
    // The toggle is greyed and forced off whenever the card isn't mounted —
    // settings_screen_apply_sd_state() does that bookkeeping, called both
    // here at boot and from main.cpp whenever the SD state changes.

    // Separator
    lv_obj_t *sep_ss = lv_obj_create(settings_screen);
    lv_obj_set_size(sep_ss, 380, 1);
    lv_obj_set_style_bg_color(sep_ss, lv_color_make(0x33, 0x33, 0x33), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sep_ss, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(sep_ss, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(sep_ss, 0, LV_PART_MAIN);
    lv_obj_align(sep_ss, LV_ALIGN_TOP_MID, 0, 1130);
    register_shiftable(sep_ss, 1130);

    screenshot_row = lv_obj_create(settings_screen);
    lv_obj_set_size(screenshot_row, 380, 40);
    lv_obj_set_style_bg_opa(screenshot_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(screenshot_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(screenshot_row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(screenshot_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(screenshot_row, LV_ALIGN_TOP_MID, 0, 1144);
    register_shiftable(screenshot_row, 1144);

    lv_obj_t *ss_lbl = lv_label_create(screenshot_row);
    lv_obj_set_style_text_color(ss_lbl, ARGUS_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(ss_lbl, &font_argus_label_20, LV_PART_MAIN);
    lv_label_set_text(ss_lbl, "Screenshot long press");
    lv_obj_align(ss_lbl, LV_ALIGN_LEFT_MID, 0, 0);

    screenshot_val_label = lv_label_create(screenshot_row);
    lv_obj_set_style_text_color(screenshot_val_label, ARGUS_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(screenshot_val_label, &font_argus_label_20, LV_PART_MAIN);
    lv_label_set_text(screenshot_val_label, "Off");
    lv_obj_align(screenshot_val_label, LV_ALIGN_RIGHT_MID, -80, 0);

    screenshot_switch = lv_switch_create(screenshot_row);
    lv_obj_set_size(screenshot_switch, 70, 34);
    lv_obj_set_style_bg_color(screenshot_switch, lv_color_make(0x44, 0x44, 0x44), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(screenshot_switch, ARGUS_ACCENT, LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_add_event_cb(screenshot_switch, on_screenshot_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_align(screenshot_switch, LV_ALIGN_RIGHT_MID, 0, 0);

    // Hint — explains the 3 s threshold + where the files land.
    screenshot_hint = lv_label_create(settings_screen);
    lv_obj_set_style_text_color(screenshot_hint, ARGUS_TEXT_DIM, LV_PART_MAIN);
    lv_obj_set_style_text_font(screenshot_hint, &font_argus_label_14, LV_PART_MAIN);
    lv_obj_set_style_text_align(screenshot_hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(screenshot_hint, 360);
    lv_label_set_long_mode(screenshot_hint, LV_LABEL_LONG_WRAP);
    lv_label_set_text(screenshot_hint,
        "Hold any point for 3 seconds to save a screenshot to "
        "/Screenshots on the SD card. Requires a mounted SD.");
    lv_obj_align(screenshot_hint, LV_ALIGN_TOP_MID, 0, 1190);
    register_shiftable(screenshot_hint, 1190);

    // ---- Wallpaper (SD-card background image) section -----------------------
    // Independent of the Matrix BG toggle so the two can coexist: with both
    // on, a faint SD image sits behind the matrix rain (the intended default
    // look). Placed at the bottom of the list so it doesn't disturb the tuned
    // absolute-Y positions of the rows above. Mirrors the Matrix BG toggle
    // wiring exactly (switch + on/off label + save/load key).
    lv_obj_t *sep_bg = lv_obj_create(settings_screen);
    lv_obj_set_size(sep_bg, 380, 1);
    lv_obj_set_style_bg_color(sep_bg, lv_color_make(0x33, 0x33, 0x33), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sep_bg, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(sep_bg, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(sep_bg, 0, LV_PART_MAIN);
    lv_obj_align(sep_bg, LV_ALIGN_TOP_MID, 0, 1250);
    register_shiftable(sep_bg, 1250);

    lv_obj_t *wallpaper_row = lv_obj_create(settings_screen);
    lv_obj_set_size(wallpaper_row, 380, 40);
    lv_obj_set_style_bg_opa(wallpaper_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(wallpaper_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(wallpaper_row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(wallpaper_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(wallpaper_row, LV_ALIGN_TOP_MID, 0, 1264);
    register_shiftable(wallpaper_row, 1264);

    lv_obj_t *wp_lbl = lv_label_create(wallpaper_row);
    lv_obj_set_style_text_color(wp_lbl, ARGUS_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(wp_lbl, &font_argus_label_20, LV_PART_MAIN);
    lv_label_set_text(wp_lbl, "Wallpaper");
    lv_obj_align(wp_lbl, LV_ALIGN_LEFT_MID, 0, 0);

    wallpaper_val_label = lv_label_create(wallpaper_row);
    lv_obj_set_style_text_color(wallpaper_val_label, ARGUS_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(wallpaper_val_label, &font_argus_label_20, LV_PART_MAIN);
    lv_label_set_text(wallpaper_val_label, "Off");
    lv_obj_align(wallpaper_val_label, LV_ALIGN_RIGHT_MID, -80, 0);

    wallpaper_switch = lv_switch_create(wallpaper_row);
    lv_obj_set_size(wallpaper_switch, 70, 34);
    lv_obj_set_style_bg_color(wallpaper_switch, lv_color_make(0x44, 0x44, 0x44), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(wallpaper_switch, ARGUS_ACCENT, LV_PART_MAIN | LV_STATE_CHECKED);
    // default off — no image until the user drops one on the card + enables it
    lv_obj_add_event_cb(wallpaper_switch, on_wallpaper_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_align(wallpaper_switch, LV_ALIGN_RIGHT_MID, 0, 0);

    // Hint — tells the user where to drop the image on the SD card.
    lv_obj_t *wp_hint = lv_label_create(settings_screen);
    lv_obj_set_style_text_color(wp_hint, ARGUS_TEXT_DIM, LV_PART_MAIN);
    lv_obj_set_style_text_font(wp_hint, &font_argus_label_14, LV_PART_MAIN);
    lv_obj_set_style_text_align(wp_hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(wp_hint, 360);
    lv_label_set_long_mode(wp_hint, LV_LABEL_LONG_WRAP);
    lv_label_set_text(wp_hint,
        "Drop a PNG/BMP/JPG at /backgrounds/wallpaper.png on the SD card. "
        "Shown faintly behind the clock; works alongside Matrix BG.");
    lv_obj_align(wp_hint, LV_ALIGN_TOP_MID, 0, 1312);
    register_shiftable(wp_hint, 1312);

    // ---- Enable at boot section ---------------------------------------------
    // Per-radio opt-in for auto-enabling a radio when the watch powers on. The
    // boot default is OFF for every radio - nothing comes up at boot unless the
    // user turns it on here. State is persisted separately in
    // /Settings/boot_radios.txt via boot_prefs (see boot_prefs.h), not in
    // settings.txt. WiFi and Bluetooth are mutually exclusive here (they cannot
    // run together), so enabling one at boot blocks the other with a warning.
    lv_obj_t *sep_boot = lv_obj_create(settings_screen);
    lv_obj_set_size(sep_boot, 380, 1);
    lv_obj_set_style_bg_color(sep_boot, lv_color_make(0x33, 0x33, 0x33), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sep_boot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(sep_boot, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(sep_boot, 0, LV_PART_MAIN);
    lv_obj_align(sep_boot, LV_ALIGN_TOP_MID, 0, 1380);
    register_shiftable(sep_boot, 1380);

    // Section header
    lv_obj_t *boot_hdr_row = lv_obj_create(settings_screen);
    lv_obj_set_size(boot_hdr_row, 380, 30);
    lv_obj_set_style_bg_opa(boot_hdr_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(boot_hdr_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(boot_hdr_row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(boot_hdr_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(boot_hdr_row, LV_ALIGN_TOP_MID, 0, 1394);
    register_shiftable(boot_hdr_row, 1394);

    lv_obj_t *boot_hdr = lv_label_create(boot_hdr_row);
    lv_obj_set_style_text_color(boot_hdr, ARGUS_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(boot_hdr, &font_argus_label_20, LV_PART_MAIN);
    lv_label_set_text(boot_hdr, "Enable at boot");
    lv_obj_align(boot_hdr, LV_ALIGN_LEFT_MID, 0, 0);

    // One toggle per boot-enableable radio. Each seeds from boot_prefs. WiFi and
    // Bluetooth are mutually exclusive (enforced in their callbacks) since the
    // two cannot run together on this board. Rows are evenly spaced 48px apart.
    boot_wifi_switch = make_boot_row("WiFi", 1434, boot_prefs_get(BOOT_RADIO_WIFI),
                                     on_boot_wifi_changed, &boot_wifi_val_label);
    boot_ble_switch  = make_boot_row("Bluetooth", 1482, boot_prefs_get(BOOT_RADIO_BLE),
                                     on_boot_ble_changed,  &boot_ble_val_label);
    boot_lora_switch = make_boot_row("LoRa", 1530, boot_prefs_get(BOOT_RADIO_LORA),
                                     on_boot_lora_changed, &boot_lora_val_label);
    boot_gps_switch  = make_boot_row("GPS",  1578, boot_prefs_get(BOOT_RADIO_GPS),
                                     on_boot_gps_changed,  &boot_gps_val_label);

    // Hint: clarifies the boot-time behaviour and why BT isn't listed.
    lv_obj_t *boot_hint = lv_label_create(settings_screen);
    lv_obj_set_style_text_color(boot_hint, ARGUS_TEXT_DIM, LV_PART_MAIN);
    lv_obj_set_style_text_font(boot_hint, &font_argus_label_14, LV_PART_MAIN);
    lv_obj_set_style_text_align(boot_hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(boot_hint, 360);
    lv_label_set_long_mode(boot_hint, LV_LABEL_LONG_WRAP);
    lv_label_set_text(boot_hint,
        "Radios turned on here power up automatically at boot. Everything is "
        "off by default. WiFi and Bluetooth cannot both auto-enable.");
    lv_obj_align(boot_hint, LV_ALIGN_TOP_MID, 0, 1626);
    register_shiftable(boot_hint, 1626);

    // --- System Info (read-only diagnostics; refreshed on screen show) --------
    lv_obj_t *sep_sys = lv_obj_create(settings_screen);
    lv_obj_set_size(sep_sys, 380, 1);
    lv_obj_set_style_bg_color(sep_sys, lv_color_make(0x33, 0x33, 0x33), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sep_sys, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(sep_sys, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(sep_sys, 0, LV_PART_MAIN);
    lv_obj_align(sep_sys, LV_ALIGN_TOP_MID, 0, 1700);
    register_shiftable(sep_sys, 1700);

    lv_obj_t *sys_hdr = lv_label_create(settings_screen);
    lv_obj_set_style_text_color(sys_hdr, ARGUS_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(sys_hdr, &font_argus_label_20, LV_PART_MAIN);
    lv_label_set_text(sys_hdr, "System Info");
    lv_obj_align(sys_hdr, LV_ALIGN_TOP_MID, 0, 1714);
    register_shiftable(sys_hdr, 1714);

    sysinfo_label = lv_label_create(settings_screen);
    lv_obj_set_style_text_color(sysinfo_label, ARGUS_TEXT_DIM, LV_PART_MAIN);
    lv_obj_set_style_text_font(sysinfo_label, &font_argus_mono_16, LV_PART_MAIN);
    lv_obj_set_width(sysinfo_label, 360);
    lv_label_set_text(sysinfo_label, "");
    lv_obj_align(sysinfo_label, LV_ALIGN_TOP_MID, 0, 1750);
    register_shiftable(sysinfo_label, 1750);

    // --- Mode (ARGUS Daily / Defense) ----------------------------------------
    // Appended at the bottom (safe insertion into the fixed-Y layout). Offense is
    // NOT here - it is reached only via the hidden BOOT-button knock + PIN (P4/P5).
    lv_obj_t *sep_mode = lv_obj_create(settings_screen);
    lv_obj_set_size(sep_mode, 380, 1);
    lv_obj_set_style_bg_color(sep_mode, lv_color_make(0x33, 0x33, 0x33), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sep_mode, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(sep_mode, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(sep_mode, 0, LV_PART_MAIN);
    lv_obj_align(sep_mode, LV_ALIGN_TOP_MID, 0, 1900);
    register_shiftable(sep_mode, 1900);

    lv_obj_t *mode_hdr = lv_label_create(settings_screen);
    lv_obj_set_style_text_color(mode_hdr, ARGUS_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(mode_hdr, &font_argus_label_20, LV_PART_MAIN);
    lv_label_set_text(mode_hdr, "Mode");
    lv_obj_align(mode_hdr, LV_ALIGN_TOP_MID, 0, 1914);
    register_shiftable(mode_hdr, 1914);

    defense_switch = make_boot_row("Defense", 1954,
        argus_mode_defense_selected(),
        on_defense_mode_changed, &defense_val_label);

    defpersist_switch = make_boot_row("Keep on boot", 2002,
        argus_mode_defense_persist(),
        on_defpersist_changed, &defpersist_val_label);

    lv_obj_t *mode_hint = lv_label_create(settings_screen);
    lv_obj_set_style_text_color(mode_hint, ARGUS_TEXT_DIM, LV_PART_MAIN);
    lv_obj_set_style_text_font(mode_hint, &font_argus_label_14, LV_PART_MAIN);
    lv_obj_set_width(mode_hint, 360);
    lv_label_set_text(mode_hint, "Defense shows the anti-surveillance detectors. Daily hides them for an innocent look.");
    lv_obj_align(mode_hint, LV_ALIGN_TOP_MID, 0, 2050);
    register_shiftable(mode_hint, 2050);

    // ONE Offense button. Outside Offense it is the TEMPORARY test entry to the PIN
    // pad (the real entry is the power-key knock; keep this until the knock is
    // verified on-wrist, or a watch with a non-firing knock has no way in short of a
    // reflash). Inside Offense it becomes "Exit Offense". Its label + action are set
    // by mode in settings_screen_show(); a single button avoids the overlapping-hit
    // bug two stacked buttons caused (the unlock click did nothing).
    s_ofs_btn = lv_button_create(settings_screen);
    lv_obj_set_size(s_ofs_btn, 380, 64);   // taller = easier tap target (was 44)
    lv_obj_set_style_bg_color(s_ofs_btn, lv_color_make(0x2A, 0x1E, 0x10), LV_PART_MAIN);
    lv_obj_set_style_border_color(s_ofs_btn, ARGUS_OFFENSE_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_ofs_btn, 1, LV_PART_MAIN);
    lv_obj_align(s_ofs_btn, LV_ALIGN_TOP_MID, 0, 2110);
    register_shiftable(s_ofs_btn, 2110);
    s_ofs_lbl = lv_label_create(s_ofs_btn);
    lv_obj_set_style_text_font(s_ofs_lbl, &font_argus_label_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_ofs_lbl, ARGUS_TEXT, LV_PART_MAIN);
    lv_label_set_text(s_ofs_lbl, "Unlock Offense (test)");
    lv_obj_center(s_ofs_lbl);
    // "Clear detection logs" - wipes every record ARGUS has kept about other
    // people's devices. Retention already expires these automatically (see
    // src/detect/log_retention.h), but a one-tap purge is the difference
    // between a policy the wearer has to trust and one they can enforce.
    lv_obj_t *det_btn = lv_button_create(settings_screen);
    lv_obj_set_size(det_btn, 380, 64);
    lv_obj_set_style_bg_color(det_btn, lv_color_make(0x1E, 0x1E, 0x1E), LV_PART_MAIN);
    lv_obj_set_style_border_color(det_btn, ARGUS_TEXT_DIM, LV_PART_MAIN);
    lv_obj_set_style_border_width(det_btn, 1, LV_PART_MAIN);
    lv_obj_align(det_btn, LV_ALIGN_TOP_MID, 0, 2190);
    register_shiftable(det_btn, 2190);
    lv_obj_t *det_lbl = lv_label_create(det_btn);
    lv_obj_set_style_text_font(det_lbl, &font_argus_label_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(det_lbl, ARGUS_TEXT, LV_PART_MAIN);
    lv_label_set_text(det_lbl, "Clear detection logs");
    lv_obj_center(det_lbl);
    lv_obj_add_event_cb(det_btn, [](lv_event_t *) {
        int n = detect_log_clear_all();
        char msg[96];
        snprintf(msg, sizeof(msg),
                 n > 0 ? "Detection logs cleared.\n%d file(s) removed."
                       : "Nothing to clear.\n(%d file(s) found)", n);
        low_mem_show_dialog(msg);
        settings_update_sysinfo();
    }, LV_EVENT_CLICKED, NULL);

    // "USB SD card reader" - a SECOND route to the same screen the Tools grid
    // reaches, and the only one that works in Daily.
    //
    // The Tools grid is hidden in Daily on purpose: Daily is the innocent-watch
    // mode and the whole point is that the grid is not there. But that also put
    // the card reader out of reach, so pulling logs off the watch required
    // switching to Defense first - which is a mode change the wearer may not
    // want to make just to copy a file, and it is not a capability worth
    // hiding: mounting a card as a USB drive is what any ordinary device does.
    // Settings is reachable in every mode (BOOT press from the clock face), so
    // the utility lives here and the disguise stays intact.
    //
    // Notify and LoRa APRS sit in the same neutral class and are still
    // Tools-only; APRS at least is an RF transmitter, so whether it belongs in
    // Daily is a real question rather than an oversight. See tools_apply_mode().
    lv_obj_t *usbsd_btn = lv_button_create(settings_screen);
    lv_obj_set_size(usbsd_btn, 380, 64);
    lv_obj_set_style_bg_color(usbsd_btn, lv_color_make(0x1E, 0x1E, 0x1E), LV_PART_MAIN);
    lv_obj_set_style_border_color(usbsd_btn, ARGUS_TEXT_DIM, LV_PART_MAIN);
    lv_obj_set_style_border_width(usbsd_btn, 1, LV_PART_MAIN);
    lv_obj_align(usbsd_btn, LV_ALIGN_TOP_MID, 0, 2270);
    register_shiftable(usbsd_btn, 2270);
    lv_obj_t *usbsd_lbl = lv_label_create(usbsd_btn);
    lv_obj_set_style_text_font(usbsd_lbl, &font_argus_label_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(usbsd_lbl, ARGUS_TEXT, LV_PART_MAIN);
    lv_label_set_text(usbsd_lbl, "USB SD card reader");
    lv_obj_center(usbsd_lbl);
    lv_obj_add_event_cb(usbsd_btn, [](lv_event_t *) { usb_sd_screen_show(); },
                        LV_EVENT_CLICKED, NULL);

    lv_obj_add_event_cb(s_ofs_btn, [](lv_event_t *) {
        if (argus_mode_current() == ArgusMode::Offense) { lock_offense(); clock_screen_show(); }
        else                                            { pin_pad_screen_show(); }
    }, LV_EVENT_CLICKED, NULL);

    // Lay the list out now that EVERY row has registered. This must be the last
    // thing the builder does: register_shiftable() only records a row's design
    // y, and apply_layout() is what collapses the hidden Manual Time / analog
    // face blocks. A row registered after an apply_layout() call keeps its raw
    // design y until the next call, which is what left a ~230 px hole between
    // Manual Time and the Screenshot section on a watch with no SD card - the
    // earlier apply_layout() ran before this whole lower half existed, and the
    // only other calls live in the settings-load path behind an
    // instance.isCardReady() early-return, so with a card the gap was silently
    // repaired on load and without one it stayed.
    apply_layout();

    // Reflect the current SD state in the row's interactability + checked
    // state. Boot order matters here — instance.isCardReady() may flip
    // later, but settings_screen_apply_sd_state() is also called from
    // main.cpp on every SD-state edge so the row stays in sync.
    settings_screen_apply_sd_state();
}

bool settings_screen_is_active()
{
    return lv_screen_active() == settings_screen;
}

void settings_screen_show()
{
    main_loop_request_lvgl_priority(12);
    settings_update_sysinfo();   // snapshot heap/PSRAM/battery/uptime

    // Heading follows the mode: red-team red in Offense, steel-blue otherwise.
    if (s_settings_title)
        lv_obj_set_style_text_color(s_settings_title, argus_base_accent(), LV_PART_MAIN);

    // Keep the Defense toggle showing the Offense EXIT target - it can be flipped
    // while inside Offense to choose whether Exit Offense lands in Defense or Daily.
    if (defense_switch) {
        bool ds = argus_mode_defense_selected();
        if (ds) lv_obj_add_state(defense_switch, LV_STATE_CHECKED);
        else    lv_obj_clear_state(defense_switch, LV_STATE_CHECKED);
        if (defense_val_label) lv_label_set_text(defense_val_label, ds ? "On" : "Off");
    }

    // Offense button label follows the mode: exit when in Offense, enter otherwise.
    if (s_ofs_lbl) {
        lv_label_set_text(s_ofs_lbl,
            argus_mode_current() == ArgusMode::Offense ? "Exit Offense" : "Unlock Offense (test)");
        lv_obj_center(s_ofs_lbl);
    }
    // Refresh percentage label to match current slider position
    int pct = (int)s_brightness * 100 / (int)DEVICE_MAX_BRIGHTNESS_LEVEL;
    lv_label_set_text_fmt(brightness_val_label, "%d%%", pct);

    // Pre-fill the manual-time rollers with the current local time so the user
    // adjusts from "now" rather than from an arbitrary default.
    struct tm now;
    clock_screen_get_local_time(&now);
    int yr_idx  = now.tm_year + 1900 - MANUAL_YEAR_BASE;
    if (yr_idx < 0)                     yr_idx = 0;
    if (yr_idx > MANUAL_YEAR_COUNT - 1) yr_idx = MANUAL_YEAR_COUNT - 1;
    int day_idx = now.tm_mday - 1;
    if (day_idx < 0)  day_idx = 0;
    if (day_idx > 30) day_idx = 30;
    lv_roller_set_selected(year_roller,   (uint32_t)yr_idx,        LV_ANIM_OFF);
    lv_roller_set_selected(month_roller,  (uint32_t)now.tm_mon,    LV_ANIM_OFF);
    lv_roller_set_selected(day_roller,    (uint32_t)day_idx,       LV_ANIM_OFF);
    lv_roller_set_selected(hour_roller,   (uint32_t)now.tm_hour,   LV_ANIM_OFF);
    lv_roller_set_selected(minute_roller, (uint32_t)now.tm_min,    LV_ANIM_OFF);

    lv_scr_load(settings_screen);
}

// Rewrite /Settings/settings.txt with the current widget states. Called from
// every on_*_changed callback. No-op if no SD is mounted or if a load is in
// progress (so applying loaded values doesn't write the file we just read).
static void settings_save_to_sd()
{
    if (g_loading) return;
    if (!instance.isCardReady()) return;
    if (usb_sd_is_running()) return;   // host owns the SD card while mounted

    if (!SD.exists("/Settings")) SD.mkdir("/Settings");

    File f = SD.open(SETTINGS_PATH, FILE_WRITE);   // FILE_WRITE = "w" (truncate)
    if (!f) return;

    f.printf("brightness=%d\n",      (int)s_brightness);
    f.printf("analog_face=%d\n",     lv_obj_has_state(face_switch,        LV_STATE_CHECKED) ? 1 : 0);
    f.printf("format_12h=%d\n",      lv_obj_has_state(hour_format_switch, LV_STATE_CHECKED) ? 1 : 0);
    f.printf("show_ampm=%d\n",       lv_obj_has_state(ampm_switch,        LV_STATE_CHECKED) ? 1 : 0);
    f.printf("show_secs=%d\n",       lv_obj_has_state(secs_switch,        LV_STATE_CHECKED) ? 1 : 0);
    f.printf("matrix=%d\n",          lv_obj_has_state(matrix_switch,      LV_STATE_CHECKED) ? 1 : 0);
    f.printf("wallpaper=%d\n",       lv_obj_has_state(wallpaper_switch,   LV_STATE_CHECKED) ? 1 : 0);
    f.printf("show_day=%d\n",        lv_obj_has_state(show_day_switch,    LV_STATE_CHECKED) ? 1 : 0);
    f.printf("show_date=%d\n",       lv_obj_has_state(show_date_switch,   LV_STATE_CHECKED) ? 1 : 0);
    f.printf("vibrate=%d\n",         lv_obj_has_state(vibrate_switch,     LV_STATE_CHECKED) ? 1 : 0);
    f.printf("dim_timeout_idx=%lu\n",(unsigned long)lv_dropdown_get_selected(dim_dropdown));
    f.printf("dim_brightness=%d\n",  (int)s_dim_brightness);
    f.printf("motion_wake=%d\n",     lv_obj_has_state(motion_wake_switch, LV_STATE_CHECKED) ? 1 : 0);
    f.printf("manual_time=%d\n",     lv_obj_has_state(manual_time_switch, LV_STATE_CHECKED) ? 1 : 0);
    f.printf("screenshot=%d\n",      lv_obj_has_state(screenshot_switch,  LV_STATE_CHECKED) ? 1 : 0);
    f.close();
}

// Helpers for the load() switch/dropdown application
static void apply_switch(lv_obj_t *sw, bool checked)
{
    if (checked) lv_obj_add_state(sw, LV_STATE_CHECKED);
    else         lv_obj_clear_state(sw, LV_STATE_CHECKED);
}

void settings_screen_load()
{
    if (!instance.isCardReady()) return;
    if (!SD.exists(SETTINGS_PATH)) return;

    File f = SD.open(SETTINGS_PATH, FILE_READ);
    if (!f) return;

    g_loading = true;

    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        int eq = line.indexOf('=');
        if (eq <= 0) continue;
        String key = line.substring(0, eq);
        long   v   = line.substring(eq + 1).toInt();
        bool   b   = (v != 0);

        if (key == "brightness") {
            if (v < 1) v = 1;
            if (v > DEVICE_MAX_BRIGHTNESS_LEVEL) v = DEVICE_MAX_BRIGHTNESS_LEVEL;
            s_brightness = (int32_t)v;
            lv_slider_set_value(brightness_slider, v, LV_ANIM_OFF);
            instance.setBrightness((uint8_t)v);
            int pct = (int)v * 100 / (int)DEVICE_MAX_BRIGHTNESS_LEVEL;
            lv_label_set_text_fmt(brightness_val_label, "%d%%", pct);
        } else if (key == "analog_face") {
            apply_switch(face_switch, b);
            lv_label_set_text(face_val_label, b ? "Analog" : "Digital");
            // Honour the saved face state both for the clock and for the
            // settings-screen layout reflow.
            apply_layout();
            clock_screen_set_analog_face(b);
        } else if (key == "format_12h") {
            apply_switch(hour_format_switch, b);
            lv_label_set_text(hour_format_val_label, b ? "12h" : "24h");
            clock_screen_set_12h(b);
        } else if (key == "show_ampm") {
            apply_switch(ampm_switch, b);
            clock_screen_set_show_ampm(b);
        } else if (key == "show_secs") {
            apply_switch(secs_switch, b);
            clock_screen_set_show_secs(b);
        } else if (key == "matrix") {
            apply_switch(matrix_switch, b);
            lv_label_set_text(matrix_val_label, b ? "On" : "Off");
            clock_screen_set_matrix(b);
        } else if (key == "wallpaper") {
            apply_switch(wallpaper_switch, b);
            lv_label_set_text(wallpaper_val_label, b ? "On" : "Off");
            clock_screen_set_wallpaper(b);
        } else if (key == "show_day") {
            apply_switch(show_day_switch, b);
            clock_screen_set_show_day(b);
        } else if (key == "show_date") {
            apply_switch(show_date_switch, b);
            clock_screen_set_show_date(b);
        } else if (key == "vibrate") {
            apply_switch(vibrate_switch, b);
            clock_screen_set_vibrate(b);
        } else if (key == "dim_timeout_idx") {
            uint32_t idx = (uint32_t)v;
            if (idx >= (sizeof(DIM_TIMEOUT_MS)/sizeof(DIM_TIMEOUT_MS[0]))) idx = 0;
            lv_dropdown_set_selected(dim_dropdown, idx);
            clock_screen_set_dim_timeout(DIM_TIMEOUT_MS[idx]);
        } else if (key == "dim_brightness") {
            if (v < 1) v = 1;
            if (v > DEVICE_MAX_BRIGHTNESS_LEVEL) v = DEVICE_MAX_BRIGHTNESS_LEVEL;
            s_dim_brightness = (int32_t)v;
            lv_slider_set_value(dim_brightness_slider, v, LV_ANIM_OFF);
            clock_screen_set_dim_brightness((uint8_t)v);
            int pct = (int)v * 100 / (int)DEVICE_MAX_BRIGHTNESS_LEVEL;
            lv_label_set_text_fmt(dim_brightness_val_label, "%d%%", pct);
        } else if (key == "motion_wake") {
            apply_switch(motion_wake_switch, b);
            lv_label_set_text(motion_wake_val_label, b ? "On" : "Off");
            clock_screen_set_motion_wake(b);
        } else if (key == "manual_time") {
            apply_switch(manual_time_switch, b);
            lv_label_set_text(manual_time_val_label, b ? "On" : "Off");
            clock_screen_set_manual_override(b);
            // Match the editor visibility + screenshot-section position
            // to the loaded switch state.
            apply_layout();
        } else if (key == "screenshot") {
            apply_switch(screenshot_switch, b);
            lv_label_set_text(screenshot_val_label, b ? "On" : "Off");
        }
    }
    // Re-evaluate SD-aware state once loading is done — a persisted
    // "screenshot=1" gets immediately forced back off if the card is
    // unavailable at load time, so the UI never claims it's on when the
    // capture would silently fail.
    settings_screen_apply_sd_state();

    f.close();
    g_loading = false;
}

// ---- Screenshot toggle: public API ----------------------------------------

bool settings_get_screenshot_long_press()
{
    if (!screenshot_switch) return false;
    if (!lv_obj_has_state(screenshot_switch, LV_STATE_CHECKED)) return false;
    // Belt-and-braces — apply_sd_state should already have cleared the
    // switch when the card vanished, but never claim the feature is on
    // if we can't actually write a file right now.
    if (usb_sd_is_running() || !instance.isCardReady()) return false;
    return true;
}

void settings_screen_apply_sd_state()
{
    if (!screenshot_switch) return;

    bool sd_usable = instance.isCardReady() && !usb_sd_is_running();

    if (sd_usable) {
        // Re-enable interaction; don't touch the checked state — the user
        // (or settings load) owns that.
        lv_obj_clear_state(screenshot_switch, LV_STATE_DISABLED);
        lv_obj_clear_flag(screenshot_row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_text_color(screenshot_val_label,
            ARGUS_TEXT, LV_PART_MAIN);
    } else {
        // Force the toggle off and grey it — set g_loading around the
        // visual change so the value-changed callback doesn't write the
        // forced-off state back to the SD file (and overwrite the user's
        // saved preference when the card eventually comes back).
        bool was_loading = g_loading;
        g_loading = true;
        if (lv_obj_has_state(screenshot_switch, LV_STATE_CHECKED))
            lv_obj_clear_state(screenshot_switch, LV_STATE_CHECKED);
        lv_label_set_text(screenshot_val_label, "Off");
        g_loading = was_loading;

        lv_obj_add_state(screenshot_switch, LV_STATE_DISABLED);
        lv_obj_set_style_text_color(screenshot_val_label,
            ARGUS_TEXT_DIM, LV_PART_MAIN);
    }
}
