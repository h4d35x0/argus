#include "stopwatch_screen.h"
#include "theme.h"
#include <LilyGoLib.h>

// Defined elsewhere.
void time_screen_show();

static lv_obj_t *stopwatch_screen;
static lv_obj_t *time_label;       // main elapsed-time display
static lv_obj_t *lap_label;        // lap time display (smaller, below main)
static lv_obj_t *start_btn;
static lv_obj_t *start_label;
static lv_obj_t *lap_btn;
static lv_obj_t *lap_btn_label;
static lv_obj_t *reset_btn;
static lv_obj_t *reset_label;
static lv_obj_t *lap_list;         // container for lap records

// Stopwatch state
static bool     s_running   = false;
static uint32_t s_start_ms  = 0;   // millis() when started (or last resume)
static uint32_t s_elapsed   = 0;   // accumulated ms before current run
static uint32_t s_lap_start = 0;   // millis() when current lap started
static uint32_t s_lap_count = 0;   // number of recorded laps
static lv_timer_t *s_timer  = NULL;

// Lap records stored as LVGL labels so they persist in the scrollable list
#define MAX_LAPS 50

static void format_time(char *buf, size_t len, uint32_t ms)
{
    uint32_t h = ms / 3600000;
    uint32_t m = (ms % 3600000) / 60000;
    uint32_t s = (ms % 60000) / 1000;
    uint32_t cs = (ms % 1000) / 10;  // centiseconds

    if (h > 0)
        snprintf(buf, len, "%lu:%02lu:%02lu.%02lu", (unsigned long)h, (unsigned long)m, (unsigned long)s, (unsigned long)cs);
    else
        snprintf(buf, len, "%lu:%02lu.%02lu", (unsigned long)m, (unsigned long)s, (unsigned long)cs);
}

static uint32_t current_elapsed()
{
    if (s_running)
        return s_elapsed + (millis() - s_start_ms);
    return s_elapsed;
}

static uint32_t current_lap_elapsed()
{
    if (s_running)
        return millis() - s_lap_start;
    return 0;
}

static void update_display()
{
    char buf[24];
    format_time(buf, sizeof(buf), current_elapsed());
    lv_label_set_text(time_label, buf);

    if (s_running) {
        format_time(buf, sizeof(buf), current_lap_elapsed());
        lv_label_set_text(lap_label, buf);
    }
}

static void update_buttons()
{
    if (s_running) {
        lv_label_set_text(start_label, LV_SYMBOL_PAUSE);
        lv_obj_set_style_bg_color(start_btn, lv_color_make(0xCC, 0x00, 0x00), LV_PART_MAIN);
        lv_obj_clear_flag(lap_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(reset_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(start_label, LV_SYMBOL_PLAY);
        lv_obj_set_style_bg_color(start_btn, lv_color_make(0x00, 0xAA, 0x44), LV_PART_MAIN);
        if (s_elapsed > 0) {
            lv_obj_clear_flag(reset_btn, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(reset_btn, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_add_flag(lap_btn, LV_OBJ_FLAG_HIDDEN);
    }
}

static void on_timer(lv_timer_t *)
{
    update_display();
}

static void on_start_stop(lv_event_t *)
{
    if (s_running) {
        // Pause
        s_elapsed += millis() - s_start_ms;
        s_running = false;
    } else {
        // Start / resume
        s_start_ms = millis();
        s_lap_start = millis();
        s_running = true;
    }
    update_buttons();
    update_display();
}

static void on_lap(lv_event_t *)
{
    if (!s_running) return;

    uint32_t now = millis();
    uint32_t lap_ms = now - s_lap_start;
    s_lap_start = now;

    s_lap_count++;

    // Build lap record label
    char buf[32];
    format_time(buf, sizeof(buf), lap_ms);
    char line[48];
    snprintf(line, sizeof(line), "Lap %lu  %s", (unsigned long)s_lap_count, buf);

    lv_obj_t *entry = lv_label_create(lap_list);
    lv_obj_set_style_text_color(entry, ARGUS_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(entry, &font_argus_label_16, LV_PART_MAIN);
    lv_obj_set_style_pad_all(entry, 2, LV_PART_MAIN);
    lv_obj_set_width(entry, 360);
    lv_label_set_text(entry, line);

    // Auto-scroll to bottom
    lv_obj_scroll_to_y(lap_list, lv_obj_get_scroll_bottom(lap_list), LV_ANIM_OFF);
}

static void on_reset(lv_event_t *)
{
    s_running   = false;
    s_elapsed   = 0;
    s_start_ms  = 0;
    s_lap_start = 0;
    s_lap_count = 0;

    // Clear lap list
    lv_obj_clean(lap_list);

    update_buttons();
    update_display();
}

// Swipe-RIGHT returns to the TIME screen (Stopwatch's parent). Using a
// horizontal direction matches the Timer and Calendar siblings and stays
// out of the way of the lap-list's vertical scrolling.
static void on_gesture(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir == LV_DIR_RIGHT)
        time_screen_show();
}

// ---- button helper ----------------------------------------------------------

static lv_obj_t *make_btn(lv_obj_t *parent, const char *text, lv_coord_t x, lv_coord_t y, lv_coord_t w)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_set_size(btn, w, 48);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_style_radius(btn, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_make(0x22, 0x22, 0x22), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_center(lbl);

    return btn;
}

// ---- public API ------------------------------------------------------------

void stopwatch_screen_create()
{
    stopwatch_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(stopwatch_screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(stopwatch_screen, 0, LV_PART_MAIN);

    // Title
    lv_obj_t *title = lv_label_create(stopwatch_screen);
    lv_obj_set_style_text_color(title, argus_base_accent(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_argus_ui, LV_PART_MAIN);
    lv_label_set_text(title, "STOPWATCH");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    // Main elapsed time display (large)
    time_label = lv_label_create(stopwatch_screen);
    lv_obj_set_style_text_color(time_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(time_label, &font_argus_mono_48, LV_PART_MAIN);
    lv_label_set_text(time_label, "0:00.00");
    lv_obj_align(time_label, LV_ALIGN_TOP_MID, 0, 60);

    // Lap time display (smaller, below main time)
    lap_label = lv_label_create(stopwatch_screen);
    lv_obj_set_style_text_color(lap_label, ARGUS_TEXT_DIM, LV_PART_MAIN);
    lv_obj_set_style_text_font(lap_label, &font_argus_label_20, LV_PART_MAIN);
    lv_label_set_text(lap_label, "");
    lv_obj_align(lap_label, LV_ALIGN_TOP_MID, 0, 120);

    // Button row: [Lap] [Start/Stop] [Reset]
    // Lap button (left)
    lap_btn = make_btn(stopwatch_screen, "Lap", 18, 150, 100);
    lap_btn_label = lv_obj_get_child(lap_btn, 0);
    lv_obj_add_flag(lap_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(lap_btn, on_lap, LV_EVENT_CLICKED, NULL);

    // Start/Stop button (centre)
    start_btn = make_btn(stopwatch_screen, LV_SYMBOL_PLAY, 140, 150, 130);
    start_label = lv_obj_get_child(start_btn, 0);
    lv_obj_set_style_bg_color(start_btn, lv_color_make(0x00, 0xAA, 0x44), LV_PART_MAIN);
    lv_obj_add_event_cb(start_btn, on_start_stop, LV_EVENT_CLICKED, NULL);

    // Reset button (right)
    reset_btn = make_btn(stopwatch_screen, "Reset", 288, 150, 100);
    reset_label = lv_obj_get_child(reset_btn, 0);
    lv_obj_add_flag(reset_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(reset_btn, on_reset, LV_EVENT_CLICKED, NULL);

    // Lap list — scrollable container below the buttons
    lap_list = lv_obj_create(stopwatch_screen);
    lv_obj_set_size(lap_list, 380, 220);
    lv_obj_align(lap_list, LV_ALIGN_TOP_MID, 0, 210);
    lv_obj_set_style_bg_color(lap_list, lv_color_make(0x11, 0x11, 0x11), LV_PART_MAIN);
    lv_obj_set_style_border_color(lap_list, lv_color_make(0x33, 0x33, 0x33), LV_PART_MAIN);
    lv_obj_set_style_border_width(lap_list, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(lap_list, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(lap_list, 4, LV_PART_MAIN);
    lv_obj_set_scroll_dir(lap_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(lap_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_layout(lap_list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(lap_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(lap_list,
        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    // "No laps" placeholder — floated out of the flex flow so we can centre it.
    lv_obj_t *placeholder = lv_label_create(lap_list);
    lv_obj_set_style_text_color(placeholder, ARGUS_TEXT_DIM, LV_PART_MAIN);
    lv_obj_set_style_text_font(placeholder, &font_argus_label_16, LV_PART_MAIN);
    lv_label_set_text(placeholder, "No laps recorded");
    lv_obj_add_flag(placeholder, LV_OBJ_FLAG_FLOATING);
    lv_obj_center(placeholder);

    lv_obj_add_event_cb(stopwatch_screen, on_gesture, LV_EVENT_GESTURE, NULL);

    // Periodic timer to update the display (20 Hz = 50 ms)
    s_timer = lv_timer_create(on_timer, 50, NULL);
}

void stopwatch_screen_show()
{
    lv_scr_load(stopwatch_screen);
}

bool stopwatch_screen_is_active()
{
    return lv_screen_active() == stopwatch_screen;
}

bool stopwatch_is_running()
{
    return s_running;
}
