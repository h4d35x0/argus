#include "pet_screen.h"
#include "hexhound.h"
#include "theme.h"
// NOTE: threat_radar.h intentionally NOT included. Threat awareness reaches the
// pet only through hexhound_set_threat_level() (see hexhound.h) so this cluster
// stays decoupled from the Threat Radar bundle. main.cpp and detect_pipeline.cpp
// are the two wired sources, each passing its own HexThreatSource.
// NOT a confirmed tail: both sources trip a full rung below TR_LVL_CONFIRMED, so
// nothing rendered here may claim a tail is confirmed.
#include "tools_screen.h"
#include "wifi_beacon_manager.h"
#include <lvgl.h>
#include <LilyGoLib.h>
#include <Arduino.h>
#include <math.h>

// ── ARGUS HexHound — LVGL renderer ─────────────────────────────────────
//
// The HexHound is a per-stage HD sprite loaded from the SD card
// (/HexHound/<stage>.png: egg / pup / beast / gremlin / sentinel), centered in a
// bobbing container inside a set of sonar rings that pulse as it sweeps the
// airwaves. The rings recolour by mood (calm steel-blue / HADES-red on a
// confirmed threat) as the at-a-glance threat cue. Everything below reads engine
// state through the hexhound_*() accessors; no game logic lives here.

static lv_obj_t  *s_screen    = nullptr;
static lv_obj_t  *s_dog       = nullptr;   // container bobbed by the idle anim
static lv_obj_t  *s_sprite    = nullptr;   // per-stage HD sprite image
static uint8_t    s_sprite_stage = 0xFF;   // stage whose sprite is loaded (0xFF=none)
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

// Point the sprite at this stage's SD asset. Only reloads (re-decodes) when the
// stage actually changes, so refresh() can call it every tick cheaply. LVGL
// mounts the SD card as drive "A" (same as the wallpaper in background.cpp).
static void update_sprite(uint8_t stage)
{
    if (!s_sprite || stage == s_sprite_stage) return;
    s_sprite_stage = stage;
    const char *path;
    switch (stage) {
        case HEX_PUP:      path = "A:/HexHound/pup.png";      break;
        case HEX_BEAST:    path = "A:/HexHound/beast.png";    break;
        case HEX_GREMLIN:  path = "A:/HexHound/gremlin.png";  break;
        case HEX_SENTINEL: path = "A:/HexHound/sentinel.png"; break;
        case HEX_EGG:
        default:           path = "A:/HexHound/egg.png";      break;
    }
    lv_image_set_src(s_sprite, NULL);   // force reload even if the pointer repeats
    lv_image_set_src(s_sprite, path);
    lv_obj_center(s_sprite);
}

static void refresh()
{
    const HexHoundState &st = hexhound_state();
    lv_color_t accent = mood_color(st.mood);

    // Swap to this stage's HD sprite (no-op unless the stage changed).
    update_sprite(st.stage);

    // Sonar rings track mood: calm steel-blue, or HADES-red on a confirmed
    // threat. With the creature now a fixed sprite, the rings are the pet's
    // at-a-glance mood/threat cue.
    for (int i = 0; i < 3; i++)
        lv_obj_set_style_border_color(s_ring[i], accent, LV_PART_MAIN);

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
        "HUN %d   ENR %d   BND %d   PWND %d\nPEERS %d      XP %ld",
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
    lv_obj_set_style_text_font(name, &font_argus_label_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(name, ARGUS_TEXT, LV_PART_MAIN);
    lv_label_set_text(name, "HEXHOUND");
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 20);

    // Sonar rings (behind the hound).
    const int rd[3] = { 250, 190, 132 };
    for (int i = 0; i < 3; i++) {
        s_ring[i] = ring(s_screen, rd[i], ARGUS_ACCENT_DIM);
        lv_obj_align(s_ring[i], LV_ALIGN_CENTER, 0, -74);
    }

    // The pet — a container so the whole sprite bobs as one.
    s_dog = lv_obj_create(s_screen);
    lv_obj_set_size(s_dog, 200, 200);
    lv_obj_set_style_bg_opa(s_dog, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_dog, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_dog, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_dog, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_dog, LV_ALIGN_CENTER, 0, -74);

    // Per-stage HD sprite (SD /HexHound/<stage>.png), transparent so it floats
    // over the sonar rings. The source is set per stage by update_sprite() in
    // refresh(); a missing card/asset just leaves it blank (no crash).
    s_sprite = lv_image_create(s_dog);
    lv_obj_center(s_sprite);

    // Speech line.
    s_speech = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_speech, &font_argus_label_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_speech, ARGUS_ACCENT_ACTIVE, LV_PART_MAIN);
    lv_label_set_text(s_speech, "...tick... incubating...");
    lv_obj_align(s_speech, LV_ALIGN_CENTER, 0, 66);

    // Stage name + ability.
    s_stage_lbl = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_stage_lbl, &font_argus_label_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_stage_lbl, ARGUS_TEXT, LV_PART_MAIN);
    lv_label_set_text(s_stage_lbl, "Egg");
    lv_obj_align(s_stage_lbl, LV_ALIGN_CENTER, 0, 92);

    s_ability = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_ability, &font_argus_label_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_ability, ARGUS_TEXT_DIM, LV_PART_MAIN);
    lv_label_set_text(s_ability, "INCUBATING");
    lv_obj_align(s_ability, LV_ALIGN_CENTER, 0, 116);

    // Level + XP-into-stage bar.
    s_lvl = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_lvl, &font_argus_label_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_lvl, ARGUS_TEXT, LV_PART_MAIN);
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
    lv_obj_set_style_text_font(s_stats, &font_argus_label_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_stats, ARGUS_TEXT_DIM, LV_PART_MAIN);
    lv_obj_set_style_text_align(s_stats, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(s_stats, "HUN 0   ENR 0   BND 0   PWND 0\nPEERS 0      XP 0");
    lv_obj_align(s_stats, LV_ALIGN_CENTER, 0, 176);

    // Evolution banner (hidden until an evolution fires).
    s_banner = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_banner, &font_argus_label_20, LV_PART_MAIN);
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
