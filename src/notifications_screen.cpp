// notifications_screen.cpp - see notifications_screen.h.
//
// Lists the notifications mirrored from the phone (newest first) and hosts the
// enable/disable toggle that flips the watch between Field-tool and Daily-wear
// mode. Enabling brings BLE/ANCS up; if WiFi is on, the toggle is refused with a
// "turn WiFi off" hint (the two radios cannot run together).
#include "notifications_screen.h"
#include "theme.h"
#include "device_mode.h"
#include "ancs.h"
#include "notify/notify_center.h"

#include <LilyGoLib.h>

// Defined in main.cpp.
void tools_screen_show();

static lv_obj_t *screen;
static lv_obj_t *status_label;
static lv_obj_t *platform_btn;
static lv_obj_t *platform_label;
static lv_obj_t *toggle_btn;
static lv_obj_t *toggle_label;
static lv_obj_t *list_box;
static lv_timer_t *refresh_timer;

static int s_shown_count = -1;   // last rendered store count, so we rebuild lazily

// ---- back gesture ----------------------------------------------------------
static void on_gesture(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    if (lv_indev_get_gesture_dir(indev) == LV_DIR_RIGHT)
        tools_screen_show();
}

// ---- status + toggle label -------------------------------------------------
static void update_status()
{
    const char *txt;
    lv_color_t col = ARGUS_TEXT_DIM;
    if (!device_mode_is_daily_wear()) {
        txt = "Notifications off - tap Enable";
    } else if (ancs::is_connected()) {
        txt = "Phone connected";
        col = lv_color_make(0x33, 0xCC, 0x66);
    } else {
        txt = "Waiting for phone to pair...";
        col = argus_base_accent();
    }
    lv_label_set_text(status_label, txt);
    lv_obj_set_style_text_color(status_label, col, LV_PART_MAIN);

    lv_label_set_text(toggle_label,
                      device_mode_is_daily_wear() ? "DISABLE" : "ENABLE NOTIFICATIONS");

    // Platform picker: shows the current choice; dimmed while notifications are
    // live (the choice only takes effect on the next enable).
    lv_label_set_text(platform_label,
        device_mode_platform() == NotifyPlatform::iOS ? "Apple (ANCS)"
                                                      : "Android (Gadgetbridge)");
    bool locked = device_mode_is_daily_wear();
    lv_obj_set_style_bg_color(platform_btn,
        locked ? lv_color_make(0x22, 0x22, 0x22) : lv_color_make(0x33, 0x33, 0x33),
        LV_PART_MAIN);
    lv_obj_set_style_text_color(platform_label,
        locked ? ARGUS_TEXT_DIM : ARGUS_TEXT, LV_PART_MAIN);
}

// ---- notification list -----------------------------------------------------
static void add_card(const notify::Notification *n)
{
    lv_obj_t *card = lv_obj_create(list_box);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, lv_color_make(0x14, 0x14, 0x14), LV_PART_MAIN);
    lv_obj_set_style_border_color(card, lv_color_make(0x33, 0x33, 0x33), LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_row(card, 2, LV_PART_MAIN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(card, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);

    if (n->app[0]) {
        lv_obj_t *app = lv_label_create(card);
        lv_obj_set_style_text_font(app, &font_argus_label_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(app, argus_base_accent(), LV_PART_MAIN);
        lv_label_set_text(app, n->app);
    }
    if (n->title[0]) {
        lv_obj_t *title = lv_label_create(card);
        lv_obj_set_style_text_font(title, &font_argus_label_16, LV_PART_MAIN);
        lv_obj_set_style_text_color(title, ARGUS_TEXT, LV_PART_MAIN);
        lv_label_set_text(title, n->title);
    }
    if (n->body[0]) {
        lv_obj_t *body = lv_label_create(card);
        lv_obj_set_style_text_font(body, &font_argus_label_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(body, ARGUS_TEXT_DIM, LV_PART_MAIN);
        lv_obj_set_width(body, LV_PCT(100));
        lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
        lv_label_set_text(body, n->body);
    }
}

static void rebuild_list()
{
    lv_obj_clean(list_box);
    int n = notify::center().count();
    if (n == 0) {
        lv_obj_t *ph = lv_label_create(list_box);
        lv_obj_set_style_text_color(ph, ARGUS_TEXT_DIM, LV_PART_MAIN);
        lv_obj_set_style_text_font(ph, &font_argus_label_16, LV_PART_MAIN);
        lv_label_set_text(ph, device_mode_is_daily_wear()
                              ? "No notifications yet"
                              : "Enable to mirror phone notifications");
        lv_obj_add_flag(ph, LV_OBJ_FLAG_FLOATING);
        lv_obj_center(ph);
    } else {
        for (int i = 0; i < n; i++) {
            const notify::Notification *item = notify::center().get(i);
            if (item) add_card(item);
        }
    }
    s_shown_count = n;
}

// ---- clear -----------------------------------------------------------------
static void on_clear(lv_event_t *)
{
    // Dismiss on the phone too (iOS/ANCS supports it), then clear locally. Grab
    // the uids first since clearing invalidates the store indices.
    if (device_mode_platform() == NotifyPlatform::iOS && ancs::is_connected()) {
        uint32_t uids[notify::kStoreCapacity];
        int m = 0;
        for (int i = 0; i < notify::center().count() && m < notify::kStoreCapacity; i++) {
            const notify::Notification *it = notify::center().get(i);
            if (it) uids[m++] = it->uid;
        }
        for (int i = 0; i < m; i++) ancs::dismiss(uids[i]);
    }
    notify::clear_all();
    rebuild_list();
}

// ---- platform picker -------------------------------------------------------
static void on_platform(lv_event_t *)
{
    if (device_mode_is_daily_wear()) return;   // locked while notifications live
    device_mode_set_platform(device_mode_platform() == NotifyPlatform::iOS
                                 ? NotifyPlatform::Android
                                 : NotifyPlatform::iOS);
    update_status();
}

// ---- toggle ----------------------------------------------------------------
static void on_toggle(lv_event_t *)
{
    DeviceMode want = device_mode_is_daily_wear() ? DeviceMode::FieldTool
                                                  : DeviceMode::DailyWear;
    ModeAction acted = device_mode_set(want);
    if (acted == ModeAction::BlockedWifiActive) {
        lv_label_set_text(status_label, "Turn WiFi off first - radios can't share");
        lv_obj_set_style_text_color(status_label, lv_color_make(0xCC, 0x66, 0x00),
                                    LV_PART_MAIN);
    } else {
        update_status();
    }
    rebuild_list();
}

// ---- refresh timer ---------------------------------------------------------
static void on_refresh(lv_timer_t *)
{
    if (lv_screen_active() != screen) return;   // only when visible
    update_status();
    if (notify::center().count() != s_shown_count) rebuild_list();
}

// ---- build -----------------------------------------------------------------
void notifications_screen_create()
{
    screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(screen);
    lv_obj_set_style_text_color(title, argus_base_accent(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_argus_ui, LV_PART_MAIN);
    lv_label_set_text(title, "Notify");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    status_label = lv_label_create(screen);
    lv_obj_set_style_text_font(status_label, &font_argus_label_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(status_label, ARGUS_TEXT_DIM, LV_PART_MAIN);
    lv_label_set_text(status_label, "Notifications off - tap Enable");
    lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 52);

    // Platform picker (iPhone / Android). Tap to switch when notifications are off.
    platform_btn = lv_obj_create(screen);
    lv_obj_set_size(platform_btn, 320, 38);
    lv_obj_align(platform_btn, LV_ALIGN_TOP_MID, 0, 76);
    lv_obj_set_style_bg_color(platform_btn, lv_color_make(0x33, 0x33, 0x33), LV_PART_MAIN);
    lv_obj_set_style_border_width(platform_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(platform_btn, 8, LV_PART_MAIN);
    lv_obj_clear_flag(platform_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(platform_btn, on_platform, LV_EVENT_CLICKED, NULL);
    platform_label = lv_label_create(platform_btn);
    lv_obj_set_style_text_font(platform_label, &font_argus_label_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(platform_label, ARGUS_TEXT, LV_PART_MAIN);
    lv_label_set_text(platform_label, "Apple (ANCS)");
    lv_obj_center(platform_label);

    toggle_btn = lv_obj_create(screen);
    lv_obj_set_size(toggle_btn, 320, 48);
    lv_obj_align(toggle_btn, LV_ALIGN_TOP_MID, 0, 120);
    lv_obj_set_style_bg_color(toggle_btn, lv_color_make(0x00, 0x88, 0xCC), LV_PART_MAIN);
    lv_obj_set_style_border_width(toggle_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(toggle_btn, 8, LV_PART_MAIN);
    lv_obj_clear_flag(toggle_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(toggle_btn, on_toggle, LV_EVENT_CLICKED, NULL);
    toggle_label = lv_label_create(toggle_btn);
    lv_obj_set_style_text_font(toggle_label, &font_argus_label_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(toggle_label, lv_color_white(), LV_PART_MAIN);
    lv_label_set_text(toggle_label, "ENABLE NOTIFICATIONS");
    lv_obj_center(toggle_label);

    list_box = lv_obj_create(screen);
    lv_obj_set_size(list_box, 404, 258);
    lv_obj_align(list_box, LV_ALIGN_TOP_MID, 0, 182);
    lv_obj_set_style_bg_color(list_box, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_color(list_box, lv_color_make(0x33, 0x33, 0x33), LV_PART_MAIN);
    lv_obj_set_style_border_width(list_box, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(list_box, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(list_box, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_row(list_box, 6, LV_PART_MAIN);
    lv_obj_set_scroll_dir(list_box, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list_box, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_layout(list_box, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(list_box, LV_FLEX_FLOW_COLUMN);

    // CLEAR button, bottom-centered (kept clear of the display's rounded corners).
    lv_obj_t *clear_btn = lv_obj_create(screen);
    lv_obj_set_size(clear_btn, 220, 42);
    lv_obj_align(clear_btn, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_bg_color(clear_btn, lv_color_make(0x55, 0x22, 0x22), LV_PART_MAIN);
    lv_obj_set_style_border_width(clear_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(clear_btn, 8, LV_PART_MAIN);
    lv_obj_clear_flag(clear_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(clear_btn, on_clear, LV_EVENT_CLICKED, NULL);
    lv_obj_t *clear_lbl = lv_label_create(clear_btn);
    lv_obj_set_style_text_font(clear_lbl, &font_argus_label_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(clear_lbl, ARGUS_TEXT, LV_PART_MAIN);
    lv_label_set_text(clear_lbl, "CLEAR");
    lv_obj_center(clear_lbl);

    lv_obj_add_event_cb(screen, on_gesture, LV_EVENT_GESTURE, NULL);

    refresh_timer = lv_timer_create(on_refresh, 1000, NULL);
}

void notifications_screen_show()
{
    update_status();
    rebuild_list();
    lv_scr_load(screen);
}
