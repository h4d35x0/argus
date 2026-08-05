#include "threat_radar_screen.h"
#include "threat_radar.h"
#include "theme.h"                 // argus_accent() — dynamic ARGUS->HADES accent
#include "tools_screen.h"          // tools_screen_show() for the back-gesture
#include <lvgl.h>
#include <stdio.h>

// Level palette — matches the rest of the UI's "green ok / amber watch / red
// alert" language used by the detector tiles.
static lv_color_t lvl_color(uint8_t level)
{
    switch (level) {
        case TR_LVL_CONFIRMED: return lv_color_make(0xFF, 0x22, 0x22);
        case TR_LVL_LIKELY:    return lv_color_make(0xFF, 0x66, 0x00);
        case TR_LVL_POSSIBLE:  return lv_color_make(0xFF, 0xCC, 0x00);
        default:               return lv_color_make(0x44, 0x44, 0x44);
    }
}

static lv_obj_t *s_screen = nullptr;
static lv_obj_t *s_title  = nullptr;
static lv_obj_t *s_banner = nullptr;
static lv_obj_t *s_banner_lbl = nullptr;
static lv_obj_t *s_list   = nullptr;   // scrollable flex-column of rows
static lv_timer_t *s_timer = nullptr;
static bool s_active = false;

#define TR_MAX_ROWS 16

static void add_row(const TrThreat *t)
{
    lv_obj_t *row = lv_obj_create(s_list);
    // A familiar vehicle (your own car/phone) is shown muted, never as a threat.
    lv_color_t accent = t->familiar ? lv_color_make(0x55, 0x66, 0x55)
                                     : lvl_color(t->level);
    lv_obj_set_size(row, 380, 84);
    lv_obj_set_style_bg_color(row, lv_color_make(0x14, 0x14, 0x14), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 10, LV_PART_MAIN);
    lv_obj_set_style_border_color(row, accent, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_left(row, 14, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    // Left accent stripe coloured by follow-level.
    lv_obj_t *bar = lv_obj_create(row);
    lv_obj_set_size(bar, 6, 52);
    lv_obj_set_style_radius(bar, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, accent, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, t->active ? LV_OPA_COVER : LV_OPA_40, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bar, 0, LV_PART_MAIN);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(bar, LV_ALIGN_LEFT_MID, -6, 0);

    // Headline: "AirTag - LIKELY" (greyed when contact has gone stale).
    // Separators here are ASCII on purpose: font_argus_label_* cover U+0020..U+007E
    // only, and LVGL's built-in Montserrat adds just U+00B0 and U+2022 on top of
    // ASCII. A "·" or an em dash renders as a tofu box on the device.
    lv_obj_t *head = lv_label_create(row);
    lv_obj_set_style_text_font(head, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(head,
        t->active ? accent : lv_color_make(0x77, 0x77, 0x77),
        LV_PART_MAIN);
    lv_label_set_text_fmt(head, "%s%s  -  %s%s",
        t->community ? LV_SYMBOL_BELL " " : "",   // flagged over the mesh
        threatradar_category_name(t->category),
        threatradar_level_name(t->level),
        t->familiar ? "  (yours)" : (t->active ? "" : "  (lost)"));
    lv_obj_align(head, LV_ALIGN_TOP_LEFT, 8, 2);

    // Metrics: distance travelled alongside, dwell minutes, waypoint count.
    lv_obj_t *m = lv_label_create(row);
    lv_obj_set_style_text_font(m, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(m, ARGUS_TEXT, LV_PART_MAIN);
    lv_label_set_text_fmt(m, LV_SYMBOL_GPS " %u m  -  %u min  -  %u pts",
        (unsigned)t->span_m, (unsigned)t->span_min, (unsigned)t->waypoints);
    lv_obj_align(m, LV_ALIGN_TOP_LEFT, 8, 30);

    // Footer: first-seen time, proximity (RSSI), MAC tail.
    lv_obj_t *f = lv_label_create(row);
    lv_obj_set_style_text_font(f, &font_argus_label_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(f, ARGUS_TEXT_DIM, LV_PART_MAIN);
    lv_label_set_text_fmt(f, "first %s  -  %d dBm  -  ..%02X:%02X:%02X",
        t->first_time, (int)t->best_rssi, t->mac[3], t->mac[4], t->mac[5]);
    lv_obj_align(f, LV_ALIGN_TOP_LEFT, 8, 54);
}

static void refresh()
{
    int top = threatradar_top_level();
    int n   = threatradar_threat_count();

    // Title tracks the live brand accent: HADES red once a tail is flagged,
    // calm steel-blue when clear (same threshold as the status bar).
    if (s_title) lv_obj_set_style_text_color(s_title, argus_accent(), LV_PART_MAIN);

    // Banner reflects the worst live contact. LIKELY+ paints the HADES alert red
    // (matching argus_accent()'s flip); a clear scope rests on the calm steel-blue
    // brand accent, so the whole screen shifts ARGUS -> HADES with the threat.
    lv_color_t bcol;
    const char *btext;
    if (top >= TR_LVL_LIKELY) {
        bcol  = HADES_RED;
        btext = LV_SYMBOL_WARNING "  SOMEONE IS FOLLOWING YOU";
    } else if (top == TR_LVL_POSSIBLE) {
        bcol  = lv_color_make(0xFF, 0xCC, 0x00);
        btext = LV_SYMBOL_EYE_OPEN "  Possible tail - keep watching";
    } else {
        bcol  = ARGUS_ACCENT;
        btext = LV_SYMBOL_OK "  Clear - nothing co-moving";
    }
    lv_obj_set_style_bg_color(s_banner, bcol, LV_PART_MAIN);
    lv_label_set_text(s_banner_lbl, btext);

    // Rebuild the list from the live store.
    lv_obj_clean(s_list);

    if (n == 0) {
        lv_obj_t *empty = lv_label_create(s_list);
        lv_obj_set_style_text_font(empty, &font_argus_label_16, LV_PART_MAIN);
        lv_obj_set_style_text_color(empty, ARGUS_TEXT_DIM, LV_PART_MAIN);
        lv_label_set_text(empty,
            "No co-moving devices.\n\n"
            "Trackers seen at a single spot\n"
            "are fixtures and don't count.\n"
            "Keep moving - a real tail builds\n"
            "score as it travels with you.");
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        return;
    }

    TrThreat threats[TR_MAX_ROWS];
    int got = threatradar_get_threats(threats, TR_MAX_ROWS);
    for (int i = 0; i < got; i++) add_row(&threats[i]);
}

static void on_timer(lv_timer_t *)
{
    if (s_active) refresh();
}

static void on_clear(lv_event_t *)
{
    threatradar_reset();
    refresh();
}

static void on_gesture(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    if (lv_indev_get_gesture_dir(indev) == LV_DIR_TOP) {
        s_active = false;
        tools_screen_show();
    }
}

void threat_radar_screen_create()
{
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_screen, on_gesture, LV_EVENT_GESTURE, NULL);

    // Title. Themed with the runtime accent so it flips to HADES red the moment
    // a tail is flagged (repainted each refresh() below) and returns to calm
    // steel-blue when clear.
    s_title = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_title, &font_argus_label_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_title, argus_accent(), LV_PART_MAIN);
    lv_label_set_text(s_title, "THREAT RADAR");
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, 28);

    // Status banner under the title.
    s_banner = lv_obj_create(s_screen);
    lv_obj_set_size(s_banner, 380, 52);
    lv_obj_set_style_radius(s_banner, 12, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_banner, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_banner, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_banner, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_banner, LV_ALIGN_TOP_MID, 0, 58);
    s_banner_lbl = lv_label_create(s_banner);
    lv_obj_set_style_text_font(s_banner_lbl, &font_argus_label_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_banner_lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_center(s_banner_lbl);

    // Scrollable list of contact cards.
    s_list = lv_obj_create(s_screen);
    lv_obj_set_size(s_list, 404, 320);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_list, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_row(s_list, 8, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_list, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(s_list, LV_ALIGN_TOP_MID, 0, 120);

    // CLEAR button (new trip / panic wipe).
    lv_obj_t *clr = lv_obj_create(s_screen);
    lv_obj_set_size(clr, 160, 44);
    lv_obj_set_style_radius(clr, 22, LV_PART_MAIN);
    lv_obj_set_style_bg_color(clr, lv_color_make(0x22, 0x22, 0x22), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(clr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(clr, lv_color_make(0x66, 0x66, 0x66), LV_PART_MAIN);
    lv_obj_set_style_border_width(clr, 1, LV_PART_MAIN);
    lv_obj_clear_flag(clr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(clr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(clr, LV_ALIGN_BOTTOM_MID, 0, -28);
    lv_obj_add_event_cb(clr, on_clear, LV_EVENT_CLICKED, NULL);
    lv_obj_t *clr_lbl = lv_label_create(clr);
    lv_obj_set_style_text_font(clr_lbl, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(clr_lbl, ARGUS_TEXT, LV_PART_MAIN);
    lv_label_set_text(clr_lbl, LV_SYMBOL_TRASH "  CLEAR");
    lv_obj_center(clr_lbl);
    lv_obj_add_flag(clr_lbl, LV_OBJ_FLAG_EVENT_BUBBLE);   // tap on label reaches the button

    // Slow self-refresh while the screen is in front.
    s_timer = lv_timer_create(on_timer, 1500, NULL);
}

void threat_radar_screen_show()
{
    if (!s_screen) threat_radar_screen_create();
    s_active = true;
    refresh();
    lv_scr_load(s_screen);
}

bool threat_radar_screen_is_active() { return s_active; }
