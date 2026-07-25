#include "portscan.h"

void clock_screen_get_local_time(struct tm *out);
#include "usb_sd.h"
#include <Arduino.h>
#include <LilyGoLib.h>
#include <SD.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <string.h>
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/inet.h"

// ---- Port lists ------------------------------------------------------------
// "Top N" sets follow nmap's --top-ports rankings, trimmed to entries that
// are actually plausible on a residential / SOHO network — the watch is the
// realistic deployment target. Both arrays are immutable; the scanner
// indexes them directly without copying.

static const uint16_t TOP_20[] = {
    21, 22, 23, 25, 53, 80, 110, 135, 139, 143,
    443, 445, 993, 995, 1723, 3306, 3389, 5900, 8080, 8443,
};
static const int TOP_20_COUNT = (int)(sizeof(TOP_20) / sizeof(TOP_20[0]));

static const uint16_t TOP_100[] = {
      7,    9,   13,   21,   22,   23,   25,   26,   37,   53,
     79,   80,   81,   88,  106,  110,  111,  113,  119,  135,
    139,  143,  144,  179,  199,  389,  427,  443,  444,  445,
    465,  513,  514,  515,  543,  544,  548,  554,  587,  631,
    646,  873,  990,  993,  995, 1025, 1026, 1027, 1028, 1029,
   1110, 1433, 1720, 1723, 1755, 1900, 2000, 2001, 2049, 2121,
   2717, 3000, 3128, 3306, 3389, 3986, 4899, 5000, 5009, 5051,
   5060, 5101, 5190, 5357, 5432, 5631, 5666, 5800, 5900, 6000,
   6001, 6646, 7070, 8000, 8008, 8009, 8080, 8081, 8443, 8888,
   9100, 9999, 10000, 32768, 49152, 49153, 49154, 49155, 49156, 49157,
};
static const int TOP_100_COUNT = (int)(sizeof(TOP_100) / sizeof(TOP_100[0]));

// ---- Scan parameters -------------------------------------------------------
// Per-port timeouts. Short enough that a Top-100 scan stays under ~20 s,
// long enough to ride out a couple of normal-network RTTs.
#define TCP_CONNECT_TIMEOUT_MS  220
#define BANNER_READ_TIMEOUT_MS  600    // wait for the server to send first
#define UDP_RESPONSE_TIMEOUT_MS 250

// ---- State -----------------------------------------------------------------

// Results are only needed while using the port scanner. Keeping this 25.6 KB
// table in internal .bss permanently starves the WiFi driver when BLE is
// active, so allocate it from PSRAM on first use instead.
static PortScanResult *s_results = nullptr;
static volatile int   s_result_count   = 0;
static volatile int   s_scanned        = 0;
static int            s_total          = 0;
static volatile bool  s_running        = false;
static volatile bool  s_done           = false;
static volatile bool  s_abort          = false;
static bool           s_logged         = false;
static volatile bool  s_just_finished  = false;

static uint32_t       s_target_ip      = 0;       // host byte order
static PortScanTech   s_tech           = PSTECH_TCP_CONNECT;  // bitmask
static PortScanPreset s_preset         = PSPRE_TOP_20;
static uint16_t       s_custom_lo      = 1;
static uint16_t       s_custom_hi      = 1024;

static TaskHandle_t   s_task           = nullptr;

// ---- helpers ---------------------------------------------------------------

static bool ensure_result_storage()
{
    if (s_results) return true;
    s_results = static_cast<PortScanResult *>(heap_caps_calloc(
        PORTSCAN_MAX_RESULTS, sizeof(PortScanResult),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!s_results)
        Serial.println("[portscan] unable to allocate result storage in PSRAM");
    return s_results != nullptr;
}

static void record(uint16_t port, uint8_t state, bool is_udp, const char *banner)
{
    if (!s_results || s_result_count >= PORTSCAN_MAX_RESULTS) return;
    PortScanResult &r = s_results[s_result_count++];
    r.port  = port;
    r.state = state;
    r.is_udp = is_udp ? 1 : 0;
    r.banner[0] = '\0';
    if (banner && banner[0]) {
        // Strip control chars while copying so a binary protocol response
        // doesn't corrupt the on-screen line. Newlines collapse to spaces.
        int oi = 0;
        for (int i = 0; banner[i] && oi < (int)sizeof(r.banner) - 1; i++) {
            unsigned char c = (unsigned char)banner[i];
            if (c == '\r' || c == '\n' || c == '\t') {
                if (oi > 0 && r.banner[oi - 1] != ' ') r.banner[oi++] = ' ';
            } else if (c >= 0x20 && c < 0x7F) {
                r.banner[oi++] = (char)c;
            }
        }
        r.banner[oi] = '\0';
    }
}

static void build_sockaddr(struct sockaddr_in *sa, uint16_t port)
{
    memset(sa, 0, sizeof(*sa));
    sa->sin_family      = AF_INET;
    sa->sin_port        = htons(port);
    // s_target_ip is in host byte order; lwIP sockets want network order.
    sa->sin_addr.s_addr = htonl(s_target_ip);
}

// Non-blocking TCP connect with timeout. Returns 1 = open, 0 = closed/filtered.
// On success and want_banner, reads up to PORTSCAN_BANNER_MAX bytes (or until
// the server stops sending for BANNER_READ_TIMEOUT_MS) into `banner`.
static int tcp_probe(uint16_t port, bool want_banner,
                     char *banner, int banner_max)
{
    if (banner && banner_max > 0) banner[0] = '\0';

    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) return 0;

    // Non-blocking so connect() returns immediately; we wait via select().
    int fl = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, fl | O_NONBLOCK);

    struct sockaddr_in sa;
    build_sockaddr(&sa, port);

    int rc = connect(s, (struct sockaddr *)&sa, sizeof(sa));
    if (rc < 0 && errno != EINPROGRESS) {
        close(s);
        return 0;
    }

    fd_set wfds; FD_ZERO(&wfds); FD_SET(s, &wfds);
    struct timeval tv = { TCP_CONNECT_TIMEOUT_MS / 1000,
                          (TCP_CONNECT_TIMEOUT_MS % 1000) * 1000 };
    rc = select(s + 1, NULL, &wfds, NULL, &tv);
    if (rc <= 0) { close(s); return 0; }          // timeout = filtered

    // Distinguish completed-handshake from connection-refused via SO_ERROR.
    int so_err = 0; socklen_t elen = sizeof(so_err);
    getsockopt(s, SOL_SOCKET, SO_ERROR, &so_err, &elen);
    if (so_err != 0) { close(s); return 0; }      // refused → closed

    if (!want_banner) { close(s); return 1; }

    // Banner grab — wait briefly for the server to send. Some protocols
    // (HTTP, RDP, raw TCP) won't speak until prodded; nudge them with a
    // generic CRLFCRLF so HTTP/SMTP/IMAP/POP3/SSH-as-banner-greeting all
    // tend to produce at least one line.
    static const char nudge[] = "\r\n\r\n";
    send(s, nudge, sizeof(nudge) - 1, MSG_DONTWAIT);

    fd_set rfds; FD_ZERO(&rfds); FD_SET(s, &rfds);
    tv.tv_sec  = BANNER_READ_TIMEOUT_MS / 1000;
    tv.tv_usec = (BANNER_READ_TIMEOUT_MS % 1000) * 1000;
    rc = select(s + 1, &rfds, NULL, NULL, &tv);
    if (rc > 0) {
        char tmp[PORTSCAN_BANNER_MAX];
        int  n = recv(s, tmp, sizeof(tmp) - 1, MSG_DONTWAIT);
        if (n > 0) {
            tmp[n] = '\0';
            int copy = n < banner_max - 1 ? n : banner_max - 1;
            memcpy(banner, tmp, copy);
            banner[copy] = '\0';
        }
    }
    close(s);
    return 1;
}

// UDP probe. Sends a generic payload (or a tiny service-specific probe for
// the most useful well-known ports), waits briefly for any response. Returns
// 1 = response → likely open, 2 = silence → open|filtered.
static int udp_probe(uint16_t port)
{
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) return PSTATE_OPEN_OR_FILT;

    struct sockaddr_in sa;
    build_sockaddr(&sa, port);

    // Service-specific probes for the few common cases that won't reply to
    // garbage payloads — everything else gets a generic short payload.
    uint8_t buf[64];
    int     n;
    switch (port) {
    case 53: {
        // DNS query for "." A IN, transaction id 0x1234
        static const uint8_t q[] = {
            0x12, 0x34, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00,
            0x00,                            // root label
            0x00, 0x01,                      // QTYPE A
            0x00, 0x01,                      // QCLASS IN
        };
        memcpy(buf, q, sizeof(q));
        n = (int)sizeof(q);
        break;
    }
    case 123: {
        // NTP client request, version 4, mode 3, otherwise zero.
        memset(buf, 0, 48);
        buf[0] = 0x23;
        n = 48;
        break;
    }
    case 161: {
        // SNMPv1 GetRequest for sysDescr (1.3.6.1.2.1.1.1.0), community "public".
        static const uint8_t snmp[] = {
            0x30, 0x29, 0x02, 0x01, 0x00, 0x04, 0x06, 'p','u','b','l','i','c',
            0xa0, 0x1c, 0x02, 0x04, 0x71, 0x6e, 0x05, 0xa4,
            0x02, 0x01, 0x00, 0x02, 0x01, 0x00, 0x30, 0x0e,
            0x30, 0x0c, 0x06, 0x08, 0x2b, 0x06, 0x01, 0x02,
            0x01, 0x01, 0x01, 0x00, 0x05, 0x00,
        };
        memcpy(buf, snmp, sizeof(snmp));
        n = (int)sizeof(snmp);
        break;
    }
    case 5353: {
        // mDNS query for _services._dns-sd._udp.local.
        static const uint8_t mdns[] = {
            0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x09,'_','s','e','r','v','i','c','e','s',
            0x07,'_','d','n','s','-','s','d',
            0x04,'_','u','d','p',
            0x05,'l','o','c','a','l', 0x00,
            0x00, 0x0c, 0x00, 0x01,
        };
        memcpy(buf, mdns, sizeof(mdns));
        n = (int)sizeof(mdns);
        break;
    }
    default:
        // Generic short payload — many services respond with an error
        // packet on malformed input, and ICMP-port-unreachable would also
        // come back through the same socket on closed ports if the host
        // doesn't filter it.
        buf[0] = 0; buf[1] = 0; buf[2] = 0; buf[3] = 0;
        n = 4;
        break;
    }

    sendto(s, buf, n, 0, (struct sockaddr *)&sa, sizeof(sa));

    fd_set rfds; FD_ZERO(&rfds); FD_SET(s, &rfds);
    struct timeval tv = { UDP_RESPONSE_TIMEOUT_MS / 1000,
                          (UDP_RESPONSE_TIMEOUT_MS % 1000) * 1000 };
    int rc = select(s + 1, &rfds, NULL, NULL, &tv);
    int state = PSTATE_OPEN_OR_FILT;
    if (rc > 0) {
        char rb[128];
        struct sockaddr_in from; socklen_t fl = sizeof(from);
        int got = recvfrom(s, rb, sizeof(rb), MSG_DONTWAIT,
                           (struct sockaddr *)&from, &fl);
        if (got > 0) state = PSTATE_OPEN;
    }
    close(s);
    return state;
}

// ---- scan task -------------------------------------------------------------

static void scan_task(void *)
{
    s_scanned       = 0;
    s_result_count  = 0;
    s_done          = false;

    int count = 0;
    auto port_at = [&](int i) -> uint16_t {
        if (s_preset == PSPRE_TOP_20)  return TOP_20[i];
        if (s_preset == PSPRE_TOP_100) return TOP_100[i];
        // Custom range — i is 0-based offset from lo.
        return (uint16_t)(s_custom_lo + i);
    };

    if (s_preset == PSPRE_TOP_20)        count = TOP_20_COUNT;
    else if (s_preset == PSPRE_TOP_100)  count = TOP_100_COUNT;
    else                                 count = (int)s_custom_hi - (int)s_custom_lo + 1;

    s_total = count;

    // The scan can run any combination of the three techniques. Per port:
    //   - TCP_CONNECT and/or BANNER selected → one TCP probe; banner grab
    //     is enabled when BANNER is in the mask (the connect implies the
    //     same handshake either way, so running both as a single probe
    //     saves the duplicate connect attempt).
    //   - UDP selected → one UDP probe (independent of TCP).
    bool do_tcp     = (s_tech & (PSTECH_TCP_CONNECT | PSTECH_BANNER)) != 0;
    bool want_banner = (s_tech & PSTECH_BANNER) != 0;
    bool do_udp     = (s_tech & PSTECH_UDP) != 0;

    for (int i = 0; i < count && !s_abort; i++) {
        uint16_t port = port_at(i);

        if (do_tcp) {
            char banner[PORTSCAN_BANNER_MAX] = "";
            int open = tcp_probe(port, want_banner, banner, (int)sizeof(banner));
            if (open) record(port, PSTATE_OPEN, false, want_banner ? banner : NULL);
        }
        if (do_udp && !s_abort) {
            // Only record definitively-OPEN UDP ports (a response came
            // back). "open|filtered" results — where silence could mean
            // either an open-but-silent service or a firewalled port —
            // are not surfaced; they're noisy and not actionable on a
            // small watch display.
            int state = udp_probe(port);
            if (state == PSTATE_OPEN)
                record(port, PSTATE_OPEN, true, NULL);
        }

        // s_scanned counts ports covered (not probe-units). A combined
        // scan still reads "47 / 100" rather than "94 / 200" so the
        // progress meter stays intuitive.
        s_scanned = i + 1;
    }

    s_running       = false;
    s_done          = true;
    s_just_finished = true;
    s_logged        = false;
    s_task          = nullptr;
    vTaskDelete(NULL);
}

// ---- public API ------------------------------------------------------------

bool portscan_start(uint32_t ip_host_order, PortScanTech tech,
                    PortScanPreset preset, uint16_t lo, uint16_t hi)
{
    if (s_running) return false;
    if (WiFi.status() != WL_CONNECTED) return false;
    if (ip_host_order == 0) return false;
    // Empty technique mask is meaningless — at least one of TCP / UDP /
    // BANNER must be set. The UI enforces this by refusing to deselect
    // the last button; this guard catches programmatic mis-calls too.
    if ((tech & (PSTECH_TCP_CONNECT | PSTECH_UDP | PSTECH_BANNER)) == 0)
        return false;
    if (!ensure_result_storage()) return false;

    if (preset == PSPRE_CUSTOM) {
        if (lo == 0) lo = 1;
        if (hi == 0) hi = lo;
        if (lo > hi) { uint16_t t = lo; lo = hi; hi = t; }
        // Cap absurd ranges so the scan stays interactive — at 220 ms per
        // port the full 65 535 sweep would take 4 hours and overflow the
        // result buffer. 1024 ports is enough for any reasonable lookup
        // and finishes in well under five minutes.
        if (hi - lo + 1 > 1024) hi = lo + 1024 - 1;
    }

    s_target_ip = ip_host_order;
    s_tech      = tech;
    s_preset    = preset;
    s_custom_lo = lo;
    s_custom_hi = hi;
    s_abort     = false;
    s_running   = true;

    // Stack 6 KB — BSD sockets + select() + small banner buffer fit
    // comfortably; bumping it up means we don't have to think about it
    // again the next time someone adds a service-specific probe.
    BaseType_t ok = xTaskCreatePinnedToCore(
        scan_task, "portscan", 6144, NULL, 1, &s_task, 1);
    if (ok != pdPASS) {
        s_running = false;
        return false;
    }
    return true;
}

void portscan_stop()
{
    if (!s_running) return;
    s_abort = true;
    // Task exits at its next iteration and self-deletes.
}

bool portscan_is_running()      { return s_running;        }
int  portscan_scanned()         { return s_scanned;        }
int  portscan_total()           { return s_total;          }
int  portscan_result_count()    { return s_result_count;   }
uint32_t       portscan_target_ip()  { return s_target_ip; }
PortScanTech   portscan_tech()       { return s_tech;      }
PortScanPreset portscan_preset()     { return s_preset;    }
uint16_t       portscan_custom_lo()  { return s_custom_lo; }
uint16_t       portscan_custom_hi()  { return s_custom_hi; }

const PortScanResult *portscan_result(int idx)
{
    if (!s_results || idx < 0 || idx >= s_result_count) return nullptr;
    return &s_results[idx];
}

void portscan_clear_results()
{
    s_result_count  = 0;
    s_scanned       = 0;
    s_total         = 0;
    s_done          = false;
    s_just_finished = false;
    s_logged        = false;
}

bool portscan_just_finished()
{
    bool v = s_just_finished;
    s_just_finished = false;
    return v;
}

void portscan_poll()
{
    if (s_running || !s_done || s_logged) return;
    s_logged = true;
    if (!s_results) return;

    if (usb_sd_is_running() || !instance.isCardReady()) return;
    if (!SD.exists("/PingSweeps")) SD.mkdir("/PingSweeps");

    struct tm t;
    clock_screen_get_local_time(&t);

    char path[80];
    snprintf(path, sizeof(path),
             "/PingSweeps/portscan_%u.%u.%u.%u_%04d%02d%02d_%02d%02d%02d.txt",
             (s_target_ip >> 24) & 0xFF, (s_target_ip >> 16) & 0xFF,
             (s_target_ip >>  8) & 0xFF,  s_target_ip        & 0xFF,
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec);

    File f = SD.open(path, FILE_WRITE);
    if (!f) return;

    // Build a "TCP+UDP+Banner"-style technique string from the bitmask.
    char tech_name[40] = "";
    if (s_tech & PSTECH_TCP_CONNECT) strcat(tech_name, "TCP");
    if (s_tech & PSTECH_UDP)         { if (tech_name[0]) strcat(tech_name, "+"); strcat(tech_name, "UDP"); }
    if (s_tech & PSTECH_BANNER)      { if (tech_name[0]) strcat(tech_name, "+"); strcat(tech_name, "Banner"); }

    f.printf("# Port scan %s\n", path);
    f.printf("# Target %u.%u.%u.%u  Techniques %s  Scanned %d ports\n",
        (s_target_ip >> 24) & 0xFF, (s_target_ip >> 16) & 0xFF,
        (s_target_ip >>  8) & 0xFF,  s_target_ip        & 0xFF,
        tech_name, s_total);

    for (int i = 0; i < s_result_count; i++) {
        const PortScanResult &r = s_results[i];
        const char *state =
            (r.state == PSTATE_OPEN_OR_FILT) ? "open|filt" : "open";
        f.printf("%u/%s\t%s",
                 r.port, r.is_udp ? "udp" : "tcp", state);
        if (r.banner[0]) f.printf("\t%s", r.banner);
        f.print("\n");
    }
    f.close();
}
