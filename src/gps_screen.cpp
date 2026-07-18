#include "gps_screen.h"
#include "theme.h"
#include <LilyGoLib.h>
#include "timezone.h"

// Defined in main.cpp
void clock_screen_set_gps_active(bool active);
void clock_screen_set_sat_count(uint32_t count);
void clock_screen_set_utc_offset(int offset_hours);
bool clock_screen_manual_time_active();

// Lower-bound longitude (×10, integer degrees) for each UTC hour offset -12 … +12.
// A longitude belongs to offset (i - 12) when lon×10 >= k_tz_lower[i].
// Zone boundaries sit at every 7.5° — the midpoint between standard meridians.
static const int16_t k_tz_lower[25] = {
    -1800, -1725, -1575, -1425, -1275, -1125,  -975,  -825,
     -675,  -525,  -375,  -225,   -75,    75,   225,   375,
      525,   675,   825,   975,  1125,  1275,  1425,  1575,
     1725
};

static int utc_offset_from_longitude(double lon)
{
    int16_t lon10 = (int16_t)(lon * 10.0);
    int offset = -12;
    for (int i = 0; i < 25; i++) {
        if (lon10 < k_tz_lower[i]) break;
        offset = i - 12;
    }
    return offset;
}

// Zeller's congruence for the Gregorian calendar. Returns 0 = Sunday,
// 1 = Monday, ... 6 = Saturday. Year is the full 4-digit year, month
// is 1..12, day is 1..31. Used by us_dst_active() to find the second
// Sunday of March / first Sunday of November without a calendar table.
static int day_of_week(int y, int m, int d)
{
    if (m < 3) { m += 12; y--; }
    int K = y % 100;
    int J = y / 100;
    int h = (d + (13 * (m + 1)) / 5 + K + K / 4 + J / 4 + 5 * J) % 7;
    // Zeller returns 0 = Saturday; shift so 0 = Sunday.
    return (h + 6) % 7;
}

// US-style daylight saving: starts at 02:00 on the second Sunday of
// March, ends at 02:00 on the first Sunday of November (post-2007
// rules). Hour parameter is local *standard* time. Approximate for
// the two transition days - we treat the whole transition day as DST
// on/off rather than handling the 02:00 boundary precisely, which is
// fine since the RTC is being seeded once at GPS-fix time and the
// user can flip Manual Time if they need exact precision on a
// transition Sunday.
static bool us_dst_active(int year, int month, int day)
{
    if (month < 3 || month > 11) return false;
    if (month > 3 && month < 11) return true;

    if (month == 3) {
        // DST starts second Sunday of March.
        int march_first_dow = day_of_week(year, 3, 1);   // 0=Sun
        int first_sunday    = (march_first_dow == 0) ? 1 : (8 - march_first_dow);
        int second_sunday   = first_sunday + 7;
        return day >= second_sunday;
    }

    // month == 11: DST ends first Sunday of November.
    int nov_first_dow = day_of_week(year, 11, 1);
    int first_sunday  = (nov_first_dow == 0) ? 1 : (8 - nov_first_dow);
    return day < first_sunday;
}

static lv_obj_t *gps_screen;
static lv_obj_t *toggle_sw;
static lv_obj_t *status_label;

static lv_obj_t *val_satellites;
static lv_obj_t *val_latitude;
static lv_obj_t *val_longitude;
static lv_obj_t *val_date;
static lv_obj_t *val_gps_time;
static lv_obj_t *val_altitude;
static lv_obj_t *val_speed;
static lv_obj_t *val_fix_age;

static bool gps_powered = false;
static bool rtc_synced  = false; // reset each GPS power cycle

// TinyGPSPlus has no reset; after a power cycle it keeps the previous session's
// values with isValid()==true until fresh NMEA overwrites them (a cold restart
// can take 30s+). Gate on commit age so stale data reads as "no fix" — this also
// stops the RTC being re-synced to the last session's timestamp. GPS streams
// ~1 Hz, so a live element is updated well within this window.
static const uint32_t GPS_FRESH_MS = 5000;

template <typename T>
static bool gps_fresh(const T &elem)
{
    return elem.isValid() && elem.age() < GPS_FRESH_MS;
}

static void update_status()
{
    lv_label_set_text(status_label, gps_powered ? "Radio: ON" : "Radio: OFF");
}

static void on_toggle(lv_event_t *e)
{
    gps_powered = lv_obj_has_state(toggle_sw, LV_STATE_CHECKED);
    // powerControl(POWER_GPS,false) gpio_reset_pin()s the UART pins but never
    // calls Serial1.end(), so the Serial1.begin() it runs on the next enable
    // re-inits a stale driver and RX stays detached → no NMEA, never locks.
    // Tear the UART down explicitly so each enable's begin() starts clean.
    if (gps_powered) {
        Serial1.end();
        instance.powerControl(POWER_GPS, true);  // enableBLDO1 + Serial1.begin
    } else {
        instance.powerControl(POWER_GPS, false); // disableBLDO1 + reset pins
        Serial1.end();
    }
    clock_screen_set_gps_active(gps_powered);
    if (!gps_powered)
        rtc_synced = false; // allow re-sync on next power-on
    update_status();
}

// Creates one key/value row in the scrollable data panel.
// The value label pointer is written to *val_out for later updates.
static void make_data_row(lv_obj_t *parent, const char *field, lv_obj_t **val_out)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, lv_pct(100), 36);
    lv_obj_set_style_bg_color(row, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 0, LV_PART_MAIN);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_border_color(row, lv_color_make(0x33, 0x33, 0x33), LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(row, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(row);
    lv_obj_set_style_text_color(lbl, lv_color_make(0xAA, 0xAA, 0xAA), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_label_set_text(lbl, field);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *val = lv_label_create(row);
    lv_obj_set_style_text_color(val, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(val, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_label_set_text(val, "--");
    lv_obj_align(val, LV_ALIGN_RIGHT_MID, 0, 0);

    *val_out = val;
}

// Fires every second via lv_timer; reads TinyGPSPlus fields and updates labels.
static void on_gps_update(lv_timer_t *timer)
{
    char buf[32];
    bool screen_active = gps_screen_is_active();

    // GPS off path. Push 0 sats to the home indicator so the badge clears
    // even if the user toggled GPS off from this screen and immediately
    // went back home; only refresh the (hidden) labels when the screen
    // is actually loaded, since those are LVGL-cost-only.
    if (!gps_powered) {
        clock_screen_set_sat_count(0);
        if (screen_active) {
            lv_label_set_text(val_satellites, "--");
            lv_label_set_text(val_latitude,   "--");
            lv_label_set_text(val_longitude,  "--");
            lv_label_set_text(val_date,       "--");
            lv_label_set_text(val_gps_time,   "--");
            lv_label_set_text(val_altitude,   "--");
            lv_label_set_text(val_speed,      "--");
            lv_label_set_text(val_fix_age,    "--");
        }
        return;
    }

    // Sync RTC + compute local UTC offset once per GPS power cycle.
    // Require a quality lock: valid position, date, time, year sanity, and ≥4 satellites.
    // Skipped entirely when the user has set the time by hand (manual override).
    // Run this even when off-screen - the user may enable GPS, leave for
    // home, and we still want the RTC to sync as soon as the fix lands.
    if (!rtc_synced
        && !clock_screen_manual_time_active()
        && gps_fresh(instance.gps.location)
        && gps_fresh(instance.gps.date)
        && gps_fresh(instance.gps.time)
        && instance.gps.date.year() > 2000
        && gps_fresh(instance.gps.satellites)
        && instance.gps.satellites.value() >= 4) {
        instance.rtc.setDateTime(
            instance.gps.date.year(),
            instance.gps.date.month(),
            instance.gps.date.day(),
            instance.gps.time.hour(),
            instance.gps.time.minute(),
            instance.gps.time.second()
        );
        instance.rtc.hwClockRead();
        // Base offset comes from longitude. North-American time zones
        // (UTC-5 Eastern through UTC-8 Pacific) get a +1 added when the
        // current date falls inside the US DST window. The check
        // ignores Arizona/Hawaii (no DST) and non-US countries that
        // happen to share those longitudes; users in those zones can
        // flip Manual Time to override.
        double gps_lat = instance.gps.location.lat();
        double gps_lon = instance.gps.location.lng();
        int base_off = utc_offset_from_longitude(gps_lon);
        int dst_bump = 0;
        if (base_off >= -8 && base_off <= -5
            && us_dst_active(instance.gps.date.year(),
                             instance.gps.date.month(),
                             instance.gps.date.day())) {
            // Arizona (UTC-7) does not observe DST. Suppress the bump when
            // the fix falls inside Arizona's bounding box. Navajo Nation
            // (NE corner of AZ) does observe DST but is a small area;
            // those users can use Manual Time if precision matters.
            bool in_arizona = (base_off == -7)
                           && (gps_lat >= 31.3 && gps_lat <= 37.0)
                           && (gps_lon >= -114.83 && gps_lon <= -109.05);
            if (!in_arizona) dst_bump = 1;
        }
        clock_screen_set_utc_offset(base_off + dst_bump);
        timezone_note_detected(base_off + dst_bump);   // persist across reboots
        rtc_synced = true;
    }

    // Satellite count - push to the home-screen status indicator every
    // tick regardless of which screen is loaded. Without this, the home
    // badge only updated while the GPS screen itself was open.
    uint32_t sat_count = 0;
    if (gps_fresh(instance.gps.satellites)) {
        sat_count = instance.gps.satellites.value();
        snprintf(buf, sizeof(buf), "%lu", (unsigned long)sat_count);
    } else {
        strcpy(buf, "--");
    }
    clock_screen_set_sat_count(sat_count);

    // From here down: pure GPS-screen UI labels. ~7 lv_label_set_text
    // calls per second + several gps.* field reads - skip when the user
    // isn't looking at this screen.
    if (!screen_active) return;

    lv_label_set_text(val_satellites, buf);

    // Location-derived fields (lat, lng, fix age share the same validity flag)
    if (gps_fresh(instance.gps.location)) {
        snprintf(buf, sizeof(buf), "%.5f", instance.gps.location.lat());
        lv_label_set_text(val_latitude, buf);
        snprintf(buf, sizeof(buf), "%.5f", instance.gps.location.lng());
        lv_label_set_text(val_longitude, buf);
        snprintf(buf, sizeof(buf), "%lu ms", instance.gps.location.age());
        lv_label_set_text(val_fix_age, buf);
    } else {
        lv_label_set_text(val_latitude,  "--");
        lv_label_set_text(val_longitude, "--");
        lv_label_set_text(val_fix_age,   "--");
    }

    // Date
    if (gps_fresh(instance.gps.date))
        snprintf(buf, sizeof(buf), "%04u-%02u-%02u",
            instance.gps.date.year(), instance.gps.date.month(), instance.gps.date.day());
    else
        strcpy(buf, "--");
    lv_label_set_text(val_date, buf);

    // Time (UTC)
    if (gps_fresh(instance.gps.time))
        snprintf(buf, sizeof(buf), "%02u:%02u:%02u",
            instance.gps.time.hour(), instance.gps.time.minute(), instance.gps.time.second());
    else
        strcpy(buf, "--");
    lv_label_set_text(val_gps_time, buf);

    // Altitude
    if (gps_fresh(instance.gps.altitude))
        snprintf(buf, sizeof(buf), "%.1f m", instance.gps.altitude.meters());
    else
        strcpy(buf, "--");
    lv_label_set_text(val_altitude, buf);

    // Speed
    if (gps_fresh(instance.gps.speed))
        snprintf(buf, sizeof(buf), "%.1f km/h", instance.gps.speed.kmph());
    else
        strcpy(buf, "--");
    lv_label_set_text(val_speed, buf);
}

void gps_screen_create()
{
    gps_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(gps_screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(gps_screen, 0, LV_PART_MAIN);

    // Title
    lv_obj_t *title = lv_label_create(gps_screen);
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_label_set_text(title, "GPS");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 5);

    // Power toggle (left of center)
    toggle_sw = lv_switch_create(gps_screen);
    lv_obj_set_size(toggle_sw, 100, 50);
    lv_obj_set_style_bg_color(toggle_sw, lv_color_make(0x44, 0x44, 0x44), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(toggle_sw, ARGUS_ACCENT, LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_add_event_cb(toggle_sw, on_toggle, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_align(toggle_sw, LV_ALIGN_TOP_MID, -90, 72);

    // Status label (right of toggle, vertically centred with it)
    status_label = lv_label_create(gps_screen);
    lv_obj_set_style_text_color(status_label, lv_color_make(0xAA, 0xAA, 0xAA), LV_PART_MAIN);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_align(status_label, LV_ALIGN_TOP_MID, 60, 87);
    update_status();

    // Scrollable data panel — occupies the bottom portion of the screen
    lv_obj_t *data_panel = lv_obj_create(gps_screen);
    lv_obj_set_size(data_panel, 380, 291);
    lv_obj_align(data_panel, LV_ALIGN_BOTTOM_MID, 0, -75);
    lv_obj_set_style_bg_color(data_panel, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_color(data_panel, lv_color_make(0x33, 0x33, 0x33), LV_PART_MAIN);
    lv_obj_set_style_border_width(data_panel, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(data_panel, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(data_panel, 0, LV_PART_MAIN);
    lv_obj_set_scroll_dir(data_panel, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(data_panel, LV_SCROLLBAR_MODE_AUTO);
    // Flex column layout stacks rows and handles overflow scrolling
    lv_obj_set_layout(data_panel, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(data_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(data_panel, 0, LV_PART_MAIN);

    make_data_row(data_panel, "Satellites", &val_satellites);
    make_data_row(data_panel, "Latitude",   &val_latitude);
    make_data_row(data_panel, "Longitude",  &val_longitude);
    make_data_row(data_panel, "Date",       &val_date);
    make_data_row(data_panel, "Time",       &val_gps_time);
    make_data_row(data_panel, "Altitude",   &val_altitude);
    make_data_row(data_panel, "Speed",      &val_speed);
    make_data_row(data_panel, "Fix Age",    &val_fix_age);

    lv_timer_create(on_gps_update, 1000, NULL);
}

void gps_screen_show()
{
    lv_scr_load(gps_screen);
}

bool gps_screen_is_active()
{
    return lv_screen_active() == gps_screen;
}

bool gps_screen_is_powered()
{
    return gps_powered;
}

bool gps_screen_has_lock()
{
    return gps_powered
        && gps_fresh(instance.gps.location)
        && gps_fresh(instance.gps.satellites)
        && instance.gps.satellites.value() >= 4;
}
