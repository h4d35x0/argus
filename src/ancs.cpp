// ancs.cpp - see ancs.h. iOS ANCS Notification Consumer on raw Bluedroid.
//
// Flow (mirrors Apple's ANCS spec and the ESP-IDF ble_ancs example):
//   1. GATTS advertises us as connectable, with ANCS in the solicitation list.
//   2. iPhone connects -> we request encryption -> user confirms pairing -> bond.
//   3. On auth-complete we open a GATTC link to the iPhone and discover ANCS.
//   4. Subscribe to Notification Source + Data Source.
//   5. Notification Source event -> write Get-Notification-Attributes to the
//      Control Point -> Data Source delivers app/title/message -> publish.
//
// We do our own controller bring-up and MUST NOT run while WiFi is up (the
// esp_bt_controller_enable() hang documented in ble_scan_manager.cpp). The mode
// owner enforces that ANCS and the BLE scanners never run together.
#include "ancs.h"
#include "notify/notify_center.h"
#include "notify/notify_log.h"

#include <Arduino.h>
#include <WiFi.h>
#include <cstring>

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_gattc_api.h"
#include "esp_gatt_common_api.h"

namespace ancs {
namespace {

// ---- ANCS UUIDs (128-bit, stored LSB-first as Bluedroid expects) -----------
// Service            7905F431-B5CE-4E99-A40F-4B1E122D00D0
// Notification Src   9FBF120D-6301-42D9-8C58-25E699A21DBD  (notify)
// Control Point      69D1D8F3-45E1-49A8-9821-9BBDFDAAD9D9  (write)
// Data Source        22EAC6E9-24D6-4BB5-BE44-B36ACE7C7BFB  (notify)
const uint8_t UUID_SVC[16] = {
    0xD0,0x00,0x2D,0x12,0x1E,0x4B,0x0F,0xA4,0x99,0x4E,0xCE,0xB5,0x31,0xF4,0x05,0x79};
const uint8_t UUID_NOTIF_SRC[16] = {
    0xBD,0x1D,0xA2,0x99,0xE6,0x25,0x58,0x8C,0xD9,0x42,0x01,0x63,0x0D,0x12,0xBF,0x9F};
const uint8_t UUID_CTRL_PT[16] = {
    0xD9,0xD9,0xAA,0xFD,0xBD,0x9B,0x21,0x98,0xA8,0x49,0xE1,0x45,0xF3,0xD8,0xD1,0x69};
const uint8_t UUID_DATA_SRC[16] = {
    0xFB,0x7B,0x7C,0xCE,0x6A,0xB3,0x44,0xBE,0xB5,0x4B,0xD6,0x24,0xE9,0xC6,0xEA,0x22};

// ANCS constants.
enum { EVENT_ADDED = 0, EVENT_MODIFIED = 1, EVENT_REMOVED = 2 };
enum { CMD_GET_NOTIFICATION_ATTRIBUTES = 0, CMD_PERFORM_NOTIFICATION_ACTION = 2 };
enum { ACTION_POSITIVE = 0, ACTION_NEGATIVE = 1 };
enum { ATTR_APP_ID = 0, ATTR_TITLE = 1, ATTR_MESSAGE = 3 };
static constexpr uint16_t TITLE_MAXLEN = 60;
static constexpr uint16_t BODY_MAXLEN  = 150;

// ---- State -----------------------------------------------------------------
bool s_running   = false;
bool s_connected = false;   // ANCS discovered + subscribed

esp_gatt_if_t s_gatts_if = ESP_GATT_IF_NONE;
esp_gatt_if_t s_gattc_if = ESP_GATT_IF_NONE;

uint16_t             s_gattc_conn = 0xFFFF;
esp_bd_addr_t        s_peer_bda   = {0};
esp_ble_addr_type_t  s_peer_type  = BLE_ADDR_TYPE_PUBLIC;

uint16_t s_svc_start = 0, s_svc_end = 0;
uint16_t s_h_notif_src = 0, s_h_ctrl_pt = 0, s_h_data_src = 0;
uint16_t s_h_svc_changed = 0;   // GATT Service Changed char (0x2A05), indicate

bool s_adv_ready   = false;  // raw scan-rsp set complete -> safe to start adv
bool s_advertising = false;  // controller confirmed advertising is on the air

// Remember each pending notification's category so we can tag it when the Data
// Source response (which carries no category) comes back. Tiny ring is plenty.
struct Pending { uint32_t uid; uint8_t cat; bool used; };
Pending s_pending[8] = {};

// Data Source reassembly (responses can span multiple notifications).
uint8_t  s_ds[512];
uint16_t s_ds_len = 0;

// ---- helpers ---------------------------------------------------------------
bool wifi_active() { return WiFi.getMode() != WIFI_MODE_NULL; }

esp_bt_uuid_t uuid128(const uint8_t b[16])
{
    esp_bt_uuid_t u;
    u.len = ESP_UUID_LEN_128;
    memcpy(u.uuid.uuid128, b, 16);
    return u;
}

uint32_t rd_u32le(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

notify::Category map_category(uint8_t ancs_cat)
{
    using C = notify::Category;
    switch (ancs_cat) {
        case 1:  return C::IncomingCall;
        case 2:  return C::MissedCall;
        case 3:  return C::Voicemail;
        case 4:  return C::Social;
        case 5:  return C::Schedule;
        case 6:  return C::Email;
        case 7:  return C::News;
        case 8:  return C::HealthFitness;
        case 9:  return C::BusinessFinance;
        case 10: return C::Location;
        case 11: return C::Entertainment;
        default: return C::Other;
    }
}

void remember_category(uint32_t uid, uint8_t cat)
{
    for (auto& p : s_pending) {
        if (p.used && p.uid == uid) { p.cat = cat; return; }
    }
    for (auto& p : s_pending) {
        if (!p.used) { p.uid = uid; p.cat = cat; p.used = true; return; }
    }
    // Ring full: overwrite slot 0 (oldest-ish). Rare; attribute round-trips are fast.
    s_pending[0] = { uid, cat, true };
}

uint8_t take_category(uint32_t uid)
{
    for (auto& p : s_pending) {
        if (p.used && p.uid == uid) { p.used = false; return p.cat; }
    }
    return 0;  // Other
}

void reset_link_state()
{
    s_connected   = false;
    s_gattc_conn  = 0xFFFF;
    s_svc_start   = s_svc_end = 0;
    s_h_notif_src = s_h_ctrl_pt = s_h_data_src = 0;
    s_h_svc_changed = 0;
    s_ds_len      = 0;
    memset(s_pending, 0, sizeof(s_pending));
}

// ---- advertising -----------------------------------------------------------
// iOS lists a peripheral by the name in its MAIN advertising packet, but it also
// needs the ANCS service SOLICITATION to offer ANCS. Both together (flags + name
// + 128-bit solicitation) exceed the 31-byte adv packet, so we split them: the
// complete local name goes in the main packet (what iOS shows), and the ANCS
// solicitation goes in the scan response (iOS active-scans, so it still sees it).
//
// Scan-response payload: 128-bit ANCS service solicitation (AD type 0x15).
const uint8_t SCAN_RSP_SOLICIT[] = {
    0x11, 0x15,
    0xD0,0x00,0x2D,0x12,0x1E,0x4B,0x0F,0xA4,0x99,0x4E,0xCE,0xB5,0x31,0xF4,0x05,0x79 };

// Kick off the adv config chain. Bluedroid processes ONE GAP config at a time,
// so we set the adv data here and set the scan response only after the adv-data
// completion event (see gap_cb); starting them back-to-back drops the second and
// advertising never begins.
void build_and_set_adv()
{
    // Main ADV packet: flags + complete local name (AD type 0x09).
    uint8_t adv[3 + 2 + 11];
    int i = 0;
    adv[i++] = 0x02; adv[i++] = 0x01; adv[i++] = 0x06;          // Flags: LE General Disc
    adv[i++] = 0x0C; adv[i++] = 0x09;                           // len=12, complete name
    const char *nm = "Argus Watch";
    memcpy(&adv[i], nm, 11); i += 11;
    esp_err_t e = esp_ble_gap_config_adv_data_raw(adv, i);
    NLOG("[ancs] config_adv_data_raw -> %d\n", (int)e);
}

void start_advertising()
{
    esp_ble_adv_params_t p;
    memset(&p, 0, sizeof(p));
    p.adv_int_min       = 0x20;
    p.adv_int_max       = 0x40;
    p.adv_type          = ADV_TYPE_IND;
    p.own_addr_type     = BLE_ADDR_TYPE_PUBLIC;
    p.channel_map       = ADV_CHNL_ALL;
    p.adv_filter_policy  = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY;
    esp_ble_gap_start_advertising(&p);
}

// ---- ANCS message handling -------------------------------------------------
void request_attributes(uint32_t uid)
{
    uint8_t cmd[16];
    int i = 0;
    cmd[i++] = CMD_GET_NOTIFICATION_ATTRIBUTES;
    cmd[i++] = (uint8_t)(uid & 0xFF);
    cmd[i++] = (uint8_t)((uid >> 8) & 0xFF);
    cmd[i++] = (uint8_t)((uid >> 16) & 0xFF);
    cmd[i++] = (uint8_t)((uid >> 24) & 0xFF);
    cmd[i++] = ATTR_APP_ID;                                     // no length -> full
    cmd[i++] = ATTR_TITLE;
    cmd[i++] = (uint8_t)(TITLE_MAXLEN & 0xFF);
    cmd[i++] = (uint8_t)(TITLE_MAXLEN >> 8);
    cmd[i++] = ATTR_MESSAGE;
    cmd[i++] = (uint8_t)(BODY_MAXLEN & 0xFF);
    cmd[i++] = (uint8_t)(BODY_MAXLEN >> 8);
    esp_ble_gattc_write_char(s_gattc_if, s_gattc_conn, s_h_ctrl_pt,
                             i, cmd, ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
}

void handle_notification_source(const uint8_t* d, uint16_t len)
{
    if (len < 8) return;
    uint8_t  event_id = d[0];
    uint8_t  cat_id   = d[2];
    uint32_t uid      = rd_u32le(&d[4]);

    if (event_id == EVENT_REMOVED) {
        notify::retract(uid);
        return;
    }
    remember_category(uid, cat_id);
    request_attributes(uid);
}

void handle_data_source(const uint8_t* d, uint16_t len)
{
    // Append to the reassembly buffer (bounded).
    if (s_ds_len + len > sizeof(s_ds)) s_ds_len = 0;   // overflow guard: drop partial
    if (len > sizeof(s_ds)) return;
    memcpy(&s_ds[s_ds_len], d, len);
    s_ds_len += len;

    if (s_ds_len < 5) return;   // need CommandID(1)+UID(4)

    uint32_t uid = rd_u32le(&s_ds[1]);
    uint16_t pos = 5;

    notify::Notification n;
    n.uid = uid;
    int parsed = 0;
    while (pos + 3 <= s_ds_len) {
        uint8_t  attr_id = s_ds[pos];
        uint16_t alen    = (uint16_t)s_ds[pos + 1] | ((uint16_t)s_ds[pos + 2] << 8);
        if (pos + 3 + alen > s_ds_len) return;   // value not fully arrived yet
        const char* val = (const char*)&s_ds[pos + 3];
        switch (attr_id) {
            case ATTR_APP_ID:
                strncpy(n.app, val, std::min<uint16_t>(alen, notify::kAppLen - 1));
                break;
            case ATTR_TITLE:
                strncpy(n.title, val, std::min<uint16_t>(alen, notify::kTitleLen - 1));
                break;
            case ATTR_MESSAGE:
                strncpy(n.body, val, std::min<uint16_t>(alen, notify::kBodyLen - 1));
                break;
            default: break;
        }
        pos += 3 + alen;
        parsed++;
        if (parsed >= 3) break;   // we asked for app + title + message
    }

    if (parsed >= 3) {
        n.category = map_category(take_category(uid));
        notify::publish(n);
        s_ds_len = 0;
    }
}

// ---- GATTC callback --------------------------------------------------------
void subscribe_char(uint16_t char_handle)
{
    // Register locally, then write the CCCD to enable notifications.
    esp_ble_gattc_register_for_notify(s_gattc_if, s_peer_bda, char_handle);
}

void write_cccd(uint16_t char_handle)
{
    esp_gattc_descr_elem_t descr;
    uint16_t count = 1;
    esp_bt_uuid_t cccd;
    cccd.len = ESP_UUID_LEN_16;
    cccd.uuid.uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;
    esp_gatt_status_t st = esp_ble_gattc_get_descr_by_char_handle(
        s_gattc_if, s_gattc_conn, char_handle, cccd, &descr, &count);
    if (st != ESP_GATT_OK || count == 0) return;
    // Service Changed is an INDICATION (0x0002); the ANCS sources are
    // NOTIFICATIONS (0x0001).
    uint8_t v[2] = { (uint8_t)(char_handle == s_h_svc_changed ? 0x02 : 0x01), 0x00 };
    esp_ble_gattc_write_char_descr(s_gattc_if, s_gattc_conn, descr.handle,
                                   sizeof(v), v, ESP_GATT_WRITE_TYPE_RSP,
                                   ESP_GATT_AUTH_REQ_NONE);
}

// Re-run the ANCS service discovery. iOS exposes ANCS asynchronously AFTER
// bonding and announces it via a GATT Service Changed indication, so the first
// search right after connecting finds nothing; we retry when told services
// changed.
void research_ancs()
{
    s_svc_start = s_svc_end = 0;
    s_h_notif_src = s_h_ctrl_pt = s_h_data_src = 0;
    esp_ble_gattc_cache_refresh(s_peer_bda);   // drop the stale (pre-ANCS) cache
    esp_bt_uuid_t svc = uuid128(UUID_SVC);
    esp_ble_gattc_search_service(s_gattc_if, s_gattc_conn, &svc);
}

// Subscribe to the peer's GATT Service Changed characteristic (0x2A05) so iOS
// can tell us the moment ANCS becomes available.
void subscribe_service_changed()
{
    esp_gattc_char_elem_t elem;
    uint16_t count = 1;
    esp_bt_uuid_t sc;
    sc.len = ESP_UUID_LEN_16;
    sc.uuid.uuid16 = 0x2A05;   // Service Changed
    if (esp_ble_gattc_get_char_by_uuid(s_gattc_if, s_gattc_conn, 0x0001, 0xFFFF,
            sc, &elem, &count) == ESP_GATT_OK && count > 0) {
        s_h_svc_changed = elem.char_handle;
        subscribe_char(s_h_svc_changed);
        NLOGLN("[ancs] subscribed to Service Changed; waiting for iOS to expose ANCS");
    } else {
        NLOGLN("[ancs] Service Changed char not found");
    }
}

void gattc_cb(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
              esp_ble_gattc_cb_param_t* param)
{
    switch (event) {
    case ESP_GATTC_REG_EVT:
        s_gattc_if = gattc_if;
        break;

    case ESP_GATTC_OPEN_EVT:
        if (param->open.status != ESP_GATT_OK) {
            NLOG("[ancs] gattc open failed: %d\n", param->open.status);
            return;
        }
        s_gattc_conn = param->open.conn_id;
        esp_ble_gattc_send_mtu_req(s_gattc_if, s_gattc_conn);
        break;

    case ESP_GATTC_CFG_MTU_EVT: {
        esp_bt_uuid_t svc = uuid128(UUID_SVC);
        esp_ble_gattc_search_service(s_gattc_if, s_gattc_conn, &svc);
        break;
    }

    case ESP_GATTC_SEARCH_RES_EVT:
        if (param->search_res.srvc_id.uuid.len == ESP_UUID_LEN_128 &&
            memcmp(param->search_res.srvc_id.uuid.uuid.uuid128, UUID_SVC, 16) == 0) {
            s_svc_start = param->search_res.start_handle;
            s_svc_end   = param->search_res.end_handle;
        }
        break;

    case ESP_GATTC_SEARCH_CMPL_EVT: {
        if (s_svc_start == 0) {
            // ANCS not exposed yet. iOS reveals it after bonding and signals via
            // Service Changed; subscribe and wait rather than giving up.
            NLOGLN("[ancs] ANCS not present yet - will wait for Service Changed");
            subscribe_service_changed();
            return;
        }
        // Resolve the three characteristic value handles.
        struct { const uint8_t* uuid; uint16_t* out; } chars[] = {
            { UUID_NOTIF_SRC, &s_h_notif_src },
            { UUID_CTRL_PT,   &s_h_ctrl_pt   },
            { UUID_DATA_SRC,  &s_h_data_src  },
        };
        for (auto& c : chars) {
            esp_gattc_char_elem_t elem;
            uint16_t count = 1;
            esp_bt_uuid_t u = uuid128(c.uuid);
            if (esp_ble_gattc_get_char_by_uuid(s_gattc_if, s_gattc_conn,
                    s_svc_start, s_svc_end, u, &elem, &count) == ESP_GATT_OK && count > 0) {
                *c.out = elem.char_handle;
            }
        }
        if (s_h_notif_src && s_h_data_src && s_h_ctrl_pt) {
            subscribe_char(s_h_notif_src);
            subscribe_char(s_h_data_src);
            s_connected = true;
            NLOGLN("[ancs] ANCS found - subscribing, notifications live");
        } else {
            NLOGLN("[ancs] missing an ANCS characteristic");
        }
        break;
    }

    case ESP_GATTC_REG_FOR_NOTIFY_EVT:
        write_cccd(param->reg_for_notify.handle);
        break;

    case ESP_GATTC_SRVC_CHG_EVT:
        // Peer's GATT database changed (iOS just added ANCS). Re-discover.
        NLOGLN("[ancs] SRVC_CHG event -> re-discovering ANCS");
        research_ancs();
        break;

    case ESP_GATTC_NOTIFY_EVT:
        if (param->notify.handle == s_h_svc_changed) {
            // Service Changed indication: iOS exposed ANCS. Re-discover now.
            NLOGLN("[ancs] Service Changed indication -> re-discovering ANCS");
            research_ancs();
        } else if (param->notify.handle == s_h_notif_src) {
            handle_notification_source(param->notify.value, param->notify.value_len);
        } else if (param->notify.handle == s_h_data_src) {
            handle_data_source(param->notify.value, param->notify.value_len);
        }
        break;

    case ESP_GATTC_DISCONNECT_EVT:
        reset_link_state();
        break;

    default:
        break;
    }
}

// ---- GATTS callback (connectable peripheral) -------------------------------
void gatts_cb(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if,
              esp_ble_gatts_cb_param_t* param)
{
    switch (event) {
    case ESP_GATTS_REG_EVT:
        s_gatts_if = gatts_if;
        esp_ble_gap_set_device_name("Argus Watch");
        build_and_set_adv();
        break;

    case ESP_GATTS_CONNECT_EVT:
        NLOGLN("[ancs] phone CONNECTED -> requesting encryption");
        memcpy(s_peer_bda, param->connect.remote_bda, sizeof(esp_bd_addr_t));
        // Ask iOS to encrypt; that triggers pairing/bonding, the gate for ANCS.
        esp_ble_set_encryption(param->connect.remote_bda, ESP_BLE_SEC_ENCRYPT);
        break;

    case ESP_GATTS_DISCONNECT_EVT:
        reset_link_state();
        if (s_running) start_advertising();   // become connectable again
        break;

    default:
        break;
    }
}

// ---- GAP callback ----------------------------------------------------------
void gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param)
{
    switch (event) {
    case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
        // Adv data (name) is set; now (and only now) set the scan response that
        // carries the ANCS solicitation. Doing this back-to-back with the adv-data
        // config drops one of them.
        NLOG("[ancs] adv_data set (status %d); setting scan rsp\n",
                      (int)param->adv_data_raw_cmpl.status);
        esp_ble_gap_config_scan_rsp_data_raw((uint8_t*)SCAN_RSP_SOLICIT,
                                             sizeof(SCAN_RSP_SOLICIT));
        break;

    case ESP_GAP_BLE_SCAN_RSP_DATA_RAW_SET_COMPLETE_EVT:
        s_adv_ready = true;
        NLOG("[ancs] scan rsp set (status %d); starting advertising\n",
                      (int)param->scan_rsp_data_raw_cmpl.status);
        if (s_running) start_advertising();
        break;

    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        // The definitive "are we actually on the air" signal.
        s_advertising = (param->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS);
        NLOG("[ancs] ADV START %s (status %d)\n",
                      s_advertising ? "OK" : "FAILED",
                      (int)param->adv_start_cmpl.status);
        break;

    case ESP_GAP_BLE_SEC_REQ_EVT:
        // Just-works accept. IO cap NONE means no PIN prompt.
        NLOGLN("[ancs] SEC_REQ -> accepting");
        esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
        break;

    case ESP_GAP_BLE_AUTH_CMPL_EVT:
        if (param->ble_security.auth_cmpl.success) {
            s_peer_type = param->ble_security.auth_cmpl.addr_type;
            NLOGLN("[ancs] AUTH OK -> opening GATTC to phone");
            // Open a GATT client link to the now-bonded iPhone and discover ANCS.
            esp_ble_gattc_open(s_gattc_if, s_peer_bda, s_peer_type, true);
        } else {
            NLOG("[ancs] pairing failed, reason 0x%x\n",
                          param->ble_security.auth_cmpl.fail_reason);
        }
        break;

    default:
        break;
    }
}

void set_security_params()
{
    esp_ble_auth_req_t auth = ESP_LE_AUTH_REQ_SC_BOND;
    esp_ble_io_cap_t   io   = ESP_IO_CAP_NONE;
    uint8_t key_size = 16;
    uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t rsp_key  = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth, 1);
    esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &io, 1);
    esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, 1);
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, 1);
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, 1);
}

bool bring_up_controller()
{
    if (wifi_active()) return false;   // enable() would hang with WiFi up

    bool ok = true;
    if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE) {
        esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
        ok = (esp_bt_controller_init(&cfg) == ESP_OK);
    }
    if (ok && esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_INITED)
        ok = (esp_bt_controller_enable(ESP_BT_MODE_BLE) == ESP_OK);
    if (ok && esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_UNINITIALIZED)
        ok = (esp_bluedroid_init() == ESP_OK);
    if (ok && esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_INITIALIZED)
        ok = (esp_bluedroid_enable() == ESP_OK);
    return ok;
}

}  // namespace

bool start()
{
    if (s_running) return true;
    if (!bring_up_controller()) return false;

    if (esp_ble_gap_register_callback(gap_cb)     != ESP_OK) return false;
    if (esp_ble_gatts_register_callback(gatts_cb) != ESP_OK) return false;
    if (esp_ble_gattc_register_callback(gattc_cb) != ESP_OK) return false;

    set_security_params();
    esp_ble_gatt_set_local_mtu(256);

    // App registrations kick off REG_EVT for each profile (stores the ifs),
    // and the GATTS REG_EVT configures advertising.
    esp_ble_gattc_app_register(0);
    esp_ble_gatts_app_register(0);

    s_running = true;
    reset_link_state();
    NLOGLN("[ancs] started - advertising, waiting for iPhone");
    return true;
}

void stop()
{
    if (!s_running) return;
    s_running = false;
    reset_link_state();
    esp_ble_gap_stop_advertising();
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
    s_gatts_if = ESP_GATT_IF_NONE;
    s_gattc_if = ESP_GATT_IF_NONE;
    s_adv_ready = false;
    s_advertising = false;
}

bool is_running()     { return s_running; }
bool is_connected()   { return s_connected; }
bool is_advertising() { return s_advertising; }

void dismiss(uint32_t uid)
{
    if (!s_connected || s_h_ctrl_pt == 0) return;
    uint8_t cmd[6];
    cmd[0] = CMD_PERFORM_NOTIFICATION_ACTION;
    cmd[1] = (uint8_t)(uid & 0xFF);
    cmd[2] = (uint8_t)((uid >> 8) & 0xFF);
    cmd[3] = (uint8_t)((uid >> 16) & 0xFF);
    cmd[4] = (uint8_t)((uid >> 24) & 0xFF);
    cmd[5] = ACTION_NEGATIVE;   // negative action = clear/dismiss
    esp_ble_gattc_write_char(s_gattc_if, s_gattc_conn, s_h_ctrl_pt,
                             sizeof(cmd), cmd, ESP_GATT_WRITE_TYPE_RSP,
                             ESP_GATT_AUTH_REQ_NONE);
}

}  // namespace ancs
