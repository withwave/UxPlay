/*
 * On-screen controls drawn over the video on Windows.
 *
 * The Windows counterpart of what renderers/uxvideosink draws on macOS: the
 * transport panel, the volume slider, the progress track and the close button
 * that appear over the picture when the mouse moves, and fade out again.
 *
 * macOS gets these by patching the videosink -- uxvideosink owns its Cocoa
 * window and draws into it with OpenGL. The stock Windows sinks own their
 * window and cannot be told to draw anything, so the controls are composited
 * into the frames instead, through a cairooverlay placed ahead of the sink.
 * Input arrives the other way round, as GstNavigation mouse events the sink
 * sends back up the pipeline.
 *
 * The two ends meet where they already did: every control reports itself by
 * synthesising the same "uxplay-..." key strings the Cocoa panel sends, which
 * video_window_key_pressed() in uxplay.cpp has always handled without caring
 * which platform they came from.
 */

#ifndef WINDOWS_OSD_H
#define WINDOWS_OSD_H

#include <gst/gst.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A bin holding "videoconvert ! cairooverlay ! videoconvert", ready to be
   handed to playbin's video-filter. Returns NULL if cairooverlay is missing,
   which leaves the pipeline without controls rather than without video. */
GstElement *windows_osd_create_filter(void);

/* Take over a cairooverlay that is already in a pipeline, by element name.
   Used for the mirroring pipeline, which is built from a launch string. */
void windows_osd_attach(GstElement *pipeline, const char *element_name);

/* Called with the "uxplay-..." string a control produced. Same handler the
   videosink's key events go to. */
void windows_osd_set_key_handler(void (*handler)(const char *key));

/* 0.0 - 1.0. Also counts as user activity, so changing the volume brings the
   controls up exactly as moving the mouse does -- the macOS behaviour. */
void windows_osd_set_volume(double fraction);

/* Seconds. A duration of 0 means there is no timeline -- mirroring -- and the
   panel shrinks to the volume row alone. */
void windows_osd_set_playback_info(double position, double duration, double rate);

/* FALSE when a session ends: clears the timeline so a stale progress bar is
   not left over the next session's first frames. */
void windows_osd_set_stream_active(bool active);

/* Bring the controls up for their usual few seconds. */
void windows_osd_note_activity(void);

/* Invoked by the panel's fullscreen button. macOS toggles its view's fill-screen
   mode from here; the caller supplies whatever the Windows sink needs. */
void windows_osd_set_fullscreen_handler(void (*handler)(void));

/* Feed a navigation event from the videosink. Returns true when the event was
   a control interaction and should not be treated as anything else. */
bool windows_osd_handle_navigation(GstEvent *event);

#ifdef __cplusplus
}
#endif

#endif /* WINDOWS_OSD_H */
