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
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#define DACP_CLOSESOCKET closesocket
#else
#include <sys/socket.h>
#include <sys/select.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#define DACP_CLOSESOCKET close
#endif

#ifdef HAVE_DNS_SD
#include <dns_sd.h>
#endif

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

#ifdef HAVE_DNS_SD

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

#else  /* !HAVE_DNS_SD */

/* The bundled mdnsd registers services but cannot browse or resolve, so this
   asks the question itself: one multicast DNS query for the SRV record of
   iTunes_Ctrl_<id>._dacp._tcp.local, and the port and address out of what comes
   back. That is all DNSServiceResolve() is doing above, and it is little enough
   to be worth doing by hand rather than taking on Bonjour as a dependency --
   which is what the rest of UxPlay moved away from when it adopted mdnsd.
 *
 * The query sets the unicast-response bit, so replies arrive directly on this
 * socket rather than on port 5353, which mdnsd already holds. */

#define MDNS_PORT       5353
#define MDNS_GROUP      "224.0.0.251"
#define DNS_TYPE_A      1
#define DNS_TYPE_SRV    33
#define DNS_CLASS_IN    1
#define DNS_QU_BIT      0x8000

/* Appends "label1.label2..." in DNS wire form. Returns the new length, or -1. */
static int dns_put_name(unsigned char *buf, int len, int cap, const char *name) {
    const char *p = name;

    while (*p) {
        const char *dot = strchr(p, '.');
        size_t part = dot ? (size_t) (dot - p) : strlen(p);

        if (part == 0 || part > 63 || len + (int) part + 1 >= cap) {
            return -1;
        }
        buf[len++] = (unsigned char) part;
        memcpy(buf + len, p, part);
        len += (int) part;
        p += part;
        if (*p == '.') {
            p++;
        }
    }
    if (len >= cap) {
        return -1;
    }
    buf[len++] = 0;
    return len;
}

/* Reads a name, following compression pointers. Writes a dotted string to out
   when out is non-NULL. Returns the offset just past the name in the message,
   or -1. */
static int dns_read_name(const unsigned char *msg, int msg_len, int pos,
                         char *out, int out_cap) {
    int out_len = 0;
    int jumped = 0;
    int next = -1;
    int guard = 0;

    if (out && out_cap > 0) {
        out[0] = '\0';
    }
    while (pos >= 0 && pos < msg_len) {
        unsigned int len = msg[pos];

        if (++guard > 128) {
            return -1;              /* a pointer loop */
        }
        if ((len & 0xC0) == 0xC0) {
            if (pos + 1 >= msg_len) {
                return -1;
            }
            if (!jumped) {
                next = pos + 2;
                jumped = 1;
            }
            pos = ((int) (len & 0x3F) << 8) | msg[pos + 1];
            continue;
        }
        pos++;
        if (len == 0) {
            return jumped ? next : pos;
        }
        if (pos + (int) len > msg_len) {
            return -1;
        }
        if (out && out_len + (int) len + 2 < out_cap) {
            if (out_len) {
                out[out_len++] = '.';
            }
            memcpy(out + out_len, msg + pos, len);
            out_len += (int) len;
            out[out_len] = '\0';
        }
        pos += (int) len;
    }
    return -1;
}

/* Sends the same query out of every IPv4 interface. The machine may well have
   several -- virtual switches from hypervisors are routinely ahead of the real
   adapter in the routing order -- and the default multicast interface is then
   one the client cannot hear. */
static void mdns_send_query(int sock, const unsigned char *query, int query_len) {
    struct sockaddr_in to;

    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(MDNS_PORT);
    to.sin_addr.s_addr = inet_addr(MDNS_GROUP);

#ifdef _WIN32
    {
        IP_ADAPTER_ADDRESSES *adapters = NULL, *a;
        ULONG size = 16384;
        ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                      GAA_FLAG_SKIP_DNS_SERVER;
        int sent = 0;

        adapters = (IP_ADAPTER_ADDRESSES *) malloc(size);
        if (adapters &&
            GetAdaptersAddresses(AF_INET, flags, NULL, adapters, &size) == NO_ERROR) {
            for (a = adapters; a; a = a->Next) {
                IP_ADAPTER_UNICAST_ADDRESS *u;

                if (a->OperStatus != IfOperStatusUp ||
                    a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
                    continue;
                }
                for (u = a->FirstUnicastAddress; u; u = u->Next) {
                    struct sockaddr_in *sa = (struct sockaddr_in *) u->Address.lpSockaddr;

                    if (!sa || sa->sin_family != AF_INET) {
                        continue;
                    }
                    if (setsockopt(sock, IPPROTO_IP, IP_MULTICAST_IF,
                                   (const char *) &sa->sin_addr,
                                   sizeof(sa->sin_addr)) == 0) {
                        sendto(sock, (const char *) query, query_len, 0,
                               (struct sockaddr *) &to, sizeof(to));
                        sent++;
                    }
                }
            }
        }
        free(adapters);
        if (sent) {
            return;
        }
    }
#endif
    sendto(sock, (const char *) query, query_len, 0,
           (struct sockaddr *) &to, sizeof(to));
}

/* Blocks for up to DACP_RESOLVE_SECS. */
static bool dacp_resolve(const char *id, dacp_resolve_t *out) {
    char wanted[256], target[256];
    unsigned char query[512], reply[2048];
    int sock, query_len, n;
    struct sockaddr_in local;
    struct timeval tv;
    unsigned char ttl = 255;
    int reuse = 1;
    time_t deadline;

    memset(out, 0, sizeof(*out));
    snprintf(wanted, sizeof(wanted), "iTunes_Ctrl_%s.%s.%s", id,
             DACP_SERVICE_TYPE, "local");
    target[0] = '\0';

    query[0] = 0; query[1] = 0;             /* id */
    query[2] = 0; query[3] = 0;             /* flags: standard query */
    query[4] = 0; query[5] = 1;             /* one question */
    query[6] = 0; query[7] = 0;
    query[8] = 0; query[9] = 0;
    query[10] = 0; query[11] = 0;
    query_len = dns_put_name(query, 12, (int) sizeof(query), wanted);
    if (query_len < 0 || query_len + 4 > (int) sizeof(query)) {
        return false;
    }
    query[query_len++] = 0;
    query[query_len++] = DNS_TYPE_SRV;
    query[query_len++] = (DNS_QU_BIT | DNS_CLASS_IN) >> 8;
    query[query_len++] = (DNS_QU_BIT | DNS_CLASS_IN) & 0xFF;

    sock = (int) socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        return false;
    }
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char *) &reuse, sizeof(reuse));
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = 0;                     /* replies come back here */
    if (bind(sock, (struct sockaddr *) &local, sizeof(local)) != 0) {
        DACP_CLOSESOCKET(sock);
        return false;
    }
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, (const char *) &ttl, sizeof(ttl));
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *) &tv, sizeof(tv));

    mdns_send_query(sock, query, query_len);

    deadline = time(NULL) + DACP_RESOLVE_SECS;
    while (time(NULL) < deadline) {
        int pos, count, section;
        unsigned int qd, an, ns, ar;

        n = recv(sock, (char *) reply, (int) sizeof(reply), 0);
        if (n < 12) {
            continue;
        }
        qd = ((unsigned) reply[4] << 8) | reply[5];
        an = ((unsigned) reply[6] << 8) | reply[7];
        ns = ((unsigned) reply[8] << 8) | reply[9];
        ar = ((unsigned) reply[10] << 8) | reply[11];

        pos = 12;
        for (count = 0; count < (int) qd && pos > 0; count++) {
            pos = dns_read_name(reply, n, pos, NULL, 0);
            pos = (pos > 0) ? pos + 4 : -1;
        }
        /* Answers, authority and additional all together: the SRV and the A it
           needs usually arrive in different sections. */
        for (section = 0; section < (int) (an + ns + ar) && pos > 0; section++) {
            char name[256];
            unsigned int type, rdlength;

            pos = dns_read_name(reply, n, pos, name, (int) sizeof(name));
            if (pos < 0 || pos + 10 > n) {
                break;
            }
            type = ((unsigned) reply[pos] << 8) | reply[pos + 1];
            rdlength = ((unsigned) reply[pos + 8] << 8) | reply[pos + 9];
            pos += 10;
            if (pos + (int) rdlength > n) {
                break;
            }
            if (type == DNS_TYPE_SRV && rdlength >= 7 &&
                !strcasecmp(name, wanted)) {
                out->port = (uint16_t) (((unsigned) reply[pos + 4] << 8) | reply[pos + 5]);
                dns_read_name(reply, n, pos + 6, target, (int) sizeof(target));
                out->resolved = true;
            } else if (type == DNS_TYPE_A && rdlength == 4 && target[0] &&
                       !strcasecmp(name, target)) {
                snprintf(out->host, sizeof(out->host), "%u.%u.%u.%u",
                         reply[pos], reply[pos + 1], reply[pos + 2], reply[pos + 3]);
            }
            pos += (int) rdlength;
        }
        if (out->resolved && out->host[0]) {
            break;                          /* both halves in hand */
        }
    }
    DACP_CLOSESOCKET(sock);

    if (out->resolved && !out->host[0] && target[0]) {
        /* No address record came with it. Windows resolves .local names itself,
           so the name is still worth handing to getaddrinfo. */
        snprintf(out->host, sizeof(out->host), "%s", target);
    }
    return (out->resolved && out->host[0]);
}

#endif  /* HAVE_DNS_SD */

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
        sock = (int) socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (sock < 0) {
            continue;
        }
        if (!connect(sock, ai->ai_addr, (int) ai->ai_addrlen)) {
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
