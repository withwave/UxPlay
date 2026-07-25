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
} statusbar_state_t;

/* Creates the menu bar item. Safe to call more than once. */
void statusbar_init(void);

/* Name of the connected client, as reported by the AirPlay handshake, or NULL
   to clear it. */
void statusbar_set_client(const char *name, const char *model);

void statusbar_set_state(statusbar_state_t state);

void statusbar_destroy(void);

#ifdef __cplusplus
}
#endif

#endif /* MACOS_STATUSBAR_H */
