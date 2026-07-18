#include "aprs_screen.h"
#include "theme.h"
#include "aprs.h"
#include "lora_screen.h"
#include <LilyGoLib.h>
#include <string.h>

// Defined in tools_screen.cpp
void tools_screen_show();

static lv_obj_t *aprs_screen;
static lv_obj_t *status_label;
static lv_obj_t *call_ta;
static lv_obj_t *btn_row;
static lv_obj_t *start_btn,  *start_label;
static lv_obj_t *beacon_btn, *beacon_label;
static lv_obj_t *pkt_list;
static lv_obj_t *keyboard;

// Packet count last drawn into the list — the list is rebuilt only when it
// changes, not on every 1 s refresh tick.
static int      s_shown_count = -1;

// Transient status message (e.g. beacon feedback) shown until this timestamp.
static char     s_flash_msg[48];
static uint32_t s_flash_until = 0;
static bool     s_flash_warn  = false;

// ---- refresh ---------------------------------------------------------------

static void update_start_button()
{
    bool running = aprs_is_running();
    lv_label_set_text(start_label, running ? "STOP" : "START");
    lv_obj_set_style_bg_color(start_btn,
        running ? lv_color_make(0xCC, 0x00, 0x00)
                : lv_color_make(0x00, 0xAA, 0x44),
        LV_PART_MAIN);
    // Beacon is only meaningful while APRS holds the radio.
    lv_obj_set_style_bg_color(beacon_btn,
        running ? lv_color_make(0xCC, 0x77, 0x00)
                : lv_color_make(0x33, 0x33, 0x33),
        LV_PART_MAIN);
}

static void update_status()
{
    char buf[64];
    const char *text;
    lv_color_t  color;

    if (s_flash_until && millis() < s_flash_until) {
        text  = s_flash_msg;
        color = s_flash_warn ? lv_color_make(0xFF, 0xAA, 0x00)
                             : ARGUS_ACCENT;
    } else if (aprs_tx_busy() || aprs_tx_pending()) {
        text  = "Transmitting beacon...";
        color = lv_color_make(0xFF, 0xCC, 0x00);
    } else if (aprs_is_running()) {
        snprintf(buf, sizeof(buf), "Listening - %d packet%s",
                 aprs_get_packet_count(),
                 aprs_get_packet_count() == 1 ? "" : "s");
        text  = buf;
        color = ARGUS_ACCENT;
    } else if (lora_screen_is_powered()) {
        text  = "LoRa radio in use - stop Meshtastic";
        color = lv_color_make(0xFF, 0xAA, 0x00);
    } else if (aprs_last_error() != 0) {
        snprintf(buf, sizeof(buf), "Radio init failed (%d)", (int)aprs_last_error());
        text  = buf;
        color = lv_color_make(0xFF, 0x55, 0x55);
    } else {
        text  = "Stopped";
        color = lv_color_make(0x88, 0x88, 0x88);
    }

    lv_label_set_text(status_label, text);
    lv_obj_set_style_text_color(status_label, color, LV_PART_MAIN);
}

static void add_packet_card(const AprsPacket *p)
{
    lv_obj_t *card = lv_obj_create(pkt_list);
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, lv_color_make(0x14, 0x14, 0x14), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(card, lv_color_make(0x33, 0x33, 0x33), LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_row(card, 2, LV_PART_MAIN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(card, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *hdr = lv_label_create(card);
    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(hdr, lv_color_make(0x00, 0xCC, 0xCC), LV_PART_MAIN);
    lv_label_set_text_fmt(hdr, "%s   %s   %.0f dBm",
        p->time_str, p->source[0] ? p->source : "?", p->rssi);

    lv_obj_t *info = lv_label_create(card);
    lv_obj_set_width(info, lv_pct(100));
    lv_label_set_long_mode(info, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(info, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(info, lv_color_white(), LV_PART_MAIN);
    lv_label_set_text(info, p->info[0] ? p->info : p->raw);
}

static void rebuild_list()
{
    int n = aprs_get_packet_count();
    if (n == s_shown_count) return;
    s_shown_count = n;

    lv_obj_clean(pkt_list);
    if (n == 0) {
        lv_obj_t *empty = lv_label_create(pkt_list);
        lv_obj_set_style_text_color(empty, lv_color_make(0x55, 0x55, 0x55), LV_PART_MAIN);
        lv_obj_set_style_text_font(empty, &lv_font_montserrat_16, LV_PART_MAIN);
        lv_label_set_text(empty, "No packets received");
        // float out of any list flex flow and centre absolutely in the box.
        lv_obj_add_flag(empty, LV_OBJ_FLAG_FLOATING);
        lv_obj_center(empty);
        return;
    }
    for (int i = 0; i < n; i++) {
        const AprsPacket *p = aprs_get_packet(i);   // 0 = newest, shown first
        if (p) add_packet_card(p);
    }
}

static void refresh()
{
    update_start_button();
    update_status();
    rebuild_list();
}

static void on_refresh(lv_timer_t *)
{
    // refresh() rebuilds the packet card list (potentially dozens of
    // lv_obj_create calls) and re-stringifies the status line; both are
    // pure UI work so we skip the whole tick when the screen isn't
    // visible. Incoming APRS packets are still drained and persisted by
    // aprs_bg_tick() in the main loop, independent of this screen timer.
    if (!aprs_screen_is_active()) return;
    refresh();
}

// ---- keyboard / callsign editing -------------------------------------------

static void show_keyboard()
{
    lv_keyboard_set_textarea(keyboard, call_ta);
    lv_obj_clear_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(btn_row,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(pkt_list, LV_OBJ_FLAG_HIDDEN);
}

static void hide_keyboard()
{
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(btn_row,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(pkt_list, LV_OBJ_FLAG_HIDDEN);
}

// Push the textarea contents into the APRS module (uppercased + saved there),
// then mirror the normalised callsign back into the field.
static void commit_callsign()
{
    aprs_set_callsign(lv_textarea_get_text(call_ta));
    lv_textarea_set_text(call_ta, aprs_get_callsign());
}

static void on_ta_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_FOCUSED) {
        show_keyboard();
    } else if (code == LV_EVENT_DEFOCUSED) {
        commit_callsign();
        hide_keyboard();
    }
}

static void on_kb_event(lv_event_t *e)
{
    // READY (checkmark) or CANCEL — finish editing.
    commit_callsign();
    lv_obj_t *ta = lv_keyboard_get_textarea(keyboard);
    if (ta) lv_obj_clear_state(ta, LV_STATE_FOCUSED);
    hide_keyboard();
}

// ---- buttons ---------------------------------------------------------------

static void on_start_clicked(lv_event_t *)
{
    if (aprs_is_running()) aprs_stop();
    else                   aprs_start();   // refresh() reports any failure
    refresh();
}

static void on_beacon_clicked(lv_event_t *)
{
    if (!aprs_is_running()) {
        strcpy(s_flash_msg, "Start APRS before beaconing");
        s_flash_warn = true;
    } else if (aprs_send_position("T-Watch Ultra")) {
        strcpy(s_flash_msg, "Position beacon queued");
        s_flash_warn = false;
    } else {
        strcpy(s_flash_msg, "No GPS fix - cannot beacon");
        s_flash_warn = true;
    }
    s_flash_until = millis() + 4000;
    refresh();
}

// ---- layout ----------------------------------------------------------------

static lv_obj_t *make_button(lv_obj_t *parent, const char *text,
                             lv_coord_t w, lv_coord_t h, lv_color_t bg,
                             lv_obj_t **label_out)
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
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_center(lbl);
    if (label_out) *label_out = lbl;
    return btn;
}

void aprs_screen_create()
{
    aprs_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(aprs_screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(aprs_screen, 0, LV_PART_MAIN);
    lv_obj_clear_flag(aprs_screen, LV_OBJ_FLAG_SCROLLABLE);

    // Title — font_48 to match the PAGER / TPMS / SETTINGS headers.
    lv_obj_t *title = lv_label_create(aprs_screen);
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_label_set_text(title, "LoRa APRS");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    // Subtitle — names the band so it's clear this is LoRa APRS.
    // All rows below this point sit 16 px lower than they used to so the
    // subtitle clears the font_48 title (which extends to ~y=58 from its
    // y=10 origin).
    lv_obj_t *subtitle = lv_label_create(aprs_screen);
    lv_obj_set_style_text_color(subtitle, lv_color_make(0x77, 0x77, 0x77), LV_PART_MAIN);
    lv_obj_set_style_text_font(subtitle, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_label_set_text(subtitle, "433.775 MHz");
    lv_obj_align(subtitle, LV_ALIGN_TOP_MID, 0, 62);

    // Status line
    status_label = lv_label_create(aprs_screen);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(status_label, lv_color_make(0x88, 0x88, 0x88), LV_PART_MAIN);
    lv_label_set_text(status_label, "Stopped");
    lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 82);

    // Callsign field — label left, one-line text area right
    lv_obj_t *call_lbl = lv_label_create(aprs_screen);
    lv_obj_set_style_text_color(call_lbl, lv_color_make(0xAA, 0xAA, 0xAA), LV_PART_MAIN);
    lv_obj_set_style_text_font(call_lbl, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_label_set_text(call_lbl, "Callsign");
    lv_obj_align(call_lbl, LV_ALIGN_TOP_LEFT, 14, 116);

    call_ta = lv_textarea_create(aprs_screen);
    lv_textarea_set_one_line(call_ta, true);
    lv_textarea_set_max_length(call_ta, APRS_CALLSIGN_MAX - 1);
    lv_obj_set_size(call_ta, 240, 44);
    lv_obj_align(call_ta, LV_ALIGN_TOP_RIGHT, -14, 108);
    lv_obj_set_style_text_font(call_ta, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_bg_color(call_ta, lv_color_make(0x11, 0x11, 0x11), LV_PART_MAIN);
    lv_obj_set_style_text_color(call_ta, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_border_color(call_ta, lv_color_make(0x44, 0x44, 0x44), LV_PART_MAIN);
    lv_obj_set_style_border_width(call_ta, 1, LV_PART_MAIN);
    lv_obj_add_event_cb(call_ta, on_ta_event, LV_EVENT_FOCUSED,   NULL);
    lv_obj_add_event_cb(call_ta, on_ta_event, LV_EVENT_DEFOCUSED, NULL);

    // Start / Beacon button row
    btn_row = lv_obj_create(aprs_screen);
    lv_obj_set_size(btn_row, 404, 60);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn_row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(btn_row, LV_ALIGN_TOP_MID, 0, 166);

    start_btn = make_button(btn_row, "START", 186, 54,
                            lv_color_make(0x00, 0xAA, 0x44), &start_label);
    lv_obj_align(start_btn, LV_ALIGN_LEFT_MID, 6, 0);
    lv_obj_add_event_cb(start_btn, on_start_clicked, LV_EVENT_CLICKED, NULL);

    beacon_btn = make_button(btn_row, "BEACON", 186, 54,
                             lv_color_make(0x33, 0x33, 0x33), &beacon_label);
    lv_obj_align(beacon_btn, LV_ALIGN_RIGHT_MID, -6, 0);
    lv_obj_add_event_cb(beacon_btn, on_beacon_clicked, LV_EVENT_CLICKED, NULL);

    // Received-packet list — scrolling column of cards
    pkt_list = lv_obj_create(aprs_screen);
    lv_obj_set_size(pkt_list, 404, 280);
    lv_obj_align(pkt_list, LV_ALIGN_TOP_MID, 0, 234);
    lv_obj_set_style_bg_color(pkt_list, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_color(pkt_list, lv_color_make(0x33, 0x33, 0x33), LV_PART_MAIN);
    lv_obj_set_style_border_width(pkt_list, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(pkt_list, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(pkt_list, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_row(pkt_list, 6, LV_PART_MAIN);
    lv_obj_set_scroll_dir(pkt_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(pkt_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_layout(pkt_list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(pkt_list, LV_FLEX_FLOW_COLUMN);

    // On-screen keyboard — hidden until the callsign field is focused
    keyboard = lv_keyboard_create(aprs_screen);
    lv_obj_set_size(keyboard, 410, 240);
    lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_TEXT_UPPER);
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(keyboard, on_kb_event, LV_EVENT_READY,  NULL);
    lv_obj_add_event_cb(keyboard, on_kb_event, LV_EVENT_CANCEL, NULL);

    lv_timer_create(on_refresh, 1000, NULL);
}

void aprs_screen_show()
{
    lv_textarea_set_text(call_ta, aprs_get_callsign());
    hide_keyboard();
    s_flash_until = 0;
    s_shown_count = -1;     // force a list rebuild on entry
    refresh();
    lv_scr_load(aprs_screen);
}

bool aprs_screen_is_active()
{
    return lv_screen_active() == aprs_screen;
}
