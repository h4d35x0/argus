#include "tesla_cp_screen.h"
#include "theme.h"
#include "tesla_cp.h"
#include "lora_screen.h"
#include "aprs.h"
#include "pager.h"
#include <LilyGoLib.h>

// Defined elsewhere.
void tools_screen_show();

static lv_obj_t *tesla_cp_screen;
static lv_obj_t *send_btn;
static lv_obj_t *send_btn_label;
static lv_obj_t *status_label;
static lv_obj_t *hint_label;
static lv_obj_t *sub_label;            // freq/region descriptor under selector
static lv_obj_t *region_btn_us;        // selector — US 315 MHz
static lv_obj_t *region_btn_eu;        // selector — EU 433.92 MHz

// Currently selected region. Defaults to US since that's what we shipped
// first and it's the band the LilyGo radio's matching network is least
// far off from on this T-Watch Ultra. The user can flip to EU per-tap;
// the on_send handler dispatches to the matching tesla_cp_transmit_*().
static bool s_region_us = true;

static void update_status(const char *msg, lv_color_t col)
{
    lv_label_set_text(status_label, msg);
    lv_obj_set_style_text_color(status_label, col, LV_PART_MAIN);
}

// Repaint the two region buttons and the descriptive sub-label to match
// s_region_us. Called whenever the user taps either button and once at
// screen-create to seed the initial visual state.
static void update_region_selection()
{
    lv_color_t sel   = lv_color_make(0xCC, 0x22, 0x22);
    lv_color_t unsel = lv_color_make(0x33, 0x33, 0x33);

    lv_obj_set_style_bg_color(region_btn_us,
        s_region_us ? sel : unsel, LV_PART_MAIN);
    lv_obj_set_style_bg_color(region_btn_eu,
        s_region_us ? unsel : sel, LV_PART_MAIN);

    lv_label_set_text(sub_label,
        s_region_us ? "315 MHz OOK  -  US RKE band"
                    : "433.92 MHz OOK  -  EU / Intl RKE band");
}

static void on_region_clicked(lv_event_t *e)
{
    s_region_us = (lv_event_get_user_data(e) != NULL);
    update_region_selection();
}

static void update_send_btn_state()
{
    // Disable the send button when another radio user has the SX1262 — same
    // gating logic the module's transmit function uses, surfaced visually
    // so the user knows ahead of time why a tap won't do anything.
    bool radio_busy = lora_screen_is_powered() || aprs_is_running() ||
                      pager_is_running();
    lv_obj_set_style_bg_color(send_btn,
        radio_busy ? lv_color_make(0x44, 0x44, 0x44)
                   : lv_color_make(0xCC, 0x22, 0x22), LV_PART_MAIN);
    if (radio_busy) {
        lv_obj_add_state(send_btn, LV_STATE_DISABLED);
        update_status("Radio in use by LoRa / APRS / Pager",
                      lv_color_make(0xFF, 0xAA, 0x00));
    } else {
        lv_obj_clear_state(send_btn, LV_STATE_DISABLED);
    }
}

static void on_send(lv_event_t *)
{
    if (lora_screen_is_powered() || aprs_is_running() || pager_is_running()) {
        update_status("Radio busy — close LoRa / APRS / Pager first",
                      lv_color_make(0xFF, 0xAA, 0x00));
        return;
    }

    const char *freq_txt = s_region_us ? "315 MHz" : "433.92 MHz";
    char tx_msg[40];
    snprintf(tx_msg, sizeof(tx_msg), "Transmitting %s...", freq_txt);
    update_status(tx_msg, lv_color_make(0xFF, 0xCC, 0x00));
    lv_refr_now(NULL);     // force a paint before the synchronous TX blocks

    bool ok = s_region_us ? tesla_cp_transmit_us() : tesla_cp_transmit_eu();
    if (ok) {
        char sent_msg[48];
        snprintf(sent_msg, sizeof(sent_msg), "Sent  (5x burst at %s)", freq_txt);
        update_status(sent_msg, ARGUS_ACCENT);
    } else {
        char buf[40];
        snprintf(buf, sizeof(buf), "Failed (err %d)", (int)tesla_cp_last_error());
        update_status(buf, lv_color_make(0xFF, 0x44, 0x44));
    }
    update_send_btn_state();
}

static void on_gesture(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir == LV_DIR_RIGHT) tools_screen_show();
}

static void on_periodic_update(lv_timer_t *)
{
    if (!tesla_cp_screen_is_active()) return;
    update_send_btn_state();
}

void tesla_cp_screen_create()
{
    tesla_cp_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(tesla_cp_screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(tesla_cp_screen, 0, LV_PART_MAIN);
    lv_obj_clear_flag(tesla_cp_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(tesla_cp_screen, on_gesture, LV_EVENT_GESTURE, NULL);

    // Title — font_48 to match the PAGER / TPMS / SETTINGS headers.
    lv_obj_t *title = lv_label_create(tesla_cp_screen);
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_label_set_text(title, "TESLA CP");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    // Region selector — two pill buttons (US 315 / EU 433.92) packed in a
    // 100+8+100 row at y=78 where the visible width opens up to ~220 px.
    // The currently-selected button is painted red; the other is grey.
    // on_region_clicked() distinguishes which was tapped via the
    // user_data pointer (non-NULL = US, NULL = EU).
    region_btn_us = lv_obj_create(tesla_cp_screen);
    lv_obj_set_size(region_btn_us, 100, 30);
    lv_obj_align(region_btn_us, LV_ALIGN_TOP_MID, -54, 78);
    lv_obj_set_style_radius(region_btn_us, 6, LV_PART_MAIN);
    lv_obj_set_style_border_width(region_btn_us, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(region_btn_us, 0, LV_PART_MAIN);
    lv_obj_clear_flag(region_btn_us, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(region_btn_us, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(region_btn_us, on_region_clicked, LV_EVENT_CLICKED, (void *)1);
    lv_obj_t *us_lbl = lv_label_create(region_btn_us);
    lv_label_set_text(us_lbl, "US 315");
    lv_obj_set_style_text_color(us_lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(us_lbl, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_center(us_lbl);

    region_btn_eu = lv_obj_create(tesla_cp_screen);
    lv_obj_set_size(region_btn_eu, 100, 30);
    lv_obj_align(region_btn_eu, LV_ALIGN_TOP_MID, 54, 78);
    lv_obj_set_style_radius(region_btn_eu, 6, LV_PART_MAIN);
    lv_obj_set_style_border_width(region_btn_eu, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(region_btn_eu, 0, LV_PART_MAIN);
    lv_obj_clear_flag(region_btn_eu, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(region_btn_eu, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(region_btn_eu, on_region_clicked, LV_EVENT_CLICKED, (void *)0);
    lv_obj_t *eu_lbl = lv_label_create(region_btn_eu);
    lv_label_set_text(eu_lbl, "EU 433.92");
    lv_obj_set_style_text_color(eu_lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(eu_lbl, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_center(eu_lbl);

    // Sub-title — repainted by update_region_selection() to reflect the
    // chosen variant ("315 MHz OOK - US RKE band" / "433.92 MHz OOK -
    // EU / Intl RKE band").
    sub_label = lv_label_create(tesla_cp_screen);
    lv_obj_set_style_text_color(sub_label, lv_color_make(0xAA, 0xAA, 0xAA), LV_PART_MAIN);
    lv_obj_set_style_text_font(sub_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_label_set_text(sub_label, "");
    lv_obj_align(sub_label, LV_ALIGN_TOP_MID, 0, 114);

    // Stylized charge-port graphic — rounded-corner outer housing with
    // three short prong rectangles inside, in the matte black look of a
    // real Tesla charge port. Shifted from y=100 to y=140 to clear the
    // new selector row and freq sub-label above.
    lv_obj_t *port = lv_obj_create(tesla_cp_screen);
    lv_obj_set_size(port, 140, 90);
    lv_obj_set_style_radius(port, 22, LV_PART_MAIN);
    lv_obj_set_style_bg_color(port, lv_color_make(0x10, 0x10, 0x10), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(port, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(port, lv_color_make(0x55, 0x55, 0x55), LV_PART_MAIN);
    lv_obj_set_style_border_width(port, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(port, 0, LV_PART_MAIN);
    lv_obj_clear_flag(port, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(port, LV_ALIGN_TOP_MID, 0, 140);

    for (int i = -1; i <= 1; i++) {
        lv_obj_t *prong = lv_obj_create(port);
        lv_obj_set_size(prong, 14, 22);
        lv_obj_set_style_radius(prong, 3, LV_PART_MAIN);
        lv_obj_set_style_bg_color(prong, lv_color_make(0x55, 0x55, 0x55), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(prong, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(prong, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(prong, 0, LV_PART_MAIN);
        lv_obj_clear_flag(prong, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(prong, LV_ALIGN_CENTER, i * 28, -8);
    }

    // Big red OPEN button — shifted from y=220 to y=248 to keep its
    // distance from the port graphic the same after the selector row
    // pushed everything else down.
    send_btn = lv_obj_create(tesla_cp_screen);
    lv_obj_set_size(send_btn, 280, 80);
    lv_obj_align(send_btn, LV_ALIGN_TOP_MID, 0, 248);
    lv_obj_set_style_radius(send_btn, 10, LV_PART_MAIN);
    lv_obj_set_style_bg_color(send_btn, lv_color_make(0xCC, 0x22, 0x22), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(send_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(send_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(send_btn, 0, LV_PART_MAIN);
    lv_obj_clear_flag(send_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(send_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(send_btn, on_send, LV_EVENT_CLICKED, NULL);

    send_btn_label = lv_label_create(send_btn);
    lv_label_set_text(send_btn_label, "OPEN CHARGE PORT");
    lv_obj_set_style_text_color(send_btn_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(send_btn_label, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_center(send_btn_label);

    // Status line for "Transmitting..." / "Sent" / "Failed (err N)".
    status_label = lv_label_create(tesla_cp_screen);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(status_label, lv_color_make(0x88, 0x88, 0x88), LV_PART_MAIN);
    lv_label_set_text(status_label, "Tap to send");
    lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 340);

    // Disclaimer line so it's obvious what this is and isn't.
    hint_label = lv_label_create(tesla_cp_screen);
    lv_obj_set_style_text_font(hint_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint_label, lv_color_make(0x66, 0x66, 0x66), LV_PART_MAIN);
    lv_obj_set_style_text_align(hint_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(hint_label,
        "Your own vehicle / authorized use only.\n"
        "Newer Tesla firmware (2022.20+) ignores this signal.");
    lv_obj_set_width(hint_label, 380);
    lv_obj_align(hint_label, LV_ALIGN_TOP_MID, 0, 366);

    // Seed the selector's visual state and the sub-label text.
    update_region_selection();

    // 500 ms tick keeps the button enabled/disabled state in sync with the
    // shared-radio holders (LoRa screen / APRS / pager) without the user
    // having to re-enter the screen.
    lv_timer_create(on_periodic_update, 500, NULL);
}

void tesla_cp_screen_show()
{
    update_status("Tap to send", lv_color_make(0x88, 0x88, 0x88));
    update_send_btn_state();
    lv_scr_load(tesla_cp_screen);
}

bool tesla_cp_screen_is_active()
{
    return lv_screen_active() == tesla_cp_screen;
}
