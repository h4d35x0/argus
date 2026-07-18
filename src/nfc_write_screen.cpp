#include "nfc_write_screen.h"
#include "theme.h"
#include <LilyGoLib.h>
#include <string.h>

// Defined in nfc_screen.cpp
void nfc_screen_show();

static lv_obj_t *wr_screen;
static lv_obj_t *type_dd;
static lv_obj_t *prefix_dd;     // URL only
static lv_obj_t *lang_ta;       // Text only
static lv_obj_t *value_ta;
static lv_obj_t *write_btn;
static lv_obj_t *write_btn_label;
static lv_obj_t *status_label;
static lv_obj_t *keyboard;

enum WrState { WR_IDLE, WR_DISCOVERING };
static WrState  s_wr_state    = WR_IDLE;
static bool     s_card_ready  = false;

// Snapshot of the field values taken when Write is pressed, so the actual
// write uses stable data independent of further UI edits.
static int  s_snap_type;                 // 0 = Text, 1 = URL, 2 = Phone
static int  s_snap_prefix_idx;
static char s_snap_lang[8];
static char s_snap_value[256];

// Dropdown index -> NDEF_URI_PREFIX_* code
static const uint8_t URI_PREFIX[] = {
    NDEF_URI_PREFIX_HTTPS,       // "https://"
    NDEF_URI_PREFIX_HTTP,        // "http://"
    NDEF_URI_PREFIX_HTTPS_WWW,   // "https://www."
    NDEF_URI_PREFIX_HTTP_WWW,    // "http://www."
    NDEF_URI_PREFIX_NONE,        // "(none)"
};

static void set_status(const char *text, lv_color_t color)
{
    lv_label_set_text(status_label, text);
    lv_obj_set_style_text_color(status_label, color, LV_PART_MAIN);
}

static void on_rfal_notify(rfalNfcState st)
{
    if (st == RFAL_NFC_STATE_ACTIVATED)
        s_card_ready = true;
}

static void start_discovery()
{
    rfalNfcDiscoverParam p;
    memset(&p, 0, sizeof(p));
    p.devLimit      = 1;
    p.techs2Find    = RFAL_NFC_POLL_TECH_A;
    p.GBLen         = RFAL_NFCDEP_GB_MAX_LEN;
    p.notifyCb      = on_rfal_notify;
    p.totalDuration = 1000U;
    p.wakeupEnabled = false;
    NFCReader.rfalNfcDiscover(&p);
}

// Reflect the selected record type: URL shows the prefix dropdown,
// Text shows the language-code field.
static void update_type_fields()
{
    int sel = lv_dropdown_get_selected(type_dd);
    if (sel == 1) {                         // URL
        lv_obj_clear_flag(prefix_dd, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lang_ta, LV_OBJ_FLAG_HIDDEN);
    } else if (sel == 2) {                  // Phone — no prefix dropdown, no lang field
        lv_obj_add_flag(prefix_dd, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lang_ta, LV_OBJ_FLAG_HIDDEN);
    } else {                                // Text
        lv_obj_add_flag(prefix_dd, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lang_ta, LV_OBJ_FLAG_HIDDEN);
    }
}

static void on_type_changed(lv_event_t *e)
{
    update_type_fields();
    // If value_ta is already focused, switch the keyboard mode to match the new type
    if (!lv_obj_has_flag(keyboard, LV_OBJ_FLAG_HIDDEN) &&
        lv_keyboard_get_textarea(keyboard) == value_ta) {
        bool phone = (lv_dropdown_get_selected(type_dd) == 2);
        lv_keyboard_set_mode(keyboard, phone ? LV_KEYBOARD_MODE_NUMBER : LV_KEYBOARD_MODE_TEXT_LOWER);
    }
}

static void show_keyboard(lv_obj_t *ta)
{
    lv_keyboard_set_textarea(keyboard, ta);
    bool phone_value = (ta == value_ta) && (lv_dropdown_get_selected(type_dd) == 2);
    lv_keyboard_set_mode(keyboard, phone_value ? LV_KEYBOARD_MODE_NUMBER : LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_clear_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
}

static void hide_keyboard()
{
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
}

static void on_ta_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *ta = (lv_obj_t *)lv_event_get_target(e);
    if (code == LV_EVENT_FOCUSED)        show_keyboard(ta);
    else if (code == LV_EVENT_DEFOCUSED) hide_keyboard();
}

static void on_kb_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        lv_obj_t *ta = lv_keyboard_get_textarea(keyboard);
        if (ta) lv_obj_clear_state(ta, LV_STATE_FOCUSED);
        hide_keyboard();
    }
}

// Encode the snapshot fields into an NDEF message and write it to the
// card that is currently in the field. Runs from the worker (main loop).
static void write_to_card()
{
    rfalNfcDevice *dev;
    NFCReader.rfalNfcGetActiveDevice(&dev);

    NdefClass ndef(&NFCReader);
    ReturnCode err = ndef.ndefPollerContextInitialization(dev);
    if (err != ST_ERR_NONE) {
        set_status("Tag not NDEF-capable", lv_color_make(0xCC, 0x44, 0x44));
        return;
    }

    // Detect; if the tag has no NDEF area yet, format it first
    ndefInfo info;
    if (ndef.ndefPollerNdefDetect(&info) != ST_ERR_NONE)
        ndef.ndefPollerTagFormat(NULL, 0);

    ndefConstBuffer bufValue = {
        (const uint8_t *)s_snap_value, (uint32_t)strlen(s_snap_value)
    };

    ndefType type;
    if (s_snap_type == 1) {
        err = ndef.ndefRtdUri(&type, URI_PREFIX[s_snap_prefix_idx], &bufValue);
    } else if (s_snap_type == 2) {
        err = ndef.ndefRtdUri(&type, NDEF_URI_PREFIX_TEL, &bufValue);
    } else {
        ndefConstBuffer8 bufLang = {
            (const uint8_t *)s_snap_lang, (uint8_t)strlen(s_snap_lang)
        };
        err = ndef.ndefRtdText(&type, TEXT_ENCODING_UTF8, &bufLang, &bufValue);
    }
    if (err != ST_ERR_NONE) {
        set_status("Encode failed", lv_color_make(0xCC, 0x44, 0x44));
        return;
    }

    ndefRecord record;
    if (ndef.ndefTypeToRecord(&type, &record) != ST_ERR_NONE) {
        set_status("Record build failed", lv_color_make(0xCC, 0x44, 0x44));
        return;
    }

    ndefMessage message;
    ndef.ndefMessageInit(&message);
    ndef.ndefMessageAppend(&message, &record);

    err = ndef.ndefPollerWriteMessage(&message);
    if (err == ST_ERR_NONE)
        set_status("Written successfully", ARGUS_ACCENT);
    else
        set_status("Write failed", lv_color_make(0xCC, 0x44, 0x44));
}

static void stop_writing()
{
    if (s_wr_state == WR_DISCOVERING) {
        NFCReader.rfalNfcDeactivate(false);
        s_wr_state = WR_IDLE;
    }
}

static void on_write_btn(lv_event_t *e)
{
    if (!instance.pmu.isEnableDLDO1()) {
        set_status("Enable NFC first", lv_color_make(0xCC, 0x44, 0x44));
        return;
    }
    if (s_wr_state == WR_DISCOVERING) {   // pressed while waiting -> cancel
        stop_writing();
        lv_label_set_text(write_btn_label, "Write");
        lv_obj_set_style_bg_color(write_btn, ARGUS_ACCENT, LV_PART_MAIN);
        set_status("Cancelled", lv_color_make(0xAA, 0xAA, 0xAA));
        return;
    }

    // Snapshot the fields
    s_snap_type       = lv_dropdown_get_selected(type_dd);
    s_snap_prefix_idx = lv_dropdown_get_selected(prefix_dd);
    strncpy(s_snap_lang, lv_textarea_get_text(lang_ta), sizeof(s_snap_lang) - 1);
    s_snap_lang[sizeof(s_snap_lang) - 1] = '\0';
    if (!s_snap_lang[0]) strcpy(s_snap_lang, "en");
    strncpy(s_snap_value, lv_textarea_get_text(value_ta), sizeof(s_snap_value) - 1);
    s_snap_value[sizeof(s_snap_value) - 1] = '\0';

    if (!s_snap_value[0]) {
        set_status("Enter a value first", lv_color_make(0xCC, 0x44, 0x44));
        return;
    }

    hide_keyboard();
    s_card_ready = false;
    s_wr_state   = WR_DISCOVERING;
    lv_label_set_text(write_btn_label, "Cancel");
    lv_obj_set_style_bg_color(write_btn, lv_color_make(0xCC, 0x33, 0x33), LV_PART_MAIN);
    set_status("Tap a card to write...", lv_color_make(0xFF, 0xCC, 0x00));
    start_discovery();
}

static lv_obj_t *make_label(const char *text, int y)
{
    lv_obj_t *lbl = lv_label_create(wr_screen);
    lv_obj_set_style_text_color(lbl, lv_color_make(0xAA, 0xAA, 0xAA), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_label_set_text(lbl, text);
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, -150, y);
    return lbl;
}

static void style_dropdown(lv_obj_t *dd)
{
    lv_obj_set_style_text_font(dd, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_bg_color(dd, lv_color_make(0x22, 0x22, 0x22), LV_PART_MAIN);
    lv_obj_set_style_text_color(dd, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_border_color(dd, lv_color_make(0x55, 0x55, 0x55), LV_PART_MAIN);
    lv_obj_set_style_border_width(dd, 1, LV_PART_MAIN);
    lv_obj_t *list = lv_dropdown_get_list(dd);
    lv_obj_set_style_bg_color(list, lv_color_make(0x22, 0x22, 0x22), LV_PART_MAIN);
    lv_obj_set_style_text_color(list, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(list, &lv_font_montserrat_16, LV_PART_MAIN);
}

static void style_textarea(lv_obj_t *ta)
{
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ta, lv_color_make(0x11, 0x11, 0x11), LV_PART_MAIN);
    lv_obj_set_style_text_color(ta, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_border_color(ta, lv_color_make(0x44, 0x44, 0x44), LV_PART_MAIN);
    lv_obj_set_style_border_width(ta, 1, LV_PART_MAIN);
}

void nfc_write_screen_create()
{
    wr_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(wr_screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(wr_screen, 0, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(wr_screen);
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_label_set_text(title, "NFC WRITE");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    // Record type
    make_label("Type", 58);
    type_dd = lv_dropdown_create(wr_screen);
    lv_dropdown_set_options(type_dd, "Text\nURL\nPhone");
    lv_obj_set_size(type_dd, 210, 38);
    lv_obj_align(type_dd, LV_ALIGN_TOP_MID, 70, 54);
    style_dropdown(type_dd);
    lv_obj_add_event_cb(type_dd, on_type_changed, LV_EVENT_VALUE_CHANGED, NULL);

    // URL prefix (URL type only)
    make_label("Prefix", 108);
    prefix_dd = lv_dropdown_create(wr_screen);
    lv_dropdown_set_options(prefix_dd,
        "https://\nhttp://\nhttps://www.\nhttp://www.\n(none)");
    lv_obj_set_size(prefix_dd, 210, 38);
    lv_obj_align(prefix_dd, LV_ALIGN_TOP_MID, 70, 104);
    style_dropdown(prefix_dd);

    // Language code (Text type only) — shares the row with the prefix
    lang_ta = lv_textarea_create(wr_screen);
    lv_textarea_set_one_line(lang_ta, true);
    lv_textarea_set_max_length(lang_ta, 5);
    lv_textarea_set_text(lang_ta, "en");
    lv_obj_set_size(lang_ta, 210, 44);
    lv_obj_align(lang_ta, LV_ALIGN_TOP_MID, 70, 104);
    style_textarea(lang_ta);
    lv_obj_add_event_cb(lang_ta, on_ta_event, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(lang_ta, on_ta_event, LV_EVENT_DEFOCUSED, NULL);

    // Value
    make_label("Value", 156);
    value_ta = lv_textarea_create(wr_screen);
    lv_textarea_set_one_line(value_ta, true);
    lv_textarea_set_max_length(value_ta, 240);
    lv_obj_set_size(value_ta, 380, 48);
    lv_obj_align(value_ta, LV_ALIGN_TOP_MID, 0, 184);
    style_textarea(value_ta);
    lv_obj_add_event_cb(value_ta, on_ta_event, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(value_ta, on_ta_event, LV_EVENT_DEFOCUSED, NULL);

    // Green Write button
    write_btn = lv_obj_create(wr_screen);
    lv_obj_set_size(write_btn, 200, 52);
    lv_obj_align(write_btn, LV_ALIGN_TOP_MID, 0, 244);
    lv_obj_set_style_radius(write_btn, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(write_btn, ARGUS_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(write_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(write_btn, 0, LV_PART_MAIN);
    lv_obj_clear_flag(write_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(write_btn, on_write_btn, LV_EVENT_CLICKED, NULL);

    write_btn_label = lv_label_create(write_btn);
    lv_obj_set_style_text_font(write_btn_label, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(write_btn_label, lv_color_white(), LV_PART_MAIN);
    lv_label_set_text(write_btn_label, "Write");
    lv_obj_center(write_btn_label);

    // Status feedback
    status_label = lv_label_create(wr_screen);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(status_label, lv_color_make(0xAA, 0xAA, 0xAA), LV_PART_MAIN);
    lv_obj_set_width(status_label, 390);
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(status_label, "Boot button to return");
    lv_obj_align(status_label, LV_ALIGN_BOTTOM_MID, 0, -12);

    // On-screen keyboard — hidden until a field is focused
    keyboard = lv_keyboard_create(wr_screen);
    lv_obj_set_size(keyboard, 410, 220);
    lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(keyboard, on_kb_event, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(keyboard, on_kb_event, LV_EVENT_CANCEL, NULL);

    update_type_fields();
}

void nfc_write_screen_show()
{
    s_wr_state = WR_IDLE;
    lv_label_set_text(write_btn_label, "Write");
    lv_obj_set_style_bg_color(write_btn, ARGUS_ACCENT, LV_PART_MAIN);
    set_status("Boot button to return", lv_color_make(0xAA, 0xAA, 0xAA));
    hide_keyboard();
    update_type_fields();
    lv_scr_load(wr_screen);
}

bool nfc_write_screen_is_active()
{
    return lv_screen_active() == wr_screen;
}

void nfc_write_screen_worker()
{
    if (s_wr_state == WR_IDLE) return;

    // Cancel a pending write if the user navigated away
    if (!nfc_write_screen_is_active()) {
        stop_writing();
        return;
    }

    NFCReader.rfalNfcWorker();

    if (s_card_ready) {
        s_card_ready = false;
        write_to_card();
        NFCReader.rfalNfcDeactivate(true);
        NFCReader.rfalNfcaPollerSleep();
        s_wr_state = WR_IDLE;
        lv_label_set_text(write_btn_label, "Write");
        lv_obj_set_style_bg_color(write_btn, ARGUS_ACCENT, LV_PART_MAIN);
    }
}
