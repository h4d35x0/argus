/**
 * @file      ui_ir_remote.cpp
 * @author    Lewis He (lewishe@outlook.com)
 * @license   MIT
 * @copyright Copyright (c) 2025  ShenZhen XinYuan Electronic Technology Co., Ltd
 * @date      2025-05-15
 *
 */
#include "ui_define.h"

#if defined(USING_IR_REMOTE)
static lv_obj_t *menu = NULL;
static uint32_t nec_code = 0x12345678;  // LilyGo Factory ir remote test nec code
static lv_obj_t *keyboard = NULL;
static lv_obj_t *input_textarea = NULL;


static void back_event_handler(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    if (lv_menu_back_button_is_root(menu, obj)) {
        lv_obj_clean(menu);
        lv_obj_del(menu);
        menu_show();
    }
}

static void send_event_handler(lv_event_t *e)
{
    hw_feedback();
    hw_set_remote_code(nec_code);
}

static void _msg_ta_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *ta = (lv_obj_t *)lv_event_get_target(e);
    bool edited =  lv_obj_has_state(ta, LV_STATE_EDITED);
    if (code == LV_EVENT_READY || code == LV_EVENT_DEFOCUSED) {
        lv_keyboard_set_textarea(keyboard, NULL);
        lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
        const char *txt = lv_textarea_get_text(input_textarea);
        if (txt[0] == '0' && (txt[1] == 'x' || txt[1] == 'X')) {
            nec_code = (uint32_t)strtoul(&txt[2], NULL, 16);
        } else {
            nec_code = (uint32_t)strtoul(&txt[0], NULL, 16);
        }
        printf("1. Input NEC Code: 0x%x\n", nec_code);

    } else if (code == LV_EVENT_CLICKED) {
        if (edited) {
            lv_group_set_editing((lv_group_t *)lv_obj_get_group(ta), false);
            disable_keyboard();
        } else {
            lv_keyboard_set_textarea(keyboard, ta);
            lv_obj_remove_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
        }
    } else if (code == LV_EVENT_FOCUSED) {
        if (edited) {
            enable_keyboard();
        }
    }
}

void ui_ir_remote_enter(lv_obj_t *parent)
{
    menu = create_menu(parent, back_event_handler);
    lv_obj_t *cont = lv_obj_create(menu);
    lv_obj_set_size(cont, lv_pct(100), lv_pct(100));

    lv_obj_t *label = lv_label_create(cont);
    lv_label_set_text(label, "NEC Code(Hex Format):");
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 10, 30);

    input_textarea = lv_textarea_create(cont);
    lv_obj_set_width(input_textarea, lv_pct(95));
    lv_textarea_set_text_selection(input_textarea, false);
    lv_textarea_set_cursor_click_pos(input_textarea, false);
    lv_textarea_set_one_line(input_textarea, true);
    lv_textarea_set_accepted_chars(input_textarea, "0123456789ABCDEFabcdef");
    lv_textarea_set_max_length(input_textarea, 8);
    lv_textarea_set_placeholder_text(input_textarea, "0x12345678");
    lv_obj_set_scrollbar_mode(input_textarea, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align_to(input_textarea, label, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 20);
    lv_obj_add_event_cb(input_textarea, _msg_ta_cb, LV_EVENT_ALL, NULL);

    keyboard = lv_keyboard_create(lv_screen_active());
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);

    int w =  lv_display_get_horizontal_resolution(NULL) / 5;
    lv_obj_t *quit_btn = create_radius_button(cont, LV_SYMBOL_LEFT, [](lv_event_t*e) {
        lv_obj_send_event(lv_menu_get_main_header_back_button(menu), LV_EVENT_CLICKED, NULL);
    }, NULL);
    lv_obj_remove_flag(quit_btn, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(quit_btn, LV_ALIGN_BOTTOM_MID, -w, -20);

    lv_obj_t *ok_btn = create_radius_button(cont, LV_SYMBOL_OK, send_event_handler,  NULL);
    lv_obj_remove_flag(ok_btn, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(ok_btn, LV_ALIGN_BOTTOM_MID, w, -20);
}

void ui_ir_remote_exit(lv_obj_t *parent)
{
}

app_t ui_ir_remote_main = {
    .setup_func_cb = ui_ir_remote_enter,
    .exit_func_cb = ui_ir_remote_exit,
    .user_data = nullptr,
};

#endif

