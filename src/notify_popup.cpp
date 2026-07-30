// notify_popup.cpp - see notify_popup.h.
#include "notify_popup.h"
#include "notify/notify_center.h"
#include "notify/notify_log.h"
#include "notifications_screen.h"
#include "theme.h"

#include <LilyGoLib.h>

static lv_obj_t   *s_banner        = nullptr;
static lv_timer_t *s_dismiss_timer = nullptr;

static constexpr uint32_t POPUP_MS = 6000;   // auto-dismiss after 6s

static void destroy_banner()
{
    if (s_dismiss_timer) { lv_timer_del(s_dismiss_timer); s_dismiss_timer = nullptr; }
    if (s_banner)        { lv_obj_del(s_banner);          s_banner        = nullptr; }
}

static void on_dismiss_timer(lv_timer_t *) { destroy_banner(); }

static void on_banner_click(lv_event_t *)
{
    destroy_banner();
    notifications_screen_show();   // tap the banner -> open the full list
}

static void show_banner(const notify::Notification &n)
{
    NLOG("[popup] show_banner: \"%s\"\n", n.title);
    destroy_banner();   // one banner at a time; a newer arrival replaces the old

    // Parent on the TOP layer so it floats above the clock and every screen and
    // survives screen loads. We own its lifetime (auto-dismiss / tap).
    s_banner = lv_obj_create(lv_layer_top());
    lv_obj_set_width(s_banner, 390);
    lv_obj_set_height(s_banner, LV_SIZE_CONTENT);
    // Sit below the display's top curve / status area rather than jammed against
    // the top edge, so the whole card is readable.
    lv_obj_align(s_banner, LV_ALIGN_TOP_MID, 0, 72);
    lv_obj_set_style_bg_color(s_banner, lv_color_make(0x1A, 0x1A, 0x1E), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_banner, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_banner, argus_base_accent(), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_banner, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(s_banner, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_banner, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_row(s_banner, 3, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(s_banner, 16, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(s_banner, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(s_banner, LV_OPA_50, LV_PART_MAIN);
    lv_obj_clear_flag(s_banner, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(s_banner, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_banner, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(s_banner, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_banner, on_banner_click, LV_EVENT_CLICKED, NULL);

    // Header: bell + app/source name in the accent colour.
    lv_obj_t *app = lv_label_create(s_banner);
    lv_obj_set_style_text_font(app, &font_argus_label_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(app, argus_base_accent(), LV_PART_MAIN);
    lv_label_set_text_fmt(app, LV_SYMBOL_BELL "  %s", n.app[0] ? n.app : "Notification");

    if (n.title[0]) {
        lv_obj_t *title = lv_label_create(s_banner);
        lv_obj_set_style_text_font(title, &font_argus_label_16, LV_PART_MAIN);
        lv_obj_set_style_text_color(title, ARGUS_TEXT, LV_PART_MAIN);
        lv_obj_set_width(title, LV_PCT(100));
        lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);   // one-line, ellipsized
        lv_label_set_text(title, n.title);
    }
    if (n.body[0]) {
        lv_obj_t *body = lv_label_create(s_banner);
        lv_obj_set_style_text_font(body, &font_argus_label_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(body, ARGUS_TEXT_DIM, LV_PART_MAIN);
        lv_obj_set_width(body, LV_PCT(100));
        lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
        lv_label_set_text(body, n.body);
    }

    s_dismiss_timer = lv_timer_create(on_dismiss_timer, POPUP_MS, NULL);
    lv_timer_set_repeat_count(s_dismiss_timer, 1);   // one-shot
}

// Runs on the UI thread (lv_timer_handler). Drains a pending arrival, if any.
static void poll(lv_timer_t *)
{
    notify::Notification n;
    if (notify::take_pending(n)) show_banner(n);
}

void notify_popup_init()
{
    lv_timer_create(poll, 250, NULL);
    NLOGLN("[popup] init: poll timer created");
}
