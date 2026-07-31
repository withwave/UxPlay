/*
 * macOS menu bar status item for UxPlay.
 *
 * The video window is not a reliable place to show whether the server is busy:
 * an AirPlay Audio session never opens one, and the window can be hidden or
 * closed. This puts the state in the menu bar instead, where it is visible for
 * every kind of session.
 *
 * Every function is safe to call from UxPlay's worker thread; the AppKit work
 * is forwarded to the main thread internally. On anything but macOS these are
 * not compiled in and the callers guard with __APPLE__.
 */

#ifndef MACOS_STATUSBAR_H
#define MACOS_STATUSBAR_H

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

/* Creates the menu bar item. Safe to call more than once. */
void statusbar_init(void);

/* Name of the connected client, as reported by the AirPlay handshake, or NULL
   to clear it. */
void statusbar_set_client(const char *name, const char *model);

void statusbar_set_state(statusbar_state_t state);

/* Now playing, for AirPlay Audio sessions. Either may be NULL. */
void statusbar_set_metadata(const char *title, const char *artist);

/* Position of the volume slider, 0.0 - 1.0. Call this when the client changes
   the volume so the menu keeps up; it does not call back into UxPlay. */
void statusbar_set_volume(double fraction);

/* Invoked when the volume slider is dragged, with the same 0.0 - 1.0 scale. */
void statusbar_set_volume_handler(void (*handler)(double fraction));

/* Which display the video window sits on, as an index into the screens the
   system reports. The menu hides itself when there is only one. */
void statusbar_set_display_handler(void (*handler)(int index));

/* Invoked when "Disconnect" is chosen. Expected to end the session and leave
   the server advertising. */
void statusbar_set_disconnect_handler(void (*handler)(void));

/* Playback progress, in seconds. A duration of 0 means the media is not
   seekable -- mirroring has no timeline -- and the row is hidden. */
void statusbar_set_progress(double position, double duration);

/* Invoked when the progress slider is dragged, with a position in seconds. */
void statusbar_set_seek_handler(void (*handler)(double position));

void statusbar_destroy(void);

#ifdef __cplusplus
}
#endif

#endif /* MACOS_STATUSBAR_H */
