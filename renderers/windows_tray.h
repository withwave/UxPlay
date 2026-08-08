/*
 * Windows notification-area ("system tray") item for UxPlay.
 *
 * The Windows counterpart of renderers/macos_statusbar.h, and deliberately the
 * same interface: uxplay.cpp calls these from shared code and only the backend
 * differs. The reasoning is the same one given there -- the video window is not
 * a reliable place to show whether the server is busy, since an AirPlay Audio
 * session never opens one and the window can be hidden or closed -- so the
 * state goes somewhere visible for every kind of session.
 *
 * Where the two platforms differ is in what a menu can hold. AppKit lets a menu
 * item carry a live NSSlider, so macOS shows volume and playback position as
 * sliders that can be dragged. A Win32 popup menu holds only items, so the
 * sliders become submenus of discrete steps and the position is shown as a
 * "1:23 / 4:56" label above them. The information and the actions are the same;
 * only the granularity of the input differs.
 *
 * Every function is safe to call from UxPlay's worker threads; the window and
 * the icon belong to a thread of their own and the work is forwarded to it
 * internally. On anything but Windows these are not compiled in and the callers
 * guard with the shared UXPLAY_STATUSBAR macro.
 */

#ifndef WINDOWS_TRAY_H
#define WINDOWS_TRAY_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Kind of session currently being served. */
typedef enum {
    STATUSBAR_IDLE = 0,     /* no client */
    STATUSBAR_AUDIO,        /* AirPlay Audio: no video window exists */
    STATUSBAR_MIRROR,       /* AirPlay Mirror */
    STATUSBAR_VIDEO,        /* AirPlay Video: HLS stream with a timeline */
} statusbar_state_t;

/* Creates the tray icon. Safe to call more than once. */
void statusbar_init(void);

/* Name of the connected client, as reported by the AirPlay handshake, or NULL
   to clear it. */
void statusbar_set_client(const char *name, const char *model);

void statusbar_set_state(statusbar_state_t state);

/* Now playing, for AirPlay Audio sessions. Either may be NULL. */
void statusbar_set_metadata(const char *title, const char *artist);

/* Position of the volume control, 0.0 - 1.0. Call this when the client changes
   the volume so the menu keeps up; it does not call back into UxPlay. */
void statusbar_set_volume(double fraction);

/* Invoked when the volume is changed from the menu, with the same 0.0 - 1.0
   scale. */
void statusbar_set_volume_handler(void (*handler)(double fraction));

/* Which display the video window sits on, as an index into the monitors the
   system reports. The menu hides itself when there is only one. */
void statusbar_set_display_handler(void (*handler)(int index));

/* Invoked when "Disconnect" is chosen. Expected to end the session and leave
   the server advertising. */
void statusbar_set_disconnect_handler(void (*handler)(void));

/* Playback progress, in seconds. A duration of 0 means the media is not
   seekable -- mirroring has no timeline -- and the rows are hidden. */
void statusbar_set_progress(double position, double duration);

/* Invoked when a seek is chosen from the menu, with a position in seconds. */
void statusbar_set_seek_handler(void (*handler)(double position));

/* Invoked when "Quit UxPlay" is chosen. This one has no macOS counterpart:
   there the menu raises SIGINT and the existing handler does the rest, which is
   not available here -- UxPlay on Windows takes its shutdown from a console
   control handler that only the console can invoke. The caller supplies the
   same teardown instead. */
void statusbar_set_quit_handler(void (*handler)(void));

/* uxplay is a console program, so a console window comes with it, and run from
   the tray it is nothing but clutter. Called once at startup with whether debug
   logging is on: without it the console is put away, with it left up, since
   that is the only place the log appears. It can be brought back either way
   from the tray menu. Does nothing when the console belongs to a terminal the
   user started uxplay from -- hiding that would take their shell with it. */
void statusbar_setup_console(bool debug_log);

/* Backs the menu's "Fullscreen on connect" item. The setting belongs to the
   video renderer, which is what acts on it; the menu only reads it to draw the
   tick and writes it when clicked. */
void statusbar_set_fullscreen_on_connect_hooks(bool (*get)(void),
                                               void (*set)(bool enable));

/* Whether the client is told this receiver plays video itself. Off keeps a
   mirrored screen mirrored instead of handing playback over. */
void statusbar_set_hls_handover_hooks(bool (*get)(void),
                                      void (*set)(bool enable));

/* The pairing pin, shown for as long as one is in force. 0 hides it. */
void statusbar_set_pin(int pin);
void statusbar_show_pin_dialog(int pin);

void statusbar_destroy(void);

#ifdef __cplusplus
}
#endif

#endif /* WINDOWS_TRAY_H */
