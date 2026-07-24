#include "security_store.h"
#include <Preferences.h>
#include <Arduino.h>
#include <string.h>
#include "esp_random.h"
#include "mbedtls/md.h"
#include "mbedtls/pkcs5.h"

static const char *NS    = "argussec";
static const int   ITERS        = 4096;    // new-hash count (~0.5s); UI no longer 6s-frozen
static const int   ITERS_LEGACY = 50000;   // pre-migration count (stored per slot as iu/is)
static const int   HLEN  = 32;
static const int   SLEN  = 16;

// Rate-limit state (RAM). A power-cycle resets it, which is acceptable: PBKDF2 at
// 50k iters makes each guess cost tens of ms, and the un-encrypted flash dump is
// the bigger weakness anyway, so this is defence-in-depth.
static int      s_fails      = 0;
static uint32_t s_lock_until = 0;

static bool load_salt(uint8_t salt[SLEN])
{
    Preferences p;
    if (!p.begin(NS, true)) return false;
    size_t n = p.getBytes("salt", salt, SLEN);
    p.end();
    return n == (size_t)SLEN;
}

static void ensure_salt(uint8_t salt[SLEN])
{
    if (load_salt(salt)) return;
    for (int i = 0; i < SLEN; i++) salt[i] = (uint8_t)esp_random();
    Preferences p;
    if (p.begin(NS, false)) { p.putBytes("salt", salt, SLEN); p.end(); }
}

static void pbkdf2(const char *pin, const uint8_t *salt, uint8_t out[HLEN], int iters)
{
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, info, 1);   // 1 = HMAC
    mbedtls_pkcs5_pbkdf2_hmac(&ctx, (const unsigned char *)pin, strlen(pin),
                              salt, SLEN, iters, HLEN, out);
    mbedtls_md_free(&ctx);
}

static bool ct_eq(const uint8_t *a, const uint8_t *b, int n)   // constant-time
{
    uint8_t d = 0;
    for (int i = 0; i < n; i++) d |= (uint8_t)(a[i] ^ b[i]);
    return d == 0;
}

static const char *check_digits(const char *s)
{
    size_t n = strlen(s);
    if (n < 4 || n > 8) return "PIN must be 4-8 digits";
    for (size_t i = 0; i < n; i++)
        if (s[i] < '0' || s[i] > '9') return "digits only";
    return nullptr;
}

bool security_pins_set(void)
{
    Preferences p;
    if (!p.begin(NS, true)) return false;
    bool set = p.getBool("set", false);
    p.end();
    return set;
}

const char *security_set_pins(const char *unlock_pin, const char *shred_pin)
{
    const char *e;
    if ((e = check_digits(unlock_pin))) return e;
    if ((e = check_digits(shred_pin)))  return e;
    size_t un = strlen(unlock_pin);
    if (strlen(shred_pin) < un + 1)          return "Shred PIN must be 1+ digit longer";
    if (strcmp(unlock_pin, shred_pin) == 0)  return "PINs must differ";
    if (strncmp(unlock_pin, shred_pin, un) == 0) return "Shred can't start with unlock PIN";

    uint8_t salt[SLEN]; ensure_salt(salt);
    uint8_t hu[HLEN], hs[HLEN];
    pbkdf2(unlock_pin, salt, hu, ITERS);
    pbkdf2(shred_pin,  salt, hs, ITERS);

    Preferences p;
    if (!p.begin(NS, false)) return "storage error";
    p.putBytes("hu", hu, HLEN);
    p.putBytes("hs", hs, HLEN);
    p.putInt("iu", ITERS);   // iteration count per slot (for transparent migration)
    p.putInt("is", ITERS);
    p.putBool("set", true);
    p.end();
    return nullptr;
}

uint32_t security_lockout_ms(void)
{
    uint32_t now = millis();
    return (now < s_lock_until) ? (s_lock_until - now) : 0;
}

PinResult security_check(const char *pin)
{
    if (security_lockout_ms() > 0) return PinResult::None;
    if (!security_pins_set())      return PinResult::None;

    uint8_t salt[SLEN];
    if (!load_salt(salt)) return PinResult::None;

    uint8_t hu[HLEN], hs[HLEN];
    int iu, is;
    {
        Preferences p;
        if (!p.begin(NS, true)) return PinResult::None;
        p.getBytes("hu", hu, HLEN);
        p.getBytes("hs", hs, HLEN);
        iu = p.getInt("iu", ITERS_LEGACY);   // legacy installs stored no count -> 50k
        is = p.getInt("is", ITERS_LEGACY);
        p.end();
    }

    // Verify each slot at its stored iteration count. On a match at a legacy (slow)
    // count, transparently re-hash at the fast ITERS and store it, so only the
    // FIRST correct entry after an upgrade pays the old ~6s cost; wrong guesses
    // keep paying it (anti-brute-force), and the correct PIN is snappy thereafter.
    uint8_t h[HLEN]; pbkdf2(pin, salt, h, iu);
    if (ct_eq(h, hu, HLEN)) {
        if (iu != ITERS) {
            pbkdf2(pin, salt, hu, ITERS);
            Preferences p;
            if (p.begin(NS, false)) { p.putBytes("hu", hu, HLEN); p.putInt("iu", ITERS); p.end(); }
        }
        s_fails = 0; return PinResult::Unlock;
    }
    uint8_t h2[HLEN];
    if (is == iu) memcpy(h2, h, HLEN);
    else          pbkdf2(pin, salt, h2, is);
    if (ct_eq(h2, hs, HLEN)) {
        if (is != ITERS) {
            pbkdf2(pin, salt, hs, ITERS);
            Preferences p;
            if (p.begin(NS, false)) { p.putBytes("hs", hs, HLEN); p.putInt("is", ITERS); p.end(); }
        }
        s_fails = 0; return PinResult::Shred;
    }

    s_fails++;                              // wrong: escalating backoff
    uint32_t delay = 0;
    if      (s_fails >= 8) delay = 300000;  // 5 min
    else if (s_fails >= 5) delay = 30000;   // 30 s
    else if (s_fails >= 3) delay = 5000;    // 5 s
    if (delay) s_lock_until = millis() + delay;
    return PinResult::None;
}
