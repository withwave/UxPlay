/**
 * Copyright (c) 2026 UxPlay contributors
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

#include "dacp_remote.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>

#ifdef HAVE_DNS_SD

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#define DACP_CLOSESOCKET closesocket
#else
#include <sys/socket.h>
#include <sys/select.h>
#include <netdb.h>
#include <arpa/inet.h>
#define DACP_CLOSESOCKET close
#endif

#include <dns_sd.h>

#define DACP_SERVICE_TYPE  "_dacp._tcp"
#define DACP_DOMAIN        "local."
#define DACP_RESOLVE_SECS  3

/* Written once when the client introduces itself, read by the sender thread.
   Both happen rarely and a torn read would only cost one command, but the
   thread takes its own copy under the lock so a client swapping out mid-flight
   cannot leave it reading a half-updated id. */
static pthread_mutex_t dacp_lock = PTHREAD_MUTEX_INITIALIZER;
static char dacp_id[64] = { 0 };
static char dacp_active_remote[64] = { 0 };

void dacp_remote_set_client(const char *id, const char *active_remote) {
    if (!id || !active_remote) {
        return;
    }
    pthread_mutex_lock(&dacp_lock);
    snprintf(dacp_id, sizeof(dacp_id), "%s", id);
    snprintf(dacp_active_remote, sizeof(dacp_active_remote), "%s", active_remote);
    pthread_mutex_unlock(&dacp_lock);
}

void dacp_remote_clear(void) {
    pthread_mutex_lock(&dacp_lock);
    dacp_id[0] = '\0';
    dacp_active_remote[0] = '\0';
    pthread_mutex_unlock(&dacp_lock);
}

bool dacp_remote_available(void) {
    bool available;

    pthread_mutex_lock(&dacp_lock);
    available = (dacp_id[0] != '\0' && dacp_active_remote[0] != '\0');
    pthread_mutex_unlock(&dacp_lock);
    return available;
}

typedef struct {
    char host[256];
    uint16_t port;
    bool resolved;
} dacp_resolve_t;

static void DNSSD_API
dacp_resolve_reply(DNSServiceRef sdRef, DNSServiceFlags flags, uint32_t interface_index,
                   DNSServiceErrorType error, const char *fullname, const char *hosttarget,
                   uint16_t port, uint16_t txt_len, const unsigned char *txt, void *context) {
    dacp_resolve_t *out = (dacp_resolve_t *) context;

    (void) sdRef; (void) flags; (void) interface_index; (void) fullname;
    (void) txt_len; (void) txt;
    if (error != kDNSServiceErr_NoError || !hosttarget) {
        return;
    }
    snprintf(out->host, sizeof(out->host), "%s", hosttarget);
    out->port = ntohs(port);
    out->resolved = true;
}

/* Blocks for up to DACP_RESOLVE_SECS. */
static bool dacp_resolve(const char *id, dacp_resolve_t *out) {
    char name[128];
    DNSServiceRef sdRef = NULL;
    DNSServiceErrorType error;
    int fd;

    memset(out, 0, sizeof(*out));
    snprintf(name, sizeof(name), "iTunes_Ctrl_%s", id);

    error = DNSServiceResolve(&sdRef, 0, kDNSServiceInterfaceIndexAny, name,
                              DACP_SERVICE_TYPE, DACP_DOMAIN, dacp_resolve_reply, out);
    if (error != kDNSServiceErr_NoError) {
        return false;
    }

    fd = DNSServiceRefSockFD(sdRef);
    while (!out->resolved && fd >= 0) {
        struct timeval tv = { DACP_RESOLVE_SECS, 0 };
        fd_set set;

        FD_ZERO(&set);
        FD_SET(fd, &set);
        if (select(fd + 1, &set, NULL, NULL, &tv) <= 0) {
            break;
        }
        if (DNSServiceProcessResult(sdRef) != kDNSServiceErr_NoError) {
            break;
        }
    }
    DNSServiceRefDeallocate(sdRef);
    return out->resolved;
}

/* Returns the HTTP status, or -1. */
static int dacp_get(const dacp_resolve_t *addr, const char *command, const char *active_remote) {
    struct addrinfo hints, *list = NULL, *ai;
    char port_str[8], request[512], reply[128];
    int sock = -1, status = -1;
    ssize_t n;

    snprintf(port_str, sizeof(port_str), "%u", (unsigned) addr->port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(addr->host, port_str, &hints, &list) || !list) {
        return -1;
    }
    for (ai = list; ai; ai = ai->ai_next) {
        sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (sock < 0) {
            continue;
        }
        if (!connect(sock, ai->ai_addr, ai->ai_addrlen)) {
            break;
        }
        DACP_CLOSESOCKET(sock);
        sock = -1;
    }
    freeaddrinfo(list);
    if (sock < 0) {
        return -1;
    }

    n = snprintf(request, sizeof(request),
                 "GET /ctrl-int/1/%s HTTP/1.1\r\n"
                 "Host: %s:%u\r\n"
                 "Active-Remote: %s\r\n"
                 "User-Agent: UxPlay\r\n"
                 "Connection: close\r\n"
                 "\r\n",
                 command, addr->host, (unsigned) addr->port, active_remote);
    if (n > 0 && send(sock, request, (size_t) n, 0) == n) {
        n = recv(sock, reply, sizeof(reply) - 1, 0);
        if (n > 0) {
            reply[n] = '\0';
            /* "HTTP/1.1 200 OK" -- only the code is of any use here. */
            if (!strncmp(reply, "HTTP/1.", 7) && n > 12) {
                status = atoi(reply + 9);
            }
        }
    }
    DACP_CLOSESOCKET(sock);
    return status;
}

typedef struct {
    logger_t *logger;
    char command[32];
    char id[64];
    char active_remote[64];
} dacp_job_t;

static void *dacp_send_thread(void *arg) {
    dacp_job_t *job = (dacp_job_t *) arg;
    dacp_resolve_t addr;
    int status;

    if (!dacp_resolve(job->id, &addr)) {
        logger_log(job->logger, LOGGER_WARNING,
                   "dacp_remote: could not resolve iTunes_Ctrl_%s%s; \"%s\" not sent",
                   job->id, "." DACP_SERVICE_TYPE "." DACP_DOMAIN, job->command);
        free(job);
        return NULL;
    }
    status = dacp_get(&addr, job->command, job->active_remote);
    if (status == 200) {
        logger_log(job->logger, LOGGER_DEBUG, "dacp_remote: %s -> %s:%u OK",
                   job->command, addr.host, (unsigned) addr.port);
    } else {
        logger_log(job->logger, LOGGER_WARNING, "dacp_remote: %s -> %s:%u failed (status %d)",
                   job->command, addr.host, (unsigned) addr.port, status);
    }
    free(job);
    return NULL;
}

void dacp_remote_send(logger_t *logger, const char *command) {
    dacp_job_t *job;
    pthread_t thread;
    pthread_attr_t attr;

    if (!command) {
        return;
    }
    job = (dacp_job_t *) calloc(1, sizeof(dacp_job_t));
    if (!job) {
        return;
    }
    job->logger = logger;
    snprintf(job->command, sizeof(job->command), "%s", command);

    pthread_mutex_lock(&dacp_lock);
    snprintf(job->id, sizeof(job->id), "%s", dacp_id);
    snprintf(job->active_remote, sizeof(job->active_remote), "%s", dacp_active_remote);
    pthread_mutex_unlock(&dacp_lock);

    if (!job->id[0] || !job->active_remote[0]) {
        logger_log(logger, LOGGER_WARNING,
                   "dacp_remote: no client remote-control details yet; \"%s\" not sent", command);
        free(job);
        return;
    }

    /* Detached: nothing waits for the result, and the caller is a UI thread. */
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&thread, &attr, dacp_send_thread, job)) {
        free(job);
    }
    pthread_attr_destroy(&attr);
}

#else  /* !HAVE_DNS_SD */

/* The bundled mdnsd registers services but cannot browse or resolve, so the
   client's DACP control server cannot be found. Everything still builds and the
   commands simply report that they are unavailable. */

void dacp_remote_set_client(const char *id, const char *active_remote) {
    (void) id; (void) active_remote;
}

void dacp_remote_clear(void) {
}

bool dacp_remote_available(void) {
    return false;
}

void dacp_remote_send(logger_t *logger, const char *command) {
    logger_log(logger, LOGGER_WARNING,
               "dacp_remote: \"%s\" needs dns_sd service resolution, which this build does not have",
               command ? command : "");
}

#endif  /* HAVE_DNS_SD */
