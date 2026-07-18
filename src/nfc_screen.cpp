#include "nfc_screen.h"
#include "theme.h"
#include "nfc_write_screen.h"
#include <LilyGoLib.h>

static lv_obj_t *nfc_screen;
static lv_obj_t *toggle_sw;
static lv_obj_t *status_label;
static lv_obj_t *read_btn;
static lv_obj_t *read_btn_label;
static lv_obj_t *write_btn;
static lv_obj_t *write_btn_label;
static lv_obj_t *data_panel;
static lv_obj_t *data_label;

static bool nfc_powered = false;

enum NfcReadState { NFC_IDLE, NFC_DISCOVERING };
static NfcReadState s_read_state  = NFC_IDLE;
static bool         s_card_ready  = false;
static uint8_t      s_raw_buf[512];

// Called by the RFAL state machine (from within rfalNfcWorker)
static void on_rfal_notify(rfalNfcState st)
{
    if (st == RFAL_NFC_STATE_ACTIVATED)
        s_card_ready = true;
}

static void start_discovery()
{
    rfalNfcDiscoverParam p;
    memset(&p, 0, sizeof(p));
    p.devLimit        = 1;
    p.techs2Find      = RFAL_NFC_POLL_TECH_A;
    p.GBLen           = RFAL_NFCDEP_GB_MAX_LEN;
    p.notifyCb        = on_rfal_notify;
    p.totalDuration   = 1000U;
    p.wakeupEnabled   = false;
    NFCReader.rfalNfcDiscover(&p);
}

// Appends NDEF record content to buf[n..bufsize]. Updates n.
static void append_ndef(NdefClass &ndef, rfalNfcDevice *dev, char *buf, int &n, int bufsize)
{
    ReturnCode err = ndef.ndefPollerContextInitialization(dev);
    if (err != ST_ERR_NONE) {
        n += snprintf(buf + n, bufsize - n, "(No NDEF support)");
        return;
    }

    ndefInfo info;
    err = ndef.ndefPollerNdefDetect(&info);
    if (err != ST_ERR_NONE) {
        n += snprintf(buf + n, bufsize - n, "(No NDEF message)");
        return;
    }

    uint32_t actual = 0;
    memset(s_raw_buf, 0, sizeof(s_raw_buf));
    err = ndef.ndefPollerReadRawMessage(s_raw_buf, sizeof(s_raw_buf), &actual);
    if (err != ST_ERR_NONE) {
        n += snprintf(buf + n, bufsize - n, "(Read error %d)", (int)err);
        return;
    }

    ndefMessage    msg;
    ndefConstBuffer ndefBuf = { s_raw_buf, actual };
    err = ndef.ndefMessageDecode(&ndefBuf, &msg);
    if (err != ST_ERR_NONE) {
        n += snprintf(buf + n, bufsize - n, "(Decode error %d)", (int)err);
        return;
    }

    ndefRecord *rec = ndefMessageGetFirstRecord(&msg);
    while (rec && n < bufsize - 120) {
        ndefType type;
        if (ndef.ndefRecordToType(rec, &type) == ST_ERR_NONE) {
            switch (type.id) {
            case NDEF_TYPE_RTD_TEXT: {
                uint8_t enc;
                ndefConstBuffer8 lang;
                ndefConstBuffer  sentence;
                ndef.ndefGetRtdText(&type, &enc, &lang, &sentence);
                n += snprintf(buf + n, bufsize - n, "Text: %.*s\n",
                    (int)sentence.length, (const char *)sentence.buffer);
                break;
            }
            case NDEF_TYPE_RTD_URI: {
                ndefConstBuffer proto, url;
                ndef.ndefGetRtdUri(&type, &proto, &url);
                n += snprintf(buf + n, bufsize - n, "URL: %.*s%.*s\n",
                    (int)proto.length,  (const char *)proto.buffer,
                    (int)url.length,    (const char *)url.buffer);
                break;
            }
            case NDEF_TYPE_MEDIA_WIFI: {
                ndefTypeWifi wifi;
                ndef.ndefGetWifi(&type, &wifi);
                n += snprintf(buf + n, bufsize - n, "WiFi: %.*s\nKey: %.*s\n",
                    (int)wifi.bufNetworkSSID.length, (const char *)wifi.bufNetworkSSID.buffer,
                    (int)wifi.bufNetworkKey.length,  (const char *)wifi.bufNetworkKey.buffer);
                break;
            }
            case NDEF_TYPE_RTD_AAR: {
                ndefConstBuffer aar;
                ndef.ndefGetRtdAar(&type, &aar);
                n += snprintf(buf + n, bufsize - n, "App: %.*s\n",
                    (int)aar.length, (const char *)aar.buffer);
                break;
            }
            default:
                n += snprintf(buf + n, bufsize - n, "Record type %d\n", (int)type.id);
                break;
            }
        }
        rec = ndefMessageGetNextRecord(rec);
    }
}

static void nfc_process_card()
{
    rfalNfcDevice *dev;
    NFCReader.rfalNfcGetActiveDevice(&dev);

    char buf[600];
    int  n = 0;

    // Card UID
    n += snprintf(buf + n, sizeof(buf) - n, "UID:");
    for (int i = 0; i < dev->nfcidLen; i++)
        n += snprintf(buf + n, sizeof(buf) - n, " %02X", dev->nfcid[i]);
    n += snprintf(buf + n, sizeof(buf) - n, "\n");

    // NDEF records
    NdefClass ndef(&NFCReader);
    append_ndef(ndef, dev, buf, n, (int)sizeof(buf));

    if (n == 0) n += snprintf(buf + n, sizeof(buf) - n, "(Empty)");
    buf[n] = '\0';

    lv_label_set_text(data_label, buf);
    lv_obj_scroll_to_y(data_panel, 0, LV_ANIM_OFF);
}

static void set_read_btn_scanning(bool scanning)
{
    if (scanning) {
        lv_obj_set_style_bg_color(read_btn, lv_color_make(0xCC, 0x22, 0x22), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(read_btn_label, lv_color_white(), LV_PART_MAIN);
        lv_label_set_text(read_btn_label, "Stop");
    } else {
        lv_color_t bg  = nfc_powered ? ARGUS_ACCENT : lv_color_make(0x33, 0x33, 0x33);
        lv_color_t txt = nfc_powered ? lv_color_white()                 : lv_color_make(0x77, 0x77, 0x77);
        lv_obj_set_style_bg_color(read_btn, bg, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(read_btn_label, txt, LV_PART_MAIN);
        lv_label_set_text(read_btn_label, "Read");
    }
}

static void update_ui()
{
    lv_label_set_text(status_label, nfc_powered ? "NFC: ON" : "NFC: OFF");

    lv_color_t bg  = nfc_powered ? ARGUS_ACCENT : lv_color_make(0x33, 0x33, 0x33);
    lv_color_t txt = nfc_powered ? lv_color_white()                 : lv_color_make(0x77, 0x77, 0x77);

    lv_obj_set_style_bg_color(write_btn, bg,  LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(write_btn_label, txt, LV_PART_MAIN);

    // Read button always reflects the live scan state so it stays correct
    // when re-entering the screen mid-scan
    set_read_btn_scanning(s_read_state == NFC_DISCOVERING);
}

static void on_toggle(lv_event_t *e)
{
    nfc_powered = lv_obj_has_state(toggle_sw, LV_STATE_CHECKED);
    instance.powerControl(POWER_NFC, nfc_powered);
    if (nfc_powered) {
        instance.initNFC();
    } else {
        if (s_read_state == NFC_DISCOVERING) {
            NFCReader.rfalNfcDeactivate(false);
        }
        s_read_state = NFC_IDLE;
        set_read_btn_scanning(false);
        lv_label_set_text(data_label, "");
    }
    update_ui();
}

static void on_read_btn(lv_event_t *e)
{
    if (!nfc_powered) return;

    if (s_read_state == NFC_DISCOVERING) {
        // Stop scanning
        s_read_state = NFC_IDLE;
        NFCReader.rfalNfcDeactivate(false);
        set_read_btn_scanning(false);
        lv_label_set_text(data_label, "");
    } else {
        // Start scanning
        s_card_ready = false;
        s_read_state = NFC_DISCOVERING;
        set_read_btn_scanning(true);
        lv_label_set_text(data_label, "Scanning...\nHold card near watch");
        start_discovery();
    }
}

static void on_write_btn(lv_event_t *e)
{
    if (!nfc_powered) return;
    // Stop any in-progress read scan before handing the reader to the writer
    if (s_read_state == NFC_DISCOVERING) {
        NFCReader.rfalNfcDeactivate(false);
        s_read_state = NFC_IDLE;
    }
    nfc_write_screen_show();
}

static lv_obj_t *make_btn(lv_obj_t *parent, const char *text, int x_ofs, int y_ofs,
                           lv_obj_t **out_label)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_set_size(btn, 160, 60);
    lv_obj_set_style_radius(btn, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_make(0x33, 0x33, 0x33), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(btn, LV_ALIGN_TOP_MID, x_ofs, y_ofs);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, lv_color_make(0x77, 0x77, 0x77), LV_PART_MAIN);
    lv_label_set_text(lbl, text);
    lv_obj_center(lbl);

    *out_label = lbl;
    return btn;
}

void nfc_screen_create()
{
    nfc_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(nfc_screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(nfc_screen, 0, LV_PART_MAIN);

    // Title
    lv_obj_t *title = lv_label_create(nfc_screen);
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_label_set_text(title, "NFC");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 5);

    // Power toggle
    toggle_sw = lv_switch_create(nfc_screen);
    lv_obj_set_size(toggle_sw, 100, 50);
    lv_obj_set_style_bg_color(toggle_sw, lv_color_make(0x44, 0x44, 0x44), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(toggle_sw, ARGUS_ACCENT, LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_add_event_cb(toggle_sw, on_toggle, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_align(toggle_sw, LV_ALIGN_TOP_MID, -90, 72);

    // Status label (right of toggle)
    status_label = lv_label_create(nfc_screen);
    lv_obj_set_style_text_color(status_label, lv_color_make(0xAA, 0xAA, 0xAA), LV_PART_MAIN);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_align(status_label, LV_ALIGN_TOP_MID, 60, 87);

    // Read / Write buttons — gray until NFC is on
    read_btn  = make_btn(nfc_screen, "Read",  -90, 160, &read_btn_label);
    write_btn = make_btn(nfc_screen, "Write", +90, 160, &write_btn_label);
    lv_obj_add_event_cb(read_btn,  on_read_btn,  LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(write_btn, on_write_btn, LV_EVENT_CLICKED, NULL);

    // Scrollable data result panel
    data_panel = lv_obj_create(nfc_screen);
    lv_obj_set_size(data_panel, 390, 210);
    lv_obj_align(data_panel, LV_ALIGN_TOP_MID, 0, 232);
    lv_obj_set_style_bg_color(data_panel, lv_color_make(0x0A, 0x0A, 0x0A), LV_PART_MAIN);
    lv_obj_set_style_border_color(data_panel, lv_color_make(0x33, 0x33, 0x33), LV_PART_MAIN);
    lv_obj_set_style_border_width(data_panel, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(data_panel, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(data_panel, 8, LV_PART_MAIN);
    lv_obj_set_scroll_dir(data_panel, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(data_panel, LV_SCROLLBAR_MODE_AUTO);

    data_label = lv_label_create(data_panel);
    lv_obj_set_width(data_label, lv_pct(100));
    lv_obj_set_style_text_color(data_label, lv_color_make(0xCC, 0xCC, 0xCC), LV_PART_MAIN);
    lv_obj_set_style_text_font(data_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_label_set_long_mode(data_label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(data_label, "");

    // Navigation hint
    lv_obj_t *hint = lv_label_create(nfc_screen);
    lv_obj_set_style_text_color(hint, lv_color_make(0x44, 0x44, 0x44), LV_PART_MAIN);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(hint, "Boot button to return");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -8);

    update_ui();
}

void nfc_screen_show()
{
    bool hw_on = instance.pmu.isEnableDLDO1();
    nfc_powered = hw_on;
    if (hw_on)
        lv_obj_add_state(toggle_sw, LV_STATE_CHECKED);
    else
        lv_obj_clear_state(toggle_sw, LV_STATE_CHECKED);
    update_ui();
    lv_scr_load(nfc_screen);
}

bool nfc_screen_is_active()
{
    return lv_screen_active() == nfc_screen;
}

void nfc_screen_worker()
{
    if (s_read_state == NFC_IDLE) return;

    // Pause while the user is on another screen, but keep the read state
    // so returning to the NFC screen resumes the scan with the "Stop" button
    if (!nfc_screen_is_active()) return;

    NFCReader.rfalNfcWorker();

    if (s_card_ready) {
        s_card_ready = false;
        nfc_process_card();
        NFCReader.rfalNfcDeactivate(true);
        NFCReader.rfalNfcaPollerSleep();
        s_read_state = NFC_IDLE;
        set_read_btn_scanning(false);
    }
}
