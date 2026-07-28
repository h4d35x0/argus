#include "theme.h"
#include "threat_radar.h"
#include "argus_mode.h"

// Runtime, state-aware brand accent (ARGUS -> HADES).
//
// The compile-time ARGUS_ACCENT macro paints the calm steel-blue resting brand
// at ~67 low-traffic sites. This function is its live, threat-aware sibling:
// callers that repaint frequently (the clock status bar, the Threat Radar
// screen) call argus_accent() instead of the macro so the accent tracks the
// threat state. When Threat Radar has a contact at TR_LVL_LIKELY or above — i.e.
// something is co-moving with the wearer — the accent flips to HADES_RED so the
// watch visibly "opens its red eyes"; otherwise it stays steel-blue. The flip is
// glanceable and returns to calm on its own once the tail clears the staleness
// window (threatradar_top_level() reads only live contacts).
// Pipeline-driven threat override. The WiFi detect_pipeline sets this true when
// its ThreatState posture is Alert+; it flips the accent to HADES_RED the same
// way a Threat Radar tail does, on top of (independent of) the radar path. Both
// sources OR together, so either one flips the brand to the alert state.
static bool s_pipeline_threat = false;

void argus_set_threat(bool active)
{
    s_pipeline_threat = active;
}

// Mode base: steel-blue in Daily/Defense, amber in Offense.
lv_color_t argus_base_accent(void)
{
    return argus_mode_current() == ArgusMode::Offense ? ARGUS_OFFENSE_ACCENT : ARGUS_ACCENT;
}

lv_color_t argus_accent(void)
{
    // Daily stays INNOCENT: it never flips to the threat-red alert state, so a
    // detector firing in the background can't give the game away at a glance.
    if (argus_mode_current() == ArgusMode::Daily) return argus_base_accent();

    bool threat = s_pipeline_threat || threatradar_top_level() >= TR_LVL_LIKELY;
    return threat ? HADES_RED : argus_base_accent();
}

// ---- Persistent per-mode indicator (lv_layer_top overlay) -------------------

static lv_obj_t *s_mode_frame    = nullptr;   // full-screen border (Offense only)

// The "DEF" / "OFF" corner chip is DISABLED (2026-07-28). Mode is already
// unmistakable without it: the wallpaper changes, the tool set changes, and the
// accent colour changes. The chip was redundant on top of that, and it sat on
// lv_layer_top so it rode along on every screen rather than just the clock.
// Kept commented rather than deleted so it can be restored in one edit.
// static lv_obj_t *s_mode_chip     = nullptr;   // "DEF" / "OFF" chip container
// static lv_obj_t *s_mode_chip_lbl = nullptr;   // the chip's text

void argus_mode_indicator_init(void)
{
    lv_obj_t *top = lv_layer_top();

    s_mode_frame = lv_obj_create(top);
    lv_obj_remove_style_all(s_mode_frame);
    lv_obj_set_size(s_mode_frame, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(s_mode_frame, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_mode_frame, 4, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_mode_frame, ARGUS_OFFENSE_ACCENT, LV_PART_MAIN);
    lv_obj_add_flag(s_mode_frame, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_clear_flag(s_mode_frame, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_mode_frame, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_mode_frame, LV_OBJ_FLAG_HIDDEN);

    // --- "DEF" / "OFF" corner chip: DISABLED, see the note on the statics. ---
    // Chip = a small container (bg + radius) with a centred label child, the
    // reliable idiom here (a bare label-with-bg did not render as a chip).
    // s_mode_chip = lv_obj_create(top);
    // lv_obj_remove_style_all(s_mode_chip);
    // lv_obj_set_size(s_mode_chip, 48, 26);
    // lv_obj_set_style_bg_opa(s_mode_chip, LV_OPA_COVER, LV_PART_MAIN);
    // lv_obj_set_style_bg_color(s_mode_chip, ARGUS_ACCENT, LV_PART_MAIN);
    // lv_obj_set_style_radius(s_mode_chip, 6, LV_PART_MAIN);
    // lv_obj_clear_flag(s_mode_chip, LV_OBJ_FLAG_CLICKABLE);
    // lv_obj_clear_flag(s_mode_chip, LV_OBJ_FLAG_SCROLLABLE);
    // // Sit the badge UP IN THE STATUS HEADER ROW (same y as the WiFi/BT/SD/GPS
    // // icons at y~20), on the LEFT half of that row - the status icons live on the
    // // right, this side is empty. Referenced from TOP_MID (screen centre, x~205 on
    // // the 410-wide panel) so it lands well clear of BOTH rounded top corners; the
    // // leftward offset keeps it left of the right-side status chain. NOTE: this is
    // // an lv_layer_top overlay so it shows on every screen's header band - the
    // // redesign will make it a proper clock-only status-row element.
    // lv_obj_align(s_mode_chip, LV_ALIGN_TOP_MID, -120, 16);
    // lv_obj_add_flag(s_mode_chip, LV_OBJ_FLAG_HIDDEN);
    //
    // s_mode_chip_lbl = lv_label_create(s_mode_chip);
    // lv_obj_set_style_text_font(s_mode_chip_lbl, &font_argus_label_14, LV_PART_MAIN);
    // lv_obj_set_style_text_color(s_mode_chip_lbl, lv_color_black(), LV_PART_MAIN);
    // lv_label_set_text(s_mode_chip_lbl, "DEF");
    // lv_obj_center(s_mode_chip_lbl);

    argus_mode_indicator_refresh();
}

void argus_mode_indicator_refresh(void)
{
    // Guard covers the frame only. The chip pointers are gone with the chip; if
    // it is ever restored, add them back here or the frame stops refreshing.
    if (!s_mode_frame) return;
    ArgusMode m = argus_mode_current();

    // Border frame: Offense only; colour follows argus_accent() (amber, or red
    // under threat).
    if (m == ArgusMode::Offense) {
        lv_obj_set_style_border_color(s_mode_frame, argus_accent(), LV_PART_MAIN);
        lv_obj_clear_flag(s_mode_frame, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_mode_frame, LV_OBJ_FLAG_HIDDEN);
    }

    // --- "DEF" / "OFF" corner chip: DISABLED, see the note on the statics. ---
    // Chip: hidden in Daily (innocent), "DEF" (steel) in Defense, "OFF" in Offense.
    // if (m == ArgusMode::Daily) {
    //     lv_obj_add_flag(s_mode_chip, LV_OBJ_FLAG_HIDDEN);
    // } else {
    //     bool off = (m == ArgusMode::Offense);
    //     lv_label_set_text(s_mode_chip_lbl, off ? "OFF" : "DEF");
    //     lv_obj_set_style_bg_color(s_mode_chip, off ? argus_accent() : ARGUS_ACCENT, LV_PART_MAIN);
    //     lv_obj_clear_flag(s_mode_chip, LV_OBJ_FLAG_HIDDEN);
    //     lv_obj_move_foreground(s_mode_chip);   // above any other top-layer content
    // }
}

// ---- On-screen keyboard placement ------------------------------------------
// See the rationale in theme.h. One geometry for every keyboard so the rounded
// corners cannot clip a bottom row, and so the numbers cannot drift apart across
// the eight screens that create one.
void argus_keyboard_fit(lv_obj_t *kb, int height)
{
    if (!kb) return;
    lv_obj_set_size(kb, ARGUS_KB_SAFE_W, height);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, -ARGUS_KB_BOTTOM_INSET);
}
