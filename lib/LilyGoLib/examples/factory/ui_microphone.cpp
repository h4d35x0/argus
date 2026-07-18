/**
 * @file      ui_microphone.cpp
 * @author    Lewis He (lewishe@outlook.com)
 * @license   MIT
 * @copyright Copyright (c) 2025  ShenZhen XinYuan Electronic Technology Co., Ltd
 * @date      2025-01-05
 *
 */
#include "ui_define.h"

#define BAR_WIDTH 8
#define BAR_HEIGHT 75
#define BAR_SPACING 2
#define CHANNEL_Y_OFFSET 90

#define HUE_START 180
#define HUE_END 360
#define HUE_START_R 0
#define HUE_END_R 180
#define SATURATION 100
#define VALUE 100

static lv_timer_t *timer = NULL;
static lv_obj_t *menu = NULL;
static lv_obj_t *quit_btn = NULL;
static lv_obj_t *left_freq_bars[FREQ_BANDS];
static lv_obj_t *right_freq_bars[FREQ_BANDS];
static lv_obj_t *left_title;
static lv_obj_t *right_title;
static lv_obj_t *status_label;

static void back_event_handler(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    if (lv_menu_back_btn_is_root(menu, obj)) {
        if (timer) {
            lv_timer_del(timer);
            timer = NULL;
        }
        hw_set_mic_stop();
        lv_obj_clean(menu);
        lv_obj_del(menu);

        if (quit_btn) {
            lv_obj_del_async(quit_btn);
            quit_btn = NULL;
        }

        menu_show();
    }
}

static lv_color_t get_gradient_color(int index, int total, int start_hue, int end_hue)
{
    float ratio = (float)index / (total - 1);
    int hue = start_hue + (int)((end_hue - start_hue) * ratio);
    if (hue > 360) hue = 360;
    if (hue < 0) hue = 0;
    return lv_color_hsv_to_rgb(hue, SATURATION, VALUE);
}


static void update_fft_display(lv_timer_t *timer)
{
    FFTData  fft_data;

    hw_audio_get_fft_data(&fft_data);

    static bool first_update = true;
    if (first_update) {
        for (int i = 0; i < FREQ_BANDS; i++) {
            lv_obj_set_height(left_freq_bars[i], 5);
            lv_obj_set_height(right_freq_bars[i], 5);
        }
        first_update = false;
    }

    for (int i = 0; i < FREQ_BANDS; i++) {
        float value = constrain(fft_data.left_bands[i], 0.0f, 1.0f);
        uint16_t bar_height = (uint16_t)(BAR_HEIGHT * value);
        if (bar_height < 2) bar_height = 2;
        lv_obj_set_height(left_freq_bars[i], bar_height);
    }

    for (int i = 0; i < FREQ_BANDS; i++) {
        float value = constrain(fft_data.right_bands[i], 0.0f, 1.0f);
        uint16_t bar_height = (uint16_t)(BAR_HEIGHT * value);
        if (bar_height < 2) bar_height = 2;
        lv_obj_set_height(right_freq_bars[i], bar_height);
    }

}

void ui_microphone_enter(lv_obj_t *parent)
{
    bool is_small = is_screen_small();

    menu = create_menu(parent, back_event_handler);
    lv_obj_t *main_page = lv_menu_page_create(menu, NULL);
    lv_obj_remove_style_all(main_page);

    uint16_t bar_y_offset = 10;
    uint16_t text_y_offset = 0;
    uint16_t text_x_offset = 0;
    bool is_small_screen = false;
    if (is_small) {
        lv_obj_set_style_pad_top(menu, 5, LV_PART_MAIN);
        is_small_screen = true;
        text_y_offset = 40;
        text_x_offset = 15;
    } else {
        lv_obj_set_style_pad_top(menu, 80, LV_PART_MAIN);
        bar_y_offset = 30;
        text_y_offset = 100;
        text_x_offset = 80;
    }

    left_title = lv_label_create(main_page);
    lv_label_set_text(left_title, "L");
    if (is_small) {
        lv_obj_set_style_text_font(left_title, &lv_font_montserrat_16, 0);
    } else {
        lv_obj_set_style_text_font(left_title, &lv_font_montserrat_48, 0);
    }
    lv_obj_set_style_text_color(left_title, get_gradient_color(0, FREQ_BANDS, HUE_START, HUE_END), 0);
    lv_obj_align(left_title, LV_ALIGN_LEFT_MID, text_x_offset, -text_y_offset);

    for (int i = 0; i < FREQ_BANDS; i++) {
        lv_obj_t *bar_bg = lv_obj_create(main_page);
        lv_obj_set_size(bar_bg, BAR_WIDTH, BAR_HEIGHT);
        lv_obj_set_style_bg_color(bar_bg, lv_color_hex(0x1a1a1a), 0);
        lv_obj_align(bar_bg, LV_ALIGN_TOP_MID,
                     (i - (FREQ_BANDS - 1) / 2.0f) * (BAR_WIDTH + BAR_SPACING), bar_y_offset);

        left_freq_bars[i] = lv_obj_create(bar_bg);
        lv_obj_set_size(left_freq_bars[i], BAR_WIDTH, 1);
        lv_color_t color = get_gradient_color(i, FREQ_BANDS, HUE_START, HUE_END);
        lv_obj_set_style_bg_color(left_freq_bars[i], color, 0);
        lv_obj_align(left_freq_bars[i], LV_ALIGN_BOTTOM_MID, 0, 15);
    }

    right_title = lv_label_create(main_page);
    lv_label_set_text(right_title, "R");
    if (is_small) {
        lv_obj_set_style_text_font(right_title, &lv_font_montserrat_16, 0);
    } else {
        lv_obj_set_style_text_font(right_title, &lv_font_montserrat_48, 0);
    }
    lv_obj_set_style_text_color(right_title, get_gradient_color(0, FREQ_BANDS, HUE_START_R, HUE_END_R), 0);
    lv_obj_align(right_title, LV_ALIGN_LEFT_MID, text_x_offset, is_small_screen ? 20 : 0);

    for (int i = 0; i < FREQ_BANDS; i++) {
        lv_obj_t *bar_bg = lv_obj_create(main_page);
        lv_obj_set_size(bar_bg, BAR_WIDTH, BAR_HEIGHT);
        lv_obj_set_style_bg_color(bar_bg, lv_color_hex(0x1a1a1a), 0);
        lv_obj_align(bar_bg, LV_ALIGN_TOP_MID,
                     (i - (FREQ_BANDS - 1) / 2.0f) * (BAR_WIDTH + BAR_SPACING), bar_y_offset + CHANNEL_Y_OFFSET);

        right_freq_bars[i] = lv_obj_create(bar_bg);
        lv_obj_set_size(right_freq_bars[i], BAR_WIDTH, 1);
        lv_color_t color = get_gradient_color(i, FREQ_BANDS, HUE_START_R, HUE_END_R);
        lv_obj_set_style_bg_color(right_freq_bars[i], color, 0);
        lv_obj_align(right_freq_bars[i], LV_ALIGN_BOTTOM_MID, 0, 15);
    }

    lv_menu_set_page(menu, main_page);

    hw_set_mic_start();

    timer = lv_timer_create(update_fft_display, 1, NULL);

#ifdef USING_TOUCHPAD
    quit_btn  = create_floating_button([](lv_event_t*e) {
        lv_obj_send_event(lv_menu_get_main_header_back_button(menu), LV_EVENT_CLICKED, NULL);
    }, NULL);
#endif

}


void ui_microphone_exit(lv_obj_t *parent)
{

}

app_t ui_microphone_main = {
    .setup_func_cb = ui_microphone_enter,
    .exit_func_cb = ui_microphone_exit,
    .user_data = nullptr,
};


