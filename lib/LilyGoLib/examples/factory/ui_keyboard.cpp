/**
 * @file      ui_keyboard.cpp
 * @author    Lewis He (lewishe@outlook.com)
 * @license   MIT
 * @copyright Copyright (c) 2025  ShenZhen XinYuan Electronic Technology Co., Ltd
 * @date      2025-01-05
 *
 */
#include "ui_define.h"

typedef struct {
    lv_obj_t *lat;
    lv_obj_t *lng;
    lv_obj_t *datetime;
    lv_obj_t *speed;
    lv_obj_t *rx_size;
    lv_obj_t *satellite;
} gps_label_t;

static gps_label_t label_gps;
static lv_obj_t *menu = NULL;
static lv_timer_t *timer = NULL;
static lv_obj_t *quit_btn = NULL;

static void back_event_handler(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    if (lv_menu_back_btn_is_root(menu, obj)) {
        if (timer) {
            lv_timer_del(timer); timer = NULL;
        }
        lv_obj_clean(menu);
        lv_obj_del(menu);

        disable_keyboard();

        if (quit_btn) {
            lv_obj_del_async(quit_btn);
            quit_btn = NULL;
        }

        menu_show();
    }
}

static void edit_textarea_event_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_get_act();
    if (indev == NULL) {
        return;
    }
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *ta = (lv_obj_t *)lv_event_get_target(e);
    bool state =  lv_obj_has_state(ta, LV_STATE_FOCUSED);
    bool edited =  lv_obj_has_state(ta, LV_STATE_EDITED);
    if (lv_indev_get_type(indev) == LV_INDEV_TYPE_ENCODER) {
        if (code == LV_EVENT_CLICKED) {
            if (edited) {
                lv_group_set_editing((lv_group_t *)lv_obj_get_group(ta), false);
                printf("disable keyboard\n");
                disable_keyboard();
            }
        } else if (code == LV_EVENT_FOCUSED) {
            if (edited) {
                printf("enable input keyboard \n");
                enable_keyboard();
            }
        }
    }
}

void ui_keyboard_enter(lv_obj_t *parent)
{
    menu = create_menu(parent, back_event_handler);

    lv_obj_t *main_page = lv_menu_page_create(menu, NULL);

    lv_obj_t *edit_textarea = lv_textarea_create(main_page);
    lv_obj_set_size(edit_textarea, lv_pct(80), lv_pct(50));
    lv_obj_add_event_cb(edit_textarea, edit_textarea_event_cb, LV_EVENT_ALL, NULL);

    lv_obj_t *label = lv_label_create(main_page);
#if DEVICE_KEYBOARD_TYPE == KEYBOARD_TYPE_1
    lv_label_set_text(label, "<Orange button on the left> + B = On/Off Backlight\n"
                             "<Middle orange button> + Key = Symbol Mode\n"
                             "CAP + Key = Capitalize");
#elif DEVICE_KEYBOARD_TYPE == KEYBOARD_TYPE_2
    lv_label_set_text(label, "Alt + B = On/Off Backlight\n"
                             "Sym + Key = Symbol Mode\n"
                             "ArrowUp + Key = Capitalize");
#else
    lv_label_set_text(label, "Keyboard instructions not available for this device.");
#endif
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);

    lv_obj_set_style_text_color(label, lv_color_black(), 0);
    lv_obj_set_width(label, lv_pct(90));
    lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);

    lv_menu_set_page(menu, main_page);

#ifdef USING_TOUCHPAD
    quit_btn  = create_floating_button([](lv_event_t*e) {
        lv_obj_send_event(lv_menu_get_main_header_back_button(menu), LV_EVENT_CLICKED, NULL);
    }, NULL);
#endif

#if defined(USING_INPUT_DEV_KEYBOARD) && !defined(USING_INPUT_DEV_ROTARY)
    enable_keyboard();
#endif
}


void ui_keyboard_exit(lv_obj_t *parent)
{

}

app_t ui_keyboard_main = {
    .setup_func_cb = ui_keyboard_enter,
    .exit_func_cb = ui_keyboard_exit,
    .user_data = nullptr,
};


