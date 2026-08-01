/*
 * On-screen controls drawn over the video on Windows. See windows_osd.h.
 *
 * Geometry and timing are taken from renderers/uxvideosink/cocoawindow.m so the
 * two platforms present the same controls in the same places: a panel 36 px up
 * from the bottom, at most 600 px wide, one row when there is no timeline and
 * two when there is; a close button inset 22 px from the top right; controls
 * that stay up 2.5 s after the last interaction and fade over the final 0.5 s.
 *
 * The one deliberate difference is the coordinate space. Cocoa draws in view
 * coordinates, so its controls keep their size as the window is resized; a
 * cairooverlay draws into the frames themselves, so these are laid out in the
 * video's own pixels and scale with it instead. It also means no redraw timer
 * is needed: frames arrive while the video plays, and each one is a repaint.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <cairo.h>
#include <gst/video/navigation.h>
#include <gst/video/video-info.h>

#include "windows_osd.h"

/* All from cocoawindow.m. */
#define CONTROLS_VISIBLE_SECS   2.5
#define CONTROLS_FADE_SECS      0.5
#define CLOSE_BUTTON_MARGIN     22.0
#define CLOSE_BUTTON_SIZE       34.0
#define PLAYBACK_BAR_MARGIN     36.0
#define PLAYBACK_BAR_HEIGHT     44.0
#define PLAYBACK_BAR_MAX_WIDTH  600.0
#define PLAYBACK_BAR_MIN_WIDTH  560.0
#define PLAYBACK_TRACK_HEIGHT   6.0
#define PLAYBACK_CLOCK_WIDTH    52.0
#define PLAYBACK_TEXT_INSET     16.0
#define TRANSPORT_ICON          22.0
#define TRANSPORT_HIT           34.0
#define TRANSPORT_SPACING       48.0
#define TRANSPORT_VOLUME_WIDTH  104.0

typedef struct {
    double x, y, w, h;
} osd_rect_t;

static GMutex osd_lock;
static gboolean osd_ready = FALSE;

/* Guarded by osd_lock. */
static double osd_volume = 1.0;
static double osd_position = 0.0;
static double osd_duration = 0.0;
static double osd_rate = 1.0;
static gint64 controls_expiry = 0;      /* g_get_monotonic_time() units */
static int frame_width = 0;
static int frame_height = 0;
static gboolean volume_dragging = FALSE;
static gboolean scrubbing = FALSE;
static double scrub_position = 0.0;
/* The panel can be dragged off centre, as on macOS, for when it covers
   something the user wants to see. */
static double panel_offset_x = 0.0;
static double panel_offset_y = 0.0;
static gboolean panel_dragging = FALSE;
static double drag_anchor_x = 0.0;
static double drag_anchor_y = 0.0;
static double drag_offset_x = 0.0;
static double drag_offset_y = 0.0;

static void (*key_handler)(const char *key) = NULL;
static void (*close_handler)(void) = NULL;
static void (*fullscreen_handler)(void) = NULL;

/* ------------------------------------------------------------------ */

static void osd_init_once(void) {
    if (!osd_ready) {
        g_mutex_init(&osd_lock);
        osd_ready = TRUE;
    }
}

static void send_control_key(const char *key) {
    if (key_handler) {
        key_handler(key);
    }
}

/* Caller holds osd_lock. Returns FALSE when the frame is too narrow to hold the
   panel, which is what the macOS side does rather than cramming it in. */
static gboolean panel_rect(osd_rect_t *panel) {
    double w = frame_width - 2.0 * PLAYBACK_BAR_MARGIN;

    if (frame_width <= 0 || frame_height <= 0) {
        return FALSE;
    }
    if (w > PLAYBACK_BAR_MAX_WIDTH) {
        w = PLAYBACK_BAR_MAX_WIDTH;
    }
    if (w < PLAYBACK_BAR_MIN_WIDTH) {
        return FALSE;
    }
    panel->w = w;
    panel->h = (osd_duration > 0.0) ? 2.0 * PLAYBACK_BAR_HEIGHT : PLAYBACK_BAR_HEIGHT;
    panel->x = floor((frame_width - w) / 2.0) + panel_offset_x;
    panel->y = frame_height - PLAYBACK_BAR_MARGIN - panel->h + panel_offset_y;

    /* Keep it on screen however far it has been dragged. */
    if (panel->x < 8.0) {
        panel->x = 8.0;
    } else if (panel->x > frame_width - panel->w - 8.0) {
        panel->x = frame_width - panel->w - 8.0;
    }
    if (panel->y < 8.0) {
        panel->y = 8.0;
    } else if (panel->y > frame_height - panel->h - 8.0) {
        panel->y = frame_height - panel->h - 8.0;
    }
    return TRUE;
}

/* The row carrying the speaker, the volume slider and the transport glyphs.
   Cocoa's "top row" is the top of the panel in both spaces; only the sign of
   the y axis differs, and this is already in Cairo's. */
static osd_rect_t top_row(const osd_rect_t *panel) {
    osd_rect_t row = *panel;

    row.h = PLAYBACK_BAR_HEIGHT;
    return row;
}

static osd_rect_t volume_track(const osd_rect_t *panel) {
    osd_rect_t row = top_row(panel);
    osd_rect_t track;

    track.x = row.x + PLAYBACK_TEXT_INSET + TRANSPORT_ICON + 12.0;
    track.y = row.y + row.h / 2.0 - PLAYBACK_TRACK_HEIGHT / 2.0;
    track.w = TRANSPORT_VOLUME_WIDTH;
    track.h = PLAYBACK_TRACK_HEIGHT;
    return track;
}

/* -2 previous item, -1 back ten, 0 play/pause, 1 forward ten, 2 next item. */
static osd_rect_t transport_button(const osd_rect_t *panel, int which) {
    osd_rect_t row = top_row(panel);
    double cx = row.x + row.w / 2.0 + which * TRANSPORT_SPACING;
    osd_rect_t hit;

    hit.x = cx - TRANSPORT_HIT / 2.0;
    hit.y = row.y + row.h / 2.0 - TRANSPORT_HIT / 2.0;
    hit.w = TRANSPORT_HIT;
    hit.h = TRANSPORT_HIT;
    return hit;
}

static osd_rect_t fullscreen_button(const osd_rect_t *panel) {
    osd_rect_t row = top_row(panel);
    osd_rect_t r;

    r.x = row.x + row.w - PLAYBACK_TEXT_INSET - TRANSPORT_HIT;
    r.y = row.y + row.h / 2.0 - TRANSPORT_HIT / 2.0;
    r.w = TRANSPORT_HIT;
    r.h = TRANSPORT_HIT;
    return r;
}

static gboolean progress_track(const osd_rect_t *panel, osd_rect_t *track) {
    double x, w;

    if (osd_duration <= 0.0) {
        return FALSE;
    }
    x = panel->x + PLAYBACK_TEXT_INSET + PLAYBACK_CLOCK_WIDTH + 12.0;
    w = panel->w - 2.0 * (PLAYBACK_TEXT_INSET + PLAYBACK_CLOCK_WIDTH + 12.0);
    if (w < 40.0) {
        return FALSE;
    }
    track->x = x;
    track->w = w;
    track->y = panel->y + PLAYBACK_BAR_HEIGHT +
               PLAYBACK_BAR_HEIGHT / 2.0 - PLAYBACK_TRACK_HEIGHT / 2.0;
    track->h = PLAYBACK_TRACK_HEIGHT;
    return TRUE;
}

static osd_rect_t close_button_rect(void) {
    osd_rect_t r;

    r.x = frame_width - CLOSE_BUTTON_MARGIN - CLOSE_BUTTON_SIZE;
    r.y = CLOSE_BUTTON_MARGIN;
    r.w = CLOSE_BUTTON_SIZE;
    r.h = CLOSE_BUTTON_SIZE;
    return r;
}

static gboolean point_in(const osd_rect_t *r, double x, double y) {
    return (x >= r->x && x <= r->x + r->w && y >= r->y && y <= r->y + r->h);
}

/* Caller holds osd_lock. 0 when the controls are down. */
static double controls_alpha(void) {
    double left = (controls_expiry - g_get_monotonic_time()) / 1000000.0;

    if (left <= 0.0) {
        return 0.0;
    }
    return (left < CONTROLS_FADE_SECS) ? (left / CONTROLS_FADE_SECS) : 1.0;
}

static void clock_string(double seconds, char *out, size_t len) {
    long total = (long) (seconds < 0.0 ? 0.0 : seconds);

    if (total >= 3600) {
        snprintf(out, len, "%ld:%02ld:%02ld", total / 3600, (total % 3600) / 60, total % 60);
    } else {
        snprintf(out, len, "%ld:%02ld", total / 60, total % 60);
    }
}

/* ------------------------------------------------------------------ */

static void path_round_rect(cairo_t *cr, double x, double y, double w, double h,
                            double radius) {
    if (radius > w / 2.0) {
        radius = w / 2.0;
    }
    if (radius > h / 2.0) {
        radius = h / 2.0;
    }
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - radius, y + radius, radius, -G_PI_2, 0);
    cairo_arc(cr, x + w - radius, y + h - radius, radius, 0, G_PI_2);
    cairo_arc(cr, x + radius, y + h - radius, radius, G_PI_2, G_PI);
    cairo_arc(cr, x + radius, y + radius, radius, G_PI, 3 * G_PI_2);
    cairo_close_path(cr);
}

static void draw_slider(cairo_t *cr, const osd_rect_t *track, double fraction,
                        double alpha) {
    double filled;

    if (fraction < 0.0) {
        fraction = 0.0;
    } else if (fraction > 1.0) {
        fraction = 1.0;
    }
    filled = track->w * fraction;

    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.30 * alpha);
    path_round_rect(cr, track->x, track->y, track->w, track->h, track->h / 2.0);
    cairo_fill(cr);

    if (filled > 0.0) {
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.90 * alpha);
        path_round_rect(cr, track->x, track->y, filled, track->h, track->h / 2.0);
        cairo_fill(cr);
    }

    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.95 * alpha);
    cairo_arc(cr, track->x + filled, track->y + track->h / 2.0, track->h, 0, 2 * G_PI);
    cairo_fill(cr);
}

static void draw_speaker(cairo_t *cr, double x, double y, double size, double alpha) {
    double body = size * 0.45;

    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.95 * alpha);
    /* Cone. */
    cairo_move_to(cr, x + size * 0.08, y + size * 0.35);
    cairo_line_to(cr, x + size * 0.28, y + size * 0.35);
    cairo_line_to(cr, x + body, y + size * 0.12);
    cairo_line_to(cr, x + body, y + size * 0.88);
    cairo_line_to(cr, x + size * 0.28, y + size * 0.65);
    cairo_line_to(cr, x + size * 0.08, y + size * 0.65);
    cairo_close_path(cr);
    cairo_fill(cr);
    /* Two arcs, so it reads as a speaker rather than a flag. */
    cairo_set_line_width(cr, size * 0.08);
    cairo_arc(cr, x + body, y + size / 2.0, size * 0.22, -G_PI / 3.0, G_PI / 3.0);
    cairo_stroke(cr);
    cairo_arc(cr, x + body, y + size / 2.0, size * 0.36, -G_PI / 3.0, G_PI / 3.0);
    cairo_stroke(cr);
}

static void draw_triangle(cairo_t *cr, double cx, double cy, double size,
                          int direction) {
    double half = size / 2.0;

    cairo_move_to(cr, cx - direction * half, cy - half);
    cairo_line_to(cr, cx + direction * half, cy);
    cairo_line_to(cr, cx - direction * half, cy + half);
    cairo_close_path(cr);
    cairo_fill(cr);
}

static void fill_triangle(cairo_t *cr, double x1, double y1, double x2,
                          double y2, double x3, double y3) {
    cairo_move_to(cr, x1, y1);
    cairo_line_to(cr, x2, y2);
    cairo_line_to(cr, x3, y3);
    cairo_close_path(cr);
    cairo_fill(cr);
}

/* Vertex for vertex from drawTransportGlyphs in cocoawindow.m. Every glyph
   there is symmetric about its own centre line, so the y-axis flip between
   Cocoa and Cairo does not change any of these; only the fullscreen mark's
   stand has a below/above sense to it, and that is handled where it is drawn.
   The numbers are absolute, as they are on macOS: the panel is a fixed size in
   both, so glyphs sized off it would drift apart. */
static void draw_transport_glyphs(cairo_t *cr, const osd_rect_t *panel,
                                  double alpha, gboolean paused) {
    osd_rect_t hit;
    double cx, cy, s;

    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.95 * alpha);

    /* Previous item: a bar with a triangle running back into it. */
    hit = transport_button(panel, -2);
    cx = hit.x + hit.w / 2.0;
    cy = hit.y + hit.h / 2.0;
    s = 6.0;
    cairo_rectangle(cr, cx - s - 3.0, cy - s, 2.5, 2.0 * s);
    cairo_fill(cr);
    fill_triangle(cr, cx + s, cy + s, cx + s, cy - s, cx - s, cy);

    /* Skip back ten seconds. */
    hit = transport_button(panel, -1);
    cx = hit.x + hit.w / 2.0;
    cy = hit.y + hit.h / 2.0;
    s = 5.0;
    fill_triangle(cr, cx - 1.0, cy + s, cx - 1.0, cy - s, cx - 1.0 - s * 1.2, cy);
    fill_triangle(cr, cx + s * 1.2 + 1.0, cy + s, cx + s * 1.2 + 1.0, cy - s,
                  cx + 1.0, cy);

    /* Play or pause, whichever the stream is not doing now. */
    hit = transport_button(panel, 0);
    cx = hit.x + hit.w / 2.0;
    cy = hit.y + hit.h / 2.0;
    if (!paused) {
        cairo_rectangle(cr, cx - 6.0, cy - 8.0, 4.0, 16.0);
        cairo_rectangle(cr, cx + 2.0, cy - 8.0, 4.0, 16.0);
        cairo_fill(cr);
    } else {
        fill_triangle(cr, cx - 5.0, cy + 8.0, cx - 5.0, cy - 8.0, cx + 8.0, cy);
    }

    /* Skip forward ten seconds. */
    hit = transport_button(panel, 1);
    cx = hit.x + hit.w / 2.0;
    cy = hit.y + hit.h / 2.0;
    s = 5.0;
    fill_triangle(cr, cx - s * 1.2 - 1.0, cy + s, cx - s * 1.2 - 1.0, cy - s,
                  cx - 1.0, cy);
    fill_triangle(cr, cx + 1.0, cy + s, cx + 1.0, cy - s, cx + 1.0 + s * 1.2, cy);

    /* Next item: the previous-item glyph mirrored. */
    hit = transport_button(panel, 2);
    cx = hit.x + hit.w / 2.0;
    cy = hit.y + hit.h / 2.0;
    s = 6.0;
    fill_triangle(cr, cx - s, cy + s, cx - s, cy - s, cx + s, cy);
    cairo_rectangle(cr, cx + s + 0.5, cy - s, 2.5, 2.0 * s);
    cairo_fill(cr);
}

/* A screen outline with a stand, as macOS draws it. The one glyph whose y-axis
   sense matters: Cocoa puts the stand below the screen with a smaller y, Cairo
   with a larger one. */
static void draw_fullscreen_glyph(cairo_t *cr, const osd_rect_t *r, double alpha) {
    double cx = r->x + r->w / 2.0;
    double cy = r->y + r->h / 2.0 - 1.0;

    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.95 * alpha);
    cairo_set_line_width(cr, 1.6);
    cairo_rectangle(cr, cx - 9.0, cy - 6.0, 18.0, 12.0);
    cairo_stroke(cr);
    cairo_rectangle(cr, cx - 4.0, cy + 7.0, 8.0, 2.0);
    cairo_fill(cr);
}

static void draw_close_button(cairo_t *cr, double alpha) {
    osd_rect_t r = close_button_rect();
    double cx = r.x + r.w / 2.0;
    double cy = r.y + r.h / 2.0;
    double arm = r.w * 0.24;

    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.55 * alpha);
    cairo_arc(cr, cx, cy, r.w / 2.0, 0, 2 * G_PI);
    cairo_fill(cr);

    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.95 * alpha);
    cairo_set_line_width(cr, r.w * 0.10);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_move_to(cr, cx - arm, cy - arm);
    cairo_line_to(cr, cx + arm, cy + arm);
    cairo_move_to(cr, cx + arm, cy - arm);
    cairo_line_to(cr, cx - arm, cy + arm);
    cairo_stroke(cr);
}

static void draw_clocks(cairo_t *cr, const osd_rect_t *panel, double alpha) {
    char left[32], right[32];
    cairo_text_extents_t extents;
    double baseline = panel->y + PLAYBACK_BAR_HEIGHT + PLAYBACK_BAR_HEIGHT / 2.0 + 5.0;

    clock_string(scrubbing ? scrub_position : osd_position, left, sizeof(left));
    clock_string(osd_duration, right, sizeof(right));

    cairo_select_font_face(cr, "Segoe UI", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 15.0);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.90 * alpha);

    cairo_move_to(cr, panel->x + PLAYBACK_TEXT_INSET, baseline);
    cairo_show_text(cr, left);

    cairo_text_extents(cr, right, &extents);
    cairo_move_to(cr, panel->x + panel->w - PLAYBACK_TEXT_INSET - extents.width,
                  baseline);
    cairo_show_text(cr, right);
}

/* ------------------------------------------------------------------ */

static void on_draw(GstElement *overlay, cairo_t *cr, guint64 timestamp,
                    guint64 duration, gpointer user_data) {
    osd_rect_t panel, track;
    double alpha;
    gboolean have_panel;
    gboolean paused;
    double position, length, volume;
    gboolean scrub;

    (void) overlay;
    (void) timestamp;
    (void) duration;
    (void) user_data;

    g_mutex_lock(&osd_lock);
    alpha = controls_alpha();
    have_panel = panel_rect(&panel);
    position = scrubbing ? scrub_position : osd_position;
    length = osd_duration;
    volume = osd_volume;
    paused = (osd_rate == 0.0);
    scrub = scrubbing;
    g_mutex_unlock(&osd_lock);

    if (alpha <= 0.0) {
        return;
    }

    cairo_save(cr);
    draw_close_button(cr, alpha);

    if (have_panel) {
        osd_rect_t vol = volume_track(&panel);
        osd_rect_t row = top_row(&panel);

        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.55 * alpha);
        path_round_rect(cr, panel.x, panel.y, panel.w, panel.h, 16.0);
        cairo_fill(cr);

        draw_speaker(cr, row.x + PLAYBACK_TEXT_INSET,
                     row.y + row.h / 2.0 - TRANSPORT_ICON / 2.0,
                     TRANSPORT_ICON, alpha);
        draw_slider(cr, &vol, volume, alpha);

        {
            osd_rect_t full = fullscreen_button(&panel);

            draw_fullscreen_glyph(cr, &full, alpha);
        }

        /* Mirroring reports no duration: no scrubber, no transport, and the
           panel is the single volume row drawn above. */
        if (length > 0.0) {
            draw_transport_glyphs(cr, &panel, alpha, paused);
            draw_clocks(cr, &panel, alpha);
            if (progress_track(&panel, &track)) {
                draw_slider(cr, &track, position / length, alpha);
            }
        }
    }
    cairo_restore(cr);
    (void) scrub;
}

static void on_caps_changed(GstElement *overlay, GstCaps *caps,
                            gpointer user_data) {
    GstVideoInfo info;

    (void) overlay;
    (void) user_data;
    if (!gst_video_info_from_caps(&info, caps)) {
        return;
    }
    g_mutex_lock(&osd_lock);
    frame_width = GST_VIDEO_INFO_WIDTH(&info);
    frame_height = GST_VIDEO_INFO_HEIGHT(&info);
    g_mutex_unlock(&osd_lock);
}

static void connect_overlay(GstElement *overlay) {
    g_signal_connect(overlay, "draw", G_CALLBACK(on_draw), NULL);
    g_signal_connect(overlay, "caps-changed", G_CALLBACK(on_caps_changed), NULL);
}

/* ------------------------------------------------------------------ */

GstElement *windows_osd_create_filter(void) {
    GstElement *bin, *pre, *overlay, *post;
    GstPad *pad;

    osd_init_once();

    overlay = gst_element_factory_make("cairooverlay", "uxosd");
    if (!overlay) {
        return NULL;
    }
    pre = gst_element_factory_make("videoconvert", NULL);
    post = gst_element_factory_make("videoconvert", NULL);
    if (!pre || !post) {
        gst_object_unref(overlay);
        if (pre) {
            gst_object_unref(pre);
        }
        if (post) {
            gst_object_unref(post);
        }
        return NULL;
    }

    bin = gst_bin_new("uxosd-filter");
    gst_bin_add_many(GST_BIN(bin), pre, overlay, post, NULL);
    if (!gst_element_link_many(pre, overlay, post, NULL)) {
        gst_object_unref(bin);
        return NULL;
    }

    pad = gst_element_get_static_pad(pre, "sink");
    gst_element_add_pad(bin, gst_ghost_pad_new("sink", pad));
    gst_object_unref(pad);
    pad = gst_element_get_static_pad(post, "src");
    gst_element_add_pad(bin, gst_ghost_pad_new("src", pad));
    gst_object_unref(pad);

    connect_overlay(overlay);
    return bin;
}

void windows_osd_attach(GstElement *pipeline, const char *element_name) {
    GstElement *overlay;

    osd_init_once();
    if (!pipeline) {
        return;
    }
    overlay = gst_bin_get_by_name(GST_BIN(pipeline), element_name);
    if (!overlay) {
        return;
    }
    connect_overlay(overlay);
    gst_object_unref(overlay);
}

void windows_osd_set_key_handler(void (*handler)(const char *key)) {
    key_handler = handler;
}

void windows_osd_set_close_handler(void (*handler)(void)) {
    close_handler = handler;
}

void windows_osd_set_fullscreen_handler(void (*handler)(void)) {
    fullscreen_handler = handler;
}

void windows_osd_note_activity(void) {
    osd_init_once();
    g_mutex_lock(&osd_lock);
    controls_expiry = g_get_monotonic_time() +
                      (gint64) (CONTROLS_VISIBLE_SECS * 1000000.0);
    g_mutex_unlock(&osd_lock);
}

void windows_osd_set_volume(double fraction) {
    osd_init_once();
    g_mutex_lock(&osd_lock);
    osd_volume = (fraction < 0.0) ? 0.0 : ((fraction > 1.0) ? 1.0 : fraction);
    g_mutex_unlock(&osd_lock);
    /* Changing the volume brings up the same controls moving the mouse does,
       on the same timer -- as on macOS. */
    windows_osd_note_activity();
}

void windows_osd_set_playback_info(double position, double length, double rate) {
    osd_init_once();
    g_mutex_lock(&osd_lock);
    osd_position = position;
    osd_duration = length;
    osd_rate = rate;
    g_mutex_unlock(&osd_lock);
}

void windows_osd_set_stream_active(bool active) {
    osd_init_once();
    if (active) {
        return;
    }
    g_mutex_lock(&osd_lock);
    osd_position = 0.0;
    osd_duration = 0.0;
    scrubbing = FALSE;
    volume_dragging = FALSE;
    controls_expiry = 0;
    g_mutex_unlock(&osd_lock);
}

/* ------------------------------------------------------------------ */

/* Caller holds osd_lock. */
static double fraction_along(const osd_rect_t *track, double x) {
    double f;

    if (track->w <= 0.0) {
        return 0.0;
    }
    f = (x - track->x) / track->w;
    return (f < 0.0) ? 0.0 : ((f > 1.0) ? 1.0 : f);
}

/* Caller holds osd_lock. A control that fires immediately reports itself
   through *pending_key, which the caller sends once the lock is dropped: the
   handler reaches back into the renderer and must not run underneath it. */
static gboolean begin_interaction(double x, double y, const char **pending_key,
                                  gboolean *want_fullscreen) {
    osd_rect_t panel, track, vol, vol_hit, full;

    if (!panel_rect(&panel)) {
        return FALSE;
    }
    if (!point_in(&panel, x, y)) {
        return FALSE;
    }

    full = fullscreen_button(&panel);
    if (point_in(&full, x, y)) {
        *want_fullscreen = TRUE;
        return TRUE;
    }

    vol = volume_track(&panel);
    vol_hit = vol;
    /* Widened the same way the macOS hit test widens it, so the slider is not a
       hairline target. */
    vol_hit.x -= 8.0;
    vol_hit.y -= 16.0;
    vol_hit.w += 16.0;
    vol_hit.h += 32.0;

    if (point_in(&vol_hit, x, y)) {
        volume_dragging = TRUE;
        osd_volume = fraction_along(&vol, x);
        return TRUE;
    }

    if (osd_duration > 0.0 && progress_track(&panel, &track)) {
        osd_rect_t hit = track;

        /* Only the track itself: the clock readings sit at either end of the
           row, and treating those as scrubber meant a click on the total time
           seeked to the end of the video, which ends the stream. */
        hit.x -= 8.0;
        hit.w += 16.0;
        hit.y = panel.y + PLAYBACK_BAR_HEIGHT;
        hit.h = PLAYBACK_BAR_HEIGHT;
        if (point_in(&hit, x, y)) {
            scrubbing = TRUE;
            scrub_position = fraction_along(&track, x) * osd_duration;
            return TRUE;
        }
    }

    if (osd_duration > 0.0) {
        static const char *const button_key[] = {
            "uxplay-previtem", "uxplay-skip:-10", "uxplay-playpause",
            "uxplay-skip:10", "uxplay-nextitem"
        };
        int i;

        for (i = -2; i <= 2; i++) {
            osd_rect_t hit = transport_button(&panel, i);

            if (point_in(&hit, x, y)) {
                *pending_key = button_key[i + 2];
                return TRUE;
            }
        }
    }

    /* Empty part of the panel: drag it, as on macOS, for when it sits over
       something the user wants to see. */
    panel_dragging = TRUE;
    drag_anchor_x = x;
    drag_anchor_y = y;
    drag_offset_x = panel_offset_x;
    drag_offset_y = panel_offset_y;
    return TRUE;
}

bool windows_osd_handle_navigation(GstEvent *event) {
    GstNavigationEventType type;
    gdouble x = 0.0, y = 0.0;
    gint button = 0;
    gboolean consumed = FALSE;
    char key[64];
    double commit_volume = -1.0;
    double commit_seek = -1.0;
    const char *pending_key = NULL;
    gboolean want_fullscreen = FALSE;

    osd_init_once();
    type = gst_navigation_event_get_type(event);

    switch (type) {
    case GST_NAVIGATION_EVENT_MOUSE_MOVE:
        if (!gst_navigation_event_parse_mouse_move_event(event, &x, &y)) {
            return false;
        }
        windows_osd_note_activity();
        g_mutex_lock(&osd_lock);
        if (volume_dragging) {
            osd_rect_t panel;

            if (panel_rect(&panel)) {
                osd_rect_t vol = volume_track(&panel);

                osd_volume = fraction_along(&vol, x);
                commit_volume = osd_volume;
            }
            consumed = TRUE;
        } else if (scrubbing) {
            osd_rect_t panel, track;

            if (panel_rect(&panel) && progress_track(&panel, &track)) {
                scrub_position = fraction_along(&track, x) * osd_duration;
            }
            consumed = TRUE;
        } else if (panel_dragging) {
            panel_offset_x = drag_offset_x + (x - drag_anchor_x);
            panel_offset_y = drag_offset_y + (y - drag_anchor_y);
            consumed = TRUE;
        }
        g_mutex_unlock(&osd_lock);
        break;

    case GST_NAVIGATION_EVENT_MOUSE_BUTTON_PRESS:
        if (!gst_navigation_event_parse_mouse_button_event(event, &button, &x, &y)) {
            return false;
        }
        if (button != 1) {
            return false;
        }
        g_mutex_lock(&osd_lock);
        /* Only while the controls are actually showing, so a click on the
           picture is not swallowed by an invisible target. */
        if (controls_alpha() > 0.0) {
            osd_rect_t close = close_button_rect();

            if (point_in(&close, x, y)) {
                g_mutex_unlock(&osd_lock);
                windows_osd_note_activity();
                /* Go out through the window, the way the title bar's X does.
                   That path is a WM_CLOSE handled in the window procedure on
                   the window's own thread, and it ends the session whatever
                   the pipeline is doing. Reporting "uxplay-disconnect" here
                   instead put the close on the same road the click came in
                   on -- sink, navigation event, pipeline bus, main loop --
                   so the one button meant for a stuck session needed a
                   working one to be pressed at all. */
                if (close_handler) {
                    close_handler();
                } else {
                    send_control_key("uxplay-disconnect");
                }
                return true;
            }
            consumed = begin_interaction(x, y, &pending_key, &want_fullscreen);
            if (volume_dragging) {
                commit_volume = osd_volume;
            }
        }
        g_mutex_unlock(&osd_lock);
        windows_osd_note_activity();
        break;

    case GST_NAVIGATION_EVENT_MOUSE_BUTTON_RELEASE:
        if (!gst_navigation_event_parse_mouse_button_event(event, &button, &x, &y)) {
            return false;
        }
        g_mutex_lock(&osd_lock);
        if (scrubbing) {
            /* Committed on release rather than continuously: every seek flushes
               the pipeline, and a drag would otherwise fire dozens of them. */
            commit_seek = scrub_position;
            scrubbing = FALSE;
            osd_position = scrub_position;
            consumed = TRUE;
        }
        if (volume_dragging) {
            volume_dragging = FALSE;
            consumed = TRUE;
        }
        if (panel_dragging) {
            panel_dragging = FALSE;
            consumed = TRUE;
        }
        g_mutex_unlock(&osd_lock);
        break;

    default:
        return false;
    }

    if (want_fullscreen && fullscreen_handler) {
        fullscreen_handler();
    }
    if (pending_key) {
        send_control_key(pending_key);
    }
    if (commit_volume >= 0.0) {
        g_snprintf(key, sizeof(key), "uxplay-volume:%.4f", commit_volume);
        send_control_key(key);
    }
    if (commit_seek >= 0.0) {
        g_snprintf(key, sizeof(key), "uxplay-seek:%.3f", commit_seek);
        send_control_key(key);
    }
    return consumed ? true : false;
}
