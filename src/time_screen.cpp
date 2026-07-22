#include "time_screen.h"
#include "theme.h"
#include <SD.h>
#include "alarm_screen.h"
#include "stopwatch_screen.h"
#include "timer_screen.h"
#include "calendar_screen.h"
#include <LilyGoLib.h>

// Defined in main.cpp
void clock_screen_show();

static lv_obj_t *time_screen;

// Swipe DOWN to return to the clock face — mirrors the swipe-UP entry from
// the clock. Other directions are no-ops so a sloppy left/right swipe inside
// a tile doesn't accidentally page away.
static void on_gesture(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir == LV_DIR_BOTTOM)
        clock_screen_show();
}

// ---- Tile helper -----------------------------------------------------------
// Identical to the tools-screen helper so the two grids feel uniform: a 180×
// 180 dark card with a bottom-anchored label and the icon drawn on top via
// LVGL primitives.
static lv_obj_t *make_tile(lv_obj_t *parent, const char *label_text)
{
    lv_obj_t *tile = lv_obj_create(parent);
    lv_obj_set_size(tile, 180, 180);
    // Match the Tools grid tile face: dark vertical gradient + steel-blue rim.
    lv_obj_set_style_bg_color(tile, lv_color_make(0x16, 0x1E, 0x28), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(tile, lv_color_make(0x0D, 0x13, 0x1B), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(tile, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(tile, ARGUS_ACCENT_DIM, LV_PART_MAIN);
    lv_obj_set_style_border_width(tile, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(tile, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_all(tile, 0, LV_PART_MAIN);
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *lbl = lv_label_create(tile);
    lv_obj_set_style_text_color(lbl, ARGUS_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &font_argus_label_20, LV_PART_MAIN);   // Orbitron (brand label)
    lv_label_set_text(lbl, label_text);
    lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, -12);

    return tile;
}

// Load the ARGUS HD sprite /Icons/<name>.png if present on the SD card,
// else fall back to the procedural draw_*_icon(). Same pattern as tools_screen.
static void tile_icon(lv_obj_t *tile, const char *name, void (*fallback)(lv_obj_t *))
{
    char sdpath[40];
    snprintf(sdpath, sizeof(sdpath), "/Icons/%s.png", name);
    if (SD.exists(sdpath)) {
        char lvpath[44];
        snprintf(lvpath, sizeof(lvpath), "A:/Icons/%s.png", name);
        lv_obj_t *img = lv_image_create(tile);
        lv_image_set_src(img, lvpath);
        lv_obj_align(img, LV_ALIGN_TOP_MID, 0, 8);
    } else {
        fallback(tile);
    }
}

// ---- Icons -----------------------------------------------------------------
//
// Twin-bell alarm clock with splayed legs, striker bar across the bells, and
// hands that read roughly "alarm time".
static void draw_alarm_icon(lv_obj_t *tile)
{
    static lv_point_precise_t alarm_leg_l[] = { {72, 116}, {62, 130} };
    lv_obj_t *leg_l = lv_line_create(tile);
    lv_line_set_points(leg_l, alarm_leg_l, 2);
    lv_obj_set_style_line_color(leg_l, lv_color_make(0xCC, 0xCC, 0xCC), 0);
    lv_obj_set_style_line_width(leg_l, 5, 0);
    lv_obj_set_style_line_rounded(leg_l, true, 0);

    static lv_point_precise_t alarm_leg_r[] = { {108, 116}, {118, 130} };
    lv_obj_t *leg_r = lv_line_create(tile);
    lv_line_set_points(leg_r, alarm_leg_r, 2);
    lv_obj_set_style_line_color(leg_r, lv_color_make(0xCC, 0xCC, 0xCC), 0);
    lv_obj_set_style_line_width(leg_r, 5, 0);
    lv_obj_set_style_line_rounded(leg_r, true, 0);

    lv_obj_t *face = lv_obj_create(tile);
    lv_obj_set_size(face, 76, 76);
    lv_obj_set_style_radius(face, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(face, lv_color_make(0x22, 0x22, 0x22), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(face, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(face, lv_color_make(0xCC, 0xCC, 0xCC), LV_PART_MAIN);
    lv_obj_set_style_border_width(face, 3, LV_PART_MAIN);
    lv_obj_set_style_pad_all(face, 0, LV_PART_MAIN);
    lv_obj_clear_flag(face, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(face, LV_ALIGN_TOP_MID, 0, 38);

    for (int side = -1; side <= 1; side += 2) {
        lv_obj_t *bell = lv_obj_create(tile);
        lv_obj_set_size(bell, 22, 22);
        lv_obj_set_style_radius(bell, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(bell, lv_color_make(0xFF, 0xCC, 0x00), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(bell, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(bell, lv_color_make(0xAA, 0x88, 0x00), LV_PART_MAIN);
        lv_obj_set_style_border_width(bell, 1, LV_PART_MAIN);
        lv_obj_set_style_pad_all(bell, 0, LV_PART_MAIN);
        lv_obj_clear_flag(bell, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(bell, LV_ALIGN_TOP_MID, side * 22, 26);
    }

    lv_obj_t *striker = lv_obj_create(tile);
    lv_obj_set_size(striker, 50, 4);
    lv_obj_set_style_radius(striker, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(striker, lv_color_make(0xCC, 0xCC, 0xCC), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(striker, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(striker, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(striker, 0, LV_PART_MAIN);
    lv_obj_clear_flag(striker, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(striker, LV_ALIGN_TOP_MID, 0, 24);

    static lv_point_precise_t alarm_min_pts[] = { {90, 76}, {90, 52} };
    lv_obj_t *minute = lv_line_create(tile);
    lv_line_set_points(minute, alarm_min_pts, 2);
    lv_obj_set_style_line_color(minute, lv_color_make(0xFF, 0xCC, 0x00), 0);
    lv_obj_set_style_line_width(minute, 3, 0);
    lv_obj_set_style_line_rounded(minute, true, 0);

    static lv_point_precise_t alarm_hour_pts[] = { {90, 76}, {76, 88} };
    lv_obj_t *hour = lv_line_create(tile);
    lv_line_set_points(hour, alarm_hour_pts, 2);
    lv_obj_set_style_line_color(hour, lv_color_make(0xFF, 0xCC, 0x00), 0);
    lv_obj_set_style_line_width(hour, 4, 0);
    lv_obj_set_style_line_rounded(hour, true, 0);

    lv_obj_t *dot = lv_obj_create(tile);
    lv_obj_set_size(dot, 6, 6);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(dot, lv_color_make(0xFF, 0xCC, 0x00), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(dot, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(dot, 0, LV_PART_MAIN);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(dot, LV_ALIGN_TOP_MID, 0, 73);
}

// Round-faced stopwatch with start/stop button, hour ticks, and a red second
// hand caught mid-sweep.
static void draw_stopwatch_icon(lv_obj_t *tile)
{
    lv_obj_t *btn = lv_obj_create(tile);
    lv_obj_set_size(btn, 18, 8);
    lv_obj_set_style_radius(btn, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_make(0xCC, 0xCC, 0xCC), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(btn, LV_ALIGN_TOP_MID, 0, 22);

    lv_obj_t *stem = lv_obj_create(tile);
    lv_obj_set_size(stem, 8, 6);
    lv_obj_set_style_radius(stem, 1, LV_PART_MAIN);
    lv_obj_set_style_bg_color(stem, lv_color_make(0xCC, 0xCC, 0xCC), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(stem, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(stem, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(stem, 0, LV_PART_MAIN);
    lv_obj_clear_flag(stem, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(stem, LV_ALIGN_TOP_MID, 0, 30);

    lv_obj_t *face = lv_obj_create(tile);
    lv_obj_set_size(face, 78, 78);
    lv_obj_set_style_radius(face, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(face, lv_color_make(0x22, 0x22, 0x22), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(face, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(face, lv_color_make(0xCC, 0xCC, 0xCC), LV_PART_MAIN);
    lv_obj_set_style_border_width(face, 3, LV_PART_MAIN);
    lv_obj_set_style_pad_all(face, 0, LV_PART_MAIN);
    lv_obj_clear_flag(face, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(face, LV_ALIGN_TOP_MID, 0, 36);

    int cx = 90, cy = 75;
    for (int i = 0; i < 12; i++) {
        bool cardinal = (i % 3 == 0);
        lv_obj_t *tick = lv_obj_create(tile);
        lv_obj_set_size(tick, cardinal ? 3 : 2, cardinal ? 8 : 5);
        lv_obj_set_style_radius(tick, 1, LV_PART_MAIN);
        lv_obj_set_style_bg_color(tick,
            cardinal ? lv_color_make(0xEE, 0xEE, 0xEE)
                     : lv_color_make(0x88, 0x88, 0x88), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(tick, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(tick, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(tick, 0, LV_PART_MAIN);
        lv_obj_clear_flag(tick, LV_OBJ_FLAG_SCROLLABLE);
        float rad = i * 3.14159f / 6.0f;
        int tx = cx + (int)(33.0f * sinf(rad));
        int ty = cy - (int)(33.0f * cosf(rad));
        lv_obj_set_pos(tick, tx - 1, ty - (cardinal ? 4 : 2));
    }

    static lv_point_precise_t sw_hand_pts[] = { {90, 75}, {112, 56} };
    lv_obj_t *hand = lv_line_create(tile);
    lv_line_set_points(hand, sw_hand_pts, 2);
    lv_obj_set_style_line_color(hand, lv_color_make(0xFF, 0x44, 0x44), 0);
    lv_obj_set_style_line_width(hand, 3, 0);
    lv_obj_set_style_line_rounded(hand, true, 0);

    lv_obj_t *dot = lv_obj_create(tile);
    lv_obj_set_size(dot, 8, 8);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(dot, lv_color_make(0xFF, 0x44, 0x44), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(dot, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(dot, 0, LV_PART_MAIN);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(dot, LV_ALIGN_TOP_MID, 0, 71);
}

// Hourglass timer: top + bottom caps, four diagonal frame edges that meet at
// the neck, sand stripes in both halves, and a falling stream at the neck.
static void draw_timer_icon(lv_obj_t *tile)
{
    lv_obj_t *top_bar = lv_obj_create(tile);
    lv_obj_set_size(top_bar, 60, 6);
    lv_obj_set_style_radius(top_bar, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(top_bar, lv_color_make(0xDD, 0xDD, 0xDD), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(top_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(top_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(top_bar, 0, LV_PART_MAIN);
    lv_obj_clear_flag(top_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(top_bar, LV_ALIGN_TOP_MID, 0, 24);

    lv_obj_t *bot_bar = lv_obj_create(tile);
    lv_obj_set_size(bot_bar, 60, 6);
    lv_obj_set_style_radius(bot_bar, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bot_bar, lv_color_make(0xDD, 0xDD, 0xDD), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bot_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(bot_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bot_bar, 0, LV_PART_MAIN);
    lv_obj_clear_flag(bot_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(bot_bar, LV_ALIGN_TOP_MID, 0, 106);

    static lv_point_precise_t timer_tl[] = { {60, 30}, {88, 68} };
    lv_obj_t *tl = lv_line_create(tile);
    lv_line_set_points(tl, timer_tl, 2);
    lv_obj_set_style_line_color(tl, lv_color_make(0xDD, 0xDD, 0xDD), 0);
    lv_obj_set_style_line_width(tl, 5, 0);
    lv_obj_set_style_line_rounded(tl, true, 0);

    static lv_point_precise_t timer_tr[] = { {120, 30}, {92, 68} };
    lv_obj_t *tr = lv_line_create(tile);
    lv_line_set_points(tr, timer_tr, 2);
    lv_obj_set_style_line_color(tr, lv_color_make(0xDD, 0xDD, 0xDD), 0);
    lv_obj_set_style_line_width(tr, 5, 0);
    lv_obj_set_style_line_rounded(tr, true, 0);

    static lv_point_precise_t timer_bl[] = { {88, 68}, {60, 106} };
    lv_obj_t *bl = lv_line_create(tile);
    lv_line_set_points(bl, timer_bl, 2);
    lv_obj_set_style_line_color(bl, lv_color_make(0xDD, 0xDD, 0xDD), 0);
    lv_obj_set_style_line_width(bl, 5, 0);
    lv_obj_set_style_line_rounded(bl, true, 0);

    static lv_point_precise_t timer_br[] = { {92, 68}, {120, 106} };
    lv_obj_t *br = lv_line_create(tile);
    lv_line_set_points(br, timer_br, 2);
    lv_obj_set_style_line_color(br, lv_color_make(0xDD, 0xDD, 0xDD), 0);
    lv_obj_set_style_line_width(br, 5, 0);
    lv_obj_set_style_line_rounded(br, true, 0);

    static const struct { int w, y; } top_sand[] = {
        { 50, 32 }, { 38, 38 }, { 26, 44 }, { 14, 50 },
    };
    for (size_t i = 0; i < sizeof(top_sand) / sizeof(top_sand[0]); i++) {
        lv_obj_t *s = lv_obj_create(tile);
        lv_obj_set_size(s, top_sand[i].w, 4);
        lv_obj_set_style_radius(s, 1, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s, lv_color_make(0xFF, 0xCC, 0x00), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(s, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(s, 0, LV_PART_MAIN);
        lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(s, LV_ALIGN_TOP_MID, 0, top_sand[i].y);
    }

    lv_obj_t *stream = lv_obj_create(tile);
    lv_obj_set_size(stream, 3, 12);
    lv_obj_set_style_radius(stream, 1, LV_PART_MAIN);
    lv_obj_set_style_bg_color(stream, lv_color_make(0xFF, 0xCC, 0x00), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(stream, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(stream, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(stream, 0, LV_PART_MAIN);
    lv_obj_clear_flag(stream, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(stream, LV_ALIGN_TOP_MID, 0, 68);

    static const struct { int w, y; } bot_sand[] = {
        { 14, 86 }, { 26, 92 }, { 38, 98 }, { 50, 104 },
    };
    for (size_t i = 0; i < sizeof(bot_sand) / sizeof(bot_sand[0]); i++) {
        lv_obj_t *s = lv_obj_create(tile);
        lv_obj_set_size(s, bot_sand[i].w, 4);
        lv_obj_set_style_radius(s, 1, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s, lv_color_make(0xFF, 0xCC, 0x00), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(s, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(s, 0, LV_PART_MAIN);
        lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(s, LV_ALIGN_TOP_MID, 0, bot_sand[i].y);
    }
}

// Spiral-bound page calendar with red header, white body, and a 5×4 grid of
// day cells with one painted gold as "today".
static void draw_calendar_icon(lv_obj_t *tile)
{
    lv_obj_t *bar = lv_obj_create(tile);
    lv_obj_set_size(bar, 84, 2);
    lv_obj_set_style_bg_color(bar, lv_color_make(0x77, 0x77, 0x77), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bar, 0, LV_PART_MAIN);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 20);

    for (int i = 0; i < 4; i++) {
        lv_obj_t *ring = lv_obj_create(tile);
        lv_obj_set_size(ring, 6, 10);
        lv_obj_set_style_radius(ring, 3, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_color(ring, lv_color_make(0xAA, 0xAA, 0xAA), LV_PART_MAIN);
        lv_obj_set_style_border_width(ring, 2, LV_PART_MAIN);
        lv_obj_set_style_pad_all(ring, 0, LV_PART_MAIN);
        lv_obj_clear_flag(ring, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(ring, LV_ALIGN_TOP_MID, -27 + i * 18, 16);
    }

    lv_obj_t *header = lv_obj_create(tile);
    lv_obj_set_size(header, 72, 16);
    lv_obj_set_style_radius(header, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(header, lv_color_make(0xCC, 0x33, 0x33), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(header, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(header, 0, LV_PART_MAIN);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 30);

    lv_obj_t *page = lv_obj_create(tile);
    lv_obj_set_size(page, 72, 68);
    lv_obj_set_style_radius(page, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(page, lv_color_make(0xEE, 0xEE, 0xEE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(page, lv_color_make(0xAA, 0xAA, 0xAA), LV_PART_MAIN);
    lv_obj_set_style_border_width(page, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_all(page, 0, LV_PART_MAIN);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(page, LV_ALIGN_TOP_MID, 0, 46);

    const int today_row = 2, today_col = 3;
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 5; c++) {
            bool is_today = (r == today_row && c == today_col);
            lv_obj_t *cell = lv_obj_create(tile);
            lv_obj_set_size(cell, 8, 8);
            lv_obj_set_style_radius(cell, 1, LV_PART_MAIN);
            lv_obj_set_style_bg_color(cell,
                is_today ? lv_color_make(0xFF, 0xCC, 0x00)
                         : lv_color_make(0x66, 0x66, 0x66), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_border_width(cell, 0, LV_PART_MAIN);
            lv_obj_set_style_pad_all(cell, 0, LV_PART_MAIN);
            lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_pos(cell, 66 + c * 10, 56 + r * 10);
        }
    }
}

// ---- Public API ------------------------------------------------------------

void time_screen_create()
{
    time_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(time_screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(time_screen, 0, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(time_screen);
    lv_obj_set_style_text_color(title, argus_accent(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_argus_ui, LV_PART_MAIN);
    lv_label_set_text(title, "TIME");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    // Two-column flex grid — same geometry as the Tools grid so the two
    // screens feel like siblings. With only four tiles the grid doesn't
    // need to scroll, but the dir is left vertical for future additions.
    lv_obj_t *grid = lv_obj_create(time_screen);
    lv_obj_set_size(grid, 400, 432);
    lv_obj_align(grid, LV_ALIGN_TOP_MID, 0, 52);
    lv_obj_set_style_bg_color(grid, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(grid, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(grid, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_row(grid, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_column(grid, 12, LV_PART_MAIN);
    lv_obj_set_scroll_dir(grid, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(grid, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_layout(grid, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid,
        LV_FLEX_ALIGN_SPACE_EVENLY,
        LV_FLEX_ALIGN_START,
        LV_FLEX_ALIGN_START);

    // Insertion order (row-major, grid wraps every 2 tiles):
    //   [Alarm]   [Stopwatch]
    //   [Timer]   [Calendar]
    lv_obj_t *t_alarm     = make_tile(grid, "Alarm");
    lv_obj_t *t_stopwatch = make_tile(grid, "Stopwatch");
    lv_obj_t *t_timer     = make_tile(grid, "Timer");
    lv_obj_t *t_calendar  = make_tile(grid, "Calendar");

    tile_icon(t_alarm,     "alarm",     draw_alarm_icon);
    tile_icon(t_stopwatch, "stopwatch", draw_stopwatch_icon);
    tile_icon(t_timer,     "timer",     draw_timer_icon);
    tile_icon(t_calendar,  "calendar",  draw_calendar_icon);

    lv_obj_add_event_cb(t_alarm,     [](lv_event_t *) { alarm_screen_show();    }, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(t_stopwatch, [](lv_event_t *) { stopwatch_screen_show(); }, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(t_timer,     [](lv_event_t *) { timer_screen_show();    }, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(t_calendar,  [](lv_event_t *) { calendar_screen_show(); }, LV_EVENT_CLICKED, NULL);

    // lv_obj_create() children are CLICKABLE by default and would otherwise
    // swallow taps on the icon shapes. EVENT_BUBBLE on every child sends taps
    // up to the tile's CLICKED handler — same trick the Tools screen uses.
    uint32_t tile_count = lv_obj_get_child_count(grid);
    for (uint32_t i = 0; i < tile_count; i++) {
        lv_obj_t *tile = lv_obj_get_child(grid, i);
        uint32_t kid_count = lv_obj_get_child_count(tile);
        for (uint32_t j = 0; j < kid_count; j++) {
            lv_obj_add_flag(lv_obj_get_child(tile, j), LV_OBJ_FLAG_EVENT_BUBBLE);
        }
    }

    lv_obj_add_event_cb(time_screen, on_gesture, LV_EVENT_GESTURE, NULL);
}

void time_screen_show()       { lv_scr_load(time_screen); }
bool time_screen_is_active()  { return lv_screen_active() == time_screen; }
