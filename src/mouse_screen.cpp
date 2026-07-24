#include "mouse_screen.h"
#include "theme.h"
#include "mouse_hid.h"
#include <LilyGoLib.h>
#include <stdlib.h>   // abs

// Defined in tools_screen.cpp
void tools_screen_show();

// Touchscreen pixels are multiplied by this before being sent as HID deltas,
// so the cursor can cross a desktop without endless re-dragging.
#define TRACKPAD_GAIN  2
// A press that travels less than this many pixels counts as a tap (left click).
#define TAP_SLOP_PX    10

static lv_obj_t *mouse_screen;
static lv_obj_t *status_label;
static lv_obj_t *start_btn;
static lv_obj_t *start_label;
static lv_obj_t *trackpad;
static lv_obj_t *hint_label;

// Set when mouse_hid_start() fails because the BLE scanners hold the radio.
static bool s_ble_busy = false;

// Trackpad touch tracking
static lv_coord_t s_last_x,  s_last_y;    // previous point — for delta movement
static lv_coord_t s_press_x, s_press_y;   // touch-down point — for the tap test
static bool       s_moved;                // travelled beyond TAP_SLOP_PX?

// ---- refresh ---------------------------------------------------------------

static void update_start_button()
{
    bool running = mouse_hid_is_running();
    lv_label_set_text(start_label, running ? "STOP" : "START");
    lv_obj_set_style_bg_color(start_btn,
        running ? lv_color_make(0xCC, 0x00, 0x00)
                : lv_color_make(0x00, 0xAA, 0x44),
        LV_PART_MAIN);
}

static void update_status()
{
    if (!mouse_hid_is_running()) {
        if (s_ble_busy) {
            lv_label_set_text(status_label, "BLE busy - stop AirTag / wardriver");
            lv_obj_set_style_text_color(status_label, lv_color_make(0xFF, 0xAA, 0x00), LV_PART_MAIN);
        } else {
            lv_label_set_text(status_label, "Stopped");
            lv_obj_set_style_text_color(status_label, ARGUS_TEXT_DIM, LV_PART_MAIN);
        }
    } else if (!mouse_hid_is_connected()) {
        lv_label_set_text(status_label, "Discoverable - pair \"T-Watch Mouse\"");
        lv_obj_set_style_text_color(status_label, lv_color_make(0xFF, 0xCC, 0x00), LV_PART_MAIN);
    } else {
        lv_label_set_text(status_label, "Connected - drag to move, tap to click");
        lv_obj_set_style_text_color(status_label, ARGUS_TEXT, LV_PART_MAIN);
    }
}

static void update_trackpad()
{
    bool live = mouse_hid_is_connected();
    lv_obj_set_style_bg_color(trackpad,
        live ? lv_color_make(0x1E, 0x1E, 0x28) : lv_color_make(0x14, 0x14, 0x14),
        LV_PART_MAIN);
    lv_label_set_text(hint_label, live ? "Trackpad" : "(not connected)");
}

static void on_refresh(lv_timer_t *)
{
    update_start_button();
    update_status();
    update_trackpad();
}

// ---- event handlers --------------------------------------------------------

static void on_start_stop(lv_event_t *)
{
    if (mouse_hid_is_running()) {
        mouse_hid_stop();
        s_ble_busy = false;
    } else {
        s_ble_busy = !mouse_hid_start();
    }
    update_start_button();
    update_status();
    update_trackpad();
}


static void on_left_click(lv_event_t *)  { mouse_hid_click(MOUSE_BTN_LEFT);  }
static void on_right_click(lv_event_t *) { mouse_hid_click(MOUSE_BTN_RIGHT); }

// The trackpad: drag = relative cursor movement, quick tap = left click.
// move/click no-op internally when no host is connected.
static void on_trackpad(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    if (!indev) return;

    lv_point_t p;
    lv_indev_get_point(indev, &p);

    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED:
        s_last_x = s_press_x = p.x;
        s_last_y = s_press_y = p.y;
        s_moved  = false;
        break;

    case LV_EVENT_PRESSING: {
        int dx = p.x - s_last_x;
        int dy = p.y - s_last_y;
        s_last_x = p.x;
        s_last_y = p.y;
        if (dx != 0 || dy != 0)
            mouse_hid_move(dx * TRACKPAD_GAIN, dy * TRACKPAD_GAIN);
        if (abs(p.x - s_press_x) > TAP_SLOP_PX || abs(p.y - s_press_y) > TAP_SLOP_PX)
            s_moved = true;
        break;
    }

    case LV_EVENT_RELEASED:
        if (!s_moved)               // a press that never travelled = a tap
            mouse_hid_click(MOUSE_BTN_LEFT);
        break;

    default:
        break;
    }
}

// ---- layout ----------------------------------------------------------------

static lv_obj_t *make_button(lv_obj_t *parent, const char *text,
                             lv_coord_t w, lv_coord_t h, lv_color_t bg)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_radius(btn, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &font_argus_label_20, LV_PART_MAIN);
    lv_obj_center(lbl);
    return btn;
}

void mouse_screen_create()
{
    mouse_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(mouse_screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(mouse_screen, 0, LV_PART_MAIN);

    // The trackpad needs swipe gestures, so this screen has no swipe-to-exit
    // either — use the hardware power / GPIO0 back button to leave.

    // Title
    lv_obj_t *title = lv_label_create(mouse_screen);
    lv_obj_set_style_text_color(title, ARGUS_OFFENSE_ACCENT, LV_PART_MAIN);  // Offense-only: red-team
    lv_obj_set_style_text_font(title, &font_argus_ui, LV_PART_MAIN);
    lv_label_set_text(title, "MOUSE");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

    // Status line
    status_label = lv_label_create(mouse_screen);
    lv_obj_set_style_text_font(status_label, &font_argus_label_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(status_label, ARGUS_TEXT_DIM, LV_PART_MAIN);
    lv_label_set_text(status_label, "Stopped");
    lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 56);

    // Start / Stop button
    start_btn = make_button(mouse_screen, "START", 170, 48,
                            lv_color_make(0x00, 0xAA, 0x44));
    lv_obj_align(start_btn, LV_ALIGN_TOP_MID, 0, 80);
    start_label = lv_obj_get_child(start_btn, 0);
    lv_obj_add_event_cb(start_btn, on_start_stop, LV_EVENT_CLICKED, NULL);

    // Trackpad area
    trackpad = lv_obj_create(mouse_screen);
    lv_obj_set_size(trackpad, 390, 268);
    lv_obj_align(trackpad, LV_ALIGN_TOP_MID, 0, 142);
    lv_obj_set_style_bg_color(trackpad, lv_color_make(0x14, 0x14, 0x14), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(trackpad, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(trackpad, lv_color_make(0x44, 0x44, 0x55), LV_PART_MAIN);
    lv_obj_set_style_border_width(trackpad, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(trackpad, 10, LV_PART_MAIN);
    lv_obj_clear_flag(trackpad, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(trackpad, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(trackpad, on_trackpad, LV_EVENT_PRESSED,  NULL);
    lv_obj_add_event_cb(trackpad, on_trackpad, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(trackpad, on_trackpad, LV_EVENT_RELEASED, NULL);

    hint_label = lv_label_create(trackpad);
    lv_obj_set_style_text_color(hint_label, ARGUS_TEXT_DIM, LV_PART_MAIN);
    lv_obj_set_style_text_font(hint_label, &font_argus_label_20, LV_PART_MAIN);
    lv_label_set_text(hint_label, "(not connected)");
    lv_obj_center(hint_label);

    // Left / Right click buttons — width trimmed 188 → 150 and re-anchored
    // BOTTOM_MID with ±83 offsets so they sit centred about the midline
    // with a 16 px gap, instead of being pinned to opposite screen edges
    // where their outer ends ran past the bezel asymmetrically. y=-30
    // preserves the original bottom-edge position (y=472).
    lv_obj_t *left_btn = make_button(mouse_screen, "LEFT", 150, 52,
                                     lv_color_make(0x22, 0x22, 0x22));
    lv_obj_align(left_btn, LV_ALIGN_BOTTOM_MID, -83, -30);
    lv_obj_add_event_cb(left_btn, on_left_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t *right_btn = make_button(mouse_screen, "RIGHT", 150, 52,
                                      lv_color_make(0x22, 0x22, 0x22));
    lv_obj_align(right_btn, LV_ALIGN_BOTTOM_MID,  83, -30);
    lv_obj_add_event_cb(right_btn, on_right_click, LV_EVENT_CLICKED, NULL);

    lv_timer_create(on_refresh, 500, NULL);
}

void mouse_screen_show()
{
    s_ble_busy = false;
    update_start_button();
    update_status();
    update_trackpad();
    lv_scr_load(mouse_screen);
}

bool mouse_screen_is_active()
{
    return lv_screen_active() == mouse_screen;
}
