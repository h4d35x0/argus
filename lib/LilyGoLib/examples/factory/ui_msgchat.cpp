/**
 * @file      ui_msgchat.cpp
 * @author    Lewis He (lewishe@outlook.com)
 * @license   MIT
 * @copyright Copyright (c) 2025  ShenZhen XinYuan Electronic Technology Co., Ltd
 * @date      2025-01-05
 *
 */
#include "ui_define.h"

#ifdef USING_TOUCHPAD
static lv_obj_t *keyboard = NULL;
#endif

static lv_timer_t *timer = NULL;
static char recv_buf[512];
static radio_rx_params_t rx_params;
static lv_obj_t *menu = NULL;
static lv_obj_t *quit_btn = NULL;
#define MAX_MSG_COUNT 20
static lv_obj_t *msg_page;
static lv_obj_t *msg_cont;
static lv_obj_t *input_textarea;
static int msg_count = 0;

extern radio_params_t radio_params_copy;

#if defined(ARDUINO_T_WATCH_S3)
static lv_align_t quit_btn_align = LV_ALIGN_TOP_LEFT;
static int16_t quit_btn_x_ofs = 0;
static int16_t quit_btn_y_ofs = 0;
#elif defined(ARDUINO_T_WATCH_S3_ULTRA)
static lv_align_t quit_btn_align = LV_ALIGN_TOP_MID;
static int16_t quit_btn_x_ofs = 0;
static int16_t quit_btn_y_ofs = 20;
#endif

static void _msg_ta_cb(lv_event_t *e);

static char *get_formatted_time(void)
{
    static char time_str[20];
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    sprintf(time_str, "%02d:%02d:%02d",
            t->tm_hour,
            t->tm_min,
            t->tm_sec);
    return time_str;
}

static void back_event_handler(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    if (lv_menu_back_btn_is_root(menu, obj)) {
#ifdef USING_TOUCHPAD
        if (keyboard) {
            lv_obj_del(keyboard);
            keyboard = NULL;
        }
#endif
        if (timer) {
            lv_timer_del(timer); timer = NULL;
        }
        hw_set_keyboard_read_callback(NULL);
        hw_set_radio_default();
        lv_obj_clean(menu);
        lv_obj_del(menu);

        disable_keyboard();

        if (quit_btn) {
            lv_obj_del_async(quit_btn);
            quit_btn = NULL;
        }
        menu_show();
    }

    else {
        hw_feedback();
        radio_params_copy.mode = RADIO_RX;
        hw_set_radio_params(radio_params_copy);
#ifdef USING_TOUCHPAD
        lv_obj_align(quit_btn, quit_btn_align, quit_btn_x_ofs, quit_btn_y_ofs);
#endif
    }
}

static void create_msg_label(const char *text, bool is_send, lv_color_t bg_color)
{
    lv_obj_t *msg_row = lv_obj_create(msg_cont);
    lv_obj_set_size(msg_row, lv_obj_get_width(msg_cont), LV_SIZE_CONTENT);
    lv_obj_set_layout(msg_row, LV_LAYOUT_FLEX);
    if (is_send) {
        lv_obj_set_flex_align(msg_row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    } else {
        lv_obj_set_flex_align(msg_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    }
    lv_obj_set_style_border_width(msg_row, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(msg_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(msg_row, 5, LV_PART_MAIN);

    lv_obj_t *label = lv_label_create(msg_row);
    lv_label_set_text(label, text);

    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    if (is_send) {
        lv_obj_set_style_bg_color(label, lv_color_hex(0x99ccff), LV_PART_MAIN);
    } else {
        lv_obj_set_style_bg_color(label, lv_color_hex(0xe6e6e6), LV_PART_MAIN);
    }
    lv_obj_set_style_bg_opa(label, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_radius(label, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_all(label, 8, LV_PART_MAIN);

    lv_obj_set_style_max_width(label, lv_obj_get_width(msg_cont) * 7 / 10, LV_PART_MAIN);
    lv_obj_set_width(label, LV_SIZE_CONTENT);
}

static void send_btn_cb(lv_event_t *e)
{
    static  char buf[512];
    const char *text = lv_textarea_get_text(input_textarea);
    if (text[0] == '\0') return;

    if (msg_count >= MAX_MSG_COUNT) {
        lv_obj_t *first_child = lv_obj_get_child(msg_cont, 0);
        if (first_child) {
            lv_obj_del(first_child);
            msg_count--;
        }
    }

    lv_snprintf(buf, sizeof(buf), "%s:%s", get_formatted_time(), text);

    create_msg_label(buf, true, lv_color_hex(0x99ccff));
    msg_count++;

    radio_transmit((const uint8_t *)text, strlen(text));

    lv_textarea_set_text(input_textarea, "");

    lv_obj_scroll_to_y(msg_page, lv_obj_get_y(msg_cont) + lv_obj_get_height(msg_cont), LV_ANIM_ON);

    hw_set_radio_listening();
}

void recv_msg(const char *text)
{
    static  char buf[512];
    if (text[0] == '\0') return;
    if (msg_count >= MAX_MSG_COUNT) {
        lv_obj_t *first_child = lv_obj_get_child(msg_cont, 0);
        if (first_child) {
            lv_obj_del(first_child);
            msg_count--;
        }
    }
    lv_snprintf(buf, sizeof(buf), "%s:%s", get_formatted_time(), text);
    create_msg_label(buf, false, lv_color_hex(0xe6e6e6));
    msg_count++;
    lv_obj_scroll_to_y(msg_page, lv_obj_get_y(msg_cont) + lv_obj_get_height(msg_cont), LV_ANIM_ON);
}


static void _msg_ta_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *ta = (lv_obj_t *)lv_event_get_target(e);
    bool state =  lv_obj_has_state(ta, LV_STATE_FOCUSED);
    bool edited =  lv_obj_has_state(ta, LV_STATE_EDITED);

    lv_indev_t *indev =   lv_indev_active();
    if (indev == NULL) {
        return;
    }

    if (indev->type == LV_INDEV_TYPE_ENCODER) {
    } else if (indev->type == LV_INDEV_TYPE_POINTER) {
        if (code == LV_EVENT_VALUE_CHANGED) {
            hw_feedback();
        }
    } else if (indev->type == LV_INDEV_TYPE_KEYPAD) {
        return ;
    }

    if (code == LV_EVENT_KEY) {
        lv_key_t key = *(lv_key_t *)lv_event_get_param(e);
        if (key == LV_KEY_ENTER) {
            lv_textarea_delete_char(input_textarea);
            lv_event_stop_processing(e);
        }
    }

#ifdef USING_TOUCHPAD
    if (code == LV_EVENT_READY || code == LV_EVENT_DEFOCUSED) {
        lv_keyboard_set_textarea(keyboard, NULL);
        lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    }
#endif

    if (code == LV_EVENT_CLICKED && indev->type == LV_INDEV_TYPE_POINTER) {
        lv_group_set_editing((lv_group_t *)lv_obj_get_group(ta), true);
#ifdef USING_TOUCHPAD
        lv_keyboard_set_textarea(keyboard, ta);
        lv_obj_remove_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(quit_btn, quit_btn_align, quit_btn_x_ofs, quit_btn_y_ofs);
#endif
    } else  if (code == LV_EVENT_CLICKED && indev->type == LV_INDEV_TYPE_ENCODER) {
        if (edited) {
            lv_group_set_editing((lv_group_t *)lv_obj_get_group(ta), false);
            disable_keyboard();
        }
    } else if (code == LV_EVENT_FOCUSED) {
        if (edited) {
            enable_keyboard();
        }
    }
}


void create_chat_ui(lv_obj_t *parent)
{
    lv_obj_t *main_cont = lv_obj_create(parent);
    lv_obj_set_size(main_cont, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(main_cont, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_border_width(main_cont, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(main_cont, 0, LV_PART_MAIN);

    msg_page = lv_obj_create(main_cont);
    lv_obj_set_size(msg_page, lv_pct(95), lv_pct(68));
    lv_obj_align(msg_page, LV_ALIGN_TOP_MID, 0, 10);
    lv_obj_set_style_border_width(msg_page, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(msg_page, 5, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(msg_page, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_pad_all(msg_page, 0, LV_PART_MAIN);

    msg_cont = lv_obj_create(msg_page);
    lv_obj_set_size(msg_cont, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(msg_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(msg_cont, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(msg_cont, 0, LV_PART_MAIN);

    lv_obj_set_scroll_dir(msg_cont, LV_DIR_NONE);
    lv_obj_set_scrollbar_mode(msg_cont, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *input_cont = lv_obj_create(main_cont);
    lv_obj_set_style_pad_all(input_cont, 0, LV_PART_MAIN);
    lv_obj_set_size(input_cont, lv_pct(95), lv_pct(20));
    lv_obj_align(input_cont, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_opa(input_cont, LV_OPA_0, LV_PART_MAIN);
    lv_obj_set_style_border_width(input_cont, 0, LV_PART_MAIN);
    lv_obj_set_layout(input_cont, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(input_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(input_cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_SPACE_EVENLY);
    lv_obj_set_style_radius(input_cont, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(input_cont, 0, LV_PART_MAIN);

    input_textarea = lv_textarea_create(input_cont);
    lv_obj_set_size(input_textarea, lv_pct(70), lv_pct(100));
    lv_textarea_set_placeholder_text(input_textarea, "Please enter your message...");
    lv_obj_set_style_radius(input_textarea, 5, LV_PART_MAIN);
    lv_obj_set_style_border_width(input_textarea, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(input_textarea, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(input_textarea, lv_color_white(), LV_PART_MAIN);
    lv_textarea_set_max_length(input_textarea, 500);
    lv_obj_set_scroll_dir(input_textarea, LV_DIR_NONE);
    lv_obj_set_scrollbar_mode(input_textarea, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(input_textarea, _msg_ta_cb, LV_EVENT_ALL, NULL);

    lv_obj_t *send_btn = lv_btn_create(input_cont);
    lv_obj_set_size(send_btn, lv_pct(10), lv_pct(100));
    lv_obj_set_style_radius(send_btn, 5, LV_PART_MAIN);
    lv_obj_set_style_bg_color(send_btn, lv_color_white(), LV_PART_MAIN);
    lv_obj_add_event_cb(send_btn, send_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_style_pad_all(send_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(send_btn, 1, LV_PART_MAIN);
    lv_obj_t *send_label = lv_label_create(send_btn);
    lv_label_set_text(send_label, LV_SYMBOL_GPS);
    lv_obj_center(send_label);
    lv_obj_set_style_text_color(send_label, lv_color_black(), LV_PART_MAIN);


    lv_obj_t *setting_btn = lv_btn_create(input_cont);
    lv_obj_set_size(setting_btn, lv_pct(10), lv_pct(100));
    lv_obj_set_style_radius(setting_btn, 5, LV_PART_MAIN);
    lv_obj_set_style_bg_color(setting_btn, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_pad_all(setting_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(setting_btn, 1, LV_PART_MAIN);
    lv_obj_t *setting_label = lv_label_create(setting_btn);
    lv_label_set_text(setting_label, LV_SYMBOL_SETTINGS);
    lv_obj_center(setting_label);
    lv_obj_set_style_text_color(setting_label, lv_color_black(), LV_PART_MAIN);

    lv_obj_t *section;
    lv_obj_t *sub_rf_setting_page = lv_menu_page_create(menu, NULL);
    lv_obj_set_style_pad_hor(sub_rf_setting_page, lv_obj_get_style_pad_left(lv_menu_get_main_header(menu), LV_PART_MAIN), 0);
    section = lv_menu_section_create(sub_rf_setting_page);
    lv_menu_set_load_page_event(menu, setting_btn, sub_rf_setting_page);


    extern lv_obj_t *create_cr_dropdown(lv_obj_t *parent);
    extern lv_obj_t *create_sf_dropdown(lv_obj_t *parent);
    extern lv_obj_t *create_tx_power_dropdown(lv_obj_t *parent);
    extern lv_obj_t *create_bandwidth_dropdown(lv_obj_t *parent);
    extern lv_obj_t *create_frequency_dropdown(lv_obj_t *parent);
    extern lv_obj_t *create_syncword_textarea(lv_obj_t *parent);

    ui_create_option(section, "Frequency:", NULL, create_frequency_dropdown, NULL);
    ui_create_option(section, "Bandwidth:", NULL, create_bandwidth_dropdown, NULL);
    ui_create_option(section, "TX Power:", NULL, create_tx_power_dropdown, NULL);
    ui_create_option(section, "Coding rate:", NULL, create_cr_dropdown, NULL);
    ui_create_option(section, "Spreading factor:", NULL, create_sf_dropdown, NULL);
    ui_create_option(section, "SyncWord:", NULL, create_syncword_textarea, NULL);
}

static void msg_chat_receiver_task(lv_timer_t *t)
{
    rx_params.data = (uint8_t *)recv_buf;
    rx_params.length = sizeof(recv_buf);
    hw_get_radio_rx(rx_params);
    if (rx_params.state == 0 && rx_params.length != 0) {
        recv_buf[rx_params.length + 1] = '\0';
        recv_msg(recv_buf);
        hw_feedback();
        hw_set_volume(70);
        hw_set_sd_music_play(AUDIO_SOURCE_FATFS, "/notification.mp3");
    }
}

void ui_msgchat_enter(lv_obj_t *parent)
{
    hw_get_radio_params(radio_params_copy);
    radio_params_copy.mode = RADIO_RX;
    hw_set_radio_params(radio_params_copy);

    menu = create_menu(parent, back_event_handler);

    lv_obj_t *sub_mechanics_page = lv_menu_page_create(menu, NULL);

    create_chat_ui(sub_mechanics_page);

    lv_menu_set_page(menu, sub_mechanics_page);

#ifdef USING_TOUCHPAD
    keyboard = lv_keyboard_create(lv_scr_act());
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event(keyboard, [](lv_event_t * e) {
        lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(quit_btn, quit_btn_align, quit_btn_x_ofs, quit_btn_y_ofs);
    }, LV_EVENT_READY, NULL);

    quit_btn  = create_floating_button([](lv_event_t*e) {
        lv_obj_send_event(lv_menu_get_main_header_back_button(menu), LV_EVENT_CLICKED, NULL);
    }, NULL);
    lv_obj_align(quit_btn, quit_btn_align, quit_btn_x_ofs, quit_btn_y_ofs);
#endif

    timer = lv_timer_create(msg_chat_receiver_task, 300, NULL);
}


void ui_msgchat_exit(lv_obj_t *parent)
{

}

app_t ui_msgchat_main = {
    .setup_func_cb = ui_msgchat_enter,
    .exit_func_cb = ui_msgchat_exit,
    .user_data = nullptr,
};

