#include "spycam_screen.h"
#include "spycam.h"
#include "theme.h"
#include <string.h>
#include <stdio.h>

void tools_screen_show();   // tools_screen.cpp

static lv_obj_t *spycam_screen;
static lv_obj_t *spycam_list;

static void rebuild()
{
    if (!spycam_list) return;
    lv_obj_clean(spycam_list);

    SpycamHit hits[16];
    int n = spycam_get(hits, 16);

    if (n == 0) {
        lv_obj_t *l = lv_label_create(spycam_list);
        lv_obj_set_style_text_font(l, &font_argus_label_16, LV_PART_MAIN);
        lv_obj_set_style_text_color(l, ARGUS_TEXT_DIM, LV_PART_MAIN);
        lv_obj_set_width(l, 360);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_label_set_text(l, "No cameras detected.\nRuns passively while WiFi is on - open a WiFi tool to scan.");
        return;
    }

    for (int i = 0; i < n; i++) {
        lv_obj_t *row = lv_obj_create(spycam_list);
        lv_obj_set_size(row, 380, 54);
        lv_obj_set_style_radius(row, 8, LV_PART_MAIN);
        lv_obj_set_style_bg_color(row, lv_color_make(0x12, 0x18, 0x20), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(row, 6, LV_PART_MAIN);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        // Confidence drives the class colour: High = threat red, Med = amber.
        const char *cn = spycam_conf_name(hits[i].conf);
        lv_color_t cc = ARGUS_ACCENT;
        if      (!strcmp(cn, "HIGH")) cc = HADES_RED;
        else if (!strcmp(cn, "MED"))  cc = lv_color_make(0xF0, 0xB4, 0x30);

        lv_obj_t *cls = lv_label_create(row);
        lv_obj_set_style_text_font(cls, &font_argus_label_16, LV_PART_MAIN);
        lv_obj_set_style_text_color(cls, cc, LV_PART_MAIN);
        lv_label_set_text(cls, spycam_class_name(hits[i].cls));
        lv_obj_align(cls, LV_ALIGN_TOP_LEFT, 4, 2);

        char sub[80];
        snprintf(sub, sizeof(sub), "%s   %s   %ddBm",
                 hits[i].ssid[0] ? hits[i].ssid : "(hidden)", cn, hits[i].rssi);
        lv_obj_t *s = lv_label_create(row);
        lv_obj_set_style_text_font(s, &font_argus_label_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(s, ARGUS_TEXT_DIM, LV_PART_MAIN);
        lv_label_set_text(s, sub);
        lv_obj_align(s, LV_ALIGN_BOTTOM_LEFT, 4, -2);
    }
}

static void on_tick(lv_timer_t *)
{
    if (lv_screen_active() == spycam_screen) rebuild();
}

static void on_gesture(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    if (lv_indev_get_gesture_dir(indev) == LV_DIR_RIGHT) tools_screen_show();
}

void spycam_screen_create()
{
    spycam_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(spycam_screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(spycam_screen, 0, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(spycam_screen);
    lv_obj_set_style_text_color(title, argus_base_accent(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_argus_ui, LV_PART_MAIN);
    lv_label_set_text(title, "SPYCAM");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    spycam_list = lv_obj_create(spycam_screen);
    lv_obj_set_size(spycam_list, 400, 420);
    lv_obj_align(spycam_list, LV_ALIGN_TOP_MID, 0, 52);
    lv_obj_set_style_bg_color(spycam_list, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(spycam_list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(spycam_list, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_row(spycam_list, 6, LV_PART_MAIN);
    lv_obj_set_layout(spycam_list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(spycam_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(spycam_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(spycam_list, LV_SCROLLBAR_MODE_AUTO);

    rebuild();
    lv_timer_create(on_tick, 2000, NULL);
    lv_obj_add_event_cb(spycam_screen, on_gesture, LV_EVENT_GESTURE, NULL);
}

void spycam_screen_show()      { rebuild(); lv_scr_load(spycam_screen); }
bool spycam_screen_is_active() { return lv_screen_active() == spycam_screen; }
