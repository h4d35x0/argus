#include "tracker_timeline_screen.h"
#include "threat_radar.h"
#include "theme.h"
#include <string.h>
#include <stdio.h>

// Defined in tools_screen.cpp
void tools_screen_show();

static lv_obj_t *tl_screen;
static lv_obj_t *tl_banner;
static lv_obj_t *tl_list;
static lv_timer_t *tl_timer;

#define TL_MAX 24

static lv_color_t level_color(uint8_t level)
{
    switch (level) {
    case TR_LVL_CONFIRMED: return HADES_RED;
    case TR_LVL_LIKELY:    return lv_color_make(0xF0, 0x8A, 0x30);   // orange
    case TR_LVL_POSSIBLE:  return lv_color_make(0xF0, 0xC0, 0x40);   // amber
    default:               return ARGUS_ACCENT_DIM;
    }
}

// Oldest-first by "HH:MM" first-seen (same-day correct; a wrap past midnight just
// orders the small hours first, acceptable for a day timeline).
static void sort_by_first_time(TrThreat *a, int n)
{
    for (int i = 1; i < n; i++) {
        TrThreat key = a[i];
        int j = i - 1;
        while (j >= 0 && strncmp(a[j].first_time, key.first_time, sizeof(key.first_time)) > 0) {
            a[j + 1] = a[j]; j--;
        }
        a[j + 1] = key;
    }
}

static void add_node(const TrThreat *t)
{
    lv_color_t col = level_color(t->level);

    lv_obj_t *row = lv_obj_create(tl_list);
    lv_obj_set_size(row, 388, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(row, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(row, lv_color_make(0x12, 0x16, 0x1C), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(row, col, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 8, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    char head[64];
    snprintf(head, sizeof(head), "%s  %s  -  %s",
             t->first_time,
             threatradar_category_name(t->category),
             threatradar_level_name(t->level));
    lv_obj_t *h = lv_label_create(row);
    lv_obj_set_style_text_font(h, &font_argus_label_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(h, col, LV_PART_MAIN);
    lv_label_set_text(h, head);
    lv_obj_align(h, LV_ALIGN_TOP_LEFT, 0, 0);

    char body[96];
    snprintf(body, sizeof(body),
             "%u m  -  rode %u min  -  %u pts  -  %d dBm%s%s",
             (unsigned)t->span_m, (unsigned)t->span_min, (unsigned)t->waypoints,
             (int)t->best_rssi,
             t->active ? "  -  near" : "",
             t->familiar ? "  (familiar)" : "");
    lv_obj_t *b = lv_label_create(row);
    lv_obj_set_style_text_font(b, &font_argus_label_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(b, ARGUS_TEXT_DIM, LV_PART_MAIN);
    lv_label_set_text(b, body);
    lv_obj_align(b, LV_ALIGN_TOP_LEFT, 0, 22);
}

static void rebuild()
{
    if (!tl_list) return;
    lv_obj_clean(tl_list);

    static TrThreat buf[TL_MAX];
    int n = threatradar_get_threats(buf, TL_MAX);
    sort_by_first_time(buf, n);

    // Banner: has any recent CONFIRMED/LIKELY tail?
    int top = threatradar_top_level();
    if (top >= TR_LVL_LIKELY) {
        lv_obj_set_style_bg_color(tl_banner, HADES_RED, LV_PART_MAIN);
        lv_label_set_text(lv_obj_get_child(tl_banner, 0), LV_SYMBOL_WARNING "  A TAIL HAS BEEN WITH YOU");
    } else {
        lv_obj_set_style_bg_color(tl_banner, ARGUS_ACCENT, LV_PART_MAIN);
        lv_label_set_text(lv_obj_get_child(tl_banner, 0), LV_SYMBOL_OK "  No sustained tail");
    }

    if (n == 0) {
        lv_obj_t *l = lv_label_create(tl_list);
        lv_obj_set_style_text_font(l, &font_argus_label_16, LV_PART_MAIN);
        lv_obj_set_style_text_color(l, ARGUS_TEXT_DIM, LV_PART_MAIN);
        lv_obj_set_width(l, 360);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_label_set_text(l, "No tracked contacts yet. Devices that\n"
                             "co-move with you as you travel appear\n"
                             "here in the order they first latched on.");
        return;
    }
    for (int i = 0; i < n; i++) add_node(&buf[i]);
}

static void on_refresh(lv_timer_t *)
{
    if (tracker_timeline_screen_is_active()) rebuild();
}

static void on_clear(lv_event_t *) { threatradar_reset(); rebuild(); }

static void on_gesture(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    if (lv_indev_get_gesture_dir(indev) == LV_DIR_RIGHT) tools_screen_show();
}

void tracker_timeline_screen_create()
{
    tl_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(tl_screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(tl_screen, 0, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(tl_screen);
    lv_obj_set_style_text_color(title, ARGUS_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_argus_label_28, LV_PART_MAIN);
    lv_label_set_text(title, "TAIL TIMELINE");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 14);

    tl_banner = lv_obj_create(tl_screen);
    lv_obj_set_size(tl_banner, 388, 40);
    lv_obj_align(tl_banner, LV_ALIGN_TOP_MID, 0, 56);
    lv_obj_set_style_radius(tl_banner, 10, LV_PART_MAIN);
    lv_obj_set_style_bg_color(tl_banner, ARGUS_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_border_width(tl_banner, 0, LV_PART_MAIN);
    lv_obj_clear_flag(tl_banner, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *bl = lv_label_create(tl_banner);
    lv_obj_set_style_text_color(bl, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_text_font(bl, &font_argus_label_16, LV_PART_MAIN);
    lv_label_set_text(bl, LV_SYMBOL_OK "  No sustained tail");
    lv_obj_center(bl);

    tl_list = lv_obj_create(tl_screen);
    lv_obj_set_size(tl_list, 404, 340);
    lv_obj_align(tl_list, LV_ALIGN_TOP_MID, 0, 104);
    lv_obj_set_style_bg_color(tl_list, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(tl_list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(tl_list, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_row(tl_list, 6, LV_PART_MAIN);
    lv_obj_set_layout(tl_list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(tl_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(tl_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(tl_list, LV_SCROLLBAR_MODE_AUTO);

    lv_obj_t *clear_btn = lv_button_create(tl_screen);
    lv_obj_set_size(clear_btn, 130, 44);
    lv_obj_align(clear_btn, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_style_radius(clear_btn, 10, LV_PART_MAIN);
    lv_obj_set_style_bg_color(clear_btn, lv_color_make(0x3A, 0x3A, 0x3C), LV_PART_MAIN);
    lv_obj_add_event_cb(clear_btn, on_clear, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(clear_btn);
    lv_label_set_text(cl, "CLEAR");
    lv_obj_set_style_text_color(cl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(cl, &font_argus_label_16, LV_PART_MAIN);
    lv_obj_center(cl);

    lv_obj_add_event_cb(tl_screen, on_gesture, LV_EVENT_GESTURE, NULL);

    tl_timer = lv_timer_create(on_refresh, 1500, NULL);
    lv_timer_pause(tl_timer);
}

void tracker_timeline_screen_show()
{
    rebuild();
    lv_timer_resume(tl_timer);
    lv_scr_load(tl_screen);
}

bool tracker_timeline_screen_is_active() { return lv_screen_active() == tl_screen; }
