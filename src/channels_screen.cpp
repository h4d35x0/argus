#include "channels_screen.h"
#include "theme.h"
#include "configuration_screen.h"
#include "meshtastic.h"
#include <LilyGoLib.h>
#include <stdio.h>

static lv_obj_t *ch_screen;
static lv_obj_t *list_root;

static void rebuild_list();

static void on_back_gesture(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    if (lv_indev_get_gesture_dir(indev) == LV_DIR_LEFT) {
        configuration_screen_show();
    }
}

// Tap a row -> mark it active. user_data holds the slot index. Slot
// must be enabled + non-empty to become active; the meshtastic
// backend enforces that and silently ignores the call otherwise.
static void on_row_clicked(lv_event_t *e)
{
    auto target = (lv_obj_t *)lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(target);
    meshtastic_set_active_channel(idx);
    rebuild_list();
}

// Switch handler. user_data holds the slot index. Enabling an empty
// slot auto-generates a PSK + default name (handled by the backend
// setter), which we want to reflect in the row's preview line - so we
// always rebuild the list after a toggle change rather than tweaking
// a single label.
static void on_switch_changed(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    auto sw  = (lv_obj_t *)lv_event_get_target(e);
    int idx  = (int)(intptr_t)lv_obj_get_user_data(sw);
    bool on  = lv_obj_has_state(sw, LV_STATE_CHECKED);
    meshtastic_set_channel_enabled(idx, on);
    rebuild_list();
}

static void make_channel_row(lv_obj_t *parent, int idx)
{
    const MeshChannel *ch = meshtastic_get_channel(idx);
    if (!ch) return;
    bool is_active = (idx == meshtastic_get_active_channel());

    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, lv_pct(100), 100);
    lv_obj_set_style_bg_color(card, lv_color_make(0x1C, 0x1C, 0x1E), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(card, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(card, 8, LV_PART_MAIN);
    lv_obj_set_style_border_color(card,
        is_active ? lv_color_make(0x34, 0xC7, 0x59)
                  : lv_color_make(0x3A, 0x3A, 0x3C), LV_PART_MAIN);
    lv_obj_set_style_border_width(card, is_active ? 2 : 1, LV_PART_MAIN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(card, (void *)(intptr_t)idx);
    lv_obj_add_event_cb(card, on_row_clicked, LV_EVENT_CLICKED, NULL);

    // Headline: name + "* active" suffix
    char headline[40];
    snprintf(headline, sizeof(headline), "%s%s",
             ch->name[0] ? ch->name : "(empty slot)",
             is_active ? "  \xe2\x80\xa2 active" : "");
    lv_obj_t *name_lbl = lv_label_create(card);
    lv_label_set_text(name_lbl, headline);
    lv_obj_set_style_text_color(name_lbl,
        is_active ? lv_color_make(0x34, 0xC7, 0x59)
                  : lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(name_lbl, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_label_set_long_mode(name_lbl, LV_LABEL_LONG_DOT);
    lv_obj_align(name_lbl, LV_ALIGN_TOP_LEFT, 0, 0);

    // Sub-line: PSK preview / status text.
    char sub[64];
    if (idx == 0) {
        snprintf(sub, sizeof(sub), "Public LongFast  (hash 0x%02X)",
                 ch->channel_hash);
    } else if (ch->psk_len == 0) {
        snprintf(sub, sizeof(sub), "Toggle on to generate a PSK");
    } else {
        char hex[16];
        int p = 0;
        for (uint8_t i = 0; i < ch->psk_len && i < 6; i++) {
            p += snprintf(hex + p, sizeof(hex) - p, "%02x", ch->psk[i]);
        }
        snprintf(sub, sizeof(sub), "PSK %s...  hash 0x%02X",
                 hex, ch->channel_hash);
    }
    lv_obj_t *sub_lbl = lv_label_create(card);
    lv_label_set_text(sub_lbl, sub);
    lv_obj_set_style_text_color(sub_lbl, lv_color_make(0xC7, 0xC7, 0xCC), LV_PART_MAIN);
    lv_obj_set_style_text_font(sub_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_label_set_long_mode(sub_lbl, LV_LABEL_LONG_DOT);
    lv_obj_align(sub_lbl, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    // Right side: enable switch. We stop click propagation from the
    // switch to the card so flipping the toggle doesn't also "set
    // active" - that'd be confusing when toggling off.
    lv_obj_t *sw = lv_switch_create(card);
    lv_obj_set_size(sw, 72, 38);
    lv_obj_set_style_bg_color(sw, lv_color_make(0x44, 0x44, 0x44),
        LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(sw, ARGUS_ACCENT,
        LV_PART_MAIN | LV_STATE_CHECKED);
    if (ch->enabled) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_set_user_data(sw, (void *)(intptr_t)idx);
    lv_obj_add_event_cb(sw, on_switch_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
}

static void rebuild_list()
{
    if (!list_root) return;
    lv_obj_clean(list_root);
    for (int i = 0; i < MESH_MAX_CHANNELS; i++) {
        make_channel_row(list_root, i);
    }
}

void channels_screen_create()
{
    ch_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(ch_screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(ch_screen, 0, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(ch_screen);
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_label_set_text(title, "CHANNELS");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    list_root = lv_obj_create(ch_screen);
    lv_obj_set_size(list_root, 400, 380);
    lv_obj_align(list_root, LV_ALIGN_TOP_MID, 0, 56);
    lv_obj_set_style_bg_color(list_root, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(list_root, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(list_root, 4, LV_PART_MAIN);
    lv_obj_set_scroll_dir(list_root, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list_root, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_layout(list_root, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(list_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list_root, 8, LV_PART_MAIN);

    // PSK-edit hint - the UI doesn't expose PSK editing on purpose
    // (no on-screen 32-byte hex entry), so this line tells the user
    // where to go when they actually need to paste a friend's PSK.
    // /Meshtastic/channels.txt is the same file that gets written on
    // every channel-state change here, and is re-read at boot.
    lv_obj_t *psk_hint = lv_label_create(ch_screen);
    lv_obj_set_style_text_color(psk_hint, lv_color_make(0x55, 0x99, 0xCC), LV_PART_MAIN);
    lv_obj_set_style_text_font(psk_hint, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_align(psk_hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(psk_hint, "Edit PSK in channels.txt on SD");
    lv_obj_align(psk_hint, LV_ALIGN_BOTTOM_MID, 0, -47);

    lv_obj_t *hint = lv_label_create(ch_screen);
    lv_obj_set_style_text_color(hint, lv_color_make(0x44, 0x44, 0x44), LV_PART_MAIN);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(hint, "Tap: activate   toggle: enable   swipe: back");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -28);

    lv_obj_add_event_cb(ch_screen, on_back_gesture, LV_EVENT_GESTURE, NULL);
}

void channels_screen_show()
{
    rebuild_list();
    lv_scr_load(ch_screen);
}

bool channels_screen_is_active()
{
    return lv_screen_active() == ch_screen;
}
