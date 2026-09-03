#include "gps_gsv.h"

#include <string.h>

// ---- checksum ---------------------------------------------------------------
// NMEA checksum is the XOR of every character between '$' and '*'. We are
// handed the body with '$' already stripped, so hash up to the '*' and compare
// against the two hex digits that follow it.
//
// This is validated independently of TinyGPSPlus rather than trusted from it:
// this parser sees the same byte stream but keeps its own state, and a sentence
// mangled by a UART overrun must not be allowed to invent a satellite.

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

bool gps_gsv_checksum_ok(const char *s, size_t len)
{
    if (!s || len < 4) return false;   // need at least one char plus "*HH"

    size_t star = len;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '*') { star = i; break; }
    }
    // The '*' must leave exactly two hex digits behind it, and must not be the
    // very first character (an empty body has no checksum to speak of).
    if (star == len || star == 0 || len - star != 3) return false;

    const int hi = hexval(s[star + 1]);
    const int lo = hexval(s[star + 2]);
    if (hi < 0 || lo < 0) return false;

    uint8_t sum = 0;
    for (size_t i = 0; i < star; i++) sum ^= (uint8_t)s[i];

    return sum == (uint8_t)((hi << 4) | lo);
}

// ---- assembler --------------------------------------------------------------

void GpsGsv::reset()
{
    _count       = 0;
    _overflow    = 0;
    _len         = 0;
    _in_sentence = false;
    memset(_sat, 0, sizeof(_sat));
}

void GpsGsv::feed(char c, uint32_t now_ms)
{
    if (c == '$') {
        // Always resync on '$', even mid-sentence: a truncated sentence is
        // worth abandoning, and this is the one character that cannot legally
        // appear inside one.
        _len         = 0;
        _in_sentence = true;
        return;
    }

    if (!_in_sentence) return;    // pre-roll garbage, or we overran and gave up

    if (c == '\r' || c == '\n') {
        if (_len) process_sentence(now_ms);
        _len         = 0;
        _in_sentence = false;
        return;
    }

    if (_len >= kGpsGsvBufBytes) {
        // Longer than any legal sentence. Drop it and wait for the next '$'
        // rather than truncating, which would corrupt the final field.
        _len         = 0;
        _in_sentence = false;
        return;
    }

    _buf[_len++] = c;
}

// ---- GSV ------------------------------------------------------------------
// $xxGSV,numMsg,msgNum,numSV,svid,elev,azim,cno,[svid,elev,azim,cno]...*HH
// and on NMEA 4.11 a trailing signalId field before the checksum.
//
// Deliberately IGNORES numMsg/msgNum/numSV. Those would require knowing how
// many talkers and how many signals per talker this receiver emits, and getting
// that wrong silently under- or over-counts. Walking the satellite quads and
// keying on (talker, svid) needs no such assumption: repeats collapse, and a
// satellite reported on two signals is still one satellite.
void GpsGsv::process_sentence(uint32_t now_ms)
{
    // "GPGSV,..." - talker is the first two chars, type the next three.
    if (_len < 6) return;
    if (_buf[2] != 'G' || _buf[3] != 'S' || _buf[4] != 'V') return;
    if (_buf[5] != ',') return;

    if (!gps_gsv_checksum_ok(_buf, _len)) return;

    const uint16_t talker = (uint16_t)(((uint8_t)_buf[0] << 8) | (uint8_t)_buf[1]);

    // Walk comma-separated fields, counting from 0 at the sentence id. The
    // satellite quads start at field 4.
    size_t i     = 0;
    int    field = 0;
    // Values of the quad currently being assembled.
    long   quad[4] = { -1, -1, -1, -1 };

    while (i <= _len) {
        // Find this field's bounds.
        const size_t start = i;
        while (i < _len && _buf[i] != ',' && _buf[i] != '*') i++;
        const size_t end = i;

        if (field >= 4) {
            const int slot = (field - 4) % 4;
            // Empty field stays -1: GSV legitimately reports a satellite in
            // view with a BLANK C/N0 when it is not being tracked, and that is
            // exactly the "visible but unusable" case worth keeping.
            if (end > start) {
                long v = 0;
                bool ok = true;
                for (size_t k = start; k < end; k++) {
                    if (_buf[k] < '0' || _buf[k] > '9') { ok = false; break; }
                    v = v * 10 + (_buf[k] - '0');
                    if (v > 99999) { ok = false; break; }
                }
                quad[slot] = ok ? v : -1;
            } else {
                quad[slot] = -1;
            }

            if (slot == 3) {
                // Quad complete. A satellite with no id is padding, not a
                // satellite; a blank C/N0 counts as in view at 0 dB-Hz.
                if (quad[0] >= 0)
                    record(talker, (uint16_t)quad[0],
                           quad[3] > 0 ? (uint8_t)(quad[3] > 255 ? 255 : quad[3]) : 0,
                           now_ms);
                quad[0] = quad[1] = quad[2] = quad[3] = -1;
            }
        }

        if (i >= _len || _buf[i] == '*') break;
        i++;        // step over the comma
        field++;
    }
}

// Insert or refresh one satellite. The LATEST C/N0 wins rather than the best
// one: the question being asked is what the signal is now, not what it once
// was, and a decaying signal is the thing worth seeing.
void GpsGsv::record(uint16_t talker, uint16_t svid, uint8_t cno, uint32_t now_ms)
{
    for (uint8_t i = 0; i < _count; i++) {
        if (_sat[i].talker == talker && _sat[i].svid == svid) {
            _sat[i].cno     = cno;
            _sat[i].seen_ms = now_ms;
            return;
        }
    }

    if (_count < kGpsGsvMaxSats) {
        _sat[_count].talker  = talker;
        _sat[_count].svid    = svid;
        _sat[_count].cno     = cno;
        _sat[_count].seen_ms = now_ms;
        _count++;
        return;
    }

    // Table full. Reclaim an expired slot before giving up, so a long run whose
    // sky has rotated does not wedge at capacity holding satellites that set
    // hours ago.
    for (uint8_t i = 0; i < _count; i++) {
        if (!live(_sat[i], now_ms)) {
            _sat[i].talker  = talker;
            _sat[i].svid    = svid;
            _sat[i].cno     = cno;
            _sat[i].seen_ms = now_ms;
            return;
        }
    }

    if (_overflow < 0xFFFFu) _overflow++;
}

bool GpsGsv::live(const GpsGsvSat &s, uint32_t now_ms) const
{
    // Unsigned wrap is intentional and correct across the millis() rollover:
    // the difference stays small and positive as long as the entry is recent.
    //
    // Deliberately does NOT also test seen_ms != 0. Occupancy is carried by
    // _count alone, and every loop here is bounded by it, so a zero timestamp
    // cannot mean "empty slot" - which matters because millis() legitimately
    // IS 0 for the first millisecond of a boot. Two states that must never be
    // confused must never share a representation.
    return (uint32_t)(now_ms - s.seen_ms) < kGpsGsvTtlMs;
}

uint8_t GpsGsv::in_view(uint32_t now_ms) const
{
    uint8_t n = 0;
    for (uint8_t i = 0; i < _count; i++)
        if (live(_sat[i], now_ms)) n++;
    return n;
}

uint8_t GpsGsv::cno_max(uint32_t now_ms) const
{
    uint8_t best = 0;
    for (uint8_t i = 0; i < _count; i++)
        if (live(_sat[i], now_ms) && _sat[i].cno > best) best = _sat[i].cno;
    return best;
}

uint8_t GpsGsv::cno_at_least(uint32_t now_ms, uint8_t db) const
{
    uint8_t n = 0;
    for (uint8_t i = 0; i < _count; i++)
        if (live(_sat[i], now_ms) && _sat[i].cno >= db) n++;
    return n;
}
