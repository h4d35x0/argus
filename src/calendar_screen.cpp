#include "calendar_screen.h"
#include "theme.h"
#include <LilyGoLib.h>
#include <time.h>
#include <stdio.h>
#include <string.h>

// Defined elsewhere.
void time_screen_show();
void clock_screen_get_local_time(struct tm *out);

// ---- Grid geometry ---------------------------------------------------------
// 7 columns × 6 rows fits every Gregorian month layout (a month that starts
// on Saturday and runs 31 days needs 6 rows). The T-Watch Ultra panel is
// 410 px wide (NOT 480 — that was the previous mis-assumption); sizing the
// grid to 304 px wide leaves exactly 53 px of padding on each side, which
// stays inside the round bezel for the full grid height instead of
// getting clipped on the right edge.
#define CAL_SCREEN_W 410
#define CAL_CELL_W   40
#define CAL_CELL_H   40
#define CAL_CELL_GAP  4
#define CAL_GRID_W   (7 * CAL_CELL_W + 6 * CAL_CELL_GAP)   // 304 px
// Top of the date grid; weekday header sits just above this.
#define CAL_GRID_Y   175

// ---- State -----------------------------------------------------------------

static lv_obj_t *calendar_screen;
static lv_obj_t *month_label;          // "October 2025" header
static lv_obj_t *cell_label[42];       // 6×7 = 42 day cells (labels)
static lv_obj_t *cell_obj[42];         // their parent containers (for bg)

// The month the user is currently viewing — not necessarily today's month.
// Re-seeded to today every time the screen opens.
static int s_view_year  = 2025;
static int s_view_month = 1;     // 1..12

// Weekday names — Sunday-first to match the standard US 7-column layout.
static const char *WEEKDAYS[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *MONTHS[12]  = {
    "January", "February", "March",     "April",   "May",      "June",
    "July",    "August",   "September", "October", "November", "December"
};

// ---- Date helpers ----------------------------------------------------------

static int days_in_month(int year, int month)
{
    static const int days[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (month < 1 || month > 12) return 30;
    int d = days[month - 1];
    if (month == 2) {
        bool leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
        if (leap) d = 29;
    }
    return d;
}

// Weekday (0=Sun … 6=Sat) for the 1st of the given month. mktime() normalises
// the struct and fills tm_wday for us, so we don't need Zeller's congruence.
static int weekday_of_first(int year, int month)
{
    struct tm t = {};
    t.tm_year = year - 1900;
    t.tm_mon  = month - 1;
    t.tm_mday = 1;
    t.tm_hour = 12;          // mid-day avoids any DST-edge weirdness
    mktime(&t);
    return t.tm_wday;
}

// ---- Cell creation ---------------------------------------------------------

static lv_obj_t *make_cell(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                           lv_obj_t **label_out)
{
    lv_obj_t *cell = lv_obj_create(parent);
    lv_obj_set_size(cell, CAL_CELL_W, CAL_CELL_H);
    lv_obj_set_style_radius(cell, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(cell, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(cell, 0, LV_PART_MAIN);
    lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(cell, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(cell, LV_ALIGN_TOP_LEFT, x, y);

    lv_obj_t *l = lv_label_create(cell);
    lv_obj_set_style_text_font(l, &font_argus_label_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, lv_color_white(), LV_PART_MAIN);
    lv_label_set_text(l, "");
    lv_obj_center(l);

    if (label_out) *label_out = l;
    return cell;
}

// ---- Population ------------------------------------------------------------

// Fills the cell grid for s_view_year / s_view_month. Highlights today (per
// the live RTC) with a green-filled cell; days from the previous / next
// month are shown dimmed to preserve the visual grid.
static void populate_grid()
{
    int dim          = days_in_month(s_view_year, s_view_month);
    int first_wday   = weekday_of_first(s_view_year, s_view_month);

    int prev_year    = s_view_month == 1 ? s_view_year - 1 : s_view_year;
    int prev_month   = s_view_month == 1 ? 12 : s_view_month - 1;
    int prev_dim     = days_in_month(prev_year, prev_month);

    struct tm today;
    clock_screen_get_local_time(&today);
    int today_year  = today.tm_year + 1900;
    int today_month = today.tm_mon  + 1;
    int today_day   = today.tm_mday;

    lv_color_t col_white = lv_color_white();
    lv_color_t col_dim   = lv_color_make(0x55, 0x55, 0x55);
    lv_color_t col_today = ARGUS_ACCENT;

    for (int i = 0; i < 42; i++) {
        int day_num;
        bool current_month;

        if (i < first_wday) {
            // Cells before the 1st show the tail of the previous month.
            day_num       = prev_dim - (first_wday - 1 - i);
            current_month = false;
        } else if (i - first_wday + 1 <= dim) {
            day_num       = i - first_wday + 1;
            current_month = true;
        } else {
            day_num       = i - first_wday + 1 - dim;
            current_month = false;
        }

        char buf[4];
        snprintf(buf, sizeof(buf), "%d", day_num);
        lv_label_set_text(cell_label[i], buf);

        bool is_today = current_month
                     && day_num    == today_day
                     && s_view_month == today_month
                     && s_view_year  == today_year;

        if (is_today) {
            lv_obj_set_style_bg_color(cell_obj[i], col_today, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(cell_obj[i], LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_text_color(cell_label[i], lv_color_black(), LV_PART_MAIN);
        } else {
            lv_obj_set_style_bg_opa(cell_obj[i], LV_OPA_TRANSP, LV_PART_MAIN);
            lv_obj_set_style_text_color(cell_label[i],
                current_month ? col_white : col_dim, LV_PART_MAIN);
        }
    }

    // Header — "Month Year".
    char hdr[32];
    snprintf(hdr, sizeof(hdr), "%s %d", MONTHS[s_view_month - 1], s_view_year);
    lv_label_set_text(month_label, hdr);
}

// ---- Navigation ------------------------------------------------------------

static void step_month(int delta)
{
    s_view_month += delta;
    while (s_view_month < 1)  { s_view_month += 12; s_view_year--; }
    while (s_view_month > 12) { s_view_month -= 12; s_view_year++; }
    populate_grid();
}

static void on_prev(lv_event_t *)   { step_month(-1); }
static void on_next(lv_event_t *)   { step_month(+1); }
static void on_today(lv_event_t *)
{
    // Snap back to today's month — convenient when the user has paged away.
    struct tm now;
    clock_screen_get_local_time(&now);
    s_view_year  = now.tm_year + 1900;
    s_view_month = now.tm_mon  + 1;
    populate_grid();
}
// Swipe-RIGHT returns to the TIME screen (Calendar's parent in the
// navigation tree). Matches Timer / Stopwatch / Alarm siblings.
static void on_gesture(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir == LV_DIR_RIGHT) time_screen_show();
}

// ---- Button helper ---------------------------------------------------------

static lv_obj_t *make_button(lv_obj_t *parent, const char *text,
                             lv_coord_t w, lv_coord_t h,
                             lv_color_t bg, const lv_font_t *font)
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

    lv_obj_t *l = lv_label_create(btn);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(l, font ? font : &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_center(l);
    return btn;
}

// ---- Public API ------------------------------------------------------------

void calendar_screen_create()
{
    calendar_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(calendar_screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(calendar_screen, 0, LV_PART_MAIN);
    lv_obj_clear_flag(calendar_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(calendar_screen, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(calendar_screen, on_gesture, LV_EVENT_GESTURE, NULL);

    // Header bar — TODAY shortcut centred along the top; swipe-right
    // returns to Tools.
    lv_obj_t *today_btn = make_button(calendar_screen, "TODAY", 88, 38,
                                      lv_color_make(0x00, 0x55, 0x22),
                                      NULL);
    lv_obj_align(today_btn, LV_ALIGN_TOP_MID, 0, 8);
    lv_obj_add_event_cb(today_btn, on_today, LV_EVENT_CLICKED, NULL);

    // Month-year title with prev/next chevrons on either side
    lv_obj_t *prev_btn = make_button(calendar_screen, "<", 50, 44,
                                     lv_color_make(0x22, 0x22, 0x22),
                                     &lv_font_montserrat_28);
    lv_obj_align(prev_btn, LV_ALIGN_TOP_LEFT, 10, 70);
    lv_obj_add_event_cb(prev_btn, on_prev, LV_EVENT_CLICKED, NULL);

    lv_obj_t *next_btn = make_button(calendar_screen, ">", 50, 44,
                                     lv_color_make(0x22, 0x22, 0x22),
                                     &lv_font_montserrat_28);
    lv_obj_align(next_btn, LV_ALIGN_TOP_RIGHT, -10, 70);
    lv_obj_add_event_cb(next_btn, on_next, LV_EVENT_CLICKED, NULL);

    month_label = lv_label_create(calendar_screen);
    lv_obj_set_style_text_color(month_label, ARGUS_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(month_label, &font_argus_ui, LV_PART_MAIN);
    lv_label_set_text(month_label, "");
    lv_obj_align(month_label, LV_ALIGN_TOP_MID, 0, 77);

    // Weekday header strip — same column widths as the date grid.
    int grid_x0 = (CAL_SCREEN_W - CAL_GRID_W) / 2;
    for (int c = 0; c < 7; c++) {
        lv_obj_t *wl = lv_label_create(calendar_screen);
        lv_obj_set_style_text_font(wl, &font_argus_label_16, LV_PART_MAIN);
        // Saturday/Sunday slightly dimmer to read as weekends at a glance.
        bool weekend = (c == 0 || c == 6);
        lv_obj_set_style_text_color(wl,
            weekend ? lv_color_make(0xAA, 0xAA, 0xAA)
                    : lv_color_make(0xDD, 0xDD, 0xDD),
            LV_PART_MAIN);
        lv_label_set_text(wl, WEEKDAYS[c]);
        // Centre each weekday in its column.
        int cell_x = grid_x0 + c * (CAL_CELL_W + CAL_CELL_GAP);
        lv_obj_set_width(wl, CAL_CELL_W);
        lv_obj_set_style_text_align(wl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_align(wl, LV_ALIGN_TOP_LEFT, cell_x, CAL_GRID_Y - 28);
    }

    // 6 × 7 date grid
    for (int r = 0; r < 6; r++) {
        for (int c = 0; c < 7; c++) {
            int idx = r * 7 + c;
            int x   = grid_x0 + c * (CAL_CELL_W + CAL_CELL_GAP);
            int y   = CAL_GRID_Y + r * (CAL_CELL_H + CAL_CELL_GAP);
            cell_obj[idx] = make_cell(calendar_screen, x, y, &cell_label[idx]);
        }
    }
}

void calendar_screen_show()
{
    // Always open on today's month — the prev/next chevrons can page from
    // there.
    struct tm now;
    clock_screen_get_local_time(&now);
    s_view_year  = now.tm_year + 1900;
    s_view_month = now.tm_mon  + 1;
    populate_grid();

    lv_scr_load(calendar_screen);
}

bool calendar_screen_is_active()
{
    return lv_screen_active() == calendar_screen;
}
