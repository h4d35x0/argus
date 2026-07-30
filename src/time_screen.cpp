#include "time_screen.h"
#include "theme.h"
#include "argus_mode.h"
#include <SD.h>
#include "alarm_screen.h"
#include "stopwatch_screen.h"
#include "timer_screen.h"
#include "calendar_screen.h"
#include "world_clock_screen.h"
#include "sun_moon_screen.h"
#include "flashlight_screen.h"
#include "meshtastic_screen.h"
#include "settings_screen.h"
#include "notifications_screen.h"
#include <math.h>
#include <LilyGoLib.h>

// Defined in main.cpp
void clock_screen_show();

static lv_obj_t *time_screen;
static lv_obj_t *s_time_title = nullptr;   // heading; repainted per mode on show
static lv_obj_t *s_time_grid  = nullptr;   // tile grid; tile borders recolored per mode on show

// Per-tile record so an icon can be REDRAWN on a mode change: Offense recolors it
// red; Daily/Defense redraws it in its natural multi-colour.
struct TimeTile { lv_obj_t *tile; const char *name; void (*draw)(lv_obj_t *); };
static TimeTile s_ttiles[10];
static int      s_ttile_n         = 0;
static bool     s_ttile_recording = true;   // record only during the initial create pass
static bool     s_ttiles_red      = false;  // last-rendered regime (true = Offense red)

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
    lv_obj_set_size(tile, 118, 118);   // 3-across grid (matches the Tools page)
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
    lv_obj_set_style_text_font(lbl, &font_argus_label_14, LV_PART_MAIN);   // Orbitron (fits 118px tile)
    lv_label_set_text(lbl, label_text);
    lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, -6);

    return tile;
}

// Load the ARGUS HD sprite /Icons/<name>.png if present on the SD card,
// else fall back to the procedural draw_*_icon(). Same pattern as tools_screen.
static void tile_icon(lv_obj_t *tile, const char *name, void (*fallback)(lv_obj_t *))
{
    if (s_ttile_recording && s_ttile_n < 10) s_ttiles[s_ttile_n++] = { tile, name, fallback };
    char sdpath[40];
    snprintf(sdpath, sizeof(sdpath), "/Icons/%s.png", name);
    if (SD.exists(sdpath)) {
        char lvpath[44];
        snprintf(lvpath, sizeof(lvpath), "A:/Icons/%s.png", name);
        lv_obj_t *img = lv_image_create(tile);
        lv_image_set_src(img, lvpath);
        // 140px sprites in a 118px tile: scale to ~78px and pin the scale pivot to
        // top-centre so the glyph renders high (not floating low over the label).
        // Same treatment as the Tools page.
        lv_image_set_scale(img, 142);
        lv_image_set_pivot(img, 70, 0);
        lv_obj_align(img, LV_ALIGN_TOP_MID, 0, 6);
    } else {
        fallback(tile);
    }
}

// Recursively tag descendants EVENT_BUBBLE so taps on a redrawn icon still reach
// the tile's click handler.
static void tt_bubble(lv_obj_t *o)
{
    uint32_t n = lv_obj_get_child_count(o);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_add_flag(lv_obj_get_child(o, i), LV_OBJ_FLAG_EVENT_BUBBLE);
        tt_bubble(lv_obj_get_child(o, i));
    }
}

// Recursively paint every shape in the subtree the Offense red. Covers both
// procedural icons (bg/border/line) AND SD PNG sprites: an lv_image ignores
// bg/border/line entirely, so without image_recolor the clock sprites stayed
// their natural colour in Offense while the Tools sprites (which DO set
// image_recolor) went red. Recolor at full opacity tints the sprite to a solid
// red silhouette, matching the Tools red-team look.
static void tt_recolor_red(lv_obj_t *o)
{
    lv_obj_set_style_bg_color(o, ARGUS_OFFENSE_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_border_color(o, ARGUS_OFFENSE_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_line_color(o, ARGUS_OFFENSE_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_image_recolor(o, ARGUS_OFFENSE_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_image_recolor_opa(o, LV_OPA_COVER, LV_PART_MAIN);
    uint32_t n = lv_obj_get_child_count(o);
    for (uint32_t i = 0; i < n; i++) tt_recolor_red(lv_obj_get_child(o, i));
}

// Redraw one tile's icon for the current mode: a fresh natural (multi-colour)
// draw, then recolor red for Offense. Label is child 0; icon is everything after.
static void tt_redraw(TimeTile *t, bool offense)
{
    while (lv_obj_get_child_count(t->tile) > 1)
        lv_obj_delete(lv_obj_get_child(t->tile, 1));
    tile_icon(t->tile, t->name, t->draw);   // recording is off now -> no re-record
    uint32_t n = lv_obj_get_child_count(t->tile);
    for (uint32_t i = 1; i < n; i++) {
        tt_bubble(lv_obj_get_child(t->tile, i));
        if (offense) tt_recolor_red(lv_obj_get_child(t->tile, i));
    }
}

// The procedural draw_*_icon() helpers below were authored in the original 180px
// tile coordinate space. Draw each into a 180x180 layer scaled to fit the 118px
// tile, so the composition is preserved without re-tuning every primitive — the
// same helper the Tools page uses.
static lv_obj_t *icon_layer(lv_obj_t *tile)
{
    lv_obj_t *layer = lv_obj_create(tile);
    lv_obj_remove_style_all(layer);
    lv_obj_set_size(layer, 180, 180);
    lv_obj_add_flag(layer, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_clear_flag(layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_transform_pivot_x(layer, 90, LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_y(layer, 73, LV_PART_MAIN);
    lv_obj_set_style_transform_scale_x(layer, 198, LV_PART_MAIN);
    lv_obj_set_style_transform_scale_y(layer, 198, LV_PART_MAIN);
    lv_obj_align(layer, LV_ALIGN_TOP_LEFT, -31, -28);
    return layer;
}

// ---- Icons -----------------------------------------------------------------
//
// Twin-bell alarm clock with splayed legs, striker bar across the bells, and
// hands that read roughly "alarm time".
static void draw_alarm_icon(lv_obj_t *tile)
{
    tile = icon_layer(tile);

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
    tile = icon_layer(tile);

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
    tile = icon_layer(tile);

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
    tile = icon_layer(tile);

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

// World Clock — a globe (steel outline) with a meridian, equator and two tropic
// lines. Procedural fallback for /Icons/worldclock.png. Drawn in the 180px tile
// space, same as the other Time icons.
static void draw_worldclock_icon(lv_obj_t *tile)
{
    tile = icon_layer(tile);

    lv_color_t steel = lv_color_make(0x9B, 0xBC, 0xD6);
    lv_color_t dim   = lv_color_make(0x5B, 0x7C, 0x96);

    lv_obj_t *g = lv_obj_create(tile);
    lv_obj_set_size(g, 100, 100);
    lv_obj_set_style_radius(g, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_color(g, steel, LV_PART_MAIN);
    lv_obj_set_style_border_width(g, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g, 0, LV_PART_MAIN);
    lv_obj_clear_flag(g, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(g, LV_ALIGN_TOP_MID, 0, 34);

    lv_obj_t *mer = lv_obj_create(tile);          // meridian (vertical stadium)
    lv_obj_set_size(mer, 44, 100);
    lv_obj_set_style_radius(mer, 22, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mer, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_color(mer, dim, LV_PART_MAIN);
    lv_obj_set_style_border_width(mer, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(mer, 0, LV_PART_MAIN);
    lv_obj_clear_flag(mer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(mer, LV_ALIGN_TOP_MID, 0, 34);

    for (int i = 0; i < 3; i++) {                 // equator + two tropics
        lv_obj_t *ln = lv_obj_create(tile);
        lv_obj_set_size(ln, i == 1 ? 100 : 80, i == 1 ? 3 : 2);
        lv_obj_set_style_bg_color(ln, dim, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(ln, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(ln, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(ln, 0, LV_PART_MAIN);
        lv_obj_clear_flag(ln, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(ln, LV_ALIGN_TOP_MID, 0, 62 + i * 20);
    }
}

// Sun & Moon — an amber sun disc beside a steel crescent moon. Procedural
// fallback for /Icons/sunmoon.png.
static void draw_sunmoon_icon(lv_obj_t *tile)
{
    tile = icon_layer(tile);

    lv_color_t amber = lv_color_make(0xFF, 0xB0, 0x20);
    lv_color_t steel = lv_color_make(0xC8, 0xDE, 0xF0);
    lv_color_t dark  = lv_color_make(0x12, 0x18, 0x20);

    for (int i = 0; i < 8; i++) {                 // sun rays
        lv_obj_t *ray = lv_obj_create(tile);
        lv_obj_set_size(ray, 4, 12);
        lv_obj_set_style_radius(ray, 2, LV_PART_MAIN);
        lv_obj_set_style_bg_color(ray, amber, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(ray, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(ray, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(ray, 0, LV_PART_MAIN);
        lv_obj_set_style_transform_pivot_x(ray, 2, LV_PART_MAIN);
        lv_obj_set_style_transform_pivot_y(ray, 44, LV_PART_MAIN);
        lv_obj_set_style_transform_rotation(ray, i * 450, LV_PART_MAIN);
        lv_obj_clear_flag(ray, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(ray, LV_ALIGN_TOP_MID, -26, 42);
    }

    lv_obj_t *sun = lv_obj_create(tile);          // sun disc (left)
    lv_obj_set_size(sun, 60, 60);
    lv_obj_set_style_radius(sun, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sun, amber, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sun, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(sun, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(sun, 0, LV_PART_MAIN);
    lv_obj_clear_flag(sun, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(sun, LV_ALIGN_TOP_MID, -26, 52);

    lv_obj_t *moon = lv_obj_create(tile);         // moon disc (right)
    lv_obj_set_size(moon, 56, 56);
    lv_obj_set_style_radius(moon, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(moon, steel, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(moon, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(moon, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(moon, 0, LV_PART_MAIN);
    lv_obj_clear_flag(moon, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(moon, LV_ALIGN_TOP_MID, 30, 56);

    lv_obj_t *carve = lv_obj_create(tile);        // carve the crescent
    lv_obj_set_size(carve, 56, 56);
    lv_obj_set_style_radius(carve, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(carve, dark, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(carve, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(carve, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(carve, 0, LV_PART_MAIN);
    lv_obj_clear_flag(carve, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(carve, LV_ALIGN_TOP_MID, 18, 52);
}

// Flashlight — a chrome torch with an amber lens and a three-ray beam.
// Procedural fallback for /Icons/flashlight.png.
static void draw_flashlight_icon(lv_obj_t *tile)
{
    tile = icon_layer(tile);
    lv_color_t body  = lv_color_make(0xB0, 0xB8, 0xC0);
    lv_color_t amber = lv_color_make(0xFF, 0xC8, 0x30);

    lv_obj_t *handle = lv_obj_create(tile);
    lv_obj_set_size(handle, 28, 52);
    lv_obj_set_style_radius(handle, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(handle, body, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(handle, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(handle, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(handle, 0, LV_PART_MAIN);
    lv_obj_clear_flag(handle, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(handle, LV_ALIGN_TOP_MID, 0, 74);

    lv_obj_t *head = lv_obj_create(tile);
    lv_obj_set_size(head, 42, 20);
    lv_obj_set_style_radius(head, 4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(head, body, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(head, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(head, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(head, 0, LV_PART_MAIN);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(head, LV_ALIGN_TOP_MID, 0, 58);

    lv_obj_t *lens = lv_obj_create(tile);
    lv_obj_set_size(lens, 34, 16);
    lv_obj_set_style_radius(lens, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(lens, amber, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(lens, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(lens, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(lens, 0, LV_PART_MAIN);
    lv_obj_clear_flag(lens, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(lens, LV_ALIGN_TOP_MID, 0, 50);

    static lv_point_precise_t fl_r1[] = { {78, 44}, {66, 30} };
    static lv_point_precise_t fl_r2[] = { {90, 42}, {90, 26} };
    static lv_point_precise_t fl_r3[] = { {102, 44}, {114, 30} };
    lv_point_precise_t *rays[] = { fl_r1, fl_r2, fl_r3 };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *ray = lv_line_create(tile);
        lv_line_set_points(ray, rays[i], 2);
        lv_obj_set_style_line_color(ray, amber, 0);
        lv_obj_set_style_line_width(ray, 3, 0);
        lv_obj_set_style_line_rounded(ray, true, 0);
    }
}

// Meshtastic — three mesh nodes (one lit green) joined by links.
// Procedural fallback for /Icons/meshtastic.png.
static void draw_meshtastic_icon(lv_obj_t *tile)
{
    tile = icon_layer(tile);
    lv_color_t steel = lv_color_make(0x9B, 0xBC, 0xD6);
    lv_color_t green = lv_color_make(0x3C, 0xDC, 0x78);

    static lv_point_precise_t ms_l1[] = { {60, 58}, {120, 58} };
    static lv_point_precise_t ms_l2[] = { {60, 58}, {90, 112} };
    static lv_point_precise_t ms_l3[] = { {120, 58}, {90, 112} };
    lv_point_precise_t *links[] = { ms_l1, ms_l2, ms_l3 };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *ln = lv_line_create(tile);
        lv_line_set_points(ln, links[i], 2);
        lv_obj_set_style_line_color(ln, steel, 0);
        lv_obj_set_style_line_width(ln, 3, 0);
        lv_obj_set_style_line_rounded(ln, true, 0);
    }
    const struct { int x, y; } node[] = { {60, 58}, {120, 58}, {90, 112} };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *n = lv_obj_create(tile);
        lv_obj_set_size(n, 26, 26);
        lv_obj_set_style_radius(n, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(n, i == 2 ? green : steel, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(n, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(n, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(n, 0, LV_PART_MAIN);
        lv_obj_clear_flag(n, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(n, node[i].x - 13, node[i].y - 13);
    }
}

// Settings — a steel cog: eight teeth around a disc with a dark hub.
// Procedural fallback for /Icons/settings.png.
static void draw_settings_icon(lv_obj_t *tile)
{
    tile = icon_layer(tile);
    lv_color_t steel = lv_color_make(0x9B, 0xBC, 0xD6);
    lv_color_t dark  = lv_color_make(0x12, 0x18, 0x20);
    int cx = 90, cy = 76;

    for (int i = 0; i < 8; i++) {
        float rad = i * 3.14159f / 4.0f;
        int tx = cx + (int)(40.0f * sinf(rad));
        int ty = cy - (int)(40.0f * cosf(rad));
        lv_obj_t *t = lv_obj_create(tile);
        lv_obj_set_size(t, 14, 14);
        lv_obj_set_style_radius(t, 3, LV_PART_MAIN);
        lv_obj_set_style_bg_color(t, steel, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(t, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(t, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(t, 0, LV_PART_MAIN);
        lv_obj_clear_flag(t, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(t, tx - 7, ty - 7);
    }

    lv_obj_t *disc = lv_obj_create(tile);
    lv_obj_set_size(disc, 68, 68);
    lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(disc, steel, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(disc, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(disc, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(disc, 0, LV_PART_MAIN);
    lv_obj_clear_flag(disc, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(disc, cx - 34, cy - 34);

    lv_obj_t *hub = lv_obj_create(tile);
    lv_obj_set_size(hub, 28, 28);
    lv_obj_set_style_radius(hub, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(hub, dark, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(hub, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(hub, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(hub, 0, LV_PART_MAIN);
    lv_obj_clear_flag(hub, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(hub, cx - 14, cy - 14);
}

// Notification bell — gold dome + rim with a clapper, in the same procedural
// style as the other Daily-app icons (Flashlight/Meshtastic/Settings), so no SD
// sprite is needed. tt_recolor_red tints it solid red in Offense like the rest.
static void draw_notify_icon(lv_obj_t *tile)
{
    tile = icon_layer(tile);
    lv_color_t gold = lv_color_make(0xFF, 0xCC, 0x00);
    lv_color_t dark = lv_color_make(0xAA, 0x88, 0x00);
    int cx = 90;

    // Top mount nub. Whole bell is sized + raised to sit clear of the label at the
    // tile bottom (the first pass hung the rim/clapper down over the "Notify" text).
    lv_obj_t *nub = lv_obj_create(tile);
    lv_obj_set_size(nub, 12, 12);
    lv_obj_set_style_radius(nub, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(nub, gold, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(nub, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(nub, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(nub, 0, LV_PART_MAIN);
    lv_obj_clear_flag(nub, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(nub, cx - 6, 26);

    // Bell dome (rounded top).
    lv_obj_t *dome = lv_obj_create(tile);
    lv_obj_set_size(dome, 54, 54);
    lv_obj_set_style_radius(dome, 27, LV_PART_MAIN);
    lv_obj_set_style_bg_color(dome, gold, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dome, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(dome, dark, LV_PART_MAIN);
    lv_obj_set_style_border_width(dome, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(dome, 0, LV_PART_MAIN);
    lv_obj_clear_flag(dome, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(dome, cx - 27, 36);

    // Rim (wider bar at the bell's mouth).
    lv_obj_t *rim = lv_obj_create(tile);
    lv_obj_set_size(rim, 72, 13);
    lv_obj_set_style_radius(rim, 5, LV_PART_MAIN);
    lv_obj_set_style_bg_color(rim, gold, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(rim, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(rim, dark, LV_PART_MAIN);
    lv_obj_set_style_border_width(rim, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(rim, 0, LV_PART_MAIN);
    lv_obj_clear_flag(rim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(rim, cx - 36, 82);

    // Clapper (small circle below the rim).
    lv_obj_t *clap = lv_obj_create(tile);
    lv_obj_set_size(clap, 13, 13);
    lv_obj_set_style_radius(clap, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(clap, gold, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(clap, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(clap, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(clap, 0, LV_PART_MAIN);
    lv_obj_clear_flag(clap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(clap, cx - 6, 94);
}

// ---- Public API ------------------------------------------------------------

void time_screen_create()
{
    time_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(time_screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(time_screen, 0, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(time_screen);
    s_time_title = title;
    lv_obj_set_style_text_color(title, argus_base_accent(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_argus_ui, LV_PART_MAIN);
    lv_label_set_text(title, "TIME");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    // Three-column flex grid — same geometry as the Tools grid so the two
    // screens feel like siblings. Six tiles fit as 3x2; the dir stays vertical
    // so the grid scrolls if more Time tools are added later.
    lv_obj_t *grid = lv_obj_create(time_screen);
    s_time_grid = grid;
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

    // Insertion order (row-major, grid wraps every 2 tiles; grid scrolls):
    //   [Alarm]   [Stopwatch]
    //   [Timer]   [Calendar]
    //   [World]   [Sun/Moon]
    lv_obj_t *t_alarm     = make_tile(grid, "Alarm");
    lv_obj_t *t_stopwatch = make_tile(grid, "Stopwatch");
    lv_obj_t *t_timer     = make_tile(grid, "Timer");
    lv_obj_t *t_calendar  = make_tile(grid, "Calendar");
    lv_obj_t *t_world     = make_tile(grid, "World");
    lv_obj_t *t_sunmoon   = make_tile(grid, "Sun/Moon");

    tile_icon(t_alarm,     "alarm",      draw_alarm_icon);
    tile_icon(t_stopwatch, "stopwatch",  draw_stopwatch_icon);
    tile_icon(t_timer,     "timer",      draw_timer_icon);
    tile_icon(t_calendar,  "calendar",   draw_calendar_icon);
    tile_icon(t_world,     "worldclock", draw_worldclock_icon);
    tile_icon(t_sunmoon,   "sunmoon",    draw_sunmoon_icon);

    lv_obj_add_event_cb(t_alarm,     [](lv_event_t *) { alarm_screen_show();     }, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(t_stopwatch, [](lv_event_t *) { stopwatch_screen_show(); }, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(t_timer,     [](lv_event_t *) { timer_screen_show();     }, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(t_calendar,  [](lv_event_t *) { calendar_screen_show();  }, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(t_world,     [](lv_event_t *) { world_clock_screen_show(); }, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(t_sunmoon,   [](lv_event_t *) { sun_moon_screen_show();  }, LV_EVENT_CLICKED, NULL);

    // Daily-wear utilities: Flashlight (torch), Meshtastic (comms) and Settings.
    // The Time hub is the one app grid reachable in Daily mode (Tools is gated),
    // so these benign apps live here alongside the clock tools.
    lv_obj_t *t_flash  = make_tile(grid, "Flashlight");
    lv_obj_t *t_mesh   = make_tile(grid, "Meshtastic");
    lv_obj_t *t_notify = make_tile(grid, "Notify");
    lv_obj_t *t_set    = make_tile(grid, "Settings");

    tile_icon(t_flash,  "flashlight", draw_flashlight_icon);
    tile_icon(t_mesh,   "meshtastic", draw_meshtastic_icon);
    tile_icon(t_notify, "notify",     draw_notify_icon);
    tile_icon(t_set,    "settings",   draw_settings_icon);

    lv_obj_add_event_cb(t_flash,  [](lv_event_t *) { flashlight_screen_show();    }, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(t_mesh,   [](lv_event_t *) { meshtastic_screen_show();    }, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(t_notify, [](lv_event_t *) { notifications_screen_show(); }, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(t_set,    [](lv_event_t *) { settings_screen_show();      }, LV_EVENT_CLICKED, NULL);

    // lv_obj_create() children are CLICKABLE by default and would otherwise
    // swallow taps on the icon shapes. EVENT_BUBBLE on every descendant sends
    // taps up to the tile's CLICKED handler — same trick the Tools screen uses.
    // Recursive because icon_layer() nests the glyph primitives one level deeper.
    struct BubbleWalk {
        static void apply(lv_obj_t *obj) {
            uint32_t n = lv_obj_get_child_count(obj);
            for (uint32_t k = 0; k < n; k++) {
                lv_obj_t *child = lv_obj_get_child(obj, k);
                lv_obj_add_flag(child, LV_OBJ_FLAG_EVENT_BUBBLE);
                apply(child);
            }
        }
    };
    uint32_t tile_count = lv_obj_get_child_count(grid);
    for (uint32_t i = 0; i < tile_count; i++)
        BubbleWalk::apply(lv_obj_get_child(grid, i));

    s_ttile_recording = false;   // initial tiles recorded; later tile_icon() calls are redraws

    lv_obj_add_event_cb(time_screen, on_gesture, LV_EVENT_GESTURE, NULL);
}

void time_screen_show()
{
    if (s_time_title) lv_obj_set_style_text_color(s_time_title, argus_base_accent(), LV_PART_MAIN);
    // Tile borders follow the mode: red-team red in Offense, steel-blue otherwise.
    if (s_time_grid) {
        lv_color_t bc = (argus_mode_current() == ArgusMode::Offense)
                      ? ARGUS_OFFENSE_ACCENT : ARGUS_ACCENT_DIM;
        uint32_t n = lv_obj_get_child_count(s_time_grid);
        for (uint32_t i = 0; i < n; i++)
            lv_obj_set_style_border_color(lv_obj_get_child(s_time_grid, i), bc, LV_PART_MAIN);
    }

    // Icons follow the mode: red in Offense, natural multi-colour otherwise. Only
    // redraw when the regime actually changes (a normal open is then cheap).
    bool off = (argus_mode_current() == ArgusMode::Offense);
    if (off != s_ttiles_red) {
        for (int i = 0; i < s_ttile_n; i++) tt_redraw(&s_ttiles[i], off);
        s_ttiles_red = off;
    }
    lv_scr_load(time_screen);
}
bool time_screen_is_active()  { return lv_screen_active() == time_screen; }
