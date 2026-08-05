#include "meshtastic.h"
#include "tracker_rep.h"
#include "usb_sd.h"
#include "gps_screen.h"
#include <LilyGoLib.h>
#include <SD.h>
#include "mesh/crypto.h"   // vendored FIPS/NIST-tested AES-CTR (replaces mbedTLS)
#include <math.h>         // NAN, isnan
#include <string.h>
#include <stdlib.h>       // strtol, sscanf
#include <esp_system.h>   // esp_efuse_mac_get_default, esp_random

// Trace logging — every TX / RX / dup-drop / node-update fires one of
// these. Useful while debugging the protocol, noisy in production. Set
// MESH_DEBUG=1 at build time (or before this include) to re-enable.
// Error-class prints below stay unconditional so a broken radio /
// failed SD write always surfaces on the serial monitor.
#ifndef MESH_DEBUG
#define MESH_DEBUG 0
#endif
#if MESH_DEBUG
  #define MESH_LOG(...) Serial.printf(__VA_ARGS__)
#else
  #define MESH_LOG(...) ((void)0)
#endif

// Defined in main.cpp
void clock_screen_set_mesh_count(int count);
void meshtastic_screen_refresh();
void nodes_screen_refresh();
int  clock_screen_get_utc_offset();

// Defined in configuration_screen.cpp — user-facing relay enable
bool configuration_screen_get_rebroadcast_enabled();
bool configuration_screen_get_vibrate_dm();
bool configuration_screen_get_vibrate_broadcast();
bool configuration_screen_get_broadcast_location();

// --- Radio config for Meshtastic LongFast (US 915 MHz band) ---
// EU_868 users: change MESH_FREQ_MHZ to 869.525
static const float   MESH_FREQ_MHZ = 906.875f;
static const float   MESH_BW_KHZ   = 250.0f;
static const uint8_t MESH_SF       = 11;
static const uint8_t MESH_CR       = 5;    // 4/5

// Default LongFast key: base64.b64decode("1PG7OiApB1nwvP+rz05pAQ==")
// Confirmed from meshtastic Python library (util.py DEFAULT_KEY).
// XOR("LongFast")=0x0A, XOR(key)=0x02 → channel hash = 0x08.
static const uint8_t MESH_KEY[16] = {
    0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59,
    0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01
};

static const uint32_t PORTNUM_TEXT         = 1;
static const uint32_t PORTNUM_POSITION     = 3;
static const uint32_t PORTNUM_NODEINFO     = 4;
static const uint32_t PORTNUM_TELEMETRY    = 67;
static const uint32_t PORTNUM_ROUTING      = 70; // ACK / NAK + traceroute
static const uint32_t PORTNUM_NEIGHBORINFO = 71;

// --- State ---
static volatile bool s_pkt_flag         = false;
static bool          s_active           = false;
static float         s_rssi             = 0.0f;
static bool          s_cad              = false;
static uint32_t      s_cad_last_ms      = 0;
static uint32_t      s_node_id          = 0;
static bool          s_nodeinfo_pending = false;
static uint32_t      s_nodeinfo_last_ms = 0;
// Periodic NodeInfo announce — gates the timer in bg_tick(). Defaults
// match the P4 firmware: on, every 10 minutes. The Configuration screen
// flips these and persists them to /Meshtastic/config.txt; if the file
// is absent on boot the defaults take effect so the watch still shows
// up in node lists out of the box.
static bool          s_announce_on       = true;
static uint32_t      s_announce_interval = 600000UL;   // 10 min
static bool          s_text_pending     = false;
static char          s_pending_text[MESH_MAX_TEXT_LEN] = {0};
// OTA dest for the next queued TEXT. 0xFFFFFFFF = broadcast (the
// default, what meshtastic_send_text() uses); a specific node ID
// when the caller went through meshtastic_send_text_to() instead.
static uint32_t      s_pending_dest = 0xFFFFFFFFu;

// In-flight traceroute. One slot - calling send_traceroute() again
// before the response arrives resets it to point at the new request.
static MeshTraceroute s_traceroute = {};

// Queued route_reply: another node traced us; on the next bg_tick we
// emit our route_reply addressed back at them with their packet_id as
// the request_id. Two-deep is plenty for a 9-radio-channel mesh.
struct PendingRouteReply { uint32_t dest; uint32_t request_id; };
static PendingRouteReply s_route_reply_queue[2];
static uint8_t           s_route_reply_queue_count = 0;

// Queued outbound traceroute - same idea as the text queue. The
// outgoing pkt_id is generated when we actually transmit (in bg_tick)
// and stamped onto s_traceroute so the route_reply matches back.
static bool          s_traceroute_pending = false;
static uint32_t      s_traceroute_pending_dest = 0;

// Queued outbound position request. Replies land in the regular
// POSITION RX path; no per-request tracking slot today.
static bool          s_pos_request_pending = false;
static uint32_t      s_pos_request_dest    = 0;

// Outgoing-TEXT tracker. Each entry holds the pkt_id we generated +
// the message body so the Send screen can poll for ACK state. Newest
// at index 0; aged out by bg_tick once PENDING crosses kAckTimeoutMs.
static MeshOutgoing  s_outgoing[MESH_MAX_OUTGOING];
static int           s_outgoing_count = 0;
static const uint32_t kAckTimeoutMs   = 30000;
static char          s_long_name[MESH_MAX_LONG_NAME]   = "ARGUS";
static char          s_short_name[MESH_MAX_SHORT_NAME]  = "ARGS";

// --- Rebroadcast (relay) queue ---
// Small ring buffer of raw packets to retransmit with hop_limit decremented.
// Sized to absorb a short burst without runaway airtime; if full when a new
// packet arrives, that packet is dropped (other relays will carry it).
#define MESH_REBROADCAST_QUEUE 4
static struct {
    uint8_t buf[256];
    size_t  len;
} s_rb_queue[MESH_REBROADCAST_QUEUE];
static uint8_t  s_rb_head         = 0;   // next slot to send
static uint8_t  s_rb_count        = 0;   // queued items
static uint32_t s_rb_send_after_ms = 0;  // earliest millis() to send next

static MeshMessage s_msgs[MESH_MAX_MESSAGES];
static int s_count  = 0;   // messages stored (max MESH_MAX_MESSAGES)
static int s_total  = 0;   // total ever received
static int s_unread = 0;

static MeshNode s_nodes[MESH_MAX_NODES];
static int      s_node_count = 0;   // distinct nodes heard (max MESH_MAX_NODES)

static volatile int  s_isr_count = 0;
static MeshDebugInfo s_dbg;

static void IRAM_ATTR on_pkt_isr() { s_pkt_flag = true; s_isr_count++; }

// --- AES-128-CTR decryption (mbedTLS) ---
// Wrapper preserved for callers that still want the default AES-128
// behaviour against MESH_KEY. New callers should use decrypt_ctr_n()
// with an explicit key_bits (128 or 256) to support user-defined
// AES-256 channel keys.
static void decrypt_ctr_n(const uint8_t *key, uint8_t key_bits,
                          const uint8_t *nonce,
                          const uint8_t *in, uint8_t *out, size_t len);

static void decrypt_ctr(const uint8_t *key, const uint8_t *nonce,
                         const uint8_t *in, uint8_t *out, size_t len)
{
    decrypt_ctr_n(key, 128, nonce, in, out, len);
}

static void decrypt_ctr_n(const uint8_t *key, uint8_t key_bits,
                          const uint8_t *nonce,
                          const uint8_t *in, uint8_t *out, size_t len)
{
    // AES-CTR via the vendored src/mesh crypto (pure AES validated against
    // FIPS-197 + NIST SP800-38A CTR vectors; see test/test_mesh_crypto.cpp).
    // Byte-identical to the previous mbedTLS path: standard AES-CTR, big-endian
    // counter incremented from the same 16-byte `nonce`. CTR is symmetric, so
    // this serves both the RX decrypt and TX encrypt callers. key_bits (128/256)
    // -> key_bytes (16/32).
    wl::mesh::aes_ctr_xcrypt(key, key_bits / 8u, nonce, in, out, len);
}

// --- Node ID (derived from ESP32 base MAC, stable across reboots) ---
static uint32_t derive_node_id()
{
    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);
    uint32_t id = ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16)
                | ((uint32_t)mac[4] <<  8) | (uint32_t)mac[5];
    return id ? id : 0xA55A0001;
}

// --- Minimal protobuf encoder ---
static size_t pb_varint(uint8_t *b, uint64_t v)
{
    size_t n = 0;
    do {
        b[n] = (uint8_t)(v & 0x7F);
        v >>= 7;
        if (v) b[n] |= 0x80;
        n++;
    } while (v);
    return n;
}
static size_t pb_ld(uint8_t *b, uint32_t field, const void *data, size_t len)
{
    size_t n = 0;
    n += pb_varint(b + n, ((uint64_t)field << 3) | 2);
    n += pb_varint(b + n, len);
    memcpy(b + n, data, len);
    return n + len;
}
static size_t pb_str(uint8_t *b, uint32_t field, const char *s)
{
    return pb_ld(b, field, s, strlen(s));
}
static size_t pb_u32(uint8_t *b, uint32_t field, uint32_t v)
{
    size_t n = 0;
    n += pb_varint(b + n, (uint64_t)field << 3);   // wire type 0
    n += pb_varint(b + n, v);
    return n;
}

// Channel hash: XOR("LongFast") ^ XOR(DEFAULT_KEY) = 0x0A ^ 0x02 = 0x08.
// Confirmed by meshtastic Python: generate_channel_hash("LongFast", "AQ==") == 8.
static const uint8_t MESH_CHAN_HASH = 0x08;

// Multi-channel table. Slot 0 is hardcoded LongFast (initialised
// lazily by ensure_channels_initialised). Slots 1..3 are user-defined
// and editable via the channels screen or /Meshtastic/channels.txt.
// RX walks the table looking for a channel_hash match; TX uses
// s_active_channel.
static MeshChannel s_channels[MESH_MAX_CHANNELS];
static int         s_active_channel = 0;

static uint8_t channel_hash_of(const char *name, const uint8_t *psk, uint8_t psk_len)
{
    uint8_t h = 0;
    for (const char *p = name; *p; ++p) h ^= (uint8_t)*p;
    for (uint8_t i = 0; i < psk_len; i++) h ^= psk[i];
    return h;
}

static void ensure_channels_initialised()
{
    if (s_channels[0].psk_len != 0) return;
    snprintf(s_channels[0].name, sizeof(s_channels[0].name), "LongFast");
    memcpy(s_channels[0].psk, MESH_KEY, sizeof(MESH_KEY));
    s_channels[0].psk_len      = sizeof(MESH_KEY);
    s_channels[0].channel_hash = channel_hash_of(
        s_channels[0].name, s_channels[0].psk, s_channels[0].psk_len);
    s_channels[0].enabled      = true;
    // Slots 1..3 stay zero-initialised (empty / disabled).
}

// Resolve the TX channel: the user's active slot if it's enabled +
// non-empty, otherwise fall back to slot 0 (LongFast) so a bad config
// doesn't take TX offline.
static const MeshChannel *active_tx_channel()
{
    ensure_channels_initialised();
    int idx = s_active_channel;
    if (idx < 0 || idx >= MESH_MAX_CHANNELS ||
        !s_channels[idx].enabled || s_channels[idx].psk_len == 0) {
        idx = 0;
    }
    return &s_channels[idx];
}

// RX side: find the first enabled channel whose hash matches the OTA
// header byte. Returns nullptr if the packet is on a channel we can't
// decrypt (different name/PSK or just not configured here).
static const MeshChannel *find_channel_by_hash(uint8_t hash)
{
    ensure_channels_initialised();
    for (int i = 0; i < MESH_MAX_CHANNELS; i++) {
        if (s_channels[i].enabled && s_channels[i].psk_len > 0 &&
            s_channels[i].channel_hash == hash) {
            return &s_channels[i];
        }
    }
    return nullptr;
}

static size_t build_nodeinfo_payload(uint8_t *buf)
{
    uint32_t nid = s_node_id;
    uint8_t  mac[6] = {0};
    esp_efuse_mac_get_default(mac);

    char id_str[12];
    snprintf(id_str, sizeof(id_str), "!%08lx", (unsigned long)nid);

    // Encode User protobuf
    uint8_t user[128];
    size_t  ulen = 0;
    ulen += pb_str(user + ulen, 1, id_str);           // id
    ulen += pb_str(user + ulen, 2, s_long_name);      // long_name
    ulen += pb_str(user + ulen, 3, s_short_name);     // short_name (≤4 chars)
    ulen += pb_ld (user + ulen, 4, mac, 6);           // macaddr
    ulen += pb_u32(user + ulen, 8, 255);              // hw_model = PRIVATE_HW

    // Wrap in Data protobuf: portnum=4 (NODEINFO_APP), payload=user
    size_t n = 0;
    n += pb_u32(buf + n, 1, 4);              // portnum
    n += pb_ld (buf + n, 2, user, ulen);     // payload
    return n;
}

static void send_nodeinfo()
{
    uint32_t nid    = s_node_id;
    uint32_t pkt_id = esp_random();

    uint8_t plain[160];
    size_t  plain_len = build_nodeinfo_payload(plain);

    // Same nonce format as receive path: [pkt_id 64-bit LE][nid 64-bit LE]
    uint8_t nonce[16] = {0};
    nonce[0]  = (pkt_id >>  0) & 0xFF;  nonce[1]  = (pkt_id >>  8) & 0xFF;
    nonce[2]  = (pkt_id >> 16) & 0xFF;  nonce[3]  = (pkt_id >> 24) & 0xFF;
    nonce[8]  = (nid    >>  0) & 0xFF;  nonce[9]  = (nid    >>  8) & 0xFF;
    nonce[10] = (nid    >> 16) & 0xFF;  nonce[11] = (nid    >> 24) & 0xFF;

    const MeshChannel *ch = active_tx_channel();
    uint8_t kbits = (ch->psk_len >= 32) ? 256 : 128;
    uint8_t ct[160];
    decrypt_ctr_n(ch->psk, kbits, nonce, plain, ct, plain_len);

    // 16-byte OTA header (Meshtastic 2.5+) + ciphertext
    uint8_t pkt[16 + 160];
    pkt[0] = 0xFF; pkt[1] = 0xFF; pkt[2] = 0xFF; pkt[3] = 0xFF; // dest = broadcast
    pkt[4] = (nid    >>  0) & 0xFF;  pkt[5] = (nid    >>  8) & 0xFF;  // from (LE)
    pkt[6] = (nid    >> 16) & 0xFF;  pkt[7] = (nid    >> 24) & 0xFF;
    pkt[8] = (pkt_id >>  0) & 0xFF;  pkt[9] = (pkt_id >>  8) & 0xFF;  // packet ID (LE)
    pkt[10]= (pkt_id >> 16) & 0xFF;  pkt[11]= (pkt_id >> 24) & 0xFF;
    pkt[12]= 0x03 | (3 << 5);        // flags: hop_limit=3, hop_start=3
    pkt[13]= ch->channel_hash;
    pkt[14]= 0x00;                   // next_hop (0 = unknown)
    pkt[15]= 0x00;                   // relay_node (0 = direct)
    memcpy(pkt + 16, ct, plain_len);

    s_dbg.nodeinfo_tx++;
    MESH_LOG("[MESH] TX NodeInfo !%08lx plain_len=%u hash=0x%02X\n",
        (unsigned long)nid, (unsigned)plain_len, ch->channel_hash);
    radio.transmit(pkt, 16 + plain_len);
    radio.startReceive();
    s_pkt_flag = false;   // discard TX-done IRQ that fires on DIO1 after transmit
}

// Broadcast our own DeviceMetrics so other nodes' UIs show our
// battery + uptime alongside our identity. Telemetry protobuf:
//   field 2 LD = DeviceMetrics { 1=battery, 2=voltage(float),
//                                 5=uptime_seconds }
// Wrapped in Data { 1=portnum=67, 2=payload }.
static void send_telemetry_broadcast()
{
    uint32_t nid    = s_node_id;
    uint32_t pkt_id = esp_random();

    // Inner DeviceMetrics. Battery percent from the AXP2101 PMU;
    // uptime is the watch's millis() since boot in seconds.
    int batt_pct = instance.pmu.getBatteryPercent();
    if (batt_pct < 0)   batt_pct = 0;
    if (batt_pct > 100) batt_pct = 100;
    uint32_t uptime_s = millis() / 1000;

    uint8_t dm[32];
    size_t  dm_len = 0;
    dm_len += pb_u32(dm + dm_len, 1, (uint32_t)batt_pct);
    dm_len += pb_u32(dm + dm_len, 5, uptime_s);

    // Outer Telemetry: field 2 = DeviceMetrics LD.
    uint8_t tlm[40];
    size_t  tlm_len = 0;
    tlm_len += pb_ld(tlm + tlm_len, 2, (const char *)dm, dm_len);

    // Data envelope: portnum=67 + payload.
    uint8_t plain[64];
    size_t  plain_len = 0;
    plain_len += pb_u32(plain + plain_len, 1, PORTNUM_TELEMETRY);
    plain_len += pb_ld (plain + plain_len, 2, (const char *)tlm, tlm_len);

    // Same nonce + framing as send_nodeinfo.
    uint8_t nonce[16] = {0};
    nonce[0]  = (pkt_id >>  0) & 0xFF;  nonce[1]  = (pkt_id >>  8) & 0xFF;
    nonce[2]  = (pkt_id >> 16) & 0xFF;  nonce[3]  = (pkt_id >> 24) & 0xFF;
    nonce[8]  = (nid    >>  0) & 0xFF;  nonce[9]  = (nid    >>  8) & 0xFF;
    nonce[10] = (nid    >> 16) & 0xFF;  nonce[11] = (nid    >> 24) & 0xFF;

    const MeshChannel *ch = active_tx_channel();
    uint8_t kbits = (ch->psk_len >= 32) ? 256 : 128;
    uint8_t ct[64];
    decrypt_ctr_n(ch->psk, kbits, nonce, plain, ct, plain_len);

    uint8_t pkt[16 + 64];
    pkt[0] = 0xFF; pkt[1] = 0xFF; pkt[2] = 0xFF; pkt[3] = 0xFF;
    pkt[4] = (nid    >>  0) & 0xFF;  pkt[5] = (nid    >>  8) & 0xFF;
    pkt[6] = (nid    >> 16) & 0xFF;  pkt[7] = (nid    >> 24) & 0xFF;
    pkt[8] = (pkt_id >>  0) & 0xFF;  pkt[9] = (pkt_id >>  8) & 0xFF;
    pkt[10]= (pkt_id >> 16) & 0xFF;  pkt[11]= (pkt_id >> 24) & 0xFF;
    pkt[12]= 0x03 | (3 << 5);
    pkt[13]= ch->channel_hash;
    pkt[14]= 0x00;
    pkt[15]= 0x00;
    memcpy(pkt + 16, ct, plain_len);

    MESH_LOG("[MESH] TX Telemetry !%08lx batt=%d%% up=%lus\n",
        (unsigned long)nid, batt_pct, (unsigned long)uptime_s);
    radio.transmit(pkt, 16 + plain_len);
    radio.startReceive();
    s_pkt_flag = false;
}

// Builds, encrypts and transmits a TEXT_MESSAGE_APP packet. Same OTA
// framing as send_nodeinfo(); portnum=1, payload = UTF-8 text. The OTA
// `dest` field comes from the caller - 0xFFFFFFFF for broadcast or a
// specific node id for a DM. Meshtastic uses the same channel encryption
// for both cases, so the only on-wire difference is bytes 0..3 of the
// header. DMs set want_ack (header byte 12 bit 3) so the recipient
// replies with a Routing ACK; broadcasts don't.
static uint32_t do_send_text(const char *text, uint32_t dest_node)
{
    uint32_t nid    = s_node_id;
    uint32_t pkt_id = esp_random();
    bool     want_ack = (dest_node != 0xFFFFFFFFu);

    // Data protobuf: field 1 (portnum)=1, field 2 (payload)=text bytes
    uint8_t plain[16 + MESH_MAX_TEXT_LEN];
    size_t  plain_len = 0;
    plain_len += pb_u32(plain + plain_len, 1, PORTNUM_TEXT);
    plain_len += pb_ld (plain + plain_len, 2, text, strlen(text));

    uint8_t nonce[16] = {0};
    nonce[0]  = (pkt_id >>  0) & 0xFF;  nonce[1]  = (pkt_id >>  8) & 0xFF;
    nonce[2]  = (pkt_id >> 16) & 0xFF;  nonce[3]  = (pkt_id >> 24) & 0xFF;
    nonce[8]  = (nid    >>  0) & 0xFF;  nonce[9]  = (nid    >>  8) & 0xFF;
    nonce[10] = (nid    >> 16) & 0xFF;  nonce[11] = (nid    >> 24) & 0xFF;

    const MeshChannel *ch = active_tx_channel();
    uint8_t kbits = (ch->psk_len >= 32) ? 256 : 128;
    uint8_t ct[16 + MESH_MAX_TEXT_LEN];
    decrypt_ctr_n(ch->psk, kbits, nonce, plain, ct, plain_len);

    uint8_t pkt[16 + 16 + MESH_MAX_TEXT_LEN];
    // dest_node little-endian: 0xFFFFFFFF for broadcast, specific node
    // ID for DMs.
    pkt[0] = (dest_node >>  0) & 0xFF;  pkt[1] = (dest_node >>  8) & 0xFF;
    pkt[2] = (dest_node >> 16) & 0xFF;  pkt[3] = (dest_node >> 24) & 0xFF;
    pkt[4] = (nid    >>  0) & 0xFF;  pkt[5] = (nid    >>  8) & 0xFF;
    pkt[6] = (nid    >> 16) & 0xFF;  pkt[7] = (nid    >> 24) & 0xFF;
    pkt[8] = (pkt_id >>  0) & 0xFF;  pkt[9] = (pkt_id >>  8) & 0xFF;
    pkt[10]= (pkt_id >> 16) & 0xFF;  pkt[11]= (pkt_id >> 24) & 0xFF;
    // Header byte 12: bits 0-2 hop_limit, bit 3 want_ack, bit 4 via_mqtt,
    // bits 5-7 hop_start. Match the existing hop_limit=3 / hop_start=3
    // values used elsewhere on this watch.
    pkt[12]= 0x03 | (3 << 5) | (want_ack ? 0x08 : 0);
    pkt[13]= ch->channel_hash;
    pkt[14]= 0x00;                   // next_hop
    pkt[15]= 0x00;                   // relay_node
    memcpy(pkt + 16, ct, plain_len);

    MESH_LOG("[MESH] TX Text !%08lx -> %08lx \"%s\" plain_len=%u want_ack=%d\n",
        (unsigned long)nid, (unsigned long)dest_node,
        text, (unsigned)plain_len, want_ack ? 1 : 0);
    radio.transmit(pkt, 16 + plain_len);
    radio.startReceive();
    s_pkt_flag = false;   // discard TX-done IRQ that fires on DIO1 after transmit
    return pkt_id;
}

// Build + transmit a generic Data packet. Returns the chosen pkt_id so
// the caller can stamp it onto in-flight tracking (s_traceroute matches
// route_replies via request_id == this pkt_id). Mirrors do_send_text's
// framing exactly - only the plain payload differs.
static uint32_t do_send_data(const uint8_t *plain, size_t plain_len,
                             uint32_t dest_node)
{
    uint32_t nid    = s_node_id;
    uint32_t pkt_id = esp_random();

    uint8_t nonce[16] = {0};
    nonce[0]  = (pkt_id >>  0) & 0xFF;  nonce[1]  = (pkt_id >>  8) & 0xFF;
    nonce[2]  = (pkt_id >> 16) & 0xFF;  nonce[3]  = (pkt_id >> 24) & 0xFF;
    nonce[8]  = (nid    >>  0) & 0xFF;  nonce[9]  = (nid    >>  8) & 0xFF;
    nonce[10] = (nid    >> 16) & 0xFF;  nonce[11] = (nid    >> 24) & 0xFF;

    const MeshChannel *ch = active_tx_channel();
    uint8_t kbits = (ch->psk_len >= 32) ? 256 : 128;
    uint8_t ct[16 + MESH_MAX_TEXT_LEN];
    if (plain_len > sizeof(ct)) return 0;
    decrypt_ctr_n(ch->psk, kbits, nonce, plain, ct, plain_len);

    uint8_t pkt[16 + 16 + MESH_MAX_TEXT_LEN];
    pkt[0] = (dest_node >>  0) & 0xFF;  pkt[1] = (dest_node >>  8) & 0xFF;
    pkt[2] = (dest_node >> 16) & 0xFF;  pkt[3] = (dest_node >> 24) & 0xFF;
    pkt[4] = (nid    >>  0) & 0xFF;  pkt[5] = (nid    >>  8) & 0xFF;
    pkt[6] = (nid    >> 16) & 0xFF;  pkt[7] = (nid    >> 24) & 0xFF;
    pkt[8] = (pkt_id >>  0) & 0xFF;  pkt[9] = (pkt_id >>  8) & 0xFF;
    pkt[10]= (pkt_id >> 16) & 0xFF;  pkt[11]= (pkt_id >> 24) & 0xFF;
    pkt[12]= 0x03 | (3 << 5);
    pkt[13]= ch->channel_hash;
    pkt[14]= 0x00;
    pkt[15]= 0x00;
    memcpy(pkt + 16, ct, plain_len);
    radio.transmit(pkt, 16 + plain_len);
    radio.startReceive();
    s_pkt_flag = false;
    return pkt_id;
}

// Sender side of traceroute. Routing protobuf with field 2 (route_request)
// = empty RouteDiscovery, plus Data field 3 (want_response=1). Returns
// the pkt_id we generated so the caller can stamp it onto s_traceroute
// for the inbound-reply matcher.
static uint32_t do_send_traceroute(uint32_t dest_node)
{
    uint8_t routing[8];
    size_t  routing_len = 0;
    routing_len += pb_ld(routing + routing_len, 2, "", 0);  // empty route_request

    uint8_t plain[16 + 32];
    size_t  plain_len = 0;
    plain_len += pb_u32(plain + plain_len, 1, PORTNUM_ROUTING);
    plain_len += pb_ld (plain + plain_len, 2, (const char *)routing, routing_len);
    plain_len += pb_u32(plain + plain_len, 3, /*want_response=*/1);

    uint32_t pkt_id = do_send_data(plain, plain_len, dest_node);
    MESH_LOG("[MESH] TX Traceroute !%08lx -> %08lx pkt_id=%08lx\n",
        (unsigned long)s_node_id, (unsigned long)dest_node, (unsigned long)pkt_id);
    return pkt_id;
}

// Build + transmit a Position protobuf to dest_node. Used both by the
// position-request responder (replying with our own coordinates when
// asked) and - in future - by the periodic broadcast loop. lat/lon are
// the on-wire degrees × 1e7 sfixed32; alt is metres MSL as varint.
static void do_send_position(int32_t lat_i, int32_t lon_i, int32_t alt_m,
                             uint32_t dest_node)
{
    // Inner Position protobuf - sfixed32 (wire type 5) for lat/lon,
    // varint for altitude. Same layout the P4 uses.
    uint8_t pos[32];
    size_t  pos_len = 0;
    pos[pos_len++] = (1 << 3) | 5;          // field 1, wire 5 (sfixed32)
    pos[pos_len++] = (uint8_t)(lat_i >>  0); pos[pos_len++] = (uint8_t)(lat_i >>  8);
    pos[pos_len++] = (uint8_t)(lat_i >> 16); pos[pos_len++] = (uint8_t)(lat_i >> 24);
    pos[pos_len++] = (2 << 3) | 5;          // field 2, wire 5 (sfixed32)
    pos[pos_len++] = (uint8_t)(lon_i >>  0); pos[pos_len++] = (uint8_t)(lon_i >>  8);
    pos[pos_len++] = (uint8_t)(lon_i >> 16); pos[pos_len++] = (uint8_t)(lon_i >> 24);
    pos_len += pb_u32(pos + pos_len, 3, (uint32_t)alt_m);

    uint8_t plain[16 + 32];
    size_t  plain_len = 0;
    plain_len += pb_u32(plain + plain_len, 1, PORTNUM_POSITION);
    plain_len += pb_ld (plain + plain_len, 2, (const char *)pos, pos_len);

    do_send_data(plain, plain_len, dest_node);
    MESH_LOG("[MESH] TX Position !%08lx -> %08lx (%ld,%ld)\n",
        (unsigned long)s_node_id, (unsigned long)dest_node,
        (long)lat_i, (long)lon_i);
}

// Position request: empty Position payload + want_response=1 in the
// outer Data envelope. The recipient (if running this firmware or stock
// Meshtastic) replies with their current Position. Returns the chosen
// pkt_id so the caller could track outstanding requests later if we
// add reply matching; we don't today.
static uint32_t do_send_position_request(uint32_t dest_node)
{
    uint8_t plain[16];
    size_t  plain_len = 0;
    plain_len += pb_u32(plain + plain_len, 1, PORTNUM_POSITION);
    plain_len += pb_u32(plain + plain_len, 3, /*want_response=*/1);
    uint32_t pkt_id = do_send_data(plain, plain_len, dest_node);
    MESH_LOG("[MESH] TX PositionReq !%08lx -> %08lx pkt_id=%08lx\n",
        (unsigned long)s_node_id, (unsigned long)dest_node, (unsigned long)pkt_id);
    return pkt_id;
}

// Responder side of traceroute. When another node's route_request lands
// addressed at us, build a route_reply Routing protobuf (Routing.field 3
// = empty RouteDiscovery from our end - we're the terminal node, the
// relays' hop_byte decrements on the way out fill in their hop list).
// Data field 6 = request_id bound to the original request's packet_id.
static void do_send_route_reply(uint32_t dest_node, uint32_t request_id)
{
    uint8_t routing[8];
    size_t  routing_len = 0;
    routing_len += pb_ld(routing + routing_len, 3, "", 0);  // empty route_reply

    uint8_t plain[16 + 32];
    size_t  plain_len = 0;
    plain_len += pb_u32(plain + plain_len, 1, PORTNUM_ROUTING);
    plain_len += pb_ld (plain + plain_len, 2, (const char *)routing, routing_len);
    plain_len += pb_u32(plain + plain_len, 6, request_id);

    do_send_data(plain, plain_len, dest_node);
    MESH_LOG("[MESH] TX RouteReply !%08lx -> %08lx req_id=%08lx\n",
        (unsigned long)s_node_id, (unsigned long)dest_node, (unsigned long)request_id);
}

// --- Minimal protobuf decoder for the Meshtastic Data message ---
// Field 1 (portnum): varint, tag byte 0x08
// Field 2 (payload): bytes, tag byte 0x12
// Returns true if a payload (field 2) was found; *portnum_out holds field 1.
static bool parse_data(const uint8_t *buf, size_t len,
                        uint32_t *portnum_out,
                        const uint8_t **payload_out, size_t *payload_len_out,
                        uint32_t *request_id_out = nullptr)
{
    *portnum_out     = 0;
    *payload_out     = nullptr;
    *payload_len_out = 0;
    if (request_id_out) *request_id_out = 0;
    size_t i = 0;

    while (i < len) {
        uint8_t tag_byte  = buf[i++];
        uint8_t field     = tag_byte >> 3;
        uint8_t wire_type = tag_byte & 0x07;

        if (wire_type == 0) {           // varint
            uint64_t val = 0; int shift = 0;
            while (i < len) {
                uint8_t b = buf[i++];
                val |= (uint64_t)(b & 0x7F) << shift;
                if (!(b & 0x80)) break;
                shift += 7;
                if (shift >= 64) break;
            }
            if      (field == 1) *portnum_out = (uint32_t)val;
            else if (field == 6 && request_id_out)
                *request_id_out = (uint32_t)val;     // Data.request_id
        } else if (wire_type == 2) {    // length-delimited
            uint32_t flen = 0; int shift = 0;
            while (i < len) {
                uint8_t b = buf[i++];
                flen |= (uint32_t)(b & 0x7F) << shift;
                if (!(b & 0x80)) break;
                shift += 7;
                if (shift >= 28) break;
            }
            if (flen > len - i) break;  // malformed
            if (field == 2) { *payload_out = buf + i; *payload_len_out = flen; }
            i += flen;
        } else {
            break;                      // unknown wire type
        }
    }
    // Routing packets are valid even with empty payload (e.g. an ACK
    // is just `field 1 varint`), so accept a parse with no payload too -
    // the caller checks portnum and dispatches accordingly.
    return *portnum_out != 0;
}

// Parse a Meshtastic User protobuf (the NodeInfo payload) into a MeshNode.
// Fields: 1=id, 2=long_name, 3=short_name, 4=macaddr, 5=hw_model,
// 6=is_licensed, 7=role, 8=public_key. Only fields present on the wire are
// filled; the caller must zero-initialise *n first so presence flags start clear.
static void parse_user(const uint8_t *buf, size_t len, MeshNode *n)
{
    size_t i = 0;

    while (i < len) {
        uint8_t tag_byte  = buf[i++];
        uint8_t field     = tag_byte >> 3;
        uint8_t wire_type = tag_byte & 0x07;

        if (wire_type == 0) {           // varint
            uint64_t v = 0; int shift = 0;
            while (i < len) {
                uint8_t b = buf[i++];
                v |= (uint64_t)(b & 0x7F) << shift;
                if (!(b & 0x80)) break;
                shift += 7;
                if (shift >= 63) break;
            }
            if (field == 5)      { n->hw_model = (uint32_t)v; n->has_hw_model = true; }
            else if (field == 6) { n->is_licensed = (v != 0); n->has_is_licensed = true; }
            else if (field == 7) { n->role = (uint32_t)v; n->has_role = true; }
        } else if (wire_type == 2) {    // length-delimited
            uint32_t flen = 0; int shift = 0;
            while (i < len) {
                uint8_t b = buf[i++];
                flen |= (uint32_t)(b & 0x7F) << shift;
                if (!(b & 0x80)) break;
                shift += 7;
                if (shift >= 28) break;
            }
            if (flen > len - i) break;  // malformed
            const uint8_t *p = buf + i;
            if (field == 1) {                       // id string
                size_t c = (flen < sizeof(n->id) - 1) ? flen : sizeof(n->id) - 1;
                memcpy(n->id, p, c); n->id[c] = '\0';
            } else if (field == 2) {                // long_name
                size_t c = (flen < sizeof(n->long_name) - 1) ? flen : sizeof(n->long_name) - 1;
                memcpy(n->long_name, p, c); n->long_name[c] = '\0';
            } else if (field == 3) {                // short_name
                size_t c = (flen < sizeof(n->short_name) - 1) ? flen : sizeof(n->short_name) - 1;
                memcpy(n->short_name, p, c); n->short_name[c] = '\0';
            } else if (field == 4 && flen == 6) {   // macaddr
                memcpy(n->macaddr, p, 6); n->has_macaddr = true;
            } else if (field == 8) {                // public_key
                size_t c = (flen < sizeof(n->public_key)) ? flen : sizeof(n->public_key);
                memcpy(n->public_key, p, c); n->public_key_len = (uint8_t)c;
            }
            i += flen;
        } else {
            break;                      // unknown wire type
        }
    }
}

const char *meshtastic_role_name(uint32_t r)
{
    switch (r) {
        case 0:  return "CLIENT";
        case 1:  return "CLIENT_MUTE";
        case 2:  return "ROUTER";
        case 3:  return "ROUTER_CLIENT";
        case 4:  return "REPEATER";
        case 5:  return "TRACKER";
        case 6:  return "SENSOR";
        case 7:  return "TAK";
        case 8:  return "CLIENT_HIDDEN";
        case 9:  return "LOST_AND_FOUND";
        case 10: return "TAK_TRACKER";
        case 11: return "ROUTER_LATE";
        default: return "UNKNOWN";
    }
}

static void save_node_to_sd(const MeshNode &n);

// Record a node heard via NodeInfo. Updates an existing entry (moving it to
// the front) or inserts a new one, newest first, capped at MESH_MAX_NODES.
static void handle_nodeinfo(uint32_t node_id, const uint8_t *payload,
                            size_t plen, const struct tm &t)
{
    MeshNode n;
    memset(&n, 0, sizeof(n));
    n.node_id = node_id;
    parse_user(payload, plen, &n);
    if (!n.long_name[0])
        snprintf(n.long_name, sizeof(n.long_name), "!%08lx", (unsigned long)node_id);
    snprintf(n.time_str, sizeof(n.time_str), "%02d:%02d", t.tm_hour, t.tm_min);

    int idx = -1;
    for (int i = 0; i < s_node_count; i++) {
        if (s_nodes[i].node_id == node_id) { idx = i; break; }
    }

    // NodeInfo carries no position / telemetry / neighbours; preserve
    // whatever the prior packets stamped in so a fresh User update
    // doesn't wipe the enrichment.
    if (idx >= 0) {
        const MeshNode &old = s_nodes[idx];
        n.latitude_i   = old.latitude_i;
        n.longitude_i  = old.longitude_i;
        n.has_position = old.has_position;
        n.altitude     = old.altitude;
        n.has_altitude = old.has_altitude;
        memcpy(n.disco_ts, old.disco_ts, sizeof(n.disco_ts));

        n.has_telemetry      = old.has_telemetry;
        n.battery_level      = old.battery_level;       n.has_battery_level = old.has_battery_level;
        n.voltage            = old.voltage;             n.has_voltage       = old.has_voltage;
        n.uptime_seconds     = old.uptime_seconds;      n.has_uptime        = old.has_uptime;
        n.has_environment    = old.has_environment;
        n.temperature_c      = old.temperature_c;       n.has_temperature   = old.has_temperature;
        n.relative_humidity  = old.relative_humidity;   n.has_humidity      = old.has_humidity;
        n.pressure_hpa       = old.pressure_hpa;        n.has_pressure      = old.has_pressure;
        n.lux                = old.lux;                 n.has_lux           = old.has_lux;
        n.iaq                = old.iaq;                 n.has_iaq           = old.has_iaq;
        n.has_power_metrics  = old.has_power_metrics;
        n.ch1_voltage        = old.ch1_voltage;         n.has_ch1_voltage   = old.has_ch1_voltage;
        n.ch1_current        = old.ch1_current;         n.has_ch1_current   = old.has_ch1_current;
        n.neighbor_count     = old.neighbor_count;
        memcpy(n.neighbors, old.neighbors, sizeof(MeshNeighbor) * MESH_MAX_NEIGHBORS);
        n.has_neighborinfo   = old.has_neighborinfo;
    } else {
        snprintf(n.disco_ts, sizeof(n.disco_ts), "%04d%02d%02d_%02d%02d%02d",
                 t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                 t.tm_hour, t.tm_min, t.tm_sec);
    }

    int shift_from;
    if (idx >= 0) {
        shift_from = idx;                       // move existing entry to front
    } else {
        shift_from = (s_node_count < MESH_MAX_NODES) ? s_node_count
                                                     : MESH_MAX_NODES - 1;
        if (s_node_count < MESH_MAX_NODES) s_node_count++;
    }
    for (int i = shift_from; i > 0; i--) s_nodes[i] = s_nodes[i - 1];
    s_nodes[0] = n;

    MESH_LOG("[MESH] NodeInfo !%08lx \"%s\" (%s)\n",
        (unsigned long)node_id, n.long_name, n.short_name);
    save_node_to_sd(n);   // create on first sighting, enrich in place after
    nodes_screen_refresh();
}

// Parse a Meshtastic Position protobuf. Fields used: 1=latitude_i,
// 2=longitude_i (both sfixed32 = wire type 5, degrees * 1e7),
// 3=altitude (int32 varint, metres). Returns true if lat AND lon were found.
static bool parse_position(const uint8_t *buf, size_t len,
                           int32_t *lat_i, int32_t *lon_i,
                           int32_t *alt, bool *has_alt)
{
    bool got_lat = false, got_lon = false;
    *has_alt = false;
    size_t i = 0;

    while (i < len) {
        uint8_t tag_byte  = buf[i++];
        uint8_t field     = tag_byte >> 3;
        uint8_t wire_type = tag_byte & 0x07;

        if (wire_type == 0) {                   // varint
            uint64_t v = 0; int shift = 0;
            while (i < len) {
                uint8_t b = buf[i++];
                v |= (uint64_t)(b & 0x7F) << shift;
                if (!(b & 0x80)) break;
                shift += 7;
                if (shift >= 63) break;
            }
            if (field == 3) { *alt = (int32_t)(uint32_t)v; *has_alt = true; }
        } else if (wire_type == 5) {            // 32-bit fixed (sfixed32/fixed32)
            if (i + 4 > len) break;
            uint32_t v = (uint32_t)buf[i]      | ((uint32_t)buf[i + 1] << 8)
                       | ((uint32_t)buf[i + 2] << 16) | ((uint32_t)buf[i + 3] << 24);
            i += 4;
            if (field == 1)      { *lat_i = (int32_t)v; got_lat = true; }
            else if (field == 2) { *lon_i = (int32_t)v; got_lon = true; }
        } else if (wire_type == 1) {            // 64-bit fixed — skip
            if (i + 8 > len) break;
            i += 8;
        } else if (wire_type == 2) {            // length-delimited — skip
            uint32_t flen = 0; int shift = 0;
            while (i < len) {
                uint8_t b = buf[i++];
                flen |= (uint32_t)(b & 0x7F) << shift;
                if (!(b & 0x80)) break;
                shift += 7;
                if (shift >= 28) break;
            }
            if (flen > len - i) break;
            i += flen;
        } else {
            break;                              // unknown wire type
        }
    }
    return got_lat && got_lon;
}

// Attach a position fix to an existing node, or create a minimal entry if the
// node has not been heard via NodeInfo yet. Moves the node to the front.
// Pending position responses queued when someone asks us where we are.
// Drained one per bg_tick (same pattern as the route_reply queue) to
// avoid stacking transmits inside the IRQ path.
struct PendingPositionReply { uint32_t dest; };
static PendingPositionReply s_pos_reply_queue[2];
static uint8_t              s_pos_reply_queue_count = 0;

static void handle_position(uint32_t node_id, uint32_t ota_dest,
                            const uint8_t *payload,
                            size_t plen, const struct tm &t)
{
    // Empty Position payload + addressed specifically to us = "what's
    // your position?" Reply only if we have a GPS fix; ignore the
    // request when no fix (sending 0,0 would lie). Don't honour
    // broadcast requests - that would have every node spam-reply.
    //
    // Answering is disclosure of the wearer's exact location to whoever
    // asked, so it is gated on the same "Broadcast Location" consent the
    // Configuration screen already owns and persists (default OFF). Any
    // peer can learn our node id from the periodic NodeInfo announce, so
    // without this gate an unsolicited query from a stranger would be
    // answered automatically.
    if (plen == 0 && ota_dest == s_node_id) {
        if (configuration_screen_get_broadcast_location()
            && gps_screen_has_lock() && instance.gps.location.isValid()
            && s_pos_reply_queue_count < 2) {
            s_pos_reply_queue[s_pos_reply_queue_count].dest = node_id;
            s_pos_reply_queue_count++;
            MESH_LOG("[MESH] queued position reply -> %08lx\n",
                (unsigned long)node_id);
        } else {
            MESH_LOG("[MESH] position request from %08lx ignored "
                "(broadcast off, no fix, or queue full)\n",
                (unsigned long)node_id);
        }
        return;
    }

    int32_t lat_i = 0, lon_i = 0, alt = 0;
    bool has_alt = false;
    if (!parse_position(payload, plen, &lat_i, &lon_i, &alt, &has_alt))
        return;   // no usable coordinates

    int idx = -1;
    for (int i = 0; i < s_node_count; i++) {
        if (s_nodes[i].node_id == node_id) { idx = i; break; }
    }

    MeshNode n;
    if (idx >= 0) {
        n = s_nodes[idx];                       // preserve known identity + disco_ts
    } else {
        memset(&n, 0, sizeof(n));
        n.node_id = node_id;
        snprintf(n.long_name, sizeof(n.long_name), "!%08lx", (unsigned long)node_id);
        snprintf(n.disco_ts, sizeof(n.disco_ts), "%04d%02d%02d_%02d%02d%02d",
                 t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                 t.tm_hour, t.tm_min, t.tm_sec);
    }
    n.latitude_i   = lat_i;
    n.longitude_i  = lon_i;
    n.has_position = true;
    if (has_alt) { n.altitude = alt; n.has_altitude = true; }
    snprintf(n.time_str, sizeof(n.time_str), "%02d:%02d", t.tm_hour, t.tm_min);

    int shift_from;
    if (idx >= 0) {
        shift_from = idx;
    } else {
        shift_from = (s_node_count < MESH_MAX_NODES) ? s_node_count
                                                     : MESH_MAX_NODES - 1;
        if (s_node_count < MESH_MAX_NODES) s_node_count++;
    }
    for (int i = shift_from; i > 0; i--) s_nodes[i] = s_nodes[i - 1];
    s_nodes[0] = n;

    MESH_LOG("[MESH] Position !%08lx lat_i=%ld lon_i=%ld\n",
        (unsigned long)node_id, (long)lat_i, (long)lon_i);
    save_node_to_sd(n);   // create on first sighting, enrich in place after
    nodes_screen_refresh();
}

// --- Protobuf scan helpers ---------------------------------------------
// Generic varint + length-delimited readers used by the telemetry +
// neighborinfo parsers below. Both advance pos through the buffer.
static bool pb_read_varint(const uint8_t *buf, size_t len, size_t &pos, uint64_t &out)
{
    out = 0;
    int shift = 0;
    while (pos < len) {
        uint8_t b = buf[pos++];
        out |= (uint64_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) return true;
        shift += 7;
        if (shift >= 64) return false;
    }
    return false;
}
static bool pb_read_ld(const uint8_t *buf, size_t len, size_t &pos,
                      const uint8_t *&out, size_t &out_len)
{
    uint64_t flen = 0;
    if (!pb_read_varint(buf, len, pos, flen)) return false;
    if (flen > len - pos) return false;
    out     = buf + pos;
    out_len = (size_t)flen;
    pos    += (size_t)flen;
    return true;
}

// --- Telemetry RX ------------------------------------------------------
// DeviceMetrics:  1=battery (varint), 2=voltage (fixed32 float),
//                 5=uptime_seconds (varint).
// Other fields (channel_util, air_util_tx) parsed-but-skipped.
static void parse_device_metrics(const uint8_t *buf, size_t len, MeshNode *n)
{
    size_t i = 0;
    bool got_any = false;
    while (i < len) {
        uint8_t tag   = buf[i++];
        uint8_t field = tag >> 3;
        uint8_t wt    = tag & 0x07;
        if (wt == 0) {
            uint64_t v = 0;
            if (!pb_read_varint(buf, len, i, v)) return;
            if (field == 1) { n->battery_level   = (uint32_t)v; n->has_battery_level = true; got_any = true; }
            else if (field == 5) { n->uptime_seconds = (uint32_t)v; n->has_uptime = true; got_any = true; }
        } else if (wt == 5) {
            if (len - i < 4) return;
            if (field == 2) {
                memcpy(&n->voltage, buf + i, 4);   // little-endian float
                n->has_voltage = true;
                got_any = true;
            }
            i += 4;
        } else if (wt == 2) {
            uint64_t flen = 0;
            if (!pb_read_varint(buf, len, i, flen)) return;
            if (flen > len - i) return;
            i += (size_t)flen;
        } else {
            return;
        }
    }
    if (got_any) n->has_telemetry = true;
}

// EnvironmentMetrics: 1=temperature, 2=relative_humidity,
// 3=barometric_pressure (hPa), 9=lux - all fixed32 floats; 7=iaq varint.
static void parse_environment_metrics(const uint8_t *buf, size_t len, MeshNode *n)
{
    size_t i = 0;
    bool got_any = false;
    while (i < len) {
        uint8_t tag   = buf[i++];
        uint8_t field = tag >> 3;
        uint8_t wt    = tag & 0x07;
        if (wt == 5) {
            if (len - i < 4) return;
            float fv; memcpy(&fv, buf + i, 4);
            switch (field) {
                case 1: n->temperature_c     = fv; n->has_temperature = true; got_any = true; break;
                case 2: n->relative_humidity = fv; n->has_humidity    = true; got_any = true; break;
                case 3: n->pressure_hpa      = fv; n->has_pressure    = true; got_any = true; break;
                case 9: n->lux               = fv; n->has_lux         = true; got_any = true; break;
                default: break;
            }
            i += 4;
        } else if (wt == 0) {
            uint64_t v = 0;
            if (!pb_read_varint(buf, len, i, v)) return;
            if (field == 7) { n->iaq = (uint32_t)v; n->has_iaq = true; got_any = true; }
        } else if (wt == 2) {
            uint64_t flen = 0;
            if (!pb_read_varint(buf, len, i, flen)) return;
            if (flen > len - i) return;
            i += (size_t)flen;
        } else {
            return;
        }
    }
    if (got_any) n->has_environment = true;
}

// PowerMetrics: 1=ch1_voltage, 2=ch1_current (fixed32 floats). We
// surface only CH1; CH2/3 are rare in field and crowd the UI.
static void parse_power_metrics(const uint8_t *buf, size_t len, MeshNode *n)
{
    size_t i = 0;
    bool got_any = false;
    while (i < len) {
        uint8_t tag   = buf[i++];
        uint8_t field = tag >> 3;
        uint8_t wt    = tag & 0x07;
        if (wt == 5) {
            if (len - i < 4) return;
            float fv; memcpy(&fv, buf + i, 4);
            if      (field == 1) { n->ch1_voltage = fv; n->has_ch1_voltage = true; got_any = true; }
            else if (field == 2) { n->ch1_current = fv; n->has_ch1_current = true; got_any = true; }
            i += 4;
        } else if (wt == 0) {
            uint64_t v = 0;
            if (!pb_read_varint(buf, len, i, v)) return;
        } else if (wt == 2) {
            uint64_t flen = 0;
            if (!pb_read_varint(buf, len, i, flen)) return;
            if (flen > len - i) return;
            i += (size_t)flen;
        } else {
            return;
        }
    }
    if (got_any) n->has_power_metrics = true;
}

// Outer Telemetry walker. Field 1 (time) is skipped; sub-messages
// at fields 2/3/4 are dispatched into the focused parsers above.
static void handle_telemetry(uint32_t node_id, const uint8_t *payload, size_t plen,
                             const struct tm &t)
{
    // Find an existing entry (preserve identity + position + neighbours)
    // or seed a new minimal one keyed by node_id.
    int idx = -1;
    for (int i = 0; i < s_node_count; i++) {
        if (s_nodes[i].node_id == node_id) { idx = i; break; }
    }
    MeshNode n;
    if (idx >= 0) {
        n = s_nodes[idx];
    } else {
        memset(&n, 0, sizeof(n));
        n.node_id = node_id;
        snprintf(n.long_name, sizeof(n.long_name), "!%08lx", (unsigned long)node_id);
        snprintf(n.disco_ts, sizeof(n.disco_ts), "%04d%02d%02d_%02d%02d%02d",
                 t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                 t.tm_hour, t.tm_min, t.tm_sec);
    }

    size_t i = 0;
    while (i < plen) {
        uint8_t tag   = payload[i++];
        uint8_t field = tag >> 3;
        uint8_t wt    = tag & 0x07;
        if (wt == 0) {
            uint64_t v = 0;
            if (!pb_read_varint(payload, plen, i, v)) return;
        } else if (wt == 2) {
            const uint8_t *sub; size_t sub_len;
            if (!pb_read_ld(payload, plen, i, sub, sub_len)) return;
            if      (field == 2) parse_device_metrics(sub, sub_len, &n);
            else if (field == 3) parse_environment_metrics(sub, sub_len, &n);
            else if (field == 4) parse_power_metrics(sub, sub_len, &n);
        } else if (wt == 5) {
            if (plen - i < 4) return;
            i += 4;
        } else {
            return;
        }
    }
    if (!n.has_telemetry && !n.has_environment && !n.has_power_metrics) return;

    snprintf(n.time_str, sizeof(n.time_str), "%02d:%02d", t.tm_hour, t.tm_min);

    int shift_from = (idx >= 0) ? idx
        : ((s_node_count < MESH_MAX_NODES) ? s_node_count : MESH_MAX_NODES - 1);
    if (idx < 0 && s_node_count < MESH_MAX_NODES) s_node_count++;
    for (int j = shift_from; j > 0; j--) s_nodes[j] = s_nodes[j - 1];
    s_nodes[0] = n;

    MESH_LOG("[MESH] Telemetry !%08lx batt=%lu%% v=%.2f up=%lus\n",
        (unsigned long)node_id,
        (unsigned long)(n.has_battery_level ? n.battery_level : 0),
        (double)(n.has_voltage ? n.voltage : 0.0f),
        (unsigned long)(n.has_uptime ? n.uptime_seconds : 0));
    save_node_to_sd(n);
    nodes_screen_refresh();
}

// --- NeighborInfo RX --------------------------------------------------
// Inner Neighbor: 1=node_id (varint OR fixed32 - both encodings exist
// across firmwares), 2=snr (fixed32 float).
static bool parse_one_neighbor(const uint8_t *buf, size_t len, MeshNeighbor *out)
{
    *out = MeshNeighbor{};
    size_t i = 0;
    while (i < len) {
        uint8_t tag   = buf[i++];
        uint8_t field = tag >> 3;
        uint8_t wt    = tag & 0x07;
        if (wt == 0) {
            uint64_t v = 0;
            if (!pb_read_varint(buf, len, i, v)) return false;
            if (field == 1) out->node_id = (uint32_t)v;
        } else if (wt == 5) {
            if (len - i < 4) return false;
            if (field == 1) {
                uint32_t nid;
                memcpy(&nid, buf + i, 4);
                out->node_id = nid;
            } else if (field == 2) {
                memcpy(&out->snr, buf + i, 4);
            }
            i += 4;
        } else if (wt == 2) {
            uint64_t flen = 0;
            if (!pb_read_varint(buf, len, i, flen)) return false;
            if (flen > len - i) return false;
            i += (size_t)flen;
        } else {
            return false;
        }
    }
    return true;
}

// Outer NeighborInfo: field 4 is the repeated Neighbor LD list. The
// rest (sender id, interval) is informational only - we don't keep it.
static void handle_neighborinfo(uint32_t node_id, const uint8_t *payload, size_t plen,
                                const struct tm &t)
{
    MeshNeighbor list[MESH_MAX_NEIGHBORS];
    uint8_t      count = 0;
    size_t       i = 0;
    while (i < plen) {
        uint8_t tag   = payload[i++];
        uint8_t field = tag >> 3;
        uint8_t wt    = tag & 0x07;
        if (wt == 0) {
            uint64_t v = 0;
            if (!pb_read_varint(payload, plen, i, v)) return;
        } else if (wt == 2) {
            const uint8_t *sub; size_t sub_len;
            if (!pb_read_ld(payload, plen, i, sub, sub_len)) return;
            if (field == 4 && count < MESH_MAX_NEIGHBORS) {
                MeshNeighbor nb;
                if (parse_one_neighbor(sub, sub_len, &nb) && nb.node_id != 0) {
                    list[count++] = nb;
                }
            }
        } else if (wt == 5) {
            if (plen - i < 4) return;
            i += 4;
        } else {
            return;
        }
    }
    if (count == 0) return;

    int idx = -1;
    for (int j = 0; j < s_node_count; j++) {
        if (s_nodes[j].node_id == node_id) { idx = j; break; }
    }
    MeshNode n;
    if (idx >= 0) {
        n = s_nodes[idx];
    } else {
        memset(&n, 0, sizeof(n));
        n.node_id = node_id;
        snprintf(n.long_name, sizeof(n.long_name), "!%08lx", (unsigned long)node_id);
        snprintf(n.disco_ts, sizeof(n.disco_ts), "%04d%02d%02d_%02d%02d%02d",
                 t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                 t.tm_hour, t.tm_min, t.tm_sec);
    }
    n.neighbor_count    = count;
    memcpy(n.neighbors, list, sizeof(MeshNeighbor) * count);
    n.has_neighborinfo  = true;
    snprintf(n.time_str, sizeof(n.time_str), "%02d:%02d", t.tm_hour, t.tm_min);

    int shift_from = (idx >= 0) ? idx
        : ((s_node_count < MESH_MAX_NODES) ? s_node_count : MESH_MAX_NODES - 1);
    if (idx < 0 && s_node_count < MESH_MAX_NODES) s_node_count++;
    for (int j = shift_from; j > 0; j--) s_nodes[j] = s_nodes[j - 1];
    s_nodes[0] = n;

    MESH_LOG("[MESH] NeighborInfo !%08lx %u neighbours\n",
        (unsigned long)node_id, (unsigned)count);
    save_node_to_sd(n);
    nodes_screen_refresh();
}

// Archive a received message to the microSD card under
// /Meshtastic/Messages/<timestamp>.txt. No-op if no card is mounted.
// SD.mkdir is not recursive, so each path level is created in turn.
static void save_message_to_sd(const MeshMessage &m, const struct tm &t)
{
    if (!instance.isCardReady() || usb_sd_is_running()) return;

    if (!SD.exists("/Meshtastic"))          SD.mkdir("/Meshtastic");
    if (!SD.exists("/Meshtastic/Messages")) SD.mkdir("/Meshtastic/Messages");

    char path[64];
    snprintf(path, sizeof(path),
             "/Meshtastic/Messages/%04d%02d%02d_%02d%02d%02d.txt",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec);

    File f = SD.open(path, FILE_WRITE);
    if (!f) {
        Serial.printf("[MESH] SD open failed: %s\n", path);
        return;
    }
    f.printf("From: !%08lx\n", (unsigned long)m.from_node);
    f.printf("Time: %s\n", m.time_str);
    f.printf("Message: %s\n", m.text);
    f.close();
    MESH_LOG("[MESH] Saved message to %s\n", path);
}

// Archive a discovered node to /Meshtastic/Nodes/<discovery-timestamp>.txt.
// The filename is fixed at first sighting (n.disco_ts); later packets rewrite
// the same file in place to enrich it. No-op if no card is mounted.
static void save_node_to_sd(const MeshNode &n)
{
    if (!instance.isCardReady() || usb_sd_is_running()) return;
    if (!n.disco_ts[0]) return;   // no discovery timestamp recorded

    if (!SD.exists("/Meshtastic"))       SD.mkdir("/Meshtastic");
    if (!SD.exists("/Meshtastic/Nodes")) SD.mkdir("/Meshtastic/Nodes");

    char path[64];
    snprintf(path, sizeof(path), "/Meshtastic/Nodes/%s.txt", n.disco_ts);

    File f = SD.open(path, FILE_WRITE);
    if (!f) {
        Serial.printf("[MESH] SD open failed: %s\n", path);
        return;
    }

    // disco_ts is "YYYYMMDD_HHMMSS"; print it back as a readable datetime
    const char *ts = n.disco_ts;
    f.printf("Node: !%08lx\n", (unsigned long)n.node_id);
    f.printf("Discovered: %.4s-%.2s-%.2s %.2s:%.2s:%.2s\n",
             ts, ts + 4, ts + 6, ts + 9, ts + 11, ts + 13);
    if (n.id[0])         f.printf("ID: %s\n", n.id);
    if (n.long_name[0])  f.printf("Long Name: %s\n", n.long_name);
    if (n.short_name[0]) f.printf("Short Name: %s\n", n.short_name);
    if (n.has_macaddr)
        f.printf("MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
                 n.macaddr[0], n.macaddr[1], n.macaddr[2],
                 n.macaddr[3], n.macaddr[4], n.macaddr[5]);
    if (n.has_hw_model)     f.printf("HW Model: %lu\n", (unsigned long)n.hw_model);
    if (n.has_role)         f.printf("Role: %s\n", meshtastic_role_name(n.role));
    if (n.has_is_licensed)  f.printf("Licensed: %s\n", n.is_licensed ? "Yes" : "No");
    if (n.public_key_len) {
        f.print("Public Key: ");
        for (int k = 0; k < n.public_key_len; k++) f.printf("%02x", n.public_key[k]);
        f.print("\n");
    }
    if (n.has_position) {
        int32_t la = n.latitude_i, lo = n.longitude_i;
        long la_w = la / 10000000, la_f = la % 10000000; if (la_f < 0) la_f = -la_f;
        long lo_w = lo / 10000000, lo_f = lo % 10000000; if (lo_f < 0) lo_f = -lo_f;
        const char *la_s = (la < 0 && la_w == 0) ? "-" : "";
        const char *lo_s = (lo < 0 && lo_w == 0) ? "-" : "";
        f.printf("Position: %s%ld.%07ld, %s%ld.%07ld\n",
                 la_s, la_w, la_f, lo_s, lo_w, lo_f);
    }
    if (n.has_altitude) f.printf("Altitude: %ldm\n", (long)n.altitude);

    f.close();
    MESH_LOG("[MESH] Saved node to %s\n", path);
}

// Mesh packets are rebroadcast by relay nodes, so the same (from_node, packet_id)
// arrives several times. Track the last N seen and drop repeats.
#define MESH_DEDUP_SIZE 16
static struct { uint32_t from; uint32_t id; } s_seen[MESH_DEDUP_SIZE];
static int s_seen_pos = 0;

static bool seen_recently(uint32_t from_node, uint32_t packet_id)
{
    for (int i = 0; i < MESH_DEDUP_SIZE; i++) {
        if (s_seen[i].from == from_node && s_seen[i].id == packet_id)
            return true;
    }
    s_seen[s_seen_pos].from = from_node;
    s_seen[s_seen_pos].id   = packet_id;
    s_seen_pos = (s_seen_pos + 1) % MESH_DEDUP_SIZE;
    return false;
}

// --- Core packet handler ---
static void process_packet(const uint8_t *buf, size_t len,
                           float rssi = NAN, float snr = NAN)
{
    if (len < 18) return;   // 16-byte header + at least 2 payload bytes

    // Parse OTA header (all little-endian). Meshtastic 2.5+ header is 16 bytes:
    // [dest 4][from 4][id 4][flags 1][chan_hash 1][next_hop 1][relay_node 1]
    uint32_t dest_node = (uint32_t)buf[0]  | ((uint32_t)buf[1]  << 8)
                       | ((uint32_t)buf[2]  << 16) | ((uint32_t)buf[3]  << 24);
    uint32_t from_node = (uint32_t)buf[4]  | ((uint32_t)buf[5]  << 8)
                       | ((uint32_t)buf[6]  << 16) | ((uint32_t)buf[7]  << 24);
    uint32_t packet_id = (uint32_t)buf[8]  | ((uint32_t)buf[9]  << 8)
                       | ((uint32_t)buf[10] << 16) | ((uint32_t)buf[11] << 24);

    // Drop mesh rebroadcasts of a packet we already processed
    if (seen_recently(from_node, packet_id)) {
        MESH_LOG("[MESH] dup from=%08lX id=%08lX dropped\n",
            (unsigned long)from_node, (unsigned long)packet_id);
        return;
    }

    // Queue the packet for rebroadcast (if user enabled it) BEFORE decrypting,
    // so packets on channels we don't have keys for still get relayed.
    // hop_limit lives in buf[12] bits 0-2; want_ack/via_mqtt/hop_start sit in
    // the upper bits and are preserved.
    if (configuration_screen_get_rebroadcast_enabled()
        && from_node != s_node_id
        && (buf[12] & 0x07) > 0
        && s_rb_count < MESH_REBROADCAST_QUEUE
        && len <= sizeof(s_rb_queue[0].buf)) {
        uint8_t slot = (s_rb_head + s_rb_count) % MESH_REBROADCAST_QUEUE;
        memcpy(s_rb_queue[slot].buf, buf, len);
        s_rb_queue[slot].len = len;
        s_rb_queue[slot].buf[12] = (buf[12] & ~0x07) | ((buf[12] & 0x07) - 1);
        if (s_rb_count == 0)
            s_rb_send_after_ms = millis() + 50 + (esp_random() % 450);
        s_rb_count++;
    }

    const uint8_t *ct  = buf + 16;
    size_t         ct_len = len - 16;

    // Meshtastic AES-CTR nonce: packet_id and from_node each zero-extended to 64-bit LE
    // = packet_id.to_bytes(8,'little') + from_node.to_bytes(8,'little') in Python
    uint8_t nonce[16] = {0};
    nonce[0]  = (packet_id >>  0) & 0xFF;  nonce[1]  = (packet_id >>  8) & 0xFF;
    nonce[2]  = (packet_id >> 16) & 0xFF;  nonce[3]  = (packet_id >> 24) & 0xFF;
    // nonce[4-7] = 0 (high 32 bits of packet_id, already zeroed)
    nonce[8]  = (from_node >>  0) & 0xFF;  nonce[9]  = (from_node >>  8) & 0xFF;
    nonce[10] = (from_node >> 16) & 0xFF;  nonce[11] = (from_node >> 24) & 0xFF;
    // nonce[12-15] = 0 (high 32 bits of from_node, already zeroed)

    // Look up the channel by the OTA hash byte. If no enabled slot
    // matches, the packet is on a channel we can't decrypt - drop it
    // silently (slot 0 = LongFast is always populated, so the default-
    // channel case never falls through).
    const MeshChannel *rx_ch = find_channel_by_hash(buf[13]);
    if (!rx_ch) return;
    uint8_t rx_kbits = (rx_ch->psk_len >= 32) ? 256 : 128;

    uint8_t plain[256];
    if (ct_len > sizeof(plain)) ct_len = sizeof(plain);
    decrypt_ctr_n(rx_ch->psk, rx_kbits, nonce, ct, plain, ct_len);

    // Capture first two bytes for diagnostics.
    // Valid protobuf Data: plain[0]=0x08 (field 1 varint tag), plain[1]=portnum value.
    // 1=TEXT  3=POSITION  4=NODEINFO  others=non-text  random=wrong AES key
    s_dbg.last_plain0  = (ct_len > 0) ? plain[0] : 0;
    s_dbg.last_plain1  = (ct_len > 1) ? plain[1] : 0;
    s_dbg.last_portnum = (plain[0] == 0x08 && ct_len > 1) ? plain[1] : 0;

    MESH_LOG("[MESH] from=%08lX id=%08lX flags=0x%02X chan=0x%02X ct_len=%u plain=%02X %02X\n",
        (unsigned long)from_node, (unsigned long)packet_id,
        buf[12], buf[13], (unsigned)ct_len, plain[0], plain[1]);

    // Parse decrypted Data protobuf
    uint32_t       portnum;
    const uint8_t *payload;
    size_t         payload_len;
    uint32_t       request_id = 0;
    if (!parse_data(plain, ct_len, &portnum, &payload, &payload_len, &request_id)) {
        Serial.printf("[MESH] parse_data FAILED portnum=%lu (1=TEXT 3=POS 4=NODEINFO)\n",
            (unsigned long)s_dbg.last_portnum);
        return;
    }

    // ROUTING (portnum 70): ACK/NAK + traceroute responses. The first
    // byte of the payload tells us which Routing oneof field is set.
    //   field 1 (varint, wire=0) error_reason - ACK if 0, NAK otherwise
    //   field 2 (LD,     wire=2) route_request - someone tracing us
    //   field 3 (LD,     wire=2) route_reply   - response to OUR trace
    if (portnum == PORTNUM_ROUTING) {
        // Routing oneof, first byte tells us which case:
        //   field 1 varint (wire=0) error_reason - ACK if 0, NAK otherwise
        //   field 2 LD     (wire=2) route_request - we reply
        //   field 3 LD     (wire=2) route_reply   - response to our trace
        // ACKs only matter when addressed at us AND tied to one of our
        // outstanding outgoing entries.
        if (payload && payload_len >= 1 &&
            (payload[0] >> 3) == 1 && (payload[0] & 0x07) == 0 &&
            dest_node == s_node_id && request_id != 0) {
            uint8_t err = (payload_len >= 2) ? payload[1] : 0;
            for (int i = 0; i < s_outgoing_count; i++) {
                if (s_outgoing[i].packet_id == request_id &&
                    s_outgoing[i].state == MeshOutgoing::PENDING) {
                    if (err == 0) {
                        s_outgoing[i].state        = MeshOutgoing::DELIVERED;
                        s_outgoing[i].error_reason = 0;
                    } else {
                        s_outgoing[i].state        = MeshOutgoing::NAKED;
                        s_outgoing[i].error_reason = err;
                    }
                    MESH_LOG("[MESH] Routing ACK pkt_id=%08lx state=%d err=%u\n",
                        (unsigned long)request_id, (int)s_outgoing[i].state,
                        (unsigned)err);
                    break;
                }
            }
            return;
        }
        if (payload && payload_len >= 1) {
            uint8_t tag   = payload[0];
            uint8_t field = tag >> 3;
            uint8_t wire  = tag & 0x07;
            if (field == 2 && wire == 2 && dest_node == s_node_id) {
                // route_request addressed at us. Queue a reply for
                // bg_tick to drain - building the reply inline from
                // process_packet would push another transmit() onto
                // the IRQ-pump path. The request's pkt_id becomes the
                // request_id on our reply.
                if (s_route_reply_queue_count < 2) {
                    s_route_reply_queue[s_route_reply_queue_count].dest       = from_node;
                    s_route_reply_queue[s_route_reply_queue_count].request_id = packet_id;
                    s_route_reply_queue_count++;
                    MESH_LOG("[MESH] queued route_reply -> %08lx req=%08lx\n",
                        (unsigned long)from_node, (unsigned long)packet_id);
                }
            } else if (field == 3 && wire == 2 &&
                       s_traceroute.request_id == request_id &&
                       request_id != 0 &&
                       !s_traceroute.has_response) {
                // route_reply matching our outstanding traceroute. The
                // RouteDiscovery body inside (payload bytes after the
                // LD length) carries the accumulated route. Walk it
                // looking for field 1 (route) in packed or unpacked
                // form. payload[1] is the inner length.
                if (payload_len >= 2) {
                    size_t rd_len = payload[1];
                    if (rd_len > payload_len - 2) rd_len = payload_len - 2;
                    const uint8_t *rd = payload + 2;
                    uint8_t hops = 0;
                    for (size_t p = 0; p < rd_len && hops < 8; ) {
                        uint8_t t = rd[p++];
                        uint8_t f = t >> 3;
                        uint8_t w = t & 0x07;
                        if (f == 1 && w == 2 && p < rd_len) {
                            size_t llen = rd[p++];
                            if (llen > rd_len - p) llen = rd_len - p;
                            for (size_t off = 0; off + 4 <= llen && hops < 8; off += 4) {
                                s_traceroute.hops[hops++] =
                                    (uint32_t)rd[p + off]      |
                                    ((uint32_t)rd[p + off + 1] << 8)  |
                                    ((uint32_t)rd[p + off + 2] << 16) |
                                    ((uint32_t)rd[p + off + 3] << 24);
                            }
                            p += llen;
                        } else if (f == 1 && w == 5 && p + 4 <= rd_len) {
                            s_traceroute.hops[hops++] =
                                (uint32_t)rd[p]     |
                                ((uint32_t)rd[p+1] << 8)  |
                                ((uint32_t)rd[p+2] << 16) |
                                ((uint32_t)rd[p+3] << 24);
                            p += 4;
                        } else {
                            break;
                        }
                    }
                    s_traceroute.hop_count    = hops;
                    s_traceroute.has_response = true;
                    s_traceroute.response_ms  = millis();
                    MESH_LOG("[MESH] route_reply for !%08lx: %u hops\n",
                        (unsigned long)s_traceroute.target_node, (unsigned)hops);
                }
            }
        }
        return;
    }

    struct tm t;
    instance.rtc.getDateTime(&t);
    int utc_off = clock_screen_get_utc_offset();
    if (utc_off != 0) { t.tm_hour += utc_off; mktime(&t); }

    // NodeInfo advertisement — record the node and stop here
    if (portnum == PORTNUM_NODEINFO) {
        handle_nodeinfo(from_node, payload, payload_len, t);
        return;
    }

    // Position broadcast — attach coordinates to the node and stop here
    if (portnum == PORTNUM_POSITION) {
        handle_position(from_node, dest_node, payload, payload_len, t);
        return;
    }
    if (portnum == PORTNUM_TELEMETRY) {
        handle_telemetry(from_node, payload, payload_len, t);
        return;
    }
    if (portnum == PORTNUM_NEIGHBORINFO) {
        handle_neighborinfo(from_node, payload, payload_len, t);
        return;
    }

    // Everything below handles plain text messages only
    if (portnum != PORTNUM_TEXT) {
        MESH_LOG("[MESH] ignored portnum=%lu (not TEXT/POS/NODEINFO)\n",
            (unsigned long)portnum);
        return;
    }

    const uint8_t *text_ptr = payload;
    size_t         text_len = payload_len;
    s_dbg.parse_ok++;
    MESH_LOG("[MESH] TEXT decoded len=%u text=\"%.*s\"\n", (unsigned)text_len, (int)text_len, text_ptr);

    // Distributed tracker-reputation flag from a peer (TRFLAG|...). Folded into
    // the reputation store and swallowed here so it never shows up as chat.
    if (tracker_rep_handle_text(from_node, (const char *)text_ptr, (uint32_t)text_len))
        return;

    // Store message — shift ring buffer, newest at index 0
    int store_slots = (s_count < MESH_MAX_MESSAGES) ? s_count : MESH_MAX_MESSAGES - 1;
    for (int i = store_slots; i > 0; i--) s_msgs[i] = s_msgs[i - 1];

    MeshMessage &m = s_msgs[0];
    m.from_node = from_node;
    m.dest_node = dest_node;       // OTA dest - 0xFFFFFFFF = broadcast,
                                   // our node id = DM addressed to us
    size_t copy_len = (text_len < MESH_MAX_TEXT_LEN - 1) ? text_len : MESH_MAX_TEXT_LEN - 1;
    memcpy(m.text, text_ptr, copy_len);
    m.text[copy_len] = '\0';

    snprintf(m.time_str, sizeof(m.time_str), "%02d:%02d", t.tm_hour, t.tm_min);

    // Link-quality stats + hop accounting. OTA header byte 12 packs
    // hop_start (bits 5-7) and hop_limit (bits 0-2). Hops traversed
    // before this packet reached us = start - limit (relays decrement
    // limit at each retransmit).
    m.rssi      = rssi;
    m.snr       = snr;
    m.hop_start = (buf[12] >> 5) & 0x07;
    m.hop_limit = buf[12] & 0x07;

    if (s_count < MESH_MAX_MESSAGES) s_count++;
    s_total++;
    s_unread++;

    // Haptic notify, gated by the per-message-type toggles in
    // configuration_screen. dest_node == our node id => DM; anything
    // else (broadcast 0xFFFFFFFF or some other recipient we overheard)
    // is broadcast/channel chatter. instance.vibrator() runs a single
    // DRV2605 effect via LilyGoLib - we don't try to vary intensity.
    bool to_me = (dest_node != 0xFFFFFFFFu && dest_node == s_node_id);
    if ((to_me && configuration_screen_get_vibrate_dm()) ||
        (!to_me && configuration_screen_get_vibrate_broadcast())) {
        instance.vibrator();
    }

    clock_screen_set_mesh_count(s_unread);
    meshtastic_screen_refresh();

    save_message_to_sd(m, t);
}

// --- Public API ---

void meshtastic_set_active(bool active)
{
    s_active = active;
    if (active) {
        if (s_node_id == 0) s_node_id = derive_node_id();
        radio.setFrequency(MESH_FREQ_MHZ);
        radio.setBandwidth(MESH_BW_KHZ);
        radio.setSpreadingFactor(MESH_SF);
        radio.setCodingRate(MESH_CR);
        // Meshtastic uses 0x2B (semi-public), not the RadioLib default 0x12
        radio.setSyncWord(0x2B);
        s_pkt_flag         = false;
        s_cad_last_ms      = millis();
        s_nodeinfo_pending = true;   // send NodeInfo on next bg tick
        s_nodeinfo_last_ms = millis();
        radio.setPacketReceivedAction(on_pkt_isr);
        radio.startReceive();
        s_pkt_flag = false;   // clear any IRQ that fired before startReceive
    } else {
        radio.clearPacketReceivedAction();
        radio.standby();
        s_rssi             = 0.0f;
        s_cad              = false;
        s_nodeinfo_pending = false;
    }
}

void meshtastic_bg_tick()
{
    if (!s_active) return;

    // Process any received packet — do this first, before any radio mode changes
    if (s_pkt_flag) {
        s_pkt_flag = false;
        s_dbg.isr_count = s_isr_count;

        uint8_t buf[256];
        size_t pkt_len = radio.getPacketLength();
        s_dbg.rx_last_pkt_len = (int)pkt_len;

        MESH_LOG("[MESH] IRQ isr#%d pkt_len=%u\n", s_isr_count, (unsigned)pkt_len);

        if (pkt_len >= 16 && pkt_len <= sizeof(buf)) {
            int16_t rc = radio.readData(buf, pkt_len);
            s_dbg.rx_last_rc = rc;
            if (rc == RADIOLIB_ERR_NONE) {
                s_dbg.rx_ok++;
                // SX1262 packet stats are valid between readData and the
                // next startReceive(). getRSSI(true) gives the packet
                // RSSI (vs false = instantaneous channel power).
                float pkt_rssi = radio.getRSSI(true);
                float pkt_snr  = radio.getSNR();
                process_packet(buf, pkt_len, pkt_rssi, pkt_snr);
            } else {
                s_dbg.rx_fail++;
                Serial.printf("[MESH] readData FAILED rc=%d\n", (int)rc);
            }
        } else {
            // pkt_len out of range — flush FIFO to unblock radio
            s_dbg.rx_fail++;
            s_dbg.rx_last_rc = -999;
            Serial.printf("[MESH] pkt_len=%u out of range [16..256] - flushing\n", (unsigned)pkt_len);
            radio.readData(buf, sizeof(buf));
        }

        radio.startReceive();
        s_cad_last_ms = millis();
        return;
    }

    uint32_t now = millis();

    // Send NodeInfo on the cadence the Configuration screen owns.
    // s_nodeinfo_pending is the "fire ASAP" trigger (set on activation
    // and on long/short name change); the periodic timer only fires
    // when s_announce_on is true. Telemetry rides the same cadence so
    // other nodes see our battery + uptime alongside our identity.
    if (s_nodeinfo_pending ||
        (s_announce_on && s_announce_interval > 0 &&
         now - s_nodeinfo_last_ms >= s_announce_interval)) {
        s_nodeinfo_pending = false;
        s_nodeinfo_last_ms = now;
        s_cad_last_ms      = now;   // don't run CAD immediately after TX
        send_nodeinfo();
        send_telemetry_broadcast();
        return;
    }

    // User-queued text message — sent from bg_tick to avoid blocking the UI
    if (s_text_pending) {
        s_text_pending = false;
        s_cad_last_ms  = now;   // don't run CAD immediately after TX
        uint32_t pkt_id = do_send_text(s_pending_text, s_pending_dest);
        // Push an outgoing-tracker entry. The Routing-app RX handler
        // flips PENDING -> DELIVERED / NAKED on ACK arrival; bg_tick
        // ages stragglers to NO_ACK after kAckTimeoutMs.
        if (pkt_id != 0) {
            int slots = (s_outgoing_count < MESH_MAX_OUTGOING)
                ? s_outgoing_count
                : MESH_MAX_OUTGOING - 1;
            for (int i = slots; i > 0; i--) s_outgoing[i] = s_outgoing[i - 1];
            MeshOutgoing &o = s_outgoing[0];
            memset(&o, 0, sizeof(o));
            o.packet_id = pkt_id;
            strncpy(o.text, s_pending_text, sizeof(o.text) - 1);
            o.sent_ms   = millis();
            o.dest_node = s_pending_dest;
            o.state     = MeshOutgoing::PENDING;
            if (s_outgoing_count < MESH_MAX_OUTGOING) s_outgoing_count++;
        }
        s_pending_dest = 0xFFFFFFFFu;   // reset to broadcast for next call
        return;
    }

    // Age PENDING tracker entries that didn't get a Routing ACK within
    // kAckTimeoutMs. Cheap O(MESH_MAX_OUTGOING) walk per tick.
    for (int i = 0; i < s_outgoing_count; i++) {
        if (s_outgoing[i].state == MeshOutgoing::PENDING &&
            now - s_outgoing[i].sent_ms > kAckTimeoutMs) {
            s_outgoing[i].state = MeshOutgoing::NO_ACK;
        }
    }

    // User-queued traceroute. Stamp the chosen pkt_id onto the
    // tracking slot so the inbound route_reply matcher can bind to it.
    if (s_traceroute_pending) {
        s_traceroute_pending = false;
        s_cad_last_ms = now;
        uint32_t pkt_id = do_send_traceroute(s_traceroute_pending_dest);
        if (pkt_id) {
            s_traceroute              = MeshTraceroute{};
            s_traceroute.target_node  = s_traceroute_pending_dest;
            s_traceroute.request_id   = pkt_id;
            s_traceroute.sent_ms      = millis();
        }
        return;
    }

    // Drain one queued route_reply per tick. These are responses to
    // someone else tracing us; only emitted when we got a route_request
    // addressed specifically at our node ID.
    if (s_route_reply_queue_count > 0) {
        // Pop the head, shift the rest down (queue is 2 deep at most).
        PendingRouteReply r = s_route_reply_queue[0];
        for (uint8_t i = 1; i < s_route_reply_queue_count; i++)
            s_route_reply_queue[i - 1] = s_route_reply_queue[i];
        s_route_reply_queue_count--;
        s_cad_last_ms = now;
        do_send_route_reply(r.dest, r.request_id);
        return;
    }

    // Drain one queued position reply per tick. Same idea as the
    // route_reply queue - someone asked where we are; we answer with
    // the current GPS fix. Fix is re-sampled at drain time (not enqueue
    // time) so we don't send stale coordinates if the queue lingered.
    if (s_pos_reply_queue_count > 0) {
        PendingPositionReply r = s_pos_reply_queue[0];
        for (uint8_t i = 1; i < s_pos_reply_queue_count; i++)
            s_pos_reply_queue[i - 1] = s_pos_reply_queue[i];
        s_pos_reply_queue_count--;
        if (gps_screen_has_lock() && instance.gps.location.isValid()) {
            int32_t lat_i = (int32_t)(instance.gps.location.lat() * 1e7);
            int32_t lon_i = (int32_t)(instance.gps.location.lng() * 1e7);
            int32_t alt_m = instance.gps.altitude.isValid()
                ? (int32_t)instance.gps.altitude.meters() : 0;
            s_cad_last_ms = now;
            do_send_position(lat_i, lon_i, alt_m, r.dest);
        }
        return;
    }

    // User-queued outbound position request - same staging as text and
    // traceroute. No tracking slot today; replies just land in the
    // normal POSITION RX path and update the asker's node table.
    if (s_pos_request_pending) {
        s_pos_request_pending = false;
        s_cad_last_ms = now;
        do_send_position_request(s_pos_request_dest);
        return;
    }

    // Drain one rebroadcast per tick after a random backoff. The backoff
    // spreads each relay's TX over ~50-500 ms so multiple watches hearing
    // the same packet don't collide with each other or the originator.
    if (s_rb_count > 0 && now >= s_rb_send_after_ms) {
        uint8_t slot = s_rb_head;
        radio.transmit(s_rb_queue[slot].buf, s_rb_queue[slot].len);
        radio.startReceive();
        s_rb_head = (s_rb_head + 1) % MESH_REBROADCAST_QUEUE;
        s_rb_count--;
        s_cad_last_ms = millis();
        s_rb_send_after_ms = (s_rb_count > 0)
            ? millis() + 50 + (esp_random() % 450)
            : 0;
        MESH_LOG("[MESH] rebroadcast sent, %u left\n", (unsigned)s_rb_count);
        return;
    }

    // Live RSSI (valid while in receive mode)
    s_rssi = radio.getRSSI(false);

    // CAD every 30 seconds — blocking call, kept infrequent to minimise the
    // window during which incoming packets could be missed.
    if (now - s_cad_last_ms >= 30000) {
        s_cad_last_ms = now;
        s_cad = (radio.scanChannel() == RADIOLIB_LORA_DETECTED);
        radio.startReceive();
    }
}

void meshtastic_set_long_name(const char *name)
{
    if (!name) return;
    strncpy(s_long_name, name, sizeof(s_long_name) - 1);
    s_long_name[sizeof(s_long_name) - 1] = '\0';
    if (s_active) s_nodeinfo_pending = true;   // re-announce with new name
}

void meshtastic_set_short_name(const char *name)
{
    if (!name) return;
    strncpy(s_short_name, name, sizeof(s_short_name) - 1);
    s_short_name[sizeof(s_short_name) - 1] = '\0';
    if (s_active) s_nodeinfo_pending = true;
}

void meshtastic_set_announce(bool on, uint32_t interval_ms)
{
    s_announce_on       = on;
    if (interval_ms > 0) s_announce_interval = interval_ms;
    // Toggling off->on while the radio is up should fire a fresh
    // announce quickly instead of waiting up to a full interval. Push
    // s_nodeinfo_last_ms back by (interval - 5 s) so bg_tick's
    // (now - last >= interval) check fires roughly 5 s from now.
    if (on && s_active) {
        uint32_t now = millis();
        s_nodeinfo_last_ms = (now > s_announce_interval - 5000)
                                 ? (now - s_announce_interval + 5000)
                                 : 0;
    }
}

bool     meshtastic_announce_enabled()      { return s_announce_on; }
uint32_t meshtastic_announce_interval_ms()  { return s_announce_interval; }

const char *meshtastic_get_long_name()  { return s_long_name; }
const char *meshtastic_get_short_name() { return s_short_name; }

bool meshtastic_send_text(const char *text)
{
    return meshtastic_send_text_to(text, 0xFFFFFFFFu);
}

bool meshtastic_send_text_to(const char *text, uint32_t dest_node)
{
    if (!s_active || !text || !text[0]) return false;
    strncpy(s_pending_text, text, sizeof(s_pending_text) - 1);
    s_pending_text[sizeof(s_pending_text) - 1] = '\0';
    s_pending_dest = dest_node;
    s_text_pending = true;
    return true;
}

bool meshtastic_send_traceroute(uint32_t dest_node)
{
    if (!s_active) return false;
    if (dest_node == 0 || dest_node == 0xFFFFFFFFu) return false;
    s_traceroute_pending      = true;
    s_traceroute_pending_dest = dest_node;
    return true;
}

bool meshtastic_traceroute_in_flight()
{
    return s_traceroute.request_id != 0 && !s_traceroute.has_response;
}

const MeshTraceroute *meshtastic_get_last_traceroute()
{
    return s_traceroute.request_id ? &s_traceroute : nullptr;
}

bool meshtastic_request_position(uint32_t dest_node)
{
    if (!s_active) return false;
    if (dest_node == 0 || dest_node == 0xFFFFFFFFu) return false;
    s_pos_request_pending = true;
    s_pos_request_dest    = dest_node;
    return true;
}

int  meshtastic_get_outgoing_count() { return s_outgoing_count; }
const MeshOutgoing *meshtastic_get_outgoing(int idx)
{
    if (idx < 0 || idx >= s_outgoing_count) return nullptr;
    return &s_outgoing[idx];
}

const MeshChannel *meshtastic_get_channel(int idx)
{
    if (idx < 0 || idx >= MESH_MAX_CHANNELS) return nullptr;
    ensure_channels_initialised();
    return &s_channels[idx];
}

void meshtastic_set_channel_enabled(int idx, bool on)
{
    if (idx < 0 || idx >= MESH_MAX_CHANNELS) return;
    ensure_channels_initialised();
    // Slot 0 is the public LongFast channel - the PSK is fixed, but
    // the user can still mute it (disable on/off) if they're on a
    // private channel and don't want the chatter.
    if (on && s_channels[idx].psk_len == 0 && idx != 0) {
        // Empty user slot - seed a random AES-128 PSK + default name
        // so the slot becomes usable. The user can edit either via
        // /Meshtastic/channels.txt on the SD card.
        snprintf(s_channels[idx].name, sizeof(s_channels[idx].name),
                 "Channel %d", idx);
        for (int i = 0; i < 16; i++) {
            s_channels[idx].psk[i] = (uint8_t)(esp_random() & 0xFF);
        }
        s_channels[idx].psk_len      = 16;
        s_channels[idx].channel_hash = channel_hash_of(
            s_channels[idx].name, s_channels[idx].psk, s_channels[idx].psk_len);
    }
    s_channels[idx].enabled = on;
    meshtastic_save_channels_to_sd();
}

int meshtastic_get_active_channel() { return s_active_channel; }

void meshtastic_set_active_channel(int idx)
{
    if (idx < 0 || idx >= MESH_MAX_CHANNELS) return;
    ensure_channels_initialised();
    if (!s_channels[idx].enabled || s_channels[idx].psk_len == 0) return;
    s_active_channel = idx;
    meshtastic_save_channels_to_sd();
}

// /Meshtastic/channels.txt - simple key=value, one line per field.
// Slot 0 isn't written (always LongFast); slots 1..3 carry name +
// hex PSK + enabled. The active_channel index sits at the top.
void meshtastic_save_channels_to_sd()
{
    if (!instance.isCardReady()) return;
    if (!SD.exists("/Meshtastic")) SD.mkdir("/Meshtastic");
    File f = SD.open("/Meshtastic/channels.txt", FILE_WRITE);
    if (!f) return;
    ensure_channels_initialised();
    f.printf("active_channel=%d\n", s_active_channel);
    f.printf("ch0_enabled=%d\n",    s_channels[0].enabled ? 1 : 0);
    for (int i = 1; i < MESH_MAX_CHANNELS; i++) {
        if (s_channels[i].psk_len == 0) continue;
        f.printf("ch%d_name=%s\n", i, s_channels[i].name);
        f.printf("ch%d_psk_hex=", i);
        for (uint8_t j = 0; j < s_channels[i].psk_len; j++) {
            f.printf("%02x", s_channels[i].psk[j]);
        }
        f.printf("\n");
        f.printf("ch%d_enabled=%d\n", i, s_channels[i].enabled ? 1 : 0);
    }
    f.close();
}

static void apply_channel_kv(const char *key, const char *val)
{
    if (strcmp(key, "active_channel") == 0) {
        int idx = (int)strtol(val, nullptr, 10);
        if (idx >= 0 && idx < MESH_MAX_CHANNELS) s_active_channel = idx;
        return;
    }
    if (strncmp(key, "ch", 2) != 0) return;
    if (key[2] < '0' || key[2] > '9' || key[3] != '_') return;
    int idx = key[2] - '0';
    if (idx < 0 || idx >= MESH_MAX_CHANNELS) return;
    const char *sub = key + 4;
    if (strcmp(sub, "name") == 0 && idx != 0) {
        strncpy(s_channels[idx].name, val, sizeof(s_channels[idx].name) - 1);
        s_channels[idx].name[sizeof(s_channels[idx].name) - 1] = '\0';
        s_channels[idx].channel_hash = channel_hash_of(
            s_channels[idx].name, s_channels[idx].psk, s_channels[idx].psk_len);
    } else if (strcmp(sub, "psk_hex") == 0 && idx != 0) {
        size_t hex_len = strlen(val);
        if (hex_len % 2 != 0 || hex_len > 64) return;
        s_channels[idx].psk_len = (uint8_t)(hex_len / 2);
        for (size_t b = 0; b < s_channels[idx].psk_len; b++) {
            unsigned v;
            sscanf(val + b * 2, "%2x", &v);
            s_channels[idx].psk[b] = (uint8_t)v;
        }
        s_channels[idx].channel_hash = channel_hash_of(
            s_channels[idx].name, s_channels[idx].psk, s_channels[idx].psk_len);
    } else if (strcmp(sub, "enabled") == 0) {
        s_channels[idx].enabled = (strtol(val, nullptr, 10) != 0);
    }
}

void meshtastic_load_channels_from_sd()
{
    ensure_channels_initialised();
    if (!instance.isCardReady()) return;
    if (!SD.exists("/Meshtastic/channels.txt")) return;
    File f = SD.open("/Meshtastic/channels.txt", FILE_READ);
    if (!f) return;
    char line[96];
    while (f.available()) {
        size_t n = 0;
        while (f.available() && n < sizeof(line) - 1) {
            int c = f.read();
            if (c < 0)    break;
            if (c == '\n') break;
            if (c == '\r') continue;
            line[n++] = (char)c;
        }
        line[n] = '\0';
        if (n == 0 || line[0] == '#') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        apply_channel_kv(line, eq + 1);
    }
    f.close();
}

// Routing.Error enum values used in practice. Codes in the 30s are PKI
// and admin-related; we surface them by name for completeness even
// though the watch doesn't generate them itself.
const char *meshtastic_routing_error_name(uint8_t r)
{
    switch (r) {
        case 0:  return "NONE";
        case 1:  return "NO_ROUTE";
        case 2:  return "GOT_NAK";
        case 3:  return "TIMEOUT";
        case 4:  return "NO_INTERFACE";
        case 5:  return "MAX_RETRANSMIT";
        case 6:  return "NO_CHANNEL";
        case 7:  return "TOO_LARGE";
        case 8:  return "NO_RESPONSE";
        case 9:  return "DUTY_CYCLE_LIMIT";
        case 32: return "BAD_REQUEST";
        case 33: return "NOT_AUTHORIZED";
        case 34: return "PKI_FAILED";
        case 35: return "PKI_UNKNOWN_PUBKEY";
        case 38: return "RATE_LIMIT_EXCEEDED";
        default: {
            static char buf[12];
            snprintf(buf, sizeof(buf), "Err %u", (unsigned)r);
            return buf;
        }
    }
}

int  meshtastic_get_count()  { return s_count; }   // ring-buffer fill (for display/iteration)
int  meshtastic_get_rx_count() { return s_dbg.rx_ok; } // total RF packets received
int  meshtastic_get_unread() { return s_unread; }
void meshtastic_mark_read()  { s_unread = 0; clock_screen_set_mesh_count(0); }

const MeshMessage *meshtastic_get_message(int idx)
{
    if (idx < 0 || idx >= s_count) return nullptr;
    return &s_msgs[idx];
}

// Wipe the in-memory message ring. The SD archive at
// /Meshtastic/Messages/<timestamp>.txt is intentionally NOT touched -
// every received packet stays on the card forever; this just clears
// what's on the screen. Also clears the unread badge counter so the
// clock-face indicator drops back to its idle state.
void meshtastic_clear_messages()
{
    memset(s_msgs, 0, sizeof(s_msgs));
    s_count  = 0;
    s_unread = 0;
    clock_screen_set_mesh_count(0);
}

// Remove a single message from the ring. Shifts older entries up to
// fill the gap. As with clear_messages, the on-disk archive is left
// alone - the per-message file at /Meshtastic/Messages/<timestamp>.txt
// stays put.
bool meshtastic_delete_message(int idx)
{
    if (idx < 0 || idx >= s_count) return false;
    for (int i = idx; i < s_count - 1; i++) s_msgs[i] = s_msgs[i + 1];
    s_count--;
    // Zero the now-tail slot so a stale entry can't leak through if
    // the ring later grows past s_count via a fresh receive.
    memset(&s_msgs[s_count], 0, sizeof(MeshMessage));
    return true;
}

int meshtastic_get_node_count() { return s_node_count; }

const MeshNode *meshtastic_get_node(int idx)
{
    if (idx < 0 || idx >= s_node_count) return nullptr;
    return &s_nodes[idx];
}

float    meshtastic_get_rssi()    { return s_rssi; }
bool     meshtastic_get_cad()     { return s_cad;  }
bool     meshtastic_is_active()   { return s_active; }
uint32_t meshtastic_get_node_id() { return s_node_id ? s_node_id : derive_node_id(); }

const MeshDebugInfo *meshtastic_get_debug()
{
    s_dbg.isr_count = s_isr_count;
    return &s_dbg;
}
