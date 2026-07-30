#include "nodes_screen.h"
#include "theme.h"
#include "meshtastic.h"
#include "send_message_screen.h"
#include <LilyGoLib.h>

// Defined in meshtastic_screen.cpp
void meshtastic_screen_show();
// Defined in main.cpp
void main_loop_request_lvgl_priority(int cycles);

static lv_obj_t *nodes_screen;
static lv_obj_t *s_nodes_title = nullptr;   // heading; repainted per mode on show
static lv_obj_t *nodes_box;

// Floating traceroute result panel + 500 ms tick that drives the
// "Waiting... (Ns)" countdown and finalises the timeout. The panel
// stays hidden until a long-press on a node row fires the trace.
static lv_obj_t   *trace_panel    = nullptr;
static lv_obj_t   *trace_label    = nullptr;
static lv_timer_t *trace_tick     = nullptr;
static uint32_t    trace_target   = 0;
static uint32_t    trace_deadline = 0;     // millis() value for timeout

static const char *short_name_for(uint32_t node_id)
{
    int n = meshtastic_get_node_count();
    for (int i = 0; i < n; i++) {
        const MeshNode *m = meshtastic_get_node(i);
        if (m && m->node_id == node_id && m->short_name[0]) return m->short_name;
    }
    return "????";
}

static void render_trace_panel()
{
    if (!trace_panel || !trace_label || trace_target == 0) return;
    char buf[256];
    int p = 0;
    p += snprintf(buf + p, sizeof(buf) - p,
                  "Trace -> [%s] !%08lx\n",
                  short_name_for(trace_target),
                  (unsigned long)trace_target);

    const MeshTraceroute *tr = meshtastic_get_last_traceroute();
    if (tr && tr->target_node == trace_target && tr->has_response) {
        if (tr->hop_count == 0) {
            p += snprintf(buf + p, sizeof(buf) - p, "Direct neighbour");
        } else {
            p += snprintf(buf + p, sizeof(buf) - p, "Hops (%u):", tr->hop_count);
            for (uint8_t i = 0; i < tr->hop_count && i < 8; i++) {
                p += snprintf(buf + p, sizeof(buf) - p, "\n  %u. !%08lx",
                              (unsigned)(i + 1),
                              (unsigned long)tr->hops[i]);
            }
        }
    } else if (millis() > trace_deadline) {
        p += snprintf(buf + p, sizeof(buf) - p, "No response (30s)");
    } else {
        uint32_t remaining = (trace_deadline - millis()) / 1000;
        p += snprintf(buf + p, sizeof(buf) - p, "Waiting... (%us)",
                      (unsigned)remaining);
    }
    p += snprintf(buf + p, sizeof(buf) - p, "\n\n(tap to dismiss)");
    lv_label_set_text(trace_label, buf);
}

static void on_trace_panel_pressed(lv_event_t *)
{
    if (trace_panel) lv_obj_add_flag(trace_panel, LV_OBJ_FLAG_HIDDEN);
    trace_target   = 0;
    trace_deadline = 0;
}

static void on_trace_tick(lv_timer_t *)
{
    if (!trace_panel || lv_obj_has_flag(trace_panel, LV_OBJ_FLAG_HIDDEN)) return;
    if (trace_target != 0) render_trace_panel();
}

// Action chooser popup. Long-press on a node row brings this up with
// three buttons - [DM] [Trace] [Pos] - rather than firing any one of
// them immediately. Lets the user pick the action they want without
// committing on a single tap. DM lives here so swipe-right from the
// nodes screen always opens the regular broadcast SEND MESSAGE screen
// and never accidentally pre-targets a node.
static lv_obj_t *action_panel    = nullptr;
static lv_obj_t *action_title    = nullptr;
static lv_obj_t *action_backdrop = nullptr;
static uint32_t  action_target   = 0;

static void action_panel_hide()
{
    if (action_panel)    lv_obj_add_flag(action_panel,    LV_OBJ_FLAG_HIDDEN);
    if (action_backdrop) lv_obj_add_flag(action_backdrop, LV_OBJ_FLAG_HIDDEN);
    action_target = 0;
}

static void on_action_dm(lv_event_t *)
{
    uint32_t nid = action_target;
    action_panel_hide();
    if (nid == 0 || nid == 0xFFFFFFFFu) return;
    send_message_screen_show_to(nid);
}

static void on_action_trace(lv_event_t *)
{
    uint32_t nid = action_target;
    action_panel_hide();
    if (nid == 0 || nid == 0xFFFFFFFFu) return;

    if (!meshtastic_send_traceroute(nid)) {
        if (trace_panel && trace_label) {
            char buf[64];
            snprintf(buf, sizeof(buf),
                     "Trace failed\n!%08lx\n(LoRa off?)\n\n(tap to dismiss)",
                     (unsigned long)nid);
            lv_label_set_text(trace_label, buf);
            lv_obj_clear_flag(trace_panel, LV_OBJ_FLAG_HIDDEN);
            trace_target   = 0;
            trace_deadline = 0;
        }
        return;
    }
    trace_target   = nid;
    trace_deadline = millis() + 30000;
    if (trace_panel) lv_obj_clear_flag(trace_panel, LV_OBJ_FLAG_HIDDEN);
    render_trace_panel();
}

static void on_action_pos(lv_event_t *)
{
    uint32_t nid = action_target;
    action_panel_hide();
    if (nid == 0 || nid == 0xFFFFFFFFu) return;

    bool ok = meshtastic_request_position(nid);
    // Reuse the trace result panel as a brief toast. There's no reply
    // tracking slot for position requests today - the reply just lands
    // in the normal POSITION RX path and updates the node row's
    // coordinates - so the panel only confirms the request was queued.
    if (trace_panel && trace_label) {
        char buf[80];
        if (ok) {
            snprintf(buf, sizeof(buf),
                     "Position request sent to\n!%08lx\n\n(tap to dismiss)",
                     (unsigned long)nid);
        } else {
            snprintf(buf, sizeof(buf),
                     "Position request failed\n(LoRa off?)\n\n(tap to dismiss)");
        }
        lv_label_set_text(trace_label, buf);
        lv_obj_clear_flag(trace_panel, LV_OBJ_FLAG_HIDDEN);
        trace_target   = 0;       // suppress trace-tick re-rendering
        trace_deadline = 0;
    }
}

// Tap outside the buttons (panel background itself) cancels.
static void on_action_panel_pressed(lv_event_t *e)
{
    // Only dismiss on a tap of the bare panel, not on the buttons.
    auto target = (lv_obj_t *)lv_event_get_target(e);
    if (target == action_panel) action_panel_hide();
}

// Long-press a node row -> show the action chooser.
static void on_node_long_pressed(lv_event_t *e)
{
    auto target = (lv_obj_t *)lv_event_get_target(e);
    uint32_t nid = (uint32_t)(uintptr_t)lv_obj_get_user_data(target);
    if (nid == 0 || nid == 0xFFFFFFFFu) return;

    action_target = nid;
    if (action_panel && action_title) {
        char buf[40];
        snprintf(buf, sizeof(buf), "Node !%08lx", (unsigned long)nid);
        lv_label_set_text(action_title, buf);
        // Backdrop first (so taps outside the panel hit it), then the
        // panel on top (so taps on the buttons hit the panel).
        if (action_backdrop) {
            lv_obj_clear_flag(action_backdrop, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(action_backdrop);
        }
        lv_obj_clear_flag(action_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(action_panel);
    }
}

static void on_gesture(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir == LV_DIR_LEFT)
        meshtastic_screen_show();
    else if (dir == LV_DIR_RIGHT)
        send_message_screen_show();
}

static void make_node_entry(lv_obj_t *parent, const MeshNode *n)
{
    lv_obj_t *entry = lv_obj_create(parent);
    lv_obj_set_width(entry, lv_pct(100));
    lv_obj_set_height(entry, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(entry, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(entry, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_side(entry, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_border_color(entry, lv_color_make(0x33, 0x33, 0x33), LV_PART_MAIN);
    lv_obj_set_style_border_width(entry, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_all(entry, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_row(entry, 2, LV_PART_MAIN);
    lv_obj_set_layout(entry, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(entry, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(entry, LV_OBJ_FLAG_SCROLLABLE);

    // Long-press a node row to open the action chooser (DM / Trace /
    // Pos). Plain tap is intentionally a no-op so a careless touch
    // never fires an RF probe or jumps screens; CLICKABLE has to stay
    // set for LV_EVENT_LONG_PRESSED to fire.
    lv_obj_set_user_data(entry, (void *)(uintptr_t)n->node_id);
    lv_obj_add_flag(entry, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(entry, on_node_long_pressed, LV_EVENT_LONG_PRESSED, NULL);

    // Short name + node id + last-heard time (green, small)
    char header[48];
    snprintf(header, sizeof(header), "[%s] !%08lx   %s",
             n->short_name[0] ? n->short_name : "?",
             (unsigned long)n->node_id, n->time_str);
    lv_obj_t *hdr = lv_label_create(entry);
    lv_obj_set_width(hdr, lv_pct(100));
    lv_obj_set_style_text_color(hdr, ARGUS_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(hdr, &font_argus_label_14, LV_PART_MAIN);
    lv_label_set_text(hdr, header);
    // LONG_DOT renders truncated content as "...". The header
    // ("[SHRT] !nodeid   HH:MM") is normally well under the 360-px row
    // width, so the dots only surface if a future field gets added or
    // a node sends an unexpectedly wide short_name.
    lv_label_set_long_mode(hdr, LV_LABEL_LONG_DOT);

    // Long name (white)
    lv_obj_t *txt = lv_label_create(entry);
    lv_obj_set_width(txt, lv_pct(100));
    lv_obj_set_style_text_color(txt, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(txt, &font_argus_label_16, LV_PART_MAIN);
    lv_label_set_text(txt, n->long_name);
    lv_label_set_long_mode(txt, LV_LABEL_LONG_WRAP);

    // Detail lines (gray) — only fields the node actually advertised.
    // Sized to fit identity + position + full device/env/power telemetry
    // + an 8-entry neighbor list, one per line.
    char detail[768];
    int  p = 0;
    if (n->id[0])
        p += snprintf(detail + p, sizeof(detail) - p, "ID: %s", n->id);
    if (n->has_hw_model)
        p += snprintf(detail + p, sizeof(detail) - p, "%sHW: %lu",
                       p ? "\n" : "", (unsigned long)n->hw_model);
    if (n->has_role)
        p += snprintf(detail + p, sizeof(detail) - p, "%sRole: %s",
                       p ? "\n" : "", meshtastic_role_name(n->role));
    if (n->has_is_licensed)
        p += snprintf(detail + p, sizeof(detail) - p, "%sLicensed: %s",
                       p ? "\n" : "", n->is_licensed ? "Yes" : "No");
    if (n->has_macaddr)
        p += snprintf(detail + p, sizeof(detail) - p,
                       "%sMAC: %02x:%02x:%02x:%02x:%02x:%02x", p ? "\n" : "",
                       n->macaddr[0], n->macaddr[1], n->macaddr[2],
                       n->macaddr[3], n->macaddr[4], n->macaddr[5]);
    if (n->has_position) {
        // lat/lon are degrees * 1e7; format with integer math (embedded
        // printf has no reliable %f). Keep the sign even when |deg| < 1.
        int32_t la = n->latitude_i,  lo = n->longitude_i;
        long la_w = la / 10000000, la_f = la % 10000000; if (la_f < 0) la_f = -la_f;
        long lo_w = lo / 10000000, lo_f = lo % 10000000; if (lo_f < 0) lo_f = -lo_f;
        const char *la_sgn = (la < 0 && la_w == 0) ? "-" : "";
        const char *lo_sgn = (lo < 0 && lo_w == 0) ? "-" : "";
        p += snprintf(detail + p, sizeof(detail) - p,
                       "%sPos: %s%ld.%07ld, %s%ld.%07ld", p ? "\n" : "",
                       la_sgn, la_w, la_f, lo_sgn, lo_w, lo_f);
    }
    if (n->has_altitude)
        p += snprintf(detail + p, sizeof(detail) - p, "%sAlt: %ldm",
                       p ? "\n" : "", (long)n->altitude);

    // Telemetry - DeviceMetrics (battery / voltage / uptime). Each
    // subfield optional. Compact one-line voltage where present.
    if (n->has_telemetry) {
        if (n->has_battery_level)
            p += snprintf(detail + p, sizeof(detail) - p, "%sBatt: %lu%%",
                          p ? "\n" : "", (unsigned long)n->battery_level);
        if (n->has_voltage)
            p += snprintf(detail + p, sizeof(detail) - p, "%s%.2f V",
                          p ? "  " : "", (double)n->voltage);
        if (n->has_uptime) {
            uint32_t s = n->uptime_seconds;
            char up[24];
            if      (s < 60)    snprintf(up, sizeof(up), "%lus", (unsigned long)s);
            else if (s < 3600)  snprintf(up, sizeof(up), "%lum", (unsigned long)(s / 60));
            else if (s < 86400) snprintf(up, sizeof(up), "%luh", (unsigned long)(s / 3600));
            else                snprintf(up, sizeof(up), "%lud", (unsigned long)(s / 86400));
            p += snprintf(detail + p, sizeof(detail) - p, "%sUp: %s",
                          p ? "\n" : "", up);
        }
    }
    // EnvironmentMetrics
    if (n->has_environment) {
        if (n->has_temperature)
            p += snprintf(detail + p, sizeof(detail) - p, "%sTemp: %.1f C",
                          p ? "\n" : "", (double)n->temperature_c);
        if (n->has_humidity)
            p += snprintf(detail + p, sizeof(detail) - p, "%sRH: %.0f%%",
                          p ? "  " : "", (double)n->relative_humidity);
        if (n->has_pressure)
            p += snprintf(detail + p, sizeof(detail) - p, "%sPres: %.0f hPa",
                          p ? "\n" : "", (double)n->pressure_hpa);
        if (n->has_lux)
            p += snprintf(detail + p, sizeof(detail) - p, "%sLux: %.0f",
                          p ? "\n" : "", (double)n->lux);
        if (n->has_iaq)
            p += snprintf(detail + p, sizeof(detail) - p, "%sIAQ: %lu",
                          p ? "\n" : "", (unsigned long)n->iaq);
    }
    // PowerMetrics - CH1 only (the watch surfaces CH1 V/A; multi-rail
    // is rare in field).
    if (n->has_power_metrics) {
        if (n->has_ch1_voltage)
            p += snprintf(detail + p, sizeof(detail) - p, "%sCh1: %.2f V",
                          p ? "\n" : "", (double)n->ch1_voltage);
        if (n->has_ch1_current)
            p += snprintf(detail + p, sizeof(detail) - p, "%s%.0f mA",
                          (n->has_ch1_voltage ? "  " : (p ? "\n" : "")),
                          (double)n->ch1_current);
    }
    // NeighborInfo - one neighbour per line with SNR.
    if (n->has_neighborinfo && n->neighbor_count > 0) {
        p += snprintf(detail + p, sizeof(detail) - p,
                      "%sNeighbors (%u):", p ? "\n" : "", n->neighbor_count);
        for (uint8_t i = 0; i < n->neighbor_count && i < MESH_MAX_NEIGHBORS; i++) {
            p += snprintf(detail + p, sizeof(detail) - p,
                          "\n  !%08lx  %.1f dB",
                          (unsigned long)n->neighbors[i].node_id,
                          (double)n->neighbors[i].snr);
        }
    }

    if (p > 0) {
        lv_obj_t *det = lv_label_create(entry);
        lv_obj_set_width(det, lv_pct(100));
        lv_obj_set_style_text_color(det, ARGUS_TEXT_DIM, LV_PART_MAIN);
        lv_obj_set_style_text_font(det, &font_argus_label_14, LV_PART_MAIN);
        lv_label_set_text(det, detail);
        lv_label_set_long_mode(det, LV_LABEL_LONG_WRAP);
    }
}

static void rebuild_list()
{
    lv_obj_clean(nodes_box);
    int node_count = meshtastic_get_node_count();
    if (node_count == 0) {
        lv_obj_t *lbl = lv_label_create(nodes_box);
        lv_obj_add_flag(lbl, LV_OBJ_FLAG_FLOATING);  // exclude from flex layout so center works
        lv_obj_set_style_text_color(lbl, ARGUS_TEXT_DIM, LV_PART_MAIN);
        lv_obj_set_style_text_font(lbl, &font_argus_label_16, LV_PART_MAIN);
        lv_label_set_text(lbl, "No nodes detected yet.");
        lv_obj_center(lbl);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    } else {
        for (int i = 0; i < node_count; i++) {
            const MeshNode *n = meshtastic_get_node(i);
            if (n) make_node_entry(nodes_box, n);
        }
    }
}

void nodes_screen_create()
{
    nodes_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(nodes_screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(nodes_screen, 0, LV_PART_MAIN);

    // Title + list + hint geometry mirrors the Meshtastic screen so
    // the two feel visually identical when swiping between them.
    lv_obj_t *title = lv_label_create(nodes_screen);
    s_nodes_title = title;
    lv_obj_set_style_text_color(title, argus_base_accent(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_argus_ui, LV_PART_MAIN);
    lv_label_set_text(title, "NODES");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 21);

    // Node list — same 360x340 box at y=87 the messages list uses.
    nodes_box = lv_obj_create(nodes_screen);
    lv_obj_set_size(nodes_box, 360, 340);
    lv_obj_align(nodes_box, LV_ALIGN_TOP_MID, 0, 87);
    lv_obj_set_style_bg_color(nodes_box, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_color(nodes_box, lv_color_make(0x33, 0x33, 0x33), LV_PART_MAIN);
    lv_obj_set_style_border_width(nodes_box, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(nodes_box, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(nodes_box, 0, LV_PART_MAIN);
    lv_obj_set_scroll_dir(nodes_box, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(nodes_box, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_layout(nodes_box, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(nodes_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(nodes_box, 0, LV_PART_MAIN);

    lv_obj_t *hint = lv_label_create(nodes_screen);
    lv_obj_set_style_text_color(hint, ARGUS_TEXT_DIM, LV_PART_MAIN);
    lv_obj_set_style_text_font(hint, &font_argus_label_14, LV_PART_MAIN);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(hint, "hold: Trace/Pos/DM   swipe: nav");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -22);

    lv_obj_add_event_cb(nodes_screen, on_gesture, LV_EVENT_GESTURE, NULL);

    // Trace result panel - floats at bottom-centre, hidden until a
    // long-press on a node fires the traceroute. Sized to fit inside
    // the watch's ~410-diameter visible disc with margin.
    trace_panel = lv_obj_create(nodes_screen);
    lv_obj_set_size(trace_panel, 340, 160);
    lv_obj_align(trace_panel, LV_ALIGN_BOTTOM_MID, 0, -40);
    lv_obj_set_style_bg_color(trace_panel, lv_color_make(0x1A, 0x1A, 0x1A), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(trace_panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(trace_panel, lv_color_make(0xFF, 0x95, 0x00), LV_PART_MAIN);
    lv_obj_set_style_border_width(trace_panel, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(trace_panel, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(trace_panel, 12, LV_PART_MAIN);
    lv_obj_add_flag(trace_panel, LV_OBJ_FLAG_FLOATING);   // ignored by flex
    lv_obj_add_flag(trace_panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(trace_panel, on_trace_panel_pressed,
                        LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(trace_panel, LV_OBJ_FLAG_HIDDEN);

    trace_label = lv_label_create(trace_panel);
    lv_obj_set_width(trace_label, lv_pct(100));
    lv_obj_set_style_text_color(trace_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(trace_label, &font_argus_label_14, LV_PART_MAIN);
    lv_label_set_text(trace_label, "");
    lv_label_set_long_mode(trace_label, LV_LABEL_LONG_WRAP);

    // Drives the countdown / timeout finalisation while the panel is
    // visible. lv_timers stay alive across screen swaps; render_trace
    // bails cheaply when the panel is hidden.
    trace_tick = lv_timer_create(on_trace_tick, 500, NULL);

    // Invisible full-screen backdrop behind the action panel. Catches
    // taps anywhere outside the three buttons and dismisses the popup
    // so the user isn't forced to commit to DM/Trace/Pos just to close
    // it. Sized 410x502 = full framebuffer, fully transparent, but
    // clickable so the tap doesn't fall through to the node list.
    action_backdrop = lv_obj_create(nodes_screen);
    lv_obj_set_size(action_backdrop, 410, 502);
    lv_obj_align(action_backdrop, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(action_backdrop, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(action_backdrop, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(action_backdrop, 0, LV_PART_MAIN);
    lv_obj_add_flag(action_backdrop, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(action_backdrop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(action_backdrop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(action_backdrop,
                        [](lv_event_t *) { action_panel_hide(); },
                        LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(action_backdrop, LV_OBJ_FLAG_HIDDEN);

    // Action chooser popup. Pops up on long-press of a node row with
    // three big buttons - [DM] [Trace] [Pos]. Sits in the same screen-
    // centre area as the trace panel; mutually exclusive (only one of
    // the two is visible at a time). Tapping the dimmed backdrop above
    // (or any non-button area of the panel) closes the popup.
    action_panel = lv_obj_create(nodes_screen);
    lv_obj_set_size(action_panel, 340, 160);
    lv_obj_align(action_panel, LV_ALIGN_BOTTOM_MID, 0, -40);
    lv_obj_set_style_bg_color(action_panel, lv_color_make(0x1A, 0x1A, 0x1A), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(action_panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(action_panel, lv_color_make(0x58, 0x56, 0xD6), LV_PART_MAIN);
    lv_obj_set_style_border_width(action_panel, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(action_panel, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(action_panel, 12, LV_PART_MAIN);
    lv_obj_add_flag(action_panel, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(action_panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(action_panel, on_action_panel_pressed,
                        LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(action_panel, LV_OBJ_FLAG_HIDDEN);

    action_title = lv_label_create(action_panel);
    lv_obj_set_style_text_color(action_title, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(action_title, &font_argus_label_16, LV_PART_MAIN);
    lv_label_set_text(action_title, "");
    lv_obj_align(action_title, LV_ALIGN_TOP_MID, 0, 0);

    // Three buttons across the bottom of the panel. Sized to fit the
    // 316-px usable width (340 panel - 24 padding): 96 + 14 + 96 + 14 + 96.
    // DM = lavender (matches the DM tint on the messages screen), Trace
    // = orange, Pos = green.
    lv_obj_t *btn_d = lv_button_create(action_panel);
    lv_obj_set_size(btn_d, 96, 64);
    lv_obj_align(btn_d, LV_ALIGN_BOTTOM_LEFT, 0, -8);
    lv_obj_set_style_radius(btn_d, 10, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn_d, lv_color_make(0x58, 0x56, 0xD6), LV_PART_MAIN);
    lv_obj_add_event_cb(btn_d, on_action_dm, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_d = lv_label_create(btn_d);
    lv_label_set_text(lbl_d, "DM");
    lv_obj_set_style_text_color(lbl_d, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl_d, &font_argus_label_20, LV_PART_MAIN);
    lv_obj_center(lbl_d);

    lv_obj_t *btn_t = lv_button_create(action_panel);
    lv_obj_set_size(btn_t, 96, 64);
    lv_obj_align(btn_t, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_radius(btn_t, 10, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn_t, lv_color_make(0xFF, 0x95, 0x00), LV_PART_MAIN);
    lv_obj_add_event_cb(btn_t, on_action_trace, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_t = lv_label_create(btn_t);
    lv_label_set_text(lbl_t, "Trace");
    lv_obj_set_style_text_color(lbl_t, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl_t, &font_argus_label_20, LV_PART_MAIN);
    lv_obj_center(lbl_t);

    lv_obj_t *btn_p = lv_button_create(action_panel);
    lv_obj_set_size(btn_p, 96, 64);
    lv_obj_align(btn_p, LV_ALIGN_BOTTOM_RIGHT, 0, -8);
    lv_obj_set_style_radius(btn_p, 10, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn_p, lv_color_make(0x34, 0xC7, 0x59), LV_PART_MAIN);
    lv_obj_add_event_cb(btn_p, on_action_pos, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_p = lv_label_create(btn_p);
    lv_label_set_text(lbl_p, "Pos");
    lv_obj_set_style_text_color(lbl_p, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl_p, &font_argus_label_20, LV_PART_MAIN);
    lv_obj_center(lbl_p);

    rebuild_list();
}

void nodes_screen_show()
{
    main_loop_request_lvgl_priority(12);
    if (s_nodes_title) lv_obj_set_style_text_color(s_nodes_title, argus_base_accent(), LV_PART_MAIN);
    rebuild_list();
    lv_scr_load(nodes_screen);
}

bool nodes_screen_is_active()
{
    return lv_screen_active() == nodes_screen;
}

void nodes_screen_refresh()
{
    if (nodes_screen_is_active())
        rebuild_list();
}
