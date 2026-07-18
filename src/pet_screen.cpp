#include "pet_screen.h"
#include "pwnagotchi_peer.h"
// NOTE: threat_radar.h intentionally not included yet — Phase 3a ships the
// pwnpet/handshake cluster only. The pet's threat-aware "wary" mood is re-wired
// to threatradar_top_level() when Phase 3b lands the Threat Radar bundle.
#include "tools_screen.h"
#include "wifi_beacon_manager.h"
#include "handshake.h"
#include <lvgl.h>
#include <LilyGoLib.h>
#include <SD.h>
#include <Arduino.h>
#include <math.h>
#include <stdio.h>

// The pwnpet is a goldfish swimming the airwaves. Mood drives its body colour
// and what it blurts out; XP grows with friends met (other Pwnagotchis) and is
// persisted. Speech is ASCII only (the on-device font has no emoji glyphs).
enum { PET_BORED = 0, PET_HAPPY, PET_EXCITED, PET_WARY };
static const char *kSpeech[4] = {
    "blub... blub...",
    "swimming the airwaves ~",
    "",                         // EXCITED filled with the peer name at refresh
    "danger in the water!"
};

static lv_obj_t  *s_screen   = nullptr;
static lv_obj_t  *s_fish     = nullptr;   // container moved by the swim animation
static lv_obj_t  *s_body     = nullptr;   // recoloured by mood
static lv_obj_t  *s_speech   = nullptr;
static lv_obj_t  *s_lvl      = nullptr;
static lv_obj_t  *s_bar_fill = nullptr;
static lv_obj_t  *s_stats    = nullptr;
static lv_obj_t  *s_bubble[3] = { nullptr, nullptr, nullptr };
static lv_timer_t *s_timer   = nullptr;   // 1 Hz mood/XP
static lv_timer_t *s_anim    = nullptr;   // ~12 Hz swim + bubbles
static bool       s_active   = false;

static float    s_bx[3] = { 40, 50, 46 };
static float    s_by[3] = { -55, -92, -130 };
static uint32_t s_phase = 0;

static long     s_xp          = 0;
static bool     s_xp_loaded   = false;
static uint8_t  s_mood        = PET_BORED;
static uint32_t s_mood_until  = 0;
static int      s_last_peers  = 0;
static int      s_last_pwnd   = 0;
static bool     s_eat         = false;   // EXCITED is an "ate a handshake" moment
static bool     s_dirty       = false;
static uint32_t s_last_save   = 0;

static int level_of(long xp) { return 1 + (int)(xp / 100); }
static int xp_into(long xp)  { return (int)(xp % 100); }

static void pet_wifi_noop(const WifiBeacon *b) { (void)b; }

static void load_xp()
{
    s_xp_loaded = true;
    if (!instance.isCardReady()) return;
    File f = SD.open("/pwn/pet.txt", FILE_READ);
    if (!f) return;
    char buf[24] = {0};
    int n = f.readBytes(buf, sizeof(buf) - 1);
    f.close();
    if (n > 0) { buf[n] = '\0'; s_xp = atol(buf); if (s_xp < 0) s_xp = 0; }
}

static void save_xp()
{
    if (!instance.isCardReady()) return;
    if (!SD.exists("/pwn")) SD.mkdir("/pwn");
    File f = SD.open("/pwn/pet.txt", FILE_WRITE);
    if (!f) return;
    f.printf("%ld", s_xp);
    f.close();
    s_dirty = false;
    s_last_save = millis();
}

static void refresh()
{
    if (s_mood == PET_EXCITED) {
        if (s_eat) lv_label_set_text(s_speech, "nom! caught a handshake!");
        else       lv_label_set_text_fmt(s_speech, "hi %s! blub!", pwnagotchi_last_name());
    } else {
        lv_label_set_text(s_speech, kSpeech[s_mood]);
    }

    lv_color_t body =
        (s_mood == PET_WARY)    ? lv_color_make(0xFF, 0x4A, 0x2A) :   // alarmed red
        (s_mood == PET_EXCITED) ? lv_color_make(0xFF, 0xB0, 0x30) :   // bright
                                  lv_color_make(0xFF, 0x8A, 0x1A);    // goldfish
    lv_obj_set_style_bg_color(s_body, body, LV_PART_MAIN);

    lv_label_set_text_fmt(s_lvl, "LVL %d", level_of(s_xp));
    lv_obj_set_width(s_bar_fill, 2 + (xp_into(s_xp) * 296) / 100);

    int peers = pwnagotchi_peer_count();
    lv_label_set_text_fmt(s_stats, "PWND %d   friends %d   xp %ld",
        handshake_pwnd_count(), peers, s_xp);
}

static void on_tick(lv_timer_t *)
{
    if (!s_active) return;
    uint32_t now = millis();

    int peers  = pwnagotchi_peer_count();
    int pwnd   = handshake_pwnd_count();

    uint8_t newmood;
    if (pwnd > s_last_pwnd) {                          // ate a handshake
        s_xp += 30L * (pwnd - s_last_pwnd);
        s_last_pwnd = pwnd;
        newmood = PET_EXCITED; s_eat = true;  s_mood_until = now + 5000; s_dirty = true;
    } else if (peers > s_last_peers) {                 // met a Pwnagotchi
        s_xp += 50L * (peers - s_last_peers);
        s_last_peers = peers;
        newmood = PET_EXCITED; s_eat = false; s_mood_until = now + 6000; s_dirty = true;
    } else if (now < s_mood_until) {
        newmood = s_mood;
    } else {
        newmood = peers > 0 ? PET_HAPPY : PET_BORED;
    }
    if (newmood == PET_WARY && s_mood != PET_WARY) { s_xp += 10; s_dirty = true; }
    s_mood = newmood;

    if (s_dirty && now - s_last_save > 15000) save_xp();
    refresh();
}

// Swim bob + rising bubbles. Position only — cheap enough at ~12 Hz.
static void on_anim(lv_timer_t *)
{
    if (!s_active) return;
    s_phase++;
    int dy = (int)(7.0f * sinf(s_phase * 0.18f));
    int dx = (int)(5.0f * sinf(s_phase * 0.09f));
    lv_obj_align(s_fish, LV_ALIGN_CENTER, dx, -40 + dy);

    for (int i = 0; i < 3; i++) {
        s_by[i] -= 1.4f;
        if (s_by[i] < -160) s_by[i] = -40;
        lv_obj_align(s_bubble[i], LV_ALIGN_CENTER, (int)s_bx[i], (int)s_by[i]);
    }
}

static void on_gesture(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    if (lv_indev_get_gesture_dir(indev) == LV_DIR_TOP) {
        s_active = false;
        if (s_dirty) save_xp();
        wifi_beacon_remove(pet_wifi_noop);
        tools_screen_show();
    }
}

// --- little helpers for the fish parts -------------------------------------
static lv_obj_t *disc(lv_obj_t *parent, int d, lv_color_t c)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_size(o, d, d);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(o, c, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(o, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(o, 0, LV_PART_MAIN);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

// A square rotated 45° — used as a triangle-ish fin/tail.
static lv_obj_t *diamond(lv_obj_t *parent, int s, lv_color_t c)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_size(o, s, s);
    lv_obj_set_style_radius(o, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(o, c, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(o, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(o, 0, LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_x(o, s / 2, LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_y(o, s / 2, LV_PART_MAIN);
    lv_obj_set_style_transform_rotation(o, 450, LV_PART_MAIN);   // 45.0 deg
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

void pet_screen_create()
{
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_make(0x04, 0x10, 0x1c), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_screen, on_gesture, LV_EVENT_GESTURE, NULL);

    // Water tint over the lower half (the "bowl").
    lv_obj_t *water = lv_obj_create(s_screen);
    lv_obj_set_size(water, 410, 252);
    lv_obj_set_style_radius(water, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(water, lv_color_make(0x07, 0x24, 0x38), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(water, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_border_width(water, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(water, 0, LV_PART_MAIN);
    lv_obj_clear_flag(water, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(water, LV_ALIGN_BOTTOM_MID, 0, 0);

    lv_obj_t *name = lv_label_create(s_screen);
    lv_obj_set_style_text_font(name, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(name, lv_color_make(0x33, 0xBB, 0xFF), LV_PART_MAIN);
    lv_label_set_text(name, "1337  the pwnfish");
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 30);

    // Bubbles (created before the fish so the fish floats over them).
    for (int i = 0; i < 3; i++) {
        s_bubble[i] = disc(s_screen, 10 - i * 2, lv_color_make(0xAE, 0xE0, 0xFF));
        lv_obj_set_style_bg_opa(s_bubble[i], LV_OPA_70, LV_PART_MAIN);
        lv_obj_align(s_bubble[i], LV_ALIGN_CENTER, (int)s_bx[i], (int)s_by[i]);
    }

    // The fish — a container so the whole creature can swim as one.
    lv_color_t tailc = lv_color_make(0xE0, 0x6A, 0x10);
    s_fish = lv_obj_create(s_screen);
    lv_obj_set_size(s_fish, 210, 120);
    lv_obj_set_style_bg_opa(s_fish, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_fish, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_fish, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_fish, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_fish, LV_ALIGN_CENTER, 0, -40);

    lv_obj_t *tail = diamond(s_fish, 50, tailc);
    lv_obj_align(tail, LV_ALIGN_CENTER, -66, 0);

    lv_obj_t *fin = diamond(s_fish, 30, tailc);
    lv_obj_align(fin, LV_ALIGN_CENTER, -8, -34);

    // Body — a horizontal ellipse (rounded rect with radius = half-height).
    s_body = lv_obj_create(s_fish);
    lv_obj_set_size(s_body, 124, 70);
    lv_obj_set_style_radius(s_body, 35, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_body, lv_color_make(0xFF, 0x8A, 0x1A), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_body, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_body, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_body, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_body, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *belly = lv_obj_create(s_fish);
    lv_obj_set_size(belly, 80, 28);
    lv_obj_set_style_radius(belly, 14, LV_PART_MAIN);
    lv_obj_set_style_bg_color(belly, lv_color_make(0xFF, 0xB8, 0x55), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(belly, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_border_width(belly, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(belly, 0, LV_PART_MAIN);
    lv_obj_clear_flag(belly, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(belly, LV_ALIGN_CENTER, -6, 12);

    lv_obj_t *eye = disc(s_fish, 20, lv_color_white());
    lv_obj_align(eye, LV_ALIGN_CENTER, 36, -8);
    lv_obj_t *pupil = disc(s_fish, 9, lv_color_make(0x10, 0x10, 0x10));
    lv_obj_align(pupil, LV_ALIGN_CENTER, 39, -8);

    lv_obj_t *mouth = lv_obj_create(s_fish);
    lv_obj_set_size(mouth, 9, 5);
    lv_obj_set_style_radius(mouth, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(mouth, lv_color_make(0xC2, 0x5A, 0x0C), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mouth, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(mouth, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(mouth, 0, LV_PART_MAIN);
    lv_obj_clear_flag(mouth, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(mouth, LV_ALIGN_CENTER, 62, 8);

    s_speech = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_speech, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_speech, lv_color_make(0x7B, 0xDC, 0xFF), LV_PART_MAIN);
    lv_label_set_text(s_speech, kSpeech[PET_BORED]);
    lv_obj_align(s_speech, LV_ALIGN_CENTER, 0, 40);

    s_lvl = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_lvl, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_lvl, lv_color_make(0xFF, 0x8C, 0x1A), LV_PART_MAIN);
    lv_label_set_text(s_lvl, "LVL 1");
    lv_obj_align(s_lvl, LV_ALIGN_CENTER, -150, 90);

    lv_obj_t *bar_bg = lv_obj_create(s_screen);
    lv_obj_set_size(bar_bg, 300, 16);
    lv_obj_set_style_radius(bar_bg, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar_bg, lv_color_make(0x0E, 0x1C, 0x28), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar_bg, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(bar_bg, lv_color_make(0x1C, 0x38, 0x50), LV_PART_MAIN);
    lv_obj_set_style_border_width(bar_bg, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bar_bg, 0, LV_PART_MAIN);
    lv_obj_clear_flag(bar_bg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(bar_bg, LV_ALIGN_CENTER, 0, 120);

    s_bar_fill = lv_obj_create(bar_bg);
    lv_obj_set_size(s_bar_fill, 2, 14);
    lv_obj_set_style_radius(s_bar_fill, 7, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar_fill, lv_color_make(0xFF, 0x8C, 0x1A), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_bar_fill, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_bar_fill, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_bar_fill, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_bar_fill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_bar_fill, LV_ALIGN_LEFT_MID, 0, 0);

    s_stats = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_stats, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_stats, lv_color_make(0x8A, 0xA8, 0xBA), LV_PART_MAIN);
    lv_label_set_text(s_stats, "friends 0   xp 0   up 0m");
    lv_obj_align(s_stats, LV_ALIGN_CENTER, 0, 160);

    s_timer = lv_timer_create(on_tick, 1000, NULL);
    s_anim  = lv_timer_create(on_anim, 80,   NULL);
}

void pet_screen_show()
{
    if (!s_screen) pet_screen_create();
    if (!s_xp_loaded) load_xp();
    s_last_peers = pwnagotchi_peer_count();
    s_last_pwnd  = handshake_pwnd_count();
    s_active = true;
    wifi_beacon_add(pet_wifi_noop);   // power the scanner so we meet peers live
    refresh();
    lv_scr_load(s_screen);
}

bool pet_screen_is_active() { return s_active; }
