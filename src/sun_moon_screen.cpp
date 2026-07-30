#include "sun_moon_screen.h"
#include "theme.h"
#include <LilyGoLib.h>
#include <time.h>
#include <math.h>
#include <stdio.h>

// Defined elsewhere.
void time_screen_show();                        // time_screen.cpp
void clock_screen_get_local_time(struct tm *);  // main.cpp
int  clock_screen_get_utc_offset();             // main.cpp

static lv_obj_t *sun_moon_screen;
static lv_obj_t *val_sunrise;
static lv_obj_t *val_sunset;
static lv_obj_t *val_daylen;
static lv_obj_t *val_moon;
static lv_obj_t *val_loc;

// ---- Astronomy -------------------------------------------------------------

static int day_of_year(int y, int m, int d)
{
    static const int cum[] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    int n = cum[m - 1] + d;
    if (m > 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) n += 1;
    return n;
}

// Sunrise/Sunset algorithm (Almanac / NOAA, official zenith 90.833 deg).
// Returns event time as decimal hours in LOCAL time (0..24); false when the sun
// does not rise/set that day at this latitude (polar day/night).
static bool sun_event(double lat, double lng, int y, int mo, int d,
                      bool rise, int utc_off, double &out_local)
{
    const double D2R = M_PI / 180.0, R2D = 180.0 / M_PI, ZEN = 90.833;
    int N = day_of_year(y, mo, d);
    double lngHour = lng / 15.0;
    double t = rise ? (N + (6.0 - lngHour) / 24.0) : (N + (18.0 - lngHour) / 24.0);
    double M = 0.9856 * t - 3.289;
    double L = M + 1.916 * sin(M * D2R) + 0.020 * sin(2 * M * D2R) + 282.634;
    L = fmod(L + 720.0, 360.0);
    double RA = R2D * atan(0.91764 * tan(L * D2R));
    RA = fmod(RA + 720.0, 360.0);
    double Lq = floor(L / 90.0) * 90.0, RAq = floor(RA / 90.0) * 90.0;
    RA = (RA + (Lq - RAq)) / 15.0;
    double sinDec = 0.39782 * sin(L * D2R);
    double cosDec = cos(asin(sinDec));
    double cosH = (cos(ZEN * D2R) - sinDec * sin(lat * D2R)) / (cosDec * cos(lat * D2R));
    if (cosH > 1.0 || cosH < -1.0) return false;
    double H = rise ? (360.0 - R2D * acos(cosH)) : (R2D * acos(cosH));
    H /= 15.0;
    double T = H + RA - 0.06571 * t - 6.622;
    double UT = fmod(T - lngHour + 240.0, 24.0);
    out_local = fmod(UT + utc_off + 240.0, 24.0);
    return true;
}

static void split_hm(double hours, int &h, int &m)
{
    h = (int)hours;
    m = (int)((hours - h) * 60.0 + 0.5);
    if (m >= 60) { m -= 60; h = (h + 1) % 24; }
}

static long julian_day(int y, int m, int d)
{
    if (m <= 2) { y -= 1; m += 12; }
    int A = y / 100, B = 2 - A + A / 4;
    return (long)(365.25 * (y + 4716)) + (long)(30.6001 * (m + 1)) + d + B - 1524;
}

// Moon phase from the date. illum is the illuminated fraction in percent.
static const char *moon_phase(int y, int m, int d, int &illum)
{
    double jd = (double)julian_day(y, m, d);
    double phase = fmod((jd - 2451550.1) / 29.53058867, 1.0);
    if (phase < 0) phase += 1.0;
    illum = (int)(((1.0 - cos(2.0 * M_PI * phase)) / 2.0) * 100.0 + 0.5);
    if (phase < 0.03 || phase > 0.97) return "New Moon";
    if (phase < 0.22) return "Waxing Crescent";
    if (phase < 0.28) return "First Quarter";
    if (phase < 0.47) return "Waxing Gibbous";
    if (phase < 0.53) return "Full Moon";
    if (phase < 0.72) return "Waning Gibbous";
    if (phase < 0.78) return "Last Quarter";
    return "Waning Crescent";
}

// ---- Refresh ---------------------------------------------------------------

static void refresh()
{
    struct tm local;
    clock_screen_get_local_time(&local);
    int y = local.tm_year + 1900, mo = local.tm_mon + 1, d = local.tm_mday;
    int off = clock_screen_get_utc_offset();

    // Moon phase is location-independent — always available.
    int illum;
    const char *phase = moon_phase(y, mo, d, illum);
    char mbuf[40];
    snprintf(mbuf, sizeof(mbuf), "%s  %d%%", phase, illum);
    lv_label_set_text(val_moon, mbuf);

    bool haveGps = instance.gps.location.isValid();
    if (!haveGps) {
        lv_label_set_text(val_sunrise, "--:--");
        lv_label_set_text(val_sunset,  "--:--");
        lv_label_set_text(val_daylen,  "no fix");
        lv_label_set_text(val_loc,     "Sun times need a GPS fix");
        return;
    }

    double lat = instance.gps.location.lat();
    double lng = instance.gps.location.lng();

    double rise, set;
    bool hasRise = sun_event(lat, lng, y, mo, d, true,  off, rise);
    bool hasSet  = sun_event(lat, lng, y, mo, d, false, off, set);

    char b[16];
    if (hasRise) { int h, m; split_hm(rise, h, m); snprintf(b, sizeof(b), "%02d:%02d", h, m); lv_label_set_text(val_sunrise, b); }
    else lv_label_set_text(val_sunrise, "--:--");
    if (hasSet)  { int h, m; split_hm(set,  h, m); snprintf(b, sizeof(b), "%02d:%02d", h, m); lv_label_set_text(val_sunset,  b); }
    else lv_label_set_text(val_sunset, "--:--");

    if (hasRise && hasSet) {
        double len = set - rise;
        if (len < 0) len += 24.0;               // set past midnight
        int h = (int)len, m = (int)((len - h) * 60.0 + 0.5);
        if (m >= 60) { m -= 60; h += 1; }
        snprintf(b, sizeof(b), "%dh %02dm", h, m);
        lv_label_set_text(val_daylen, b);
    } else {
        lv_label_set_text(val_daylen, (!hasRise && !hasSet) ? "polar" : "--");
    }

    char loc[40];
    snprintf(loc, sizeof(loc), "%.2f%c  %.2f%c",
             fabs(lat), lat >= 0 ? 'N' : 'S',
             fabs(lng), lng >= 0 ? 'E' : 'W');
    lv_label_set_text(val_loc, loc);
}

static void on_tick(lv_timer_t *)
{
    if (lv_screen_active() == sun_moon_screen) refresh();
}

static void on_gesture(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    if (lv_indev_get_gesture_dir(indev) == LV_DIR_RIGHT) time_screen_show();
}

// ---- Layout ----------------------------------------------------------------

// One caption/value row in the vertical stack; returns the value label so the
// refresh path can update it.
static lv_obj_t *make_row(lv_obj_t *list, const char *caption, lv_color_t cap_col)
{
    lv_obj_t *row = lv_obj_create(list);
    lv_obj_set_size(row, 380, 56);
    lv_obj_set_style_radius(row, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(row, lv_color_make(0x12, 0x18, 0x20), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cap = lv_label_create(row);
    lv_obj_set_style_text_font(cap, &font_argus_label_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(cap, cap_col, LV_PART_MAIN);
    lv_label_set_text(cap, caption);
    lv_obj_align(cap, LV_ALIGN_LEFT_MID, 16, 0);

    lv_obj_t *val = lv_label_create(row);
    lv_obj_set_style_text_font(val, &font_argus_label_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(val, ARGUS_TEXT, LV_PART_MAIN);
    lv_label_set_text(val, "--");
    lv_obj_align(val, LV_ALIGN_RIGHT_MID, -16, 0);
    return val;
}

void sun_moon_screen_create()
{
    sun_moon_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(sun_moon_screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(sun_moon_screen, 0, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(sun_moon_screen);
    lv_obj_set_style_text_color(title, argus_base_accent(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_argus_ui, LV_PART_MAIN);
    lv_label_set_text(title, "SUN & MOON");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    lv_obj_t *list = lv_obj_create(sun_moon_screen);
    lv_obj_set_size(list, 400, 420);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 52);
    lv_obj_set_style_bg_color(list, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(list, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_row(list, 10, LV_PART_MAIN);
    lv_obj_set_layout(list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(list, LV_OBJ_FLAG_SCROLLABLE);

    val_sunrise = make_row(list, "Sunrise",   lv_color_make(0xF0, 0xB4, 0x30));  // amber
    val_sunset  = make_row(list, "Sunset",    HADES_RED);
    val_daylen  = make_row(list, "Daylight",  ARGUS_ACCENT);
    val_moon    = make_row(list, "Moon",      ARGUS_ACCENT);
    // Moon phase names are long — give that value its own smaller font.
    lv_obj_set_style_text_font(val_moon, &font_argus_label_16, LV_PART_MAIN);

    val_loc = lv_label_create(list);
    lv_obj_set_style_text_font(val_loc, &font_argus_label_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(val_loc, ARGUS_TEXT_DIM, LV_PART_MAIN);
    lv_label_set_text(val_loc, "");

    refresh();
    lv_timer_create(on_tick, 5000, NULL);
    lv_obj_add_event_cb(sun_moon_screen, on_gesture, LV_EVENT_GESTURE, NULL);
}

void sun_moon_screen_show()      { refresh(); lv_scr_load(sun_moon_screen); }
bool sun_moon_screen_is_active() { return lv_screen_active() == sun_moon_screen; }
