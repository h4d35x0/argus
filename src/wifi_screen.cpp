#include "wifi_screen.h"
#include "theme.h"
#include "pingsweep.h"
#include "hostresolve.h"
#include "portscan_screen.h"
#include <LilyGoLib.h>
#include <WiFi.h>
#include <string.h>

// Defined in tools_screen.cpp
void tools_screen_show();

// UI phases. "Connected, sweeping" is not a separate phase — it is WST_CONNECTED
// with pingsweep_is_running() true.
enum WifiState {
    WST_IDLE,        // nothing scanned yet
    WST_SCANNING,    // async site survey running
    WST_LIST,        // network list shown, tap to connect
    WST_PASSWORD,    // entering a network password
    WST_CONNECTING,  // WiFi.begin() in progress
    WST_CONNECTED,   // associated to a network
};

struct ScanNet {
    char    ssid[33];
    int32_t rssi;
    uint8_t channel;
    bool    open;
};

static lv_obj_t *wifi_screen;
static lv_obj_t *status_label;
static lv_obj_t *pw_label;
static lv_obj_t *pw_ta;
static lv_obj_t *btn_row;
static lv_obj_t *btn1, *btn1_label;
static lv_obj_t *btn2, *btn2_label;
static lv_obj_t *list_box;
static lv_obj_t *keyboard;

static WifiState s_state = WST_IDLE;
static ScanNet   s_nets[24];
static int       s_net_count = 0;
static int       s_pending   = -1;   // network index awaiting a password
static uint32_t  s_connect_start = 0;
static int       s_shown_dev = -1;   // device count last drawn into the list

// ---- list rendering --------------------------------------------------------

static lv_obj_t *make_card(bool clickable)
{
    lv_obj_t *card = lv_obj_create(list_box);
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, lv_color_make(0x16, 0x16, 0x16), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(card, lv_color_make(0x33, 0x33, 0x33), LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_row(card, 2, LV_PART_MAIN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(card, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    if (clickable) lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    return card;
}

static void add_text(lv_obj_t *card, const char *txt, const lv_font_t *font, lv_color_t col)
{
    lv_obj_t *l = lv_label_create(card);
    lv_obj_set_style_text_font(l, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, col, LV_PART_MAIN);
    lv_label_set_text(l, txt);
}

static void placeholder(const char *txt)
{
    lv_obj_t *l = lv_label_create(list_box);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, lv_color_make(0x66, 0x66, 0x66), LV_PART_MAIN);
    lv_label_set_text(l, txt);
    // float out of the flex flow so we can centre absolutely in the list box.
    lv_obj_add_flag(l, LV_OBJ_FLAG_FLOATING);
    lv_obj_center(l);
}

static void on_net_clicked(lv_event_t *e);

static void show_networks()
{
    lv_obj_clean(list_box);
    if (s_net_count == 0) { placeholder("No networks found"); return; }
    for (int i = 0; i < s_net_count; i++) {
        lv_obj_t *card = make_card(true);
        lv_obj_add_event_cb(card, on_net_clicked, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        add_text(card, s_nets[i].ssid[0] ? s_nets[i].ssid : "(hidden)",
                 &lv_font_montserrat_20, lv_color_white());
        char det[48];
        snprintf(det, sizeof(det), "%d dBm   ch %d   %s",
                 (int)s_nets[i].rssi, s_nets[i].channel,
                 s_nets[i].open ? "open" : "secured");
        add_text(card, det, &lv_font_montserrat_14, lv_color_make(0x99, 0x99, 0x99));
    }
}

static void show_devices()
{
    lv_obj_clean(list_box);
    int n = pingsweep_device_count();
    if (n == 0) {
        placeholder(pingsweep_is_running() ? "Sweeping..."
                                           : "Press PING SWEEP to discover devices");
        s_shown_dev = 0;
        return;
    }
    for (int i = 0; i < n; i++) {
        const PingDevice *d = pingsweep_device(i);
        if (!d) continue;
        lv_obj_t *card = make_card(true);   // clickable → opens port scanner
        // Carry the host-order IP on the row as user_data so the click
        // handler can pull it back out without re-indexing through
        // pingsweep_device(i) (which can mutate during a sweep).
        lv_obj_set_user_data(card, (void *)(uintptr_t)d->ip);
        lv_obj_add_event_cb(card, [](lv_event_t *e) {
            lv_obj_t *t = (lv_obj_t *)lv_event_get_target(e);
            uint32_t ip = (uint32_t)(uintptr_t)lv_obj_get_user_data(t);
            if (!ip) return;
            // Look up the current row's resolved name (if any) and pass
            // it to the port scanner so the title can read e.g.
            // "printer.local — TCP+UDP" instead of just the IP.
            const char *name = nullptr;
            int dn = pingsweep_device_count();
            for (int j = 0; j < dn; j++) {
                const PingDevice *dd = pingsweep_device(j);
                if (dd && dd->ip == ip &&
                    dd->name_source != PNAME_NONE && dd->name[0])
                {
                    name = dd->name;
                    break;
                }
            }
            portscan_screen_show(ip, name);
        }, LV_EVENT_CLICKED, NULL);

        // Primary line — hostname when we have one, IP otherwise. Putting
        // the resolved name first puts the most useful identifier where
        // the user's eye lands.
        char ip[20];
        snprintf(ip, sizeof(ip), "%u.%u.%u.%u",
                 (d->ip >> 24) & 0xFF, (d->ip >> 16) & 0xFF,
                 (d->ip >> 8) & 0xFF, d->ip & 0xFF);
        if (d->name_source != PNAME_NONE && d->name[0])
            add_text(card, d->name, &lv_font_montserrat_20, lv_color_white());
        else
            add_text(card, ip, &lv_font_montserrat_20, lv_color_white());

        // Secondary line — the IP (when we showed a name above) plus the
        // RTT and MAC. The OUI vendor goes on this line too via the name
        // tag if we want, but it's already shown as the primary in the
        // OUI-only case.
        char det[96];
        const char *src_tag =
            (d->name_source == PNAME_MDNS) ? " (mdns)"  :
            (d->name_source == PNAME_NBNS) ? " (nbns)"  :
            (d->name_source == PNAME_PTR)  ? " (dns)"   :
            (d->name_source == PNAME_OUI)  ? " (oui)"   : "";
        bool show_ip_on_line2 = (d->name_source != PNAME_NONE && d->name[0]);
        if (d->has_mac) {
            snprintf(det, sizeof(det),
                "%s%s  %u ms  %02X:%02X:%02X:%02X:%02X:%02X",
                show_ip_on_line2 ? ip : "", src_tag,
                d->rtt_ms, d->mac[0], d->mac[1], d->mac[2],
                d->mac[3], d->mac[4], d->mac[5]);
        } else {
            snprintf(det, sizeof(det), "%s%s  %u ms",
                show_ip_on_line2 ? ip : "", src_tag, d->rtt_ms);
        }
        add_text(card, det, &lv_font_montserrat_14, lv_color_make(0x99, 0x99, 0x99));
    }
    s_shown_dev = n;
}

// ---- status / buttons ------------------------------------------------------

static void update_status()
{
    char buf[80];
    const char *txt = buf;
    lv_color_t  col = lv_color_make(0x88, 0x88, 0x88);

    switch (s_state) {
    case WST_IDLE:
        txt = "Tap SCAN to survey nearby networks";
        break;
    case WST_SCANNING:
        txt = "Scanning...";
        col = lv_color_make(0xFF, 0xCC, 0x00);
        break;
    case WST_LIST:
        snprintf(buf, sizeof(buf), "%d networks - tap one to connect", s_net_count);
        break;
    case WST_PASSWORD:
        txt = "Enter the network password";
        col = lv_color_make(0xFF, 0xCC, 0x00);
        break;
    case WST_CONNECTING:
        txt = "Connecting...";
        col = lv_color_make(0xFF, 0xCC, 0x00);
        break;
    case WST_CONNECTED:
        if (pingsweep_is_running()) {
            snprintf(buf, sizeof(buf), "Sweeping %d/%d - %d found",
                     pingsweep_scanned(), pingsweep_total(),
                     pingsweep_device_count());
            col = lv_color_make(0xFF, 0xCC, 0x00);
        } else if (pingsweep_device_count() > 0) {
            // Once the sweep ends the hostname resolver kicks off — show
            // its pass + running totals so the user can see what's
            // actually happening (and whether DNS / mDNS are producing
            // any responses at all on this network).
            HostResolveStats hs;
            hostresolve_get_stats(&hs);
            if (hostresolve_is_running()) {
                if (hs.pass == HRPASS_MDNS) {
                    snprintf(buf, sizeof(buf),
                             "Resolving mDNS - sent %u, %u replies, %u named",
                             hs.mdns_sent, hs.mdns_replies, hs.mdns_named);
                } else if (hs.pass == HRPASS_NBNS) {
                    snprintf(buf, sizeof(buf),
                             "Resolving NetBIOS - sent %u, %u replies, %u named",
                             hs.nbns_sent, hs.nbns_replies, hs.nbns_named);
                } else if (hs.pass == HRPASS_DNS) {
                    if (hs.dns_server_ip == 0) {
                        snprintf(buf, sizeof(buf),
                                 "Resolving DNS - no LAN resolver configured");
                    } else {
                        snprintf(buf, sizeof(buf),
                                 "Resolving DNS %u.%u.%u.%u - sent %u, %u replies, %u named",
                                 (hs.dns_server_ip >> 24) & 0xFF,
                                 (hs.dns_server_ip >> 16) & 0xFF,
                                 (hs.dns_server_ip >>  8) & 0xFF,
                                  hs.dns_server_ip        & 0xFF,
                                 hs.dns_sent, hs.dns_replies, hs.dns_named);
                    }
                } else {
                    snprintf(buf, sizeof(buf), "Resolving OUI vendors...");
                }
                col = lv_color_make(0xFF, 0xCC, 0x00);
            } else if (hs.pass == HRPASS_DONE) {
                // Resolver just finished — show a breakdown so the user
                // sees what each pass actually contributed.
                snprintf(buf, sizeof(buf),
                         "%d dev  mdns=%u nbns=%u dns=%u oui=%u",
                         pingsweep_device_count(),
                         hs.mdns_named, hs.nbns_named,
                         hs.dns_named, hs.oui_named);
                col = ARGUS_ACCENT;
            } else {
                snprintf(buf, sizeof(buf), "Sweep complete - %d devices",
                         pingsweep_device_count());
                col = ARGUS_ACCENT;
            }
        } else {
            IPAddress ip = WiFi.localIP();
            snprintf(buf, sizeof(buf), "%s   %u.%u.%u.%u",
                     WiFi.SSID().c_str(), ip[0], ip[1], ip[2], ip[3]);
            col = ARGUS_ACCENT;
        }
        break;
    }
    lv_label_set_text(status_label, txt);
    lv_obj_set_style_text_color(status_label, col, LV_PART_MAIN);
}

static void update_buttons()
{
    if (s_state == WST_PASSWORD) {
        lv_obj_add_flag(btn_row, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_HIDDEN);

    bool two = (s_state == WST_CONNECTED);
    if (two) {
        lv_label_set_text(btn1_label,
            pingsweep_is_running() ? "SWEEPING..." : "PING SWEEP");
        lv_obj_set_style_bg_color(btn1,
            pingsweep_is_running() ? lv_color_make(0x55, 0x55, 0x55)
                                   : lv_color_make(0x00, 0x88, 0xCC), LV_PART_MAIN);
        lv_label_set_text(btn2_label, "DISCONNECT");
        lv_obj_clear_flag(btn2, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(btn1, LV_ALIGN_LEFT_MID,  6, 0);
        lv_obj_align(btn2, LV_ALIGN_RIGHT_MID, -6, 0);
    } else {
        const char *l = (s_state == WST_SCANNING)   ? "SCANNING..." :
                        (s_state == WST_CONNECTING) ? "CONNECTING..." : "SCAN";
        lv_label_set_text(btn1_label, l);
        lv_obj_set_style_bg_color(btn1, lv_color_make(0x00, 0x88, 0xCC), LV_PART_MAIN);
        lv_obj_add_flag(btn2, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(btn1, LV_ALIGN_CENTER, 0, 0);
    }
}

// ---- WiFi flow -------------------------------------------------------------

static void enter_password_mode()
{
    char buf[48];
    snprintf(buf, sizeof(buf), "Password for %s", s_nets[s_pending].ssid);
    lv_label_set_text(pw_label, buf);
    lv_textarea_set_text(pw_ta, "");
    lv_obj_clear_flag(pw_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(pw_ta,    LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(list_box,   LV_OBJ_FLAG_HIDDEN);
    lv_keyboard_set_textarea(keyboard, pw_ta);
    lv_obj_clear_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
}

static void exit_password_mode()
{
    lv_obj_add_flag(pw_label,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(pw_ta,     LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(keyboard,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(list_box, LV_OBJ_FLAG_HIDDEN);
}

static void begin_connect(const char *ssid, const char *pass)
{
    WiFi.mode(WIFI_STA);
    if (pass && pass[0]) WiFi.begin(ssid, pass);
    else                 WiFi.begin(ssid);
    s_connect_start = millis();
    s_state = WST_CONNECTING;
}

static void on_net_clicked(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_net_count) return;
    s_pending = idx;
    if (s_nets[idx].open) {
        begin_connect(s_nets[idx].ssid, nullptr);
    } else {
        s_state = WST_PASSWORD;
        enter_password_mode();
    }
    update_status();
    update_buttons();
}

static void on_kb_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY && s_pending >= 0) {
        begin_connect(s_nets[s_pending].ssid, lv_textarea_get_text(pw_ta));
    } else {
        s_state = WST_LIST;   // cancelled
    }
    exit_password_mode();
    update_status();
    update_buttons();
}

static void start_scan()
{
    WiFi.mode(WIFI_STA);
    WiFi.scanDelete();
    WiFi.scanNetworks(true);   // async
    s_state = WST_SCANNING;
}

static void on_btn1(lv_event_t *)
{
    if (s_state == WST_CONNECTED) {
        if (!pingsweep_is_running()) {
            pingsweep_start();
            s_shown_dev = -1;
        }
    } else if (s_state == WST_IDLE || s_state == WST_LIST) {
        start_scan();
    }
    update_status();
    update_buttons();
}

static void on_btn2(lv_event_t *)   // DISCONNECT
{
    pingsweep_stop();
    WiFi.disconnect(true);
    s_state = (s_net_count > 0) ? WST_LIST : WST_IDLE;
    if (s_state == WST_LIST) show_networks();
    else { lv_obj_clean(list_box); placeholder("Tap SCAN to begin"); }
    update_status();
    update_buttons();
}


// ---- periodic refresh ------------------------------------------------------

static void on_refresh(lv_timer_t *)
{
    switch (s_state) {
    case WST_SCANNING: {
        int n = WiFi.scanComplete();
        if (n >= 0) {
            s_net_count = n > 24 ? 24 : n;
            for (int i = 0; i < s_net_count; i++) {
                strncpy(s_nets[i].ssid, WiFi.SSID(i).c_str(), 32);
                s_nets[i].ssid[32] = '\0';
                s_nets[i].rssi    = WiFi.RSSI(i);
                s_nets[i].channel = WiFi.channel(i);
                s_nets[i].open    = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
            }
            WiFi.scanDelete();
            s_state = WST_LIST;
            show_networks();
        } else if (n == WIFI_SCAN_FAILED) {
            s_state = WST_IDLE;
        }
        break;
    }
    case WST_CONNECTING:
        if (WiFi.status() == WL_CONNECTED) {
            s_state = WST_CONNECTED;
            s_shown_dev = -1;
            show_devices();
        } else if (millis() - s_connect_start > 15000) {
            WiFi.disconnect(true);
            s_state = WST_LIST;
            show_networks();
        }
        break;
    case WST_CONNECTED:
        if (WiFi.status() != WL_CONNECTED) {
            s_state = (s_net_count > 0) ? WST_LIST : WST_IDLE;
            if (s_state == WST_LIST) show_networks();
        } else if (pingsweep_device_count() != s_shown_dev) {
            show_devices();
        } else {
            // Resolver writes names in waves (mDNS → DNS → OUI). Repaint
            // each refresh tick while it's working so newly-resolved
            // names appear on the row as they arrive — and do one final
            // repaint on the running → idle transition to catch the
            // last batch.
            static bool s_was_resolving = false;
            bool now_resolving = hostresolve_is_running();
            if (now_resolving || s_was_resolving) show_devices();
            s_was_resolving = now_resolving;
        }
        break;
    default:
        break;
    }
    update_status();
    update_buttons();
}

// ---- layout ----------------------------------------------------------------

static lv_obj_t *make_button(lv_obj_t *parent, lv_coord_t w, lv_coord_t h,
                             lv_color_t bg, lv_obj_t **label_out)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(b, bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(b, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(b, 0, LV_PART_MAIN);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_color(l, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_center(l);
    if (label_out) *label_out = l;
    return b;
}

void wifi_screen_create()
{
    wifi_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(wifi_screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(wifi_screen, 0, LV_PART_MAIN);
    lv_obj_clear_flag(wifi_screen, LV_OBJ_FLAG_SCROLLABLE);

    // Title — font_48 to match the PAGER / TPMS / SETTINGS / analyze headers.
    lv_obj_t *title = lv_label_create(wifi_screen);
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_label_set_text(title, "WiFi");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    status_label = lv_label_create(wifi_screen);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(status_label, lv_color_make(0x88, 0x88, 0x88), LV_PART_MAIN);
    lv_label_set_text(status_label, "Tap SCAN to survey nearby networks");
    lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 56);

    // Password field (hidden until a secured network is chosen)
    pw_label = lv_label_create(wifi_screen);
    lv_obj_set_style_text_font(pw_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(pw_label, lv_color_make(0xAA, 0xAA, 0xAA), LV_PART_MAIN);
    lv_label_set_text(pw_label, "Password");
    lv_obj_align(pw_label, LV_ALIGN_TOP_MID, 0, 78);
    lv_obj_add_flag(pw_label, LV_OBJ_FLAG_HIDDEN);

    pw_ta = lv_textarea_create(wifi_screen);
    lv_textarea_set_one_line(pw_ta, true);
    lv_textarea_set_password_mode(pw_ta, true);
    lv_textarea_set_max_length(pw_ta, 63);
    lv_obj_set_size(pw_ta, 376, 44);
    lv_obj_align(pw_ta, LV_ALIGN_TOP_MID, 0, 100);
    lv_obj_set_style_text_font(pw_ta, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_bg_color(pw_ta, lv_color_make(0x11, 0x11, 0x11), LV_PART_MAIN);
    lv_obj_set_style_text_color(pw_ta, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_border_color(pw_ta, lv_color_make(0x44, 0x44, 0x44), LV_PART_MAIN);
    lv_obj_set_style_border_width(pw_ta, 1, LV_PART_MAIN);
    lv_obj_add_flag(pw_ta, LV_OBJ_FLAG_HIDDEN);

    // Button row
    btn_row = lv_obj_create(wifi_screen);
    lv_obj_set_size(btn_row, 404, 50);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn_row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(btn_row, LV_ALIGN_TOP_MID, 0, 80);

    btn1 = make_button(btn_row, 196, 48, lv_color_make(0x00, 0x88, 0xCC), &btn1_label);
    lv_obj_align(btn1, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(btn1, on_btn1, LV_EVENT_CLICKED, NULL);
    lv_label_set_text(btn1_label, "SCAN");

    btn2 = make_button(btn_row, 196, 48, lv_color_make(0x88, 0x22, 0x22), &btn2_label);
    lv_obj_align(btn2, LV_ALIGN_RIGHT_MID, -6, 0);
    lv_obj_add_event_cb(btn2, on_btn2, LV_EVENT_CLICKED, NULL);
    lv_label_set_text(btn2_label, "DISCONNECT");
    lv_obj_add_flag(btn2, LV_OBJ_FLAG_HIDDEN);

    // Scrolling list — networks, then discovered devices
    list_box = lv_obj_create(wifi_screen);
    lv_obj_set_size(list_box, 404, 352);
    lv_obj_align(list_box, LV_ALIGN_TOP_MID, 0, 140);
    lv_obj_set_style_bg_color(list_box, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_color(list_box, lv_color_make(0x33, 0x33, 0x33), LV_PART_MAIN);
    lv_obj_set_style_border_width(list_box, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(list_box, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(list_box, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_row(list_box, 6, LV_PART_MAIN);
    lv_obj_set_scroll_dir(list_box, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list_box, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_layout(list_box, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(list_box, LV_FLEX_FLOW_COLUMN);

    keyboard = lv_keyboard_create(wifi_screen);
    lv_obj_set_size(keyboard, 410, 240);
    lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(keyboard, on_kb_event, LV_EVENT_READY,  NULL);
    lv_obj_add_event_cb(keyboard, on_kb_event, LV_EVENT_CANCEL, NULL);

    lv_timer_create(on_refresh, 1000, NULL);
}

void wifi_screen_show()
{
    exit_password_mode();
    if (WiFi.status() == WL_CONNECTED) {
        s_state = WST_CONNECTED;
        s_shown_dev = -1;
        show_devices();
    } else if (s_net_count > 0) {
        s_state = WST_LIST;
        show_networks();
    } else {
        s_state = WST_IDLE;
        lv_obj_clean(list_box);
        placeholder("Tap SCAN to begin");
    }
    update_status();
    update_buttons();
    lv_scr_load(wifi_screen);
}

bool wifi_screen_is_active()
{
    return lv_screen_active() == wifi_screen;
}
