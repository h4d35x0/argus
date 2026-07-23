#include "flashlight_screen.h"
#include <lvgl.h>

// Defined in time_screen.cpp / main.cpp
void time_screen_show();
// Public dim-system hook (main.cpp): resets the idle timer and restores active
// brightness. Called on show + on a keep-awake tick so the torch never dims.
void ui_reset_dim_activity();

static lv_obj_t   *fl_screen = nullptr;
static lv_timer_t *fl_awake  = nullptr;

// Keep the display at active brightness while the torch is up.
static void fl_keep_awake(lv_timer_t *) { ui_reset_dim_activity(); }

// Tap anywhere exits back to the Time hub.
static void fl_on_click(lv_event_t *)
{
    if (fl_awake) lv_timer_pause(fl_awake);
    time_screen_show();
}

void flashlight_screen_create()
{
    fl_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(fl_screen, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(fl_screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(fl_screen, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(fl_screen, 0, LV_PART_MAIN);
    lv_obj_clear_flag(fl_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(fl_screen, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(fl_screen, fl_on_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t *hint = lv_label_create(fl_screen);
    lv_label_set_text(hint, "tap to exit");
    lv_obj_set_style_text_color(hint, lv_color_make(0x66, 0x66, 0x66), LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -14);

    fl_awake = lv_timer_create(fl_keep_awake, 2000, NULL);
    lv_timer_pause(fl_awake);
}

void flashlight_screen_show()
{
    ui_reset_dim_activity();
    if (fl_awake) lv_timer_resume(fl_awake);
    lv_scr_load(fl_screen);
}

bool flashlight_screen_is_active() { return lv_screen_active() == fl_screen; }
