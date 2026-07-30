#include "world_clock_screen.h"
#include "theme.h"
#include <LilyGoLib.h>
#include <time.h>
#include <stdio.h>

// Defined elsewhere.
void time_screen_show();                        // time_screen.cpp
void clock_screen_get_local_time(struct tm *);  // main.cpp
int  clock_screen_get_utc_offset();             // main.cpp

static lv_obj_t *world_clock_screen;

// Fixed zone set, west -> east. Offsets are standard (no DST applied to the
// remote cities); the LOCAL row at the top uses the watch's live offset, so it
// always matches the face. DST for the remote cities is intentionally not
// modelled — this is a reference clock, and a single int offset per city keeps
// it honest about that rather than silently wrong twice a year.
struct Zone { const char *city; int off; };
static const Zone ZONES[] = {
    { "Honolulu",    -10 },
    { "Los Angeles",  -8 },
    { "Denver",       -7 },
    { "New York",     -5 },
    { "UTC",           0 },
    { "Paris",         1 },
    { "Moscow",        3 },
    { "Dubai",         4 },
    { "Tokyo",         9 },
    { "Sydney",       10 },
};
static const int ZONE_N = (int)(sizeof(ZONES) / sizeof(ZONES[0]));
static lv_obj_t *row_time[ZONE_N];
static lv_obj_t *row_local_time;                // the always-on LOCAL row

// UTC from the RTC, shifted by a whole-hour offset. mktime() only normalises the
// wrapped fields here (system TZ is UTC-neutral on this build) — the same trick
// main.cpp uses to apply clock_utc_offset.
static void zone_hm(int off, int &h, int &m)
{
    struct tm utc;
    instance.rtc.getDateTime(&utc);
    utc.tm_hour += off;
    mktime(&utc);
    h = utc.tm_hour;
    m = utc.tm_min;
}

static void refresh()
{
    char buf[8];

    struct tm local;
    clock_screen_get_local_time(&local);
    snprintf(buf, sizeof(buf), "%02d:%02d", local.tm_hour, local.tm_min);
    lv_label_set_text(row_local_time, buf);

    for (int i = 0; i < ZONE_N; i++) {
        int h, m;
        zone_hm(ZONES[i].off, h, m);
        snprintf(buf, sizeof(buf), "%02d:%02d", h, m);
        lv_label_set_text(row_time[i], buf);
    }
}

static void on_tick(lv_timer_t *)
{
    if (lv_screen_active() == world_clock_screen) refresh();
}

static void on_gesture(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    if (lv_indev_get_gesture_dir(indev) == LV_DIR_RIGHT) time_screen_show();
}

// One "City  UTC±N  HH:MM" row; returns the time label for later updates.
static lv_obj_t *make_row(lv_obj_t *list, const char *city, int off, bool highlight)
{
    lv_obj_t *row = lv_obj_create(list);
    lv_obj_set_size(row, 380, 40);
    lv_obj_set_style_radius(row, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(row,
        highlight ? ARGUS_ACCENT_DIM : lv_color_make(0x12, 0x18, 0x20), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *name = lv_label_create(row);
    lv_obj_set_style_text_font(name, &font_argus_label_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(name, highlight ? lv_color_black() : ARGUS_TEXT, LV_PART_MAIN);
    lv_label_set_text(name, city);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 14, 0);

    char ob[12];
    snprintf(ob, sizeof(ob), "UTC%+d", off);
    lv_obj_t *offl = lv_label_create(row);
    lv_obj_set_style_text_font(offl, &font_argus_label_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(offl, highlight ? lv_color_black() : ARGUS_TEXT_DIM, LV_PART_MAIN);
    lv_label_set_text(offl, ob);
    lv_obj_align(offl, LV_ALIGN_CENTER, 34, 0);

    lv_obj_t *tm = lv_label_create(row);
    lv_obj_set_style_text_font(tm, &font_argus_label_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(tm, highlight ? lv_color_black() : ARGUS_ACCENT, LV_PART_MAIN);
    lv_label_set_text(tm, "--:--");
    lv_obj_align(tm, LV_ALIGN_RIGHT_MID, -16, 0);
    return tm;
}

void world_clock_screen_create()
{
    world_clock_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(world_clock_screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(world_clock_screen, 0, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(world_clock_screen);
    lv_obj_set_style_text_color(title, argus_base_accent(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_argus_ui, LV_PART_MAIN);
    lv_label_set_text(title, "WORLD CLOCK");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    lv_obj_t *list = lv_obj_create(world_clock_screen);
    lv_obj_set_size(list, 400, 420);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 52);
    lv_obj_set_style_bg_color(list, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(list, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_row(list, 6, LV_PART_MAIN);
    lv_obj_set_layout(list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);

    // Always-on LOCAL row at the top, highlighted, from the watch's live offset.
    row_local_time = make_row(list, "LOCAL", clock_screen_get_utc_offset(), true);

    for (int i = 0; i < ZONE_N; i++)
        row_time[i] = make_row(list, ZONES[i].city, ZONES[i].off, false);

    refresh();
    lv_timer_create(on_tick, 1000, NULL);
    lv_obj_add_event_cb(world_clock_screen, on_gesture, LV_EVENT_GESTURE, NULL);
}

void world_clock_screen_show()      { refresh(); lv_scr_load(world_clock_screen); }
bool world_clock_screen_is_active() { return lv_screen_active() == world_clock_screen; }
