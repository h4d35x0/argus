#include "pet_screen.h"
#include "hexhound.h"
#include "theme.h"
// NOTE: threat_radar.h intentionally NOT included. Threat awareness reaches the
// pet only through hexhound_set_threat_level() (see hexhound.h) so this cluster
// stays decoupled from the team-owned Threat Radar bundle. The integrator wires
// a confirmed tail -> hexhound_set_threat_level(1) to flip the pet HADES-red.
#include "tools_screen.h"
#include "wifi_beacon_manager.h"
#include <lvgl.h>
#include <LilyGoLib.h>
#include <Arduino.h>
#include <math.h>

// ── HexHound — LVGL renderer ─────────────────────────────────────
//
// The HexHound is drawn from LVGL primitives (no image assets): an angular
// "cyber-hound" head — hexagon-ish skull, pointed ears, a snout, and two eyes —
// sitting inside a set of sonar rings that pulse as it sweeps the airwaves. Mood
// recolours the head and eyes (calm steel-blue / bright when excited / HADES-red
// when a threat is confirmed). Everything below reads engine state through the
// hexhound_*() accessors; no game logic lives here.

static lv_obj_t  *s_screen    = nullptr;
static lv_obj_t  *s_dog       = nullptr;   // container bobbed by the idle anim
static lv_obj_t  *s_skull     = nullptr;   // recoloured by mood
static lv_obj_t  *s_snout     = nullptr;
static lv_obj_t  *s_eye[2]    = { nullptr, nullptr };
static lv_obj_t  *s_ear[2]    = { nullptr, nullptr };
static lv_obj_t  *s_ring[3]   = { nullptr, nullptr, nullptr };  // sonar sweep
static lv_obj_t  *s_stage_lbl = nullptr;
static lv_obj_t  *s_ability   = nullptr;
static lv_obj_t  *s_speech    = nullptr;
static lv_obj_t  *s_lvl       = nullptr;
static lv_obj_t  *s_bar_fill  = nullptr;
static lv_obj_t  *s_stats     = nullptr;
static lv_obj_t  *s_banner    = nullptr;    // transient "EVOLVED" flash

static lv_timer_t *s_timer    = nullptr;    // 1 Hz engine tick + refresh
static lv_timer_t *s_anim     = nullptr;    // ~12 Hz idle bob + ring pulse
static bool        s_active   = false;
static uint32_t    s_phase    = 0;
static uint32_t    s_banner_until = 0;

// Mood -> accent colour for the head/eyes.
static lv_color_t mood_color(uint8_t mood)
{
    switch (mood) {
        case HEX_WARY:    return HADES_RED;               // threat: red eyes
        case HEX_EXCITED: return ARGUS_ACCENT_ACTIVE;     // bright steel
        case HEX_HUNGRY:  return lv_color_make(0xC8, 0x9B, 0x5A); // amber-ish
        case HEX_SLEEPY:  return ARGUS_ACCENT_DIM;
        default:          return ARGUS_ACCENT;            // calm steel-blue
    }
}

static void pet_wifi_cb(const WifiBeacon *b)
{
    if (b) hexhound_note_wifi(b->bssid);   // feed distinct-AP XP
}

static void refresh()
{
    const HexHoundState &st = hexhound_state();
    lv_color_t accent = mood_color(st.mood);

    // Head + facial features track mood.
    lv_obj_set_style_bg_color(s_skull, accent, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_snout, accent, LV_PART_MAIN);
    for (int i = 0; i < 2; i++) {
        lv_obj_set_style_bg_color(s_ear[i], accent, LV_PART_MAIN);
        // Eyes glow the alert colour on threat, else a warm off-white.
        lv_obj_set_style_bg_color(s_eye[i],
            st.mood == HEX_WARY ? HADES_RED : lv_color_make(0xF0, 0xF4, 0xF8),
            LV_PART_MAIN);
    }
    for (int i = 0; i < 3; i++)
        lv_obj_set_style_border_color(s_ring[i], accent, LV_PART_MAIN);

    // Egg hides the face; the hound only shows once hatched.
    bool egg = (st.stage == HEX_EGG);
    for (int i = 0; i < 2; i++) {
        egg ? lv_obj_add_flag(s_eye[i], LV_OBJ_FLAG_HIDDEN)
            : lv_obj_clear_flag(s_eye[i], LV_OBJ_FLAG_HIDDEN);
        egg ? lv_obj_add_flag(s_ear[i], LV_OBJ_FLAG_HIDDEN)
            : lv_obj_clear_flag(s_ear[i], LV_OBJ_FLAG_HIDDEN);
    }
    egg ? lv_obj_add_flag(s_snout, LV_OBJ_FLAG_HIDDEN)
        : lv_obj_clear_flag(s_snout, LV_OBJ_FLAG_HIDDEN);

    lv_label_set_text(s_stage_lbl, hexhound_stage_name());
    lv_obj_set_style_text_color(s_stage_lbl, accent, LV_PART_MAIN);
    lv_label_set_text(s_ability, hexhound_ability_name());
    lv_label_set_text(s_speech, hexhound_mood_speech());

    lv_label_set_text_fmt(s_lvl, "LVL %d", hexhound_level());

    int span = hexhound_xp_stage_span();
    int into = hexhound_xp_into_stage();
    int pct  = span > 0 ? (into * 100) / span : 100;
    if (pct > 100) pct = 100;
    lv_obj_set_width(s_bar_fill, 2 + (pct * 296) / 100);
    lv_obj_set_style_bg_color(s_bar_fill, accent, LV_PART_MAIN);

    lv_label_set_text_fmt(s_stats,
        "HUN %d  ENR %d  BND %d   PWND %d  peers %d  xp %ld",
        st.hunger, st.energy, st.bond, st.pwnd, st.peers, st.xp);

    // Evolution banner (one-shot, 4 s).
    if (hexhound_take_evolved_flag()) {
        lv_label_set_text_fmt(s_banner, "EVOLVED  ->  %s", hexhound_stage_name());
        lv_obj_clear_flag(s_banner, LV_OBJ_FLAG_HIDDEN);
        s_banner_until = millis() + 4000;
    } else if (s_banner_until && millis() > s_banner_until) {
        lv_obj_add_flag(s_banner, LV_OBJ_FLAG_HIDDEN);
        s_banner_until = 0;
    }
}

static void on_tick(lv_timer_t *)
{
    if (!s_active) return;
    hexhound_update();
    refresh();
}

// Idle bob + sonar ring pulse. Position/opacity only — cheap at ~12 Hz.
static void on_anim(lv_timer_t *)
{
    if (!s_active) return;
    s_phase++;

    int dy = (int)(6.0f * sinf(s_phase * 0.16f));
    lv_obj_align(s_dog, LV_ALIGN_CENTER, 0, -74 + dy);

    // Rings expand/fade in sequence to read as an outward recon sweep.
    for (int i = 0; i < 3; i++) {
        float t = fmodf(s_phase * 0.03f + i * 0.33f, 1.0f);
        lv_opa_t opa = (lv_opa_t)(LV_OPA_COVER * (1.0f - t) * 0.6f);
        lv_obj_set_style_border_opa(s_ring[i], opa, LV_PART_MAIN);
    }
}

static void on_gesture(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    if (lv_indev_get_gesture_dir(indev) == LV_DIR_TOP) {
        s_active = false;
        hexhound_save();
        wifi_beacon_remove(pet_wifi_cb);
        tools_screen_show();
    }
}

// ── little primitive helpers ───────────────────────────────────────────────

static lv_obj_t *box(lv_obj_t *parent, int w, int h, int radius, lv_color_t c)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, radius, LV_PART_MAIN);
    lv_obj_set_style_bg_color(o, c, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(o, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(o, 0, LV_PART_MAIN);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

// A square rotated 45deg — an angular ear / cyber facet.
static lv_obj_t *diamond(lv_obj_t *parent, int s, lv_color_t c)
{
    lv_obj_t *o = box(parent, s, s, 3, c);
    lv_obj_set_style_transform_pivot_x(o, s / 2, LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_y(o, s / 2, LV_PART_MAIN);
    lv_obj_set_style_transform_rotation(o, 450, LV_PART_MAIN);   // 45.0 deg
    return o;
}

static lv_obj_t *ring(lv_obj_t *parent, int d, lv_color_t c)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_size(o, d, d);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_color(o, c, LV_PART_MAIN);
    lv_obj_set_style_border_width(o, 3, LV_PART_MAIN);
    lv_obj_set_style_pad_all(o, 0, LV_PART_MAIN);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

void pet_screen_create()
{
    hexhound_init();

    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_make(0x06, 0x0B, 0x11), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_screen, on_gesture, LV_EVENT_GESTURE, NULL);

    // Title.
    lv_obj_t *name = lv_label_create(s_screen);
    lv_obj_set_style_text_font(name, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(name, ARGUS_ACCENT, LV_PART_MAIN);
    lv_label_set_text(name, "HEXHOUND");
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 20);

    // Sonar rings (behind the hound).
    const int rd[3] = { 250, 190, 132 };
    for (int i = 0; i < 3; i++) {
        s_ring[i] = ring(s_screen, rd[i], ARGUS_ACCENT_DIM);
        lv_obj_align(s_ring[i], LV_ALIGN_CENTER, 0, -74);
    }

    // The hound — a container so the whole head bobs as one.
    s_dog = lv_obj_create(s_screen);
    lv_obj_set_size(s_dog, 200, 170);
    lv_obj_set_style_bg_opa(s_dog, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_dog, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_dog, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_dog, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_dog, LV_ALIGN_CENTER, 0, -74);

    // Ears — angular diamonds poking up behind the skull.
    s_ear[0] = diamond(s_dog, 46, ARGUS_ACCENT);
    lv_obj_align(s_ear[0], LV_ALIGN_CENTER, -44, -46);
    s_ear[1] = diamond(s_dog, 46, ARGUS_ACCENT);
    lv_obj_align(s_ear[1], LV_ALIGN_CENTER, 44, -46);

    // Skull — angular head (small radius reads as a cyber hexagon-ish plate).
    s_skull = box(s_dog, 132, 112, 18, ARGUS_ACCENT);
    lv_obj_align(s_skull, LV_ALIGN_CENTER, 0, -6);

    // Snout — a shorter block jutting down from the skull.
    s_snout = box(s_dog, 58, 46, 12, ARGUS_ACCENT);
    lv_obj_align(s_snout, LV_ALIGN_CENTER, 0, 58);

    // Nose (fixed dark tip on the snout).
    lv_obj_t *nose = box(s_dog, 20, 14, 6, lv_color_make(0x0A, 0x10, 0x16));
    lv_obj_align(nose, LV_ALIGN_CENTER, 0, 66);

    // Eyes — two bright scanners on the skull, with dark pupils.
    for (int i = 0; i < 2; i++) {
        s_eye[i] = box(s_dog, 30, 22, 8, lv_color_make(0xF0, 0xF4, 0xF8));
        lv_obj_align(s_eye[i], LV_ALIGN_CENTER, i == 0 ? -30 : 30, -8);
        lv_obj_t *pupil = box(s_eye[i], 12, 14, 6, lv_color_make(0x0A, 0x10, 0x16));
        lv_obj_center(pupil);
    }

    // Speech line.
    s_speech = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_speech, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_speech, ARGUS_ACCENT_ACTIVE, LV_PART_MAIN);
    lv_label_set_text(s_speech, "...tick... incubating...");
    lv_obj_align(s_speech, LV_ALIGN_CENTER, 0, 66);

    // Stage name + ability.
    s_stage_lbl = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_stage_lbl, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_stage_lbl, ARGUS_ACCENT, LV_PART_MAIN);
    lv_label_set_text(s_stage_lbl, "Egg");
    lv_obj_align(s_stage_lbl, LV_ALIGN_CENTER, 0, 92);

    s_ability = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_ability, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_ability, ARGUS_ACCENT_DIM, LV_PART_MAIN);
    lv_label_set_text(s_ability, "INCUBATING");
    lv_obj_align(s_ability, LV_ALIGN_CENTER, 0, 116);

    // Level + XP-into-stage bar.
    s_lvl = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_lvl, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_lvl, ARGUS_ACCENT, LV_PART_MAIN);
    lv_label_set_text(s_lvl, "LVL 1");
    lv_obj_align(s_lvl, LV_ALIGN_CENTER, -150, 138);

    lv_obj_t *bar_bg = box(s_screen, 300, 16, 8, lv_color_make(0x10, 0x18, 0x22));
    lv_obj_set_style_border_color(bar_bg, ARGUS_ACCENT_DIM, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar_bg, 1, LV_PART_MAIN);
    lv_obj_align(bar_bg, LV_ALIGN_CENTER, 0, 138);

    s_bar_fill = box(bar_bg, 2, 14, 7, ARGUS_ACCENT);
    lv_obj_align(s_bar_fill, LV_ALIGN_LEFT_MID, 0, 0);

    // Stats line.
    s_stats = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_stats, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_stats, ARGUS_ACCENT_DIM, LV_PART_MAIN);
    lv_label_set_text(s_stats, "HUN 0  ENR 0  BND 0   PWND 0  peers 0  xp 0");
    lv_obj_align(s_stats, LV_ALIGN_CENTER, 0, 168);

    // Evolution banner (hidden until an evolution fires).
    s_banner = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_banner, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_banner, ARGUS_ACCENT_ACTIVE, LV_PART_MAIN);
    lv_label_set_text(s_banner, "");
    lv_obj_align(s_banner, LV_ALIGN_TOP_MID, 0, 48);
    lv_obj_add_flag(s_banner, LV_OBJ_FLAG_HIDDEN);

    s_timer = lv_timer_create(on_tick, 1000, NULL);
    s_anim  = lv_timer_create(on_anim, 80,   NULL);
}

void pet_screen_show()
{
    if (!s_screen) pet_screen_create();
    hexhound_init();
    s_active = true;
    wifi_beacon_add(pet_wifi_cb);   // power the scanner so we meet peers/APs live
    hexhound_update();
    refresh();
    lv_scr_load(s_screen);
}

bool pet_screen_is_active() { return s_active; }
