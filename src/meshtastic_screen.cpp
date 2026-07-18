#include "meshtastic_screen.h"
#include "theme.h"
#include "meshtastic.h"
#include "nodes_screen.h"
#include "send_message_screen.h"
#include <LilyGoLib.h>
#include <math.h>
#include <stdio.h>

// Defined in main.cpp
void clock_screen_show();
void main_loop_request_lvgl_priority(int cycles);

static lv_obj_t *mesh_screen;
static lv_obj_t *msg_list;

// "Clear All" link at the bottom of the screen. Hidden when there
// are no messages so it doesn't tease a tap with nothing to clear;
// rebuild_list() flips visibility based on get_count().
static lv_obj_t *clear_link;

// Delete-message confirmation modal. Long-press on an entry pops
// this with the message preview + [Cancel] [Delete] buttons. Tap on
// the dim backdrop falls through to Cancel. confirm_idx remembers
// which slot the user is being asked to delete.
static lv_obj_t *confirm_overlay;
static lv_obj_t *confirm_text;
static int       confirm_idx = -1;

// Forward decls - the long-press / clear handlers need to call into
// rebuild_list + the modal helpers below.
static void rebuild_list();
static void show_confirm(int idx);
static void hide_confirm();

static void on_gesture(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir == LV_DIR_LEFT)
        clock_screen_show();
    else if (dir == LV_DIR_RIGHT)
        nodes_screen_show();
}

// Tap the "Clear All" link - wipes the in-memory ring. SD archive at
// /Meshtastic/Messages/<timestamp>.txt is left intact, so every
// received packet stays on the card forever.
static void on_clear_pressed(lv_event_t *)
{
    meshtastic_clear_messages();
    rebuild_list();
}

// Long-press a message entry -> show the delete confirmation modal.
// user_data carries the slot index that was passed when the entry
// was created.
static void on_msg_long_pressed(lv_event_t *e)
{
    auto target = (lv_obj_t *)lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(target);
    show_confirm(idx);
}

// Short click on a message entry -> reply to its sender. Opens the
// Send screen pre-targeted at the from_node, which re-titles to
// "DM -> [SHRT]" automatically. CLICKED is mutually exclusive with
// LONG_PRESSED in LVGL, so a held finger fires the delete dialog
// while a tap fires the reply.
static void on_msg_clicked(lv_event_t *e)
{
    auto target = (lv_obj_t *)lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(target);
    const MeshMessage *m = meshtastic_get_message(idx);
    if (!m) return;
    // Don't try to DM ourselves (e.g. the message-store path could
    // theoretically end up with our own node id; defensive). Also
    // don't DM the broadcast pseudo-address.
    if (m->from_node == 0 || m->from_node == 0xFFFFFFFFu) return;
    if (m->from_node == meshtastic_get_node_id()) return;
    send_message_screen_show_to(m->from_node);
}

static void on_confirm_delete(lv_event_t *)
{
    int idx = confirm_idx;
    hide_confirm();
    if (idx >= 0) {
        meshtastic_delete_message(idx);
        rebuild_list();
    }
}

static void on_confirm_cancel(lv_event_t *)
{
    hide_confirm();
}

// Tap on the dim backdrop = cancel. The inner panel swallows clicks
// so its buttons handle their own events.
static void on_confirm_backdrop(lv_event_t *e)
{
    auto target = (lv_obj_t *)lv_event_get_target(e);
    if (target == confirm_overlay) hide_confirm();
}

static void make_message_entry(lv_obj_t *parent, const MeshMessage *msg, int idx)
{
    // A message is "to me" when its OTA dest matches our derived node id.
    // Anything else (0xFFFFFFFF broadcast or another node's id we happened
    // to overhear) is channel chatter.
    bool to_me = (msg->dest_node != 0xFFFFFFFFu &&
                  msg->dest_node == meshtastic_get_node_id());

    lv_obj_t *entry = lv_obj_create(parent);
    lv_obj_set_width(entry, lv_pct(100));
    lv_obj_set_height(entry, LV_SIZE_CONTENT);
    // Tap = reply-to-sender (opens Send pre-targeted at from_node).
    // Long-press = delete confirmation modal. The slot index is
    // stashed on the entry's user_data so both handlers can address
    // the right MeshMessage without a per-row closure. LVGL fires
    // LONG_PRESSED instead of CLICKED when the finger lingers, so
    // the two actions don't conflict.
    lv_obj_add_flag(entry, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(entry, (void *)(intptr_t)idx);
    lv_obj_add_event_cb(entry, on_msg_clicked,       LV_EVENT_CLICKED,      NULL);
    lv_obj_add_event_cb(entry, on_msg_long_pressed,  LV_EVENT_LONG_PRESSED, NULL);
    // DMs to us get a dim purple tint + a thicker left accent so they
    // stand out from broadcast chatter in the message list.
    lv_obj_set_style_bg_color(entry,
        to_me ? lv_color_make(0x1A, 0x11, 0x30) : lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(entry, LV_OPA_COVER, LV_PART_MAIN);
    if (to_me) {
        lv_obj_set_style_border_side(entry,
            (lv_border_side_t)(LV_BORDER_SIDE_BOTTOM | LV_BORDER_SIDE_LEFT),
            LV_PART_MAIN);
        lv_obj_set_style_border_color(entry, lv_color_make(0x58, 0x56, 0xD6), LV_PART_MAIN);
        lv_obj_set_style_border_width(entry, 3, LV_PART_MAIN);
    } else {
        lv_obj_set_style_border_side(entry, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
        lv_obj_set_style_border_color(entry, lv_color_make(0x33, 0x33, 0x33), LV_PART_MAIN);
        lv_obj_set_style_border_width(entry, 1, LV_PART_MAIN);
    }
    lv_obj_set_style_pad_all(entry, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_row(entry, 3, LV_PART_MAIN);
    lv_obj_set_layout(entry, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(entry, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(entry, LV_OBJ_FLAG_SCROLLABLE);

    // Sender + timestamp line. DMs get a "DM" prefix and render in
    // purple instead of green so they're scannable in a busy list.
    const char *prefix = to_me ? "DM " : "";
    char header[40];
    snprintf(header, sizeof(header), "%s!%08lx   %s",
             prefix, (unsigned long)msg->from_node, msg->time_str);
    lv_obj_t *hdr = lv_label_create(entry);
    lv_obj_set_width(hdr, lv_pct(100));
    lv_obj_set_style_text_color(hdr,
        to_me ? lv_color_make(0xB8, 0xA4, 0xFF) : ARGUS_ACCENT,
        LV_PART_MAIN);
    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_label_set_text(hdr, header);
    // Match the Nodes-screen header treatment: LONG_DOT shows "..."
    // for overflow instead of hard-clipping into the bezel.
    lv_label_set_long_mode(hdr, LV_LABEL_LONG_DOT);

    // Message text (white, normal)
    lv_obj_t *txt = lv_label_create(entry);
    lv_obj_set_width(txt, lv_pct(100));
    lv_obj_set_style_text_color(txt, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(txt, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_label_set_text(txt, msg->text);
    lv_label_set_long_mode(txt, LV_LABEL_LONG_WRAP);

    // Link-quality footer: "RSSI -75 dBm  SNR 8.5 dB  Hops 1/3".
    // Only rendered when we have packet stats - NaN rssi means the
    // message came in before this firmware was tracking stats. With
    // hop_start = 0 the sender never set a hop budget; show just the
    // remaining limit so we still convey something useful.
    if (!isnan(msg->rssi)) {
        char qual[64];
        int  p = 0;
        p += snprintf(qual + p, sizeof(qual) - p, "RSSI %.0f dBm  SNR %.1f dB",
                      (double)msg->rssi, (double)msg->snr);
        if (msg->hop_start > 0) {
            uint8_t traversed = (msg->hop_start >= msg->hop_limit)
                ? (msg->hop_start - msg->hop_limit) : 0;
            p += snprintf(qual + p, sizeof(qual) - p, "  Hops %u/%u",
                          (unsigned)traversed, (unsigned)msg->hop_start);
        } else if (msg->hop_limit > 0) {
            p += snprintf(qual + p, sizeof(qual) - p, "  HopLimit %u",
                          (unsigned)msg->hop_limit);
        }
        lv_obj_t *q = lv_label_create(entry);
        lv_obj_set_width(q, lv_pct(100));
        lv_obj_set_style_text_color(q, lv_color_make(0x66, 0x66, 0x66), LV_PART_MAIN);
        lv_obj_set_style_text_font(q, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_label_set_text(q, qual);
        lv_label_set_long_mode(q, LV_LABEL_LONG_DOT);
    }
}

static void rebuild_list()
{
    lv_obj_clean(msg_list);
    int count = meshtastic_get_count();
    if (count == 0) {
        lv_obj_t *lbl = lv_label_create(msg_list);
        lv_obj_add_flag(lbl, LV_OBJ_FLAG_FLOATING);  // exclude from flex layout so center works
        lv_obj_set_style_text_color(lbl, lv_color_make(0x55, 0x55, 0x55), LV_PART_MAIN);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, LV_PART_MAIN);
        lv_label_set_text(lbl, "No messages yet.");
        lv_obj_center(lbl);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    } else {
        for (int i = 0; i < count; i++) {
            const MeshMessage *m = meshtastic_get_message(i);
            if (m) make_message_entry(msg_list, m, i);
        }
    }
    // Hide the Clear link when there's nothing to clear.
    if (clear_link) {
        if (count > 0) lv_obj_clear_flag(clear_link, LV_OBJ_FLAG_HIDDEN);
        else           lv_obj_add_flag  (clear_link, LV_OBJ_FLAG_HIDDEN);
    }
}

static void show_confirm(int idx)
{
    if (!confirm_overlay) return;
    const MeshMessage *m = meshtastic_get_message(idx);
    if (!m) return;
    confirm_idx = idx;
    // Quote the first ~60 chars of the body in the prompt so the user
    // can tell which message they're about to nuke. SD log line is
    // mentioned so they know it's not actually gone for good.
    char body_snip[80];
    strncpy(body_snip, m->text, sizeof(body_snip) - 1);
    body_snip[sizeof(body_snip) - 1] = '\0';
    if (strlen(m->text) >= sizeof(body_snip)) {
        body_snip[sizeof(body_snip) - 4] = '.';
        body_snip[sizeof(body_snip) - 3] = '.';
        body_snip[sizeof(body_snip) - 2] = '.';
        body_snip[sizeof(body_snip) - 1] = '\0';
    }
    char buf[160];
    snprintf(buf, sizeof(buf),
             "Delete this message?\n\n\"%s\"\n\nA copy stays on the SD log.",
             body_snip);
    lv_label_set_text(confirm_text, buf);
    lv_obj_clear_flag(confirm_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(confirm_overlay);
}

static void hide_confirm()
{
    if (confirm_overlay) lv_obj_add_flag(confirm_overlay, LV_OBJ_FLAG_HIDDEN);
    confirm_idx = -1;
}

void meshtastic_screen_create()
{
    mesh_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(mesh_screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(mesh_screen, 0, LV_PART_MAIN);

    // Title — font_montserrat_48 (~60 px tall) at y=21 (5 px lower
    // than the previous y=16 for a touch more bezel clearance).
    lv_obj_t *title = lv_label_create(mesh_screen);
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_label_set_text(title, "MESHTASTIC");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 21);

    // Message list — grown 5 px up + 10 px down vs prior 360x325@y=92,
    // so now 360x340 starting at y=87 and ending at y=427.
    msg_list = lv_obj_create(mesh_screen);
    lv_obj_set_size(msg_list, 360, 340);
    lv_obj_align(msg_list, LV_ALIGN_TOP_MID, 0, 87);
    lv_obj_set_style_bg_color(msg_list, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_color(msg_list, lv_color_make(0x33, 0x33, 0x33), LV_PART_MAIN);
    lv_obj_set_style_border_width(msg_list, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(msg_list, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(msg_list, 0, LV_PART_MAIN);
    lv_obj_set_scroll_dir(msg_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(msg_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_layout(msg_list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(msg_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(msg_list, 0, LV_PART_MAIN);

    // Clear All link - sits between the list and the swipe hint.
    // Red so it reads as destructive; hidden when the list is empty
    // (rebuild_list toggles visibility). The trash glyph at the front
    // is the standard LVGL Montserrat-bundled icon.
    clear_link = lv_label_create(mesh_screen);
    lv_label_set_text(clear_link, LV_SYMBOL_TRASH "  Clear All");
    lv_obj_set_style_text_color(clear_link, lv_color_make(0xFF, 0x3B, 0x30), LV_PART_MAIN);
    lv_obj_set_style_text_font(clear_link, &lv_font_montserrat_16, LV_PART_MAIN);
    // Sit just below the (now even taller) message list. Slid down
    // from -65 to -55 to clear msg_list's new bottom edge (y=427).
    lv_obj_align(clear_link, LV_ALIGN_BOTTOM_MID, 0, -55);
    lv_obj_add_flag(clear_link, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(clear_link, 16);
    lv_obj_add_event_cb(clear_link, on_clear_pressed, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(clear_link, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *hint = lv_label_create(mesh_screen);
    lv_obj_set_style_text_color(hint, lv_color_make(0x44, 0x44, 0x44), LV_PART_MAIN);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(hint, "Tap: reply   hold: delete   swipe: nav");
    // -22 (was -42) per request — drops the hint another 20 px
    // closer to the bottom edge.
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -22);

    // Delete confirmation modal - dim full-screen backdrop, inner
    // panel with body + Cancel/Delete buttons. Hidden until a
    // long-press fires show_confirm().
    confirm_overlay = lv_obj_create(mesh_screen);
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
    lv_obj_set_size(confirm_panel, 340, 260);
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
    lv_obj_set_style_text_font(confirm_text, &lv_font_montserrat_14, LV_PART_MAIN);
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

    lv_obj_add_event_cb(mesh_screen, on_gesture, LV_EVENT_GESTURE, NULL);

    rebuild_list();
}

void meshtastic_screen_show()
{
    main_loop_request_lvgl_priority(12);
    rebuild_list();
    lv_scr_load(mesh_screen);
}

bool meshtastic_screen_is_active()
{
    return lv_screen_active() == mesh_screen;
}

void meshtastic_screen_refresh()
{
    if (meshtastic_screen_is_active())
        rebuild_list();
}
