/**
 * @file      ui_nfc.cpp
 * @author    Lewis He (lewishe@outlook.com)
 * @license   MIT
 * @copyright Copyright (c) 2025  ShenZhen XinYuan Electronic Technology Co., Ltd
 * @date      2025-01-05
 *
 */
#include "ui_define.h"

static lv_obj_t *msgbox = NULL;
static lv_obj_t *menu = NULL;
static lv_obj_t *quit_btn = NULL;

static void goto_connect_wifi_event_cb(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_current_target(e);
    uint16_t id = 0;
#if LVGL_VERSION_MAJOR == 9
    lv_obj_t *btn = lv_event_get_target_obj(e);
    lv_obj_t *label = lv_obj_get_child(btn, 0);
    const char *text = lv_label_get_text(label);
    printf("Button %s clicked\n", text);
    if (strcmp(text, "Close") == 0) {
        id = 1;
    }
#else
    uint16_t id =  lv_msgbox_get_active_btn(obj);
    printf("id=%d\n", id);
#endif

    if (id == 0) {
        wifi_conn_params_t *params = static_cast<wifi_conn_params_t *>(lv_event_get_user_data(e));
        if (params) {
            hw_set_wifi_connect(*params);
        }
        if (msgbox) {
            destroy_msgbox(msgbox);
            msgbox = NULL;
        }
        ui_show_wifi_process_bar();
    } else {
        if (msgbox) {
            destroy_msgbox(msgbox);
            msgbox = NULL;
        }
    }
}

void ui_nfc_pop_up(wifi_conn_params_t &params)
{
    set_low_power_mode_flag(false);
    if (msgbox) {
        return;
    }
    printf("Do you want to connect to %s , pwd:%s\n", params.ssid.c_str(), params.password.c_str());
    static const char *btns[] = {"Connect", "Close", ""};
    char msg_txt[128];
    snprintf(msg_txt, 128, "Do you want to connect to \"%s\"", params.ssid.c_str());
    msgbox = create_msgbox(lv_scr_act(), "NFC WiFi",
                           msg_txt, btns,
                           goto_connect_wifi_event_cb, &params);
}

static void back_event_handler(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    if (lv_menu_back_btn_is_root(menu, obj)) {
        lv_obj_clean(menu);
        lv_obj_del(menu);

        hw_stop_nfc_discovery();

        if (quit_btn) {
            lv_obj_del_async(quit_btn);
            quit_btn = NULL;
        }

        menu_show();
    }
}

void ui_nfc_enter(lv_obj_t *parent)
{
    bool nfc_started = hw_start_nfc_discovery();

    menu = create_menu(parent, back_event_handler);
    lv_menu_set_mode_root_back_btn(menu, LV_MENU_ROOT_BACK_BTN_ENABLED);

    lv_obj_t *main_page = lv_menu_page_create(menu, NULL);

    lv_obj_remove_style_all(main_page);

    LV_IMG_DECLARE(img_nfc_bg);
    lv_obj_t *image = lv_image_create(main_page);
    lv_image_set_src(image, &img_nfc_bg);
    lv_obj_align(image, LV_ALIGN_CENTER, 0, -120);

    lv_obj_t *label = lv_label_create(main_page);
    lv_obj_set_width(label, lv_pct(90));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);

    if (nfc_started) {
        lv_label_set_text(label, NFC_TIPS_STRING);
    } else {
        lv_label_set_text(label, "NFC initialization failed. Please remove the SD card, then exit and re-enter the NFC page.");
    }

    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(label, lv_color_black(), 0);

    lv_obj_align_to(label, image, LV_ALIGN_OUT_BOTTOM_MID, 0, 0);


    if (lv_disp_get_physical_ver_res(NULL) > 320) {
        lv_obj_set_width(label, lv_pct(90));
        lv_obj_align(image, LV_ALIGN_CENTER, 0, -120);
        lv_obj_align(label, LV_ALIGN_CENTER, 0, 50);
    } else {
        lv_obj_set_width(label, lv_pct(50));
        lv_obj_set_style_text_font(label, &lv_font_montserrat_18, 0);
        lv_obj_align(image, LV_ALIGN_LEFT_MID, 50, 0);
        lv_obj_align(label, LV_ALIGN_RIGHT_MID, -50, 0);
    }

    lv_menu_set_page(menu, main_page);

#ifdef USING_TOUCHPAD
    quit_btn  = create_floating_button([](lv_event_t*e) {
        lv_obj_send_event(lv_menu_get_main_header_back_button(menu), LV_EVENT_CLICKED, lv_event_get_target_obj(e));
    }, NULL);
#endif
}

void ui_nfc_exit(lv_obj_t *parent)
{

}

app_t ui_nfc_main = {
    .setup_func_cb = ui_nfc_enter,
    .exit_func_cb = ui_nfc_exit,
    .user_data = nullptr,
};
