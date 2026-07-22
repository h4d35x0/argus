#include "map_screen.h"
#include "theme.h"
#include "gps_screen.h"
#include "meshtastic.h"
#include <LilyGoLib.h>
#include <SD.h>
#include <math.h>
#include <stdio.h>

// Defined by their respective screens. The map sits between send_message
// and configuration in the swipe chain (LEFT = back to send, RIGHT = on
// to configuration).
void send_message_screen_show();
void configuration_screen_show();
// Defined in main.cpp
void main_loop_request_lvgl_priority(int cycles);

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MAP_W   410
#define MAP_H   502
#define TILE_PX 256

// 3x3 grid of tiles covers any sub-tile centring on a 410x502 screen
#define TILES   9

static lv_obj_t *map_screen;
static lv_obj_t *tile_imgs[TILES];
static lv_obj_t *marker;
static lv_obj_t *info_badge;
static lv_obj_t *info_label;
static lv_obj_t *status_label;
static lv_obj_t *zoom_in_btn, *zoom_out_btn, *recentre_btn;

// Peer-node markers - one dot + short_name label per node slot. Pool
// is sized to MESH_MAX_NODES (20) and lives for the lifetime of the
// screen; refresh() hides/shows individual entries based on which
// nodes currently have a known position + fall within the view.
static lv_obj_t *node_dots[MESH_MAX_NODES];
static lv_obj_t *node_labels[MESH_MAX_NODES];

// Per-tile path buffers — kept alive while LVGL uses them as the image source.
static char  s_paths[TILES][80];

static int    s_zoom        = 6;
static int    s_zoom_min    = 1;
static int    s_zoom_max    = 6;
static double s_last_lat    = 1000;   // sentinel: forces first render
static double s_last_lon    = 1000;
static int    s_last_zoom   = -1;

// Drag-to-pan state. While s_manual_pan is true, refresh() centres the
// map on (s_view_lat, s_view_lon) instead of the GPS fix, and the red
// marker is offset from screen centre to reflect where the GPS actually
// is. Cleared by the recentre button OR by the swipe-nav handler (since
// a swipe shouldn't leave the map permanently pinned away from GPS).
static bool   s_manual_pan = false;
static double s_view_lat   = 0;
static double s_view_lon   = 0;

// Last-known GPS fix persisted to /Meshtastic/last_fix.txt so the map
// can open centred where we last had a position - even if GPS is off
// or hasn't relocked. Loaded once on first map_screen_show(); written
// from refresh() whenever a fresh live fix is observed, rate-limited
// to once a minute so the SD doesn't get hammered.
static bool     s_have_saved_fix = false;
static double   s_saved_lat      = 0;
static double   s_saved_lon      = 0;
static bool     s_saved_loaded   = false;
static uint32_t s_last_save_ms   = 0;
static const uint32_t kSaveIntervalMs = 60000;
static const char    *kLastFixPath    = "/Meshtastic/last_fix.txt";

static void save_last_fix(double lat, double lon)
{
    uint32_t now = millis();
    if (s_last_save_ms != 0 && now - s_last_save_ms < kSaveIntervalMs) return;
    if (!instance.isCardReady()) return;
    if (!SD.exists("/Meshtastic")) SD.mkdir("/Meshtastic");
    File f = SD.open(kLastFixPath, FILE_WRITE);
    if (!f) return;
    f.printf("%.7f,%.7f\n", lat, lon);
    f.close();
    s_last_save_ms   = now;
    s_have_saved_fix = true;
    s_saved_lat      = lat;
    s_saved_lon      = lon;
}

// Read the persisted fix once. File format is "lat,lon\n". Missing
// or malformed -> s_have_saved_fix stays false and the cold-start
// (0,0 world view) path takes over.
static void load_last_fix()
{
    if (s_saved_loaded) return;
    s_saved_loaded = true;
    if (!instance.isCardReady()) return;
    if (!SD.exists(kLastFixPath)) return;
    File f = SD.open(kLastFixPath, FILE_READ);
    if (!f) return;
    char buf[64];
    size_t n = 0;
    while (f.available() && n < sizeof(buf) - 1) {
        int c = f.read();
        if (c < 0 || c == '\n') break;
        if (c == '\r') continue;
        buf[n++] = (char)c;
    }
    buf[n] = '\0';
    f.close();
    double lat, lon;
    if (sscanf(buf, "%lf,%lf", &lat, &lon) == 2) {
        s_have_saved_fix = true;
        s_saved_lat      = lat;
        s_saved_lon      = lon;
    }
}

// Slippy-map degrees-per-pixel at the current zoom. Lon scale is
// uniform across the projection; lat scale collapses with cos(lat) due
// to Mercator. Linearisation around the view centre - fine for the
// small per-event finger deltas we get from lv_indev_get_vect().
static inline double lon_per_px(int z) {
    return 360.0 / (256.0 * pow(2.0, (double)z));
}
static inline double lat_per_px(int z, double lat_deg) {
    double lat_rad = lat_deg * M_PI / 180.0;
    return 360.0 * cos(lat_rad) / (256.0 * pow(2.0, (double)z));
}

// ---- tile maths (standard slippy-map) --------------------------------------

static void rebuild_tiles(double lat, double lon, int z)
{
    double lat_rad = lat * M_PI / 180.0;
    double n       = pow(2.0, (double)z);
    double xtile_f = (lon + 180.0) / 360.0 * n;
    double ytile_f = (1.0 - asinh(tan(lat_rad)) / M_PI) / 2.0 * n;

    int cx = (int)floor(xtile_f);
    int cy = (int)floor(ytile_f);
    int px = (int)((xtile_f - cx) * TILE_PX);   // pixel offset within centre tile
    int py = (int)((ytile_f - cy) * TILE_PX);

    int cx_screen = MAP_W / 2 - px;
    int cy_screen = MAP_H / 2 - py;
    int maxt      = (int)n - 1;

    int idx = 0;
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            int tx = cx + dx;
            int ty = cy + dy;
            if (tx < 0 || tx > maxt || ty < 0 || ty > maxt) {
                lv_obj_add_flag(tile_imgs[idx], LV_OBJ_FLAG_HIDDEN);
                idx++;
                continue;
            }
            snprintf(s_paths[idx], sizeof(s_paths[idx]),
                     "A:/map/%d/%d/%d.png", z, tx, ty);
            // Force a reload even when the path-buffer pointer is unchanged.
            lv_image_set_src(tile_imgs[idx], NULL);
            lv_image_set_src(tile_imgs[idx], s_paths[idx]);
            lv_obj_clear_flag(tile_imgs[idx], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(tile_imgs[idx],
                cx_screen + dx * TILE_PX,
                cy_screen + dy * TILE_PX);
            idx++;
        }
    }
}

// Walks /map/0 … /map/18 on the SD card to learn which zoom levels are present.
static void detect_zooms()
{
    if (!instance.isCardReady()) {
        s_zoom_min = 1;
        s_zoom_max = 6;
        return;
    }
    int lo = 99, hi = -1;
    for (int z = 0; z <= 18; z++) {
        char p[24];
        snprintf(p, sizeof(p), "/map/%d", z);
        if (SD.exists(p)) {
            if (z < lo) lo = z;
            if (z > hi) hi = z;
        }
    }
    if (hi < 0) { s_zoom_min = 1; s_zoom_max = 6; }
    else        { s_zoom_min = lo; s_zoom_max = hi; }
}

static void hide_tiles()
{
    for (int i = 0; i < TILES; i++)
        lv_obj_add_flag(tile_imgs[i], LV_OBJ_FLAG_HIDDEN);
}

// Project each known peer node's lat/lon onto the screen and either
// position+show its dot+label or hide it (off-screen / no position /
// is-our-own-node). view_lat/view_lon is the current map centre.
static void render_peer_nodes(double view_lat, double view_lon)
{
    uint32_t self_id = meshtastic_get_node_id();
    int n = meshtastic_get_node_count();
    int slot = 0;
    double lon_dpp = lon_per_px(s_zoom);
    double lat_dpp = lat_per_px(s_zoom, view_lat);
    for (int i = 0; i < n && slot < MESH_MAX_NODES; i++) {
        const MeshNode *node = meshtastic_get_node(i);
        if (!node) continue;
        if (!node->has_position) continue;
        if (node->node_id == self_id) continue;   // our own GPS marker

        double node_lat = node->latitude_i / 1e7;
        double node_lon = node->longitude_i / 1e7;
        double dx_px =  (node_lon - view_lon) / lon_dpp;
        double dy_px = -(node_lat - view_lat) / lat_dpp;
        int sx = (int)(MAP_W / 2.0 + dx_px);
        int sy = (int)(MAP_H / 2.0 + dy_px);

        // Cull off-screen dots; leave a small margin so a half-on-edge
        // marker still appears clipped rather than vanishing entirely.
        if (sx < -10 || sx >= MAP_W + 10 || sy < -10 || sy >= MAP_H + 10) {
            lv_obj_add_flag(node_dots[slot],   LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(node_labels[slot], LV_OBJ_FLAG_HIDDEN);
            slot++;
            continue;
        }

        lv_obj_align(node_dots[slot], LV_ALIGN_TOP_LEFT, sx - 5, sy - 5);
        lv_obj_clear_flag(node_dots[slot], LV_OBJ_FLAG_HIDDEN);

        // Label: short_name if the node sent one, else first 4 hex
        // chars of the node ID so they're at least somewhat
        // identifiable. Positioned 8 px below the dot.
        char lbl[12];
        if (node->short_name[0]) {
            snprintf(lbl, sizeof(lbl), "%s", node->short_name);
        } else {
            snprintf(lbl, sizeof(lbl), "%04lx",
                     (unsigned long)(node->node_id & 0xFFFF));
        }
        lv_label_set_text(node_labels[slot], lbl);
        lv_obj_align(node_labels[slot], LV_ALIGN_TOP_LEFT, sx - 14, sy + 8);
        lv_obj_clear_flag(node_labels[slot], LV_OBJ_FLAG_HIDDEN);
        slot++;
    }
    // Hide any slots we didn't use (node went away, lost position, etc).
    for (int j = slot; j < MESH_MAX_NODES; j++) {
        lv_obj_add_flag(node_dots[j],   LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(node_labels[j], LV_OBJ_FLAG_HIDDEN);
    }
}

// Hide every peer dot/label at once - used when we don't have a GPS
// fix and the tiles + markers are all hidden.
static void hide_peer_nodes()
{
    for (int i = 0; i < MESH_MAX_NODES; i++) {
        lv_obj_add_flag(node_dots[i],   LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(node_labels[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void refresh(bool force)
{
    // Three-source fallback chain for the map centre:
    //   1) live GPS fix - preferred; also stamped to SD for next run
    //   2) last-known fix loaded from SD - we've been here before,
    //      we just don't have a current lock
    //   3) (0,0) world view at min zoom - cold start, no live fix
    //      and nothing on disk; user drags from here.
    // Manual-pan always wins regardless of which source seeded the
    // pre-pan centre.
    bool has_live = gps_screen_has_lock() && instance.gps.location.isValid();
    double gps_lat = 0, gps_lon = 0;
    enum class Source { LIVE, SAVED, NONE } src = Source::NONE;
    if (has_live) {
        gps_lat = instance.gps.location.lat();
        gps_lon = instance.gps.location.lng();
        src     = Source::LIVE;
        save_last_fix(gps_lat, gps_lon);   // rate-limited internally
    } else if (s_have_saved_fix) {
        gps_lat = s_saved_lat;
        gps_lon = s_saved_lon;
        src     = Source::SAVED;
    }

    double lat, lon;
    if (s_manual_pan) {
        lat = s_view_lat;
        lon = s_view_lon;
    } else if (src != Source::NONE) {
        lat = gps_lat;
        lon = gps_lon;
    } else {
        // Cold start. Drop to the lowest available zoom so the world
        // view actually renders something; user can drag from there.
        lat = 0.0;
        lon = 0.0;
        if (s_zoom != s_zoom_min) s_zoom = s_zoom_min;
    }

    if (force || s_zoom != s_last_zoom
        || fabs(lat - s_last_lat) > 1e-5
        || fabs(lon - s_last_lon) > 1e-5) {
        s_last_lat  = lat;
        s_last_lon  = lon;
        s_last_zoom = s_zoom;
        rebuild_tiles(lat, lon, s_zoom);
    }
    // Old "No GPS fix" overlay is gone - the map is always interactive
    // now. Status label widget is kept around in case other code
    // references it but stays hidden.
    lv_obj_add_flag(status_label, LV_OBJ_FLAG_HIDDEN);

    // GPS marker means "you are here right now". Only show it when
    // the source is LIVE - saved-fix mode gives us a view centre but
    // doesn't tell us where the device currently is, so rendering a
    // marker there would be a lie.
    if (src != Source::LIVE) {
        lv_obj_add_flag(marker, LV_OBJ_FLAG_HIDDEN);
    } else if (s_manual_pan) {
        double dx_px =  (gps_lon - lon) / lon_per_px(s_zoom);
        double dy_px = -(gps_lat - lat) / lat_per_px(s_zoom, lat);
        int    mx    = (int)(MAP_W / 2.0 + dx_px) - 7;   // marker is 14x14
        int    my    = (int)(MAP_H / 2.0 + dy_px) - 7;
        if (mx < -14 || mx >= MAP_W || my < -14 || my >= MAP_H) {
            lv_obj_add_flag(marker, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_align(marker, LV_ALIGN_TOP_LEFT, mx, my);
            lv_obj_clear_flag(marker, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        lv_obj_center(marker);
        lv_obj_clear_flag(marker, LV_OBJ_FLAG_HIDDEN);
    }

    // Info badge with provenance suffix. Pre-format with snprintf and
    // pass to lv_label_set_text (rather than lv_label_set_text_fmt)
    // to dodge LVGL's builtin vsnprintf, which has trouble with %s
    // args trailing %.4f doubles on RV32 soft-float (same bug we hit
    // on the P4).
    const char *src_tag =
        (src == Source::LIVE)  ? "" :
        (src == Source::SAVED) ? "  (saved)" : "  (no fix)";
    const char *pan_tag = s_manual_pan ? "  *" : "";
    char info[64];
    snprintf(info, sizeof(info), "MAP  z%d  %.4f, %.4f%s%s",
             s_zoom, lat, lon, src_tag, pan_tag);
    lv_label_set_text(info_label, info);

    // Plot every other node we've heard a position from. Always uses
    // the current view centre (lat/lon, which is GPS or manual-pan
    // depending on s_manual_pan), so peer dots track pan and zoom
    // without any extra work.
    render_peer_nodes(lat, lon);
}

// ---- events ----------------------------------------------------------------

static void on_zoom_in(lv_event_t *)
{
    if (s_zoom < s_zoom_max) { s_zoom++; refresh(true); }
}

static void on_zoom_out(lv_event_t *)
{
    if (s_zoom > s_zoom_min) { s_zoom--; refresh(true); }
}

static void on_gesture(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir == LV_DIR_LEFT || dir == LV_DIR_RIGHT) {
        // The swipe also fed PRESSING events to on_pan, which may have
        // shifted the view a bit during the gesture. Clear that side
        // effect so coming back to the map snaps to GPS rather than
        // leaving it permanently nudged.
        s_manual_pan = false;
    }
    if      (dir == LV_DIR_LEFT)  send_message_screen_show();   // back
    else if (dir == LV_DIR_RIGHT) configuration_screen_show();  // forward
}

// Press-and-drag panning. Fires for every LVGL frame while the finger
// is moving on the map. lv_indev_get_vect returns the pixel delta since
// the previous event, which we convert to lat/lon shifts at the current
// zoom and apply to the view centre. First non-zero delta promotes us
// out of GPS-follow mode into s_manual_pan; the recentre button or a
// swipe-nav clears that flag.
static void on_pan(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    if (!indev) return;
    lv_point_t v;
    lv_indev_get_vect(indev, &v);
    if (v.x == 0 && v.y == 0) return;

    if (!s_manual_pan) {
        // Seed the view from the current GPS fix so the first drag
        // doesn't snap the map to the world origin.
        if (gps_screen_has_lock() && instance.gps.location.isValid()) {
            s_view_lat = instance.gps.location.lat();
            s_view_lon = instance.gps.location.lng();
        } else {
            // No fix - start from the last rendered centre, which is
            // sane (it was either GPS or our previous view value).
            s_view_lat = (s_last_lat < 360) ? s_last_lat : 0.0;
            s_view_lon = (s_last_lon < 360) ? s_last_lon : 0.0;
        }
        s_manual_pan = true;
    }
    // Drag finger right -> view shifts west (lon decreases).
    // Drag finger down  -> view shifts north (lat increases).
    s_view_lon -= (double)v.x * lon_per_px(s_zoom);
    s_view_lat += (double)v.y * lat_per_px(s_zoom, s_view_lat);
    refresh(true);
}

static void on_recentre(lv_event_t *)
{
    s_manual_pan = false;
    if (gps_screen_has_lock() && instance.gps.location.isValid()) {
        s_view_lat = instance.gps.location.lat();
        s_view_lon = instance.gps.location.lng();
    }
    refresh(true);
}

static void on_timer(lv_timer_t *)
{
    if (lv_screen_active() != map_screen) return;
    refresh(false);
}

// ---- layout ----------------------------------------------------------------

static lv_obj_t *make_round_btn(const char *text, lv_align_t align,
                                lv_coord_t ox, lv_coord_t oy,
                                lv_event_cb_t cb)
{
    lv_obj_t *b = lv_obj_create(map_screen);
    lv_obj_set_size(b, 52, 52);
    lv_obj_set_style_radius(b, 26, LV_PART_MAIN);
    lv_obj_set_style_bg_color(b, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(b, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_border_width(b, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(b, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_pad_all(b, 0, LV_PART_MAIN);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(b, align, ox, oy);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_color(l, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(l, &font_argus_label_28, LV_PART_MAIN);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    return b;
}

void map_screen_create()
{
    map_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(map_screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(map_screen, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(map_screen, 0, LV_PART_MAIN);
    lv_obj_clear_flag(map_screen, LV_OBJ_FLAG_SCROLLABLE);

    // 3x3 tile images — empty until refresh() loads them.
    for (int i = 0; i < TILES; i++) {
        tile_imgs[i] = lv_image_create(map_screen);
        lv_obj_set_size(tile_imgs[i], TILE_PX, TILE_PX);
        lv_obj_add_flag(tile_imgs[i], LV_OBJ_FLAG_HIDDEN);
        s_paths[i][0] = '\0';
    }

    // Centre marker — sits exactly over the watch's GPS location.
    marker = lv_obj_create(map_screen);
    lv_obj_set_size(marker, 14, 14);
    lv_obj_set_style_radius(marker, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(marker, lv_color_make(0xFF, 0x33, 0x33), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(marker, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(marker, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_border_width(marker, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(marker, 0, LV_PART_MAIN);
    lv_obj_clear_flag(marker, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(marker);

    // Peer-node marker pool. Smaller (10x10) and blue so they're
    // visually distinct from the red GPS-centre marker. Labels are
    // 12-pt Montserrat white-on-translucent-black; positioned just
    // below the dot at refresh time. All hidden by default.
    for (int i = 0; i < MESH_MAX_NODES; i++) {
        node_dots[i] = lv_obj_create(map_screen);
        lv_obj_set_size(node_dots[i], 10, 10);
        lv_obj_set_style_radius(node_dots[i], LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(node_dots[i], lv_color_make(0x0A, 0x84, 0xFF), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(node_dots[i], LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(node_dots[i], lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_border_width(node_dots[i], 1, LV_PART_MAIN);
        lv_obj_set_style_pad_all(node_dots[i], 0, LV_PART_MAIN);
        lv_obj_clear_flag(node_dots[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(node_dots[i], LV_OBJ_FLAG_HIDDEN);

        node_labels[i] = lv_label_create(map_screen);
        lv_obj_set_style_text_color(node_labels[i], lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_text_font(node_labels[i], &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_style_bg_color(node_labels[i], lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(node_labels[i], LV_OPA_60, LV_PART_MAIN);
        lv_obj_set_style_pad_hor(node_labels[i], 3, LV_PART_MAIN);
        lv_obj_set_style_pad_ver(node_labels[i], 0, LV_PART_MAIN);
        lv_obj_set_style_radius(node_labels[i], 3, LV_PART_MAIN);
        lv_label_set_text(node_labels[i], "");
        lv_obj_add_flag(node_labels[i], LV_OBJ_FLAG_HIDDEN);
    }

    // Info badge floating at the top: "MAP z<level> lat,lon"
    info_badge = lv_obj_create(map_screen);
    lv_obj_set_size(info_badge, LV_SIZE_CONTENT, 28);
    lv_obj_set_style_bg_color(info_badge, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(info_badge, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_border_width(info_badge, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(info_badge, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(info_badge, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(info_badge, 0, LV_PART_MAIN);
    lv_obj_clear_flag(info_badge, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(info_badge, LV_ALIGN_TOP_MID, 0, 8);

    info_label = lv_label_create(info_badge);
    lv_obj_set_style_text_color(info_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(info_label, &font_argus_label_14, LV_PART_MAIN);
    lv_label_set_text(info_label, "MAP");
    lv_obj_center(info_label);

    // Status overlay shown when there is no GPS fix.
    status_label = lv_label_create(map_screen);
    lv_obj_set_style_text_color(status_label, ARGUS_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(status_label, &font_argus_label_20, LV_PART_MAIN);
    lv_label_set_text(status_label, "No GPS fix\n(enable GPS to centre the map)");
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_center(status_label);

    // BOTTOM_LEFT / BOTTOM_RIGHT would land the buttons at roughly
    // (38, 464) and (372, 464) on the 410x502 framebuffer - both well
    // outside the ~410-diameter visible disc (centred at 205,251 with
    // radius ~205). Pull them inward to a 7 o'clock / 5 o'clock pair
    // centred on the screen: a 164 px radial offset keeps the button
    // edge ~15 px clear of the bezel and well inside the y=60..440
    // safe-content band documented in the README.
    zoom_out_btn = make_round_btn("-", LV_ALIGN_CENTER, -110, 215, on_zoom_out);
    zoom_in_btn  = make_round_btn("+", LV_ALIGN_CENTER,  110, 215, on_zoom_in);

    // Recentre / "lock to GPS" button - sits between the two zoom
    // buttons so the trio reads as a single map-control cluster.
    // GPS pin icon from the built-in LVGL symbol set.
    recentre_btn = make_round_btn(LV_SYMBOL_GPS, LV_ALIGN_CENTER, 0, 215,
                                  on_recentre);

    lv_obj_add_event_cb(map_screen, on_gesture, LV_EVENT_GESTURE, NULL);
    // Press-and-drag panning runs alongside swipe-nav. Fast directional
    // flicks still fire LV_EVENT_GESTURE for nav; slow drags accumulate
    // in on_pan via the indev vector.
    lv_obj_add_event_cb(map_screen, on_pan, LV_EVENT_PRESSING, NULL);
    lv_timer_create(on_timer, 2000, NULL);
}

void map_screen_show()
{
    main_loop_request_lvgl_priority(12);
    detect_zooms();
    if (s_zoom < s_zoom_min) s_zoom = s_zoom_min;
    if (s_zoom > s_zoom_max) s_zoom = s_zoom_max;
    // Pull the persisted last-known fix once (no-op on subsequent
    // entries). Lets the map open at the place we last had a position
    // even if GPS is currently off or unlocked.
    load_last_fix();
    refresh(true);
    lv_scr_load(map_screen);
}

bool map_screen_is_active()
{
    return lv_screen_active() == map_screen;
}

bool map_screen_available()
{
    if (!instance.isCardReady()) return false;
    return SD.exists("/map");
}
