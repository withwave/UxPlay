/**
 * Sending remote-control commands back to the client over DACP.
 *
 * The client hands us a DACP-ID and an Active-Remote token on its first
 * RTSP request, and advertises a control server as
 * "iTunes_Ctrl_<DACP-ID>._dacp._tcp". Resolving that and issuing
 * GET /ctrl-int/1/<command> with the token is how a receiver drives the
 * client's own playlist -- which is the only way to reach "previous item"
 * and "next item": nothing in the AirPlay HTTP channel carries them.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 */

#ifndef DACP_REMOTE_H
#define DACP_REMOTE_H

#include <stdbool.h>
#include "logger.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Remembered from the client's DACP-ID and Active-Remote headers. */
void dacp_remote_set_client(const char *dacp_id, const char *active_remote);
void dacp_remote_clear(void);
bool dacp_remote_available(void);

/* "nextitem", "previtem", ... Resolving the service and the request itself
   both block, so this hands the work to a detached thread and returns at
   once: it is called from the window's input path and from the menu bar,
   neither of which can afford to wait. */
void dacp_remote_send(logger_t *logger, const char *command);

#ifdef __cplusplus
}
#endif

#endif
