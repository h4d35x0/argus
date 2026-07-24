#include "pin_pad_screen.h"
#include "security_store.h"
#include "argus_mode.h"
#include "theme.h"
#include "tools_screen.h"
#include <string.h>
#include <stdio.h>

void clock_screen_show();   // main.cpp

enum PadMode { PAD_CHECK, PAD_SET_UNLOCK, PAD_SET_SHRED };

static lv_obj_t *pin_pad_screen;
static lv_obj_t *title_label;
static lv_obj_t *dots_label;
static lv_obj_t *msg_label;
static lv_obj_t *keys;

static PadMode s_mode;
static char    s_entry[16];
static int     s_len;
static char    s_new_unlock[16];

static const char *KEYMAP[] = {
    "1", "2", "3", "\n",
    "4", "5", "6", "\n",
    "7", "8", "9", "\n",
    LV_SYMBOL_BACKSPACE, "0", LV_SYMBOL_OK, ""
};

static void set_msg(const char *m) { lv_label_set_text(msg_label, m ? m : ""); }

static void update_dots(void)
{
    char d[16];
    int n = s_len < 15 ? s_len : 15;
    for (int i = 0; i < n; i++) d[i] = '*';
    d[n] = '\0';
    lv_label_set_text(dots_label, d);
}

static void enter_mode(PadMode m)
{
    s_mode = m;
    s_len = 0; s_entry[0] = '\0';
    switch (m) {
    case PAD_SET_UNLOCK: lv_label_set_text(title_label, "SET UNLOCK PIN");        break;
    case PAD_SET_SHRED:  lv_label_set_text(title_label, "SET SHRED PIN (longer)"); break;
    default:             lv_label_set_text(title_label, "ENTER PIN");             break;
    }
    update_dots();
}

// Decoy completion: land on the Tools grid. Offense is now locked out, so the
// gating leaves the offensive tiles hidden - the coercer sees an "empty" unlock.
static void shred_finish_cb(lv_timer_t *t)
{
    lv_timer_delete(t);
    tools_screen_show();
}

static void on_ok(void)
{
    if (s_len < 4) { set_msg("4-8 digits"); return; }

    if (s_mode == PAD_SET_UNLOCK) {
        strncpy(s_new_unlock, s_entry, sizeof(s_new_unlock) - 1);
        s_new_unlock[sizeof(s_new_unlock) - 1] = '\0';
        set_msg("");
        enter_mode(PAD_SET_SHRED);
        return;
    }
    if (s_mode == PAD_SET_SHRED) {
        const char *err = security_set_pins(s_new_unlock, s_entry);
        if (err) { set_msg(err); enter_mode(PAD_SET_UNLOCK); return; }
        set_msg("PINs saved - enter unlock");
        enter_mode(PAD_CHECK);
        return;
    }

    // PAD_CHECK. The PBKDF2 verify blocks the UI (up to ~6s on the first, legacy
    // hash; ~0.5s once migrated). Show feedback and paint it BEFORE the stall so
    // the check never feels dead / like it needs a second press.
    set_msg("Checking...");
    lv_refr_now(NULL);
    PinResult r = security_check(s_entry);
    if (r == PinResult::Unlock) {
        enter_offense();          // tiles auto-reveal via the gating callback
        tools_screen_show();
    } else if (r == PinResult::Shred) {
        // Duress: burn the lockout FIRST, then a fake "Unlocking..." decoy.
        offense_shred();
        lv_label_set_text(title_label, "Unlocking...");
        set_msg("");
        lv_timer_t *t = lv_timer_create(shred_finish_cb, 1200, NULL);
        lv_timer_set_repeat_count(t, 1);
    } else {
        uint32_t lk = security_lockout_ms();
        if (lk > 0) { char b[24]; snprintf(b, sizeof(b), "locked  %lus", (unsigned long)(lk / 1000)); set_msg(b); }
        else        set_msg("Wrong PIN");
        s_len = 0; s_entry[0] = '\0'; update_dots();
    }
}

static void on_key(lv_event_t *e)
{
    uint32_t id = lv_buttonmatrix_get_selected_button(keys);
    const char *txt = lv_buttonmatrix_get_button_text(keys, id);
    if (!txt) return;
    if (!strcmp(txt, LV_SYMBOL_BACKSPACE)) {
        if (s_len > 0) { s_entry[--s_len] = '\0'; update_dots(); }
    } else if (!strcmp(txt, LV_SYMBOL_OK)) {
        on_ok();
    } else if (s_len < 8 && txt[0] >= '0' && txt[0] <= '9') {
        s_entry[s_len++] = txt[0]; s_entry[s_len] = '\0';
        update_dots();
    }
}

static void on_gesture(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    if (lv_indev_get_gesture_dir(indev) == LV_DIR_RIGHT) clock_screen_show();
}

void pin_pad_screen_create()
{
    pin_pad_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(pin_pad_screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(pin_pad_screen, 0, LV_PART_MAIN);

    title_label = lv_label_create(pin_pad_screen);
    lv_obj_set_style_text_color(title_label, ARGUS_OFFENSE_ACCENT, LV_PART_MAIN);  // Offense unlock: red-team
    lv_obj_set_style_text_font(title_label, &font_argus_label_20, LV_PART_MAIN);
    lv_label_set_text(title_label, "ENTER PIN");
    lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 14);

    dots_label = lv_label_create(pin_pad_screen);
    lv_obj_set_style_text_color(dots_label, ARGUS_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(dots_label, &font_argus_label_28, LV_PART_MAIN);
    lv_label_set_text(dots_label, "");
    lv_obj_align(dots_label, LV_ALIGN_TOP_MID, 0, 48);

    msg_label = lv_label_create(pin_pad_screen);
    lv_obj_set_style_text_color(msg_label, ARGUS_TEXT_DIM, LV_PART_MAIN);
    lv_obj_set_style_text_font(msg_label, &font_argus_label_14, LV_PART_MAIN);
    lv_label_set_text(msg_label, "");
    lv_obj_align(msg_label, LV_ALIGN_TOP_MID, 0, 84);

    keys = lv_buttonmatrix_create(pin_pad_screen);
    lv_buttonmatrix_set_map(keys, KEYMAP);
    lv_obj_set_size(keys, 300, 300);
    lv_obj_align(keys, LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_obj_set_style_bg_opa(keys, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(keys, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(keys, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_gap(keys, 6, LV_PART_MAIN);
    // Montserrat (not the Orbitron brand subset font_argus_label_28) for the keys:
    // the brand font carries only digits/colon/space/AMP, so the LV_SYMBOL_BACKSPACE
    // and LV_SYMBOL_OK action keys rendered as empty "tofu" squares. The built-in
    // Montserrat font bundles those FontAwesome glyphs, so both action keys show as
    // real icons while the digits stay clean and legible.
    lv_obj_set_style_text_font(keys, &lv_font_montserrat_28, LV_PART_ITEMS);
    lv_obj_set_style_text_color(keys, ARGUS_TEXT, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(keys, lv_color_make(0x16, 0x1E, 0x28), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(keys, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_border_color(keys, ARGUS_ACCENT_DIM, LV_PART_ITEMS);
    lv_obj_set_style_border_width(keys, 1, LV_PART_ITEMS);
    lv_obj_set_style_radius(keys, 8, LV_PART_ITEMS);
    lv_obj_add_event_cb(keys, on_key, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_add_event_cb(pin_pad_screen, on_gesture, LV_EVENT_GESTURE, NULL);
}

void pin_pad_screen_show()
{
    enter_mode(security_pins_set() ? PAD_CHECK : PAD_SET_UNLOCK);
    set_msg(security_pins_set() ? "" : "first run: set your PINs");
    lv_scr_load(pin_pad_screen);
}

bool pin_pad_screen_is_active() { return lv_screen_active() == pin_pad_screen; }
