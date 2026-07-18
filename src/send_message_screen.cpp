#include "send_message_screen.h"
#include "theme.h"
#include "meshtastic.h"
#include "configuration_screen.h"
#include "nodes_screen.h"
#include "map_screen.h"
#include <LilyGoLib.h>
#include <SD.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// Defined in main.cpp
void main_loop_request_lvgl_priority(int cycles);

// One built-in preset that's always at the top of the list, plus up
// to MAX_CUSTOM user-typed messages that get persisted to SD on send.
// Cap at 8 so the scrollable list stays scannable on the 410-disc.
#define MAX_CUSTOM      8
#define CUSTOM_MAX_LEN  120
static const char *PRESET_MSG = "Hello Meshtastic!";
static const char *CUSTOM_FILE = "/Meshtastic/custom_messages.txt";

static lv_obj_t *send_screen;
static lv_obj_t *title_label;
static lv_obj_t *list_box;          // flex column of message-card buttons
static lv_obj_t *status_label;
static lv_obj_t *nav_hint;

// Compose overlay - textarea + keyboard. Hidden until the user taps
// the "+ Compose..." card at the bottom of the list. READY/CANCEL on
// the keyboard sends + dismisses; the typed message is added to the
// custom list AND persisted to SD so it appears as a preset next time.
static lv_obj_t *compose_ta;
static lv_obj_t *keyboard;

// Delete-custom-message confirmation modal. Long-press on a custom
// card pops this with [Cancel] [Delete] buttons. Tapping outside the
// inner panel falls through to Cancel.
static lv_obj_t *confirm_overlay;
static lv_obj_t *confirm_text;
static int       confirm_idx = -1;  // index into s_customs

// In-memory copy of the persisted custom messages. Loaded once at
// boot from CUSTOM_FILE; rewritten in full on each add/delete.
static char  s_customs[MAX_CUSTOM][CUSTOM_MAX_LEN];
static int   s_custom_count = 0;

// 0xFFFFFFFFu = broadcast (the default, what swipe-into-send sets). A
// specific node id when the caller went through send_message_screen_show_to
// (per-node DM tap from the Nodes screen). Reset back to broadcast each
// time the user leaves the send screen via swipe-nav.
static uint32_t   s_dest_node = 0xFFFFFFFFu;

// Most-recent DM we sent. Once meshtastic_send_text_to() returns true we
// look up the latest outgoing tracker entry, latch its packet_id here,
// and an lv_timer polls the tracker every second to update the status
// label as PENDING -> DELIVERED / NO_ACK / NAKED.
static uint32_t   s_watched_pkt_id   = 0;
static int        s_last_render_state = -1;
static lv_timer_t *s_ack_poll_timer  = nullptr;

// ---- forward decls --------------------------------------------------------
static void rebuild_list();
static void show_compose();
static void hide_compose();
static void show_confirm(int idx);
static void hide_confirm();

// ---- persistence ----------------------------------------------------------

// Read /Meshtastic/custom_messages.txt one line at a time into the
// s_customs ring. Tolerates CRLF / CR / LF and silently truncates
// over-long lines to CUSTOM_MAX_LEN-1. Capped at MAX_CUSTOM entries;
// extras on disk are dropped.
static void load_customs()
{
    s_custom_count = 0;
    if (!instance.isCardReady()) return;
    if (!SD.exists(CUSTOM_FILE)) return;
    File f = SD.open(CUSTOM_FILE, FILE_READ);
    if (!f) return;
    while (f.available() && s_custom_count < MAX_CUSTOM) {
        int n = 0;
        char *dst = s_customs[s_custom_count];
        while (f.available() && n < CUSTOM_MAX_LEN - 1) {
            int c = f.read();
            if (c < 0)    break;
            if (c == '\n') break;
            if (c == '\r') continue;
            dst[n++] = (char)c;
        }
        dst[n] = '\0';
        if (n > 0) s_custom_count++;
    }
    f.close();
}

static void save_customs()
{
    if (!instance.isCardReady()) return;
    if (!SD.exists("/Meshtastic")) SD.mkdir("/Meshtastic");
    File f = SD.open(CUSTOM_FILE, FILE_WRITE);
    if (!f) return;
    for (int i = 0; i < s_custom_count; i++) {
        f.printf("%s\n", s_customs[i]);
    }
    f.close();
}

// Append a freshly-typed message to the customs ring. Dedup against
// the existing entries (re-sending the same text shouldn't bloat the
// list) and against the built-in preset. Drops the oldest entry if
// we're at capacity.
static void add_custom(const char *text)
{
    if (!text || !text[0]) return;
    if (strcmp(text, PRESET_MSG) == 0) return;
    for (int i = 0; i < s_custom_count; i++) {
        if (strcmp(s_customs[i], text) == 0) return;
    }
    if (s_custom_count >= MAX_CUSTOM) {
        // Shift older entries down to make room at the tail.
        for (int i = 0; i < MAX_CUSTOM - 1; i++) {
            memcpy(s_customs[i], s_customs[i + 1], CUSTOM_MAX_LEN);
        }
        s_custom_count = MAX_CUSTOM - 1;
    }
    strncpy(s_customs[s_custom_count], text, CUSTOM_MAX_LEN - 1);
    s_customs[s_custom_count][CUSTOM_MAX_LEN - 1] = '\0';
    s_custom_count++;
    save_customs();
}

static void delete_custom(int idx)
{
    if (idx < 0 || idx >= s_custom_count) return;
    for (int i = idx; i < s_custom_count - 1; i++) {
        memcpy(s_customs[i], s_customs[i + 1], CUSTOM_MAX_LEN);
    }
    s_custom_count--;
    s_customs[s_custom_count][0] = '\0';
    save_customs();
}

// ---- send + title ---------------------------------------------------------

static const char *short_name_for(uint32_t dest)
{
    int n = meshtastic_get_node_count();
    for (int i = 0; i < n; i++) {
        const MeshNode *m = meshtastic_get_node(i);
        if (m && m->node_id == dest && m->short_name[0]) return m->short_name;
    }
    return "????";
}

static void update_title()
{
    if (!title_label) return;
    if (s_dest_node != 0xFFFFFFFFu) {
        char buf[48];
        snprintf(buf, sizeof(buf), "DM -> [%s]",
                 short_name_for(s_dest_node));
        lv_label_set_text(title_label, buf);
        lv_obj_set_style_text_color(title_label,
            lv_color_make(0xB8, 0xA4, 0xFF), LV_PART_MAIN);
    } else {
        lv_label_set_text(title_label, "SEND MESSAGE");
        lv_obj_set_style_text_color(title_label, lv_color_white(), LV_PART_MAIN);
    }
}

static void show_send_status(const char *text, bool ok)
{
    if (!status_label) return;
    lv_label_set_text(status_label, text);
    lv_obj_set_style_text_color(status_label,
        ok ? ARGUS_ACCENT : lv_color_make(0xCC, 0x44, 0x44),
        LV_PART_MAIN);
}

static void send_text(const char *text)
{
    if (!meshtastic_send_text_to(text, s_dest_node)) {
        show_send_status("Enable LoRa first", false);
        return;
    }
    if (s_dest_node != 0xFFFFFFFFu) {
        char buf[40];
        snprintf(buf, sizeof(buf), "DM sending... !%08lx",
                 (unsigned long)s_dest_node);
        show_send_status(buf, true);
    } else {
        // Broadcasts don't have ACKs - they're done as soon as TX
        // queues, so the label can read "sent" immediately.
        show_send_status("Message sent", true);
    }
    // Latch the just-queued message's pkt_id so the ACK poll knows
    // which tracker entry to watch. bg_tick generates pkt_id at TX
    // time, which can land between the send_text_to() call and the
    // next poll; the poll handles a not-yet-stamped slot gracefully.
    s_watched_pkt_id    = 0;
    s_last_render_state = -1;
    const MeshOutgoing *o = meshtastic_get_outgoing(0);
    if (o && strncmp(o->text, text, sizeof(o->text)) == 0) {
        s_watched_pkt_id = o->packet_id;
    }
}

// Polled by an lv_timer every 1 s. Watches the most-recent outgoing
// tracker entry by pkt_id and updates the status label whenever its
// state transitions. Cheap when there's nothing watched.
static void on_ack_poll(lv_timer_t *)
{
    if (s_watched_pkt_id == 0) {
        // First scan after a DM send - bg_tick may have just stamped
        // the pkt_id onto the tracker. Look for our just-queued entry.
        const MeshOutgoing *o = meshtastic_get_outgoing(0);
        if (!o || o->state != MeshOutgoing::PENDING) return;
        s_watched_pkt_id = o->packet_id;
    }
    int n = meshtastic_get_outgoing_count();
    for (int i = 0; i < n; i++) {
        const MeshOutgoing *o = meshtastic_get_outgoing(i);
        if (!o || o->packet_id != s_watched_pkt_id) continue;
        if ((int)o->state == s_last_render_state) return;
        s_last_render_state = (int)o->state;
        switch (o->state) {
            case MeshOutgoing::PENDING:
                // First render happens via send_text(); no need to
                // overwrite with the same text here.
                break;
            case MeshOutgoing::DELIVERED:
                show_send_status("Delivered", true);
                break;
            case MeshOutgoing::NO_ACK:
                show_send_status("Sent - no ack received", false);
                break;
            case MeshOutgoing::NAKED: {
                char buf[40];
                snprintf(buf, sizeof(buf), "NAK: %s",
                         meshtastic_routing_error_name(o->error_reason));
                show_send_status(buf, false);
                break;
            }
        }
        return;
    }
}

// ---- event handlers -------------------------------------------------------

// Card click. user_data carries idx: -1 = preset, -2 = compose card,
// otherwise idx into s_customs.
static void on_card_clicked(lv_event_t *e)
{
    auto target = (lv_obj_t *)lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(target);
    if (idx == -1)       send_text(PRESET_MSG);
    else if (idx == -2)  show_compose();
    else if (idx >= 0 && idx < s_custom_count) send_text(s_customs[idx]);
}

// Long-press a custom card -> ask to delete it. Built-in preset and
// the compose card don't have deletes (user_data >= 0 path only).
static void on_card_long_pressed(lv_event_t *e)
{
    auto target = (lv_obj_t *)lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(target);
    if (idx < 0) return;     // preset / compose - nothing to delete
    show_confirm(idx);
}

static void on_kb_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {
        const char *text = lv_textarea_get_text(compose_ta);
        if (text && text[0]) {
            send_text(text);
            add_custom(text);
        }
        hide_compose();
        rebuild_list();
    } else if (code == LV_EVENT_CANCEL) {
        hide_compose();
    }
}

static void on_confirm_delete(lv_event_t *)
{
    int idx = confirm_idx;
    hide_confirm();
    if (idx >= 0) {
        delete_custom(idx);
        rebuild_list();
    }
}

static void on_confirm_cancel(lv_event_t *)
{
    hide_confirm();
}

// Tap on the dim backdrop dismisses the confirmation (not on the
// inner panel - that one swallows clicks so the buttons handle them).
static void on_confirm_backdrop(lv_event_t *e)
{
    auto target = (lv_obj_t *)lv_event_get_target(e);
    if (target == confirm_overlay) hide_confirm();
}

static void on_gesture(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir == LV_DIR_LEFT) {
        nodes_screen_show();
    } else if (dir == LV_DIR_RIGHT) {
        if (map_screen_available()) map_screen_show();
        else                        configuration_screen_show();
    }
}

// ---- list rendering -------------------------------------------------------

static lv_obj_t *make_card(lv_obj_t *parent, const char *text, int user_idx,
                           lv_color_t bg, lv_color_t border)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(card, border, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 12, LV_PART_MAIN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(card, (void *)(intptr_t)user_idx);
    lv_obj_add_event_cb(card, on_card_clicked,      LV_EVENT_CLICKED,      NULL);
    lv_obj_add_event_cb(card, on_card_long_pressed, LV_EVENT_LONG_PRESSED, NULL);

    lv_obj_t *lbl = lv_label_create(card);
    lv_obj_set_width(lbl, lv_pct(100));
    lv_obj_set_style_text_color(lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_label_set_text(lbl, text);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    return card;
}

static void rebuild_list()
{
    if (!list_box) return;
    lv_obj_clean(list_box);

    // Preset card - hardcoded text, never deletable.
    make_card(list_box, PRESET_MSG, -1,
              lv_color_make(0x11, 0x11, 0x11),
              lv_color_make(0x33, 0x33, 0x33));

    // Custom cards - long-press deletes (with confirmation).
    for (int i = 0; i < s_custom_count; i++) {
        make_card(list_box, s_customs[i], i,
                  lv_color_make(0x1A, 0x1A, 0x22),
                  lv_color_make(0x44, 0x44, 0x5A));
    }

    // Compose card at the bottom - visually distinct, opens the
    // textarea + keyboard overlay on tap.
    lv_obj_t *compose_card = make_card(list_box, "+ Compose...", -2,
                                       lv_color_make(0x10, 0x20, 0x30),
                                       lv_color_make(0x0A, 0x84, 0xFF));
    lv_obj_t *lbl = lv_obj_get_child(compose_card, 0);
    if (lbl) lv_obj_set_style_text_color(lbl,
        lv_color_make(0x0A, 0x84, 0xFF), LV_PART_MAIN);
}

// ---- overlays -------------------------------------------------------------

static void show_compose()
{
    lv_obj_add_flag(list_box,     LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(status_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(nav_hint,     LV_OBJ_FLAG_HIDDEN);
    lv_textarea_set_text(compose_ta, "");
    lv_obj_clear_flag(compose_ta, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(keyboard,   LV_OBJ_FLAG_HIDDEN);
    lv_keyboard_set_textarea(keyboard, compose_ta);
    lv_obj_move_foreground(keyboard);
}

static void hide_compose()
{
    lv_obj_add_flag(compose_ta, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(keyboard,   LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(list_box,     LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(status_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(nav_hint,     LV_OBJ_FLAG_HIDDEN);
}

static void show_confirm(int idx)
{
    if (!confirm_overlay) return;
    confirm_idx = idx;
    char buf[CUSTOM_MAX_LEN + 32];
    snprintf(buf, sizeof(buf), "Delete this message?\n\n\"%s\"", s_customs[idx]);
    lv_label_set_text(confirm_text, buf);
    lv_obj_clear_flag(confirm_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(confirm_overlay);
}

static void hide_confirm()
{
    if (confirm_overlay) lv_obj_add_flag(confirm_overlay, LV_OBJ_FLAG_HIDDEN);
    confirm_idx = -1;
}

// ---- screen build ---------------------------------------------------------

void send_message_screen_create()
{
    send_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(send_screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(send_screen, 0, LV_PART_MAIN);

    // Title - re-titled to "DM -> [SHRT]" for pre-targeted DMs.
    // Title + list + hint geometry mirrors the Meshtastic + Nodes
    // screens so all three feel visually identical when swiping.
    title_label = lv_label_create(send_screen);
    lv_obj_set_style_text_color(title_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_36, LV_PART_MAIN);
    lv_label_set_text(title_label, "SEND MESSAGE");
    lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 21);

    // Scrollable list of message cards. Preset is always at the top,
    // typed customs in the middle, "+ Compose..." card at the bottom.
    // Same 360x340 box at y=87 the Meshtastic / Nodes screens use.
    list_box = lv_obj_create(send_screen);
    lv_obj_set_size(list_box, 360, 340);
    lv_obj_align(list_box, LV_ALIGN_TOP_MID, 0, 87);
    lv_obj_set_style_bg_color(list_box, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(list_box, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(list_box, 4, LV_PART_MAIN);
    lv_obj_set_scroll_dir(list_box, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list_box, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_layout(list_box, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(list_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list_box, 6, LV_PART_MAIN);

    status_label = lv_label_create(send_screen);
    lv_obj_set_style_text_color(status_label, ARGUS_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_label_set_text(status_label, "");
    lv_obj_align(status_label, LV_ALIGN_BOTTOM_MID, 0, -55);

    nav_hint = lv_label_create(send_screen);
    lv_obj_set_style_text_color(nav_hint, lv_color_make(0x44, 0x44, 0x44), LV_PART_MAIN);
    lv_obj_set_style_text_font(nav_hint, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_align(nav_hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(nav_hint, "Tap: send   hold custom: delete   swipe: nav");
    lv_obj_align(nav_hint, LV_ALIGN_BOTTOM_MID, 0, -42);

    // Compose overlay - textarea + standard LVGL keyboard. Hidden by
    // default; shown when the "+ Compose..." card is tapped. The
    // keyboard is rooted on the screen (not the list) so it stacks
    // above the cards via lv_obj_move_foreground.
    compose_ta = lv_textarea_create(send_screen);
    lv_obj_set_size(compose_ta, 400, 100);
    lv_obj_align(compose_ta, LV_ALIGN_TOP_MID, 0, 56);
    lv_textarea_set_max_length(compose_ta, CUSTOM_MAX_LEN - 1);
    lv_textarea_set_placeholder_text(compose_ta, "Type your message...");
    lv_obj_set_style_text_font(compose_ta, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_bg_color(compose_ta, lv_color_make(0x11, 0x11, 0x11), LV_PART_MAIN);
    lv_obj_set_style_text_color(compose_ta, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_border_color(compose_ta, lv_color_make(0x44, 0x44, 0x44), LV_PART_MAIN);
    lv_obj_set_style_border_width(compose_ta, 1, LV_PART_MAIN);
    lv_obj_add_flag(compose_ta, LV_OBJ_FLAG_HIDDEN);

    keyboard = lv_keyboard_create(send_screen);
    lv_obj_set_size(keyboard, 410, 240);
    lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(keyboard, on_kb_event, LV_EVENT_READY,  NULL);
    lv_obj_add_event_cb(keyboard, on_kb_event, LV_EVENT_CANCEL, NULL);

    // Delete confirmation modal - dim backdrop covering the screen,
    // inner panel with text + Cancel/Delete buttons.
    confirm_overlay = lv_obj_create(send_screen);
    lv_obj_set_size(confirm_overlay, 410, 502);
    lv_obj_align(confirm_overlay, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(confirm_overlay, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(confirm_overlay, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_border_width(confirm_overlay, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(confirm_overlay, 0, LV_PART_MAIN);
    lv_obj_clear_flag(confirm_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(confirm_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(confirm_overlay, on_confirm_backdrop,
                        LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(confirm_overlay, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *confirm_panel = lv_obj_create(confirm_overlay);
    lv_obj_set_size(confirm_panel, 340, 220);
    lv_obj_align(confirm_panel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(confirm_panel, lv_color_make(0x1C, 0x1C, 0x1E), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(confirm_panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(confirm_panel, 16, LV_PART_MAIN);
    lv_obj_set_style_border_color(confirm_panel, lv_color_make(0x44, 0x44, 0x48), LV_PART_MAIN);
    lv_obj_set_style_border_width(confirm_panel, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_all(confirm_panel, 16, LV_PART_MAIN);
    lv_obj_add_flag(confirm_panel, LV_OBJ_FLAG_CLICKABLE);  // swallow backdrop taps

    confirm_text = lv_label_create(confirm_panel);
    lv_obj_set_width(confirm_text, lv_pct(100));
    lv_obj_set_style_text_color(confirm_text, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(confirm_text, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_align(confirm_text, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(confirm_text, "Delete this message?");
    lv_label_set_long_mode(confirm_text, LV_LABEL_LONG_WRAP);
    lv_obj_align(confirm_text, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *cancel_btn = lv_button_create(confirm_panel);
    lv_obj_set_size(cancel_btn, 140, 56);
    lv_obj_align(cancel_btn, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_radius(cancel_btn, 10, LV_PART_MAIN);
    lv_obj_set_style_bg_color(cancel_btn, lv_color_make(0x3A, 0x3A, 0x3C), LV_PART_MAIN);
    lv_obj_add_event_cb(cancel_btn, on_confirm_cancel, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(cancel_btn);
    lv_label_set_text(cl, "Cancel");
    lv_obj_set_style_text_color(cl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(cl, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_center(cl);

    lv_obj_t *del_btn = lv_button_create(confirm_panel);
    lv_obj_set_size(del_btn, 140, 56);
    lv_obj_align(del_btn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_radius(del_btn, 10, LV_PART_MAIN);
    lv_obj_set_style_bg_color(del_btn, lv_color_make(0xFF, 0x3B, 0x30), LV_PART_MAIN);
    lv_obj_add_event_cb(del_btn, on_confirm_delete, LV_EVENT_CLICKED, NULL);
    lv_obj_t *dl = lv_label_create(del_btn);
    lv_label_set_text(dl, "Delete");
    lv_obj_set_style_text_color(dl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(dl, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_center(dl);

    lv_obj_add_event_cb(send_screen, on_gesture, LV_EVENT_GESTURE, NULL);

    // 1 Hz ACK poll - cheap when nothing is watched. Stays alive
    // across screen swaps; the handler bails when s_watched_pkt_id is
    // 0 (no in-flight DM).
    s_ack_poll_timer = lv_timer_create(on_ack_poll, 1000, NULL);

    // Pull the persisted custom messages and seed the list. Safe to
    // call when SD isn't mounted - just leaves s_custom_count at 0.
    load_customs();
    rebuild_list();
}

void send_message_screen_show()
{
    main_loop_request_lvgl_priority(12);
    s_dest_node = 0xFFFFFFFFu;
    update_title();
    lv_label_set_text(status_label, "");
    // Make sure any half-open compose / confirm overlay is dismissed
    // when re-entering via swipe (e.g. user typed, swiped away mid-edit).
    hide_compose();
    hide_confirm();
    rebuild_list();
    lv_scr_load(send_screen);
}

void send_message_screen_show_to(uint32_t dest_node)
{
    main_loop_request_lvgl_priority(12);
    s_dest_node = dest_node;
    update_title();
    lv_label_set_text(status_label, "");
    hide_compose();
    hide_confirm();
    rebuild_list();
    lv_scr_load(send_screen);
}

bool send_message_screen_is_active()
{
    return lv_screen_active() == send_screen;
}
