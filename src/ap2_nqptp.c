/*
 * AirPlay 2 PTP - NQPTP compatibility for the PTP daemon
 *
 * See ap2_nqptp.h. Written from shairport-sync's side of the interface; no
 * nqptp code is used.
 *
 * Copyright (C) 2024-2026 Music Assistant Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <inttypes.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include "cross_log.h"
#include "ap2_nqptp.h"

extern log_level *loglevel;

static struct ap2_nqptp_client *client_get(struct ap2_nqptp *nq, const char *name,
                                           uint64_t now);
static struct ap2_nqptp_smi *smi_attach(const char *name);
static void smi_write(struct ap2_nqptp_smi *map, const struct ap2_nqptp_smi_set *s);
static void client_set_follow(struct ap2_nqptp *nq, struct ap2_nqptp_client *cl,
                              struct in_addr ip);
static void client_unfollow(struct ap2_nqptp *nq, struct ap2_nqptp_client *cl);
static void client_publish(struct ap2_nqptp *nq, struct ap2_nqptp_client *cl, uint64_t now);
static int bind_loopback(int family, int port);

/* ---- parsing ---- */

bool ap2_nqptp_name_valid(const char *name)
{
    size_t n = strlen(name);
    if (n < 2 || n > AP2_NQPTP_NAME_MAX || name[0] != '/') return false;
    if (strchr(name + 1, '/')) return false;
    /* becomes a file in AP2_NQPTP_SHM_DIR */
    if (strcmp(name, "/.") == 0 || strcmp(name, "/..") == 0) return false;
    for (const char *p = name + 1; *p; p++)
        if ((unsigned char)*p <= ' ' || (unsigned char)*p == 0x7f) return false;
    return true;
}

static bool parse_ip(const char *tok, struct in_addr *out)
{
    if (inet_pton(AF_INET, tok, out) == 1) return true;
    struct in6_addr a6;
    if (inet_pton(AF_INET6, tok, &a6) != 1) return false;
    static const uint8_t mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    if (memcmp(a6.s6_addr, mapped, sizeof(mapped)) != 0) return false;
    memcpy(&out->s_addr, a6.s6_addr + 12, 4);
    return true;
}

enum ap2_nqptp_parse_result ap2_nqptp_parse(const char *buf, size_t len,
                                            struct ap2_nqptp_msg *out)
{
    memset(out, 0, sizeof(*out));
    char line[AP2_NQPTP_MSG_MAX + 1];
    if (len > AP2_NQPTP_MSG_MAX) len = AP2_NQPTP_MSG_MAX;
    memcpy(line, buf, len);
    line[len] = '\0';

    char *save = NULL;
    char *name = strtok_r(line, " \t\r\n", &save);
    if (!name) return AP2_NQPTP_ERR_EMPTY;
    if (!ap2_nqptp_name_valid(name)) return AP2_NQPTP_ERR_NAME;
    snprintf(out->name, sizeof(out->name), "%s", name);

    char *cmd = strtok_r(NULL, " \t\r\n", &save);
    if (!cmd || cmd[1] != '\0' || !strchr("TBEP", cmd[0])) return AP2_NQPTP_ERR_COMMAND;
    out->cmd = cmd[0];

    char *arg = strtok_r(NULL, " \t\r\n", &save);
    if (out->cmd != 'T') return arg ? AP2_NQPTP_ERR_ARGS : AP2_NQPTP_OK;
    if (!arg) return AP2_NQPTP_OK;
    /* the rest are the other timing peers */
    if (!parse_ip(arg, &out->ip)) return AP2_NQPTP_ERR_ADDRESS;
    out->have_ip = true;
    return AP2_NQPTP_OK;
}

/* ---- clock domains ---- */

void ap2_nqptp_to_client_clock(const struct ap2_nqptp_clock *c, uint64_t host_now_ns,
                               int64_t clock_offset_ns, struct ap2_nqptp_smi_set *out)
{
    /* master = host + offset = raw + (offset + clock_offset_ns) */
    out->master_clock_id = c->clock_id;
    out->local_time = host_now_ns - (uint64_t)clock_offset_ns;
    out->local_to_master_time_offset = (uint64_t)(c->offset_ns + clock_offset_ns);
    out->master_clock_start_time = c->lock_ns - (uint64_t)clock_offset_ns;
}

/* ---- service ---- */

int ap2_nqptp_open(struct ap2_nqptp *nq, const struct ap2_nqptp_ops *ops,
                   int64_t clock_offset_ns)
{
    memset(nq, 0, sizeof(*nq));
    nq->sock4 = nq->sock6 = -1;
    nq->ops = *ops;
    nq->clock_offset_ns = clock_offset_ns;

    nq->sock4 = bind_loopback(AF_INET, AP2_NQPTP_CTRL_PORT);
    if (nq->sock4 < 0) return -nq->sock4;
    int fd6 = bind_loopback(AF_INET6, AP2_NQPTP_CTRL_PORT);
    if (fd6 >= 0) {
        nq->sock6 = fd6;
    } else if (-fd6 == EADDRINUSE) {
        close(nq->sock4);
        nq->sock4 = -1;
        return EADDRINUSE;
    } else {
        LOG_INFO("[NQPTP] no IPv6 loopback (%s); listening on 127.0.0.1 only",
                 strerror(-fd6));
    }
    LOG_INFO("[NQPTP] compatibility on localhost:%d (IPv4%s)", AP2_NQPTP_CTRL_PORT,
             nq->sock6 >= 0 ? " and IPv6" : "");
    return 0;
}

void ap2_nqptp_receive(struct ap2_nqptp *nq, int fd, uint64_t host_now_ns)
{
    char buf[AP2_NQPTP_MSG_MAX];
    ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) return;
    struct ap2_nqptp_msg m;
    enum ap2_nqptp_parse_result pr = ap2_nqptp_parse(buf, (size_t)n, &m);
    ap2_nqptp_handle(nq, &m, pr, host_now_ns);
}

void ap2_nqptp_handle(struct ap2_nqptp *nq, const struct ap2_nqptp_msg *m,
                      enum ap2_nqptp_parse_result pr, uint64_t now)
{
    if (pr != AP2_NQPTP_OK && pr != AP2_NQPTP_ERR_ADDRESS) {
        LOG_WARN("[NQPTP] ignoring malformed control message (%s)",
                 pr == AP2_NQPTP_ERR_NAME ? "bad interface name" :
                 pr == AP2_NQPTP_ERR_COMMAND ? "bad command" :
                 pr == AP2_NQPTP_ERR_ARGS ? "unexpected arguments" : "empty");
        return;
    }
    struct ap2_nqptp_client *cl = client_get(nq, m->name, now);
    if (!cl) return;
    cl->seen_ns = now;
    LOG_DEBUG("[NQPTP] %s: %c%s%s", cl->name, m->cmd, m->have_ip ? " " : "",
              m->have_ip ? inet_ntoa(m->ip) : "");

    switch (m->cmd) {
    case 'T': {
        struct in_addr none = {0};
        if (pr == AP2_NQPTP_ERR_ADDRESS) {
            /* the engine is IPv4 only: no clock beats a wrong one */
            LOG_WARN("[NQPTP] %s: cannot follow a sender over IPv6; advertise the "
                     "receiver on IPv4 only", cl->name);
            client_set_follow(nq, cl, none);
        } else {
            client_set_follow(nq, cl, m->have_ip ? m->ip : none);
        }
        cl->stopped_ns = now;
        break;
    }
    case 'B':
        cl->playing = true;
        /* released after the grace: follow again */
        if (cl->follow.s_addr && !cl->following) client_set_follow(nq, cl, cl->follow);
        break;
    case 'E':
        cl->playing = false;
        cl->stopped_ns = now;
        break;
    case 'P':   /* the sender's clock keeps running */
        break;
    }
    client_publish(nq, cl, now);
}

void ap2_nqptp_tick(struct ap2_nqptp *nq, uint64_t now)
{
    for (int i = 0; i < AP2_NQPTP_MAX_CLIENTS; i++) {
        struct ap2_nqptp_client *cl = &nq->c[i];
        if (!cl->name[0]) continue;
        /* Release it so the engine serves that address again (a Mac can be
         * both our sender and our speaker). Kept for B. */
        if (cl->following && !cl->playing && now - cl->stopped_ns > AP2_NQPTP_STOP_GRACE_NS) {
            LOG_INFO("[NQPTP] %s: stopped; no longer following %s", cl->name,
                     inet_ntoa(cl->follow));
            client_unfollow(nq, cl);
        }
        client_publish(nq, cl, now);
    }
}

void ap2_nqptp_close(struct ap2_nqptp *nq)
{
    struct ap2_nqptp_smi_set none = {0};
    for (int i = 0; i < AP2_NQPTP_MAX_CLIENTS; i++) {
        struct ap2_nqptp_client *cl = &nq->c[i];
        if (!cl->name[0]) continue;
        client_unfollow(nq, cl);
        if (cl->map) {
            smi_write(cl->map, &none);
            munmap(cl->map, sizeof(*cl->map));
        }
        memset(cl, 0, sizeof(*cl));
    }
    if (nq->sock4 >= 0) close(nq->sock4);
    if (nq->sock6 >= 0) close(nq->sock6);
    nq->sock4 = nq->sock6 = -1;
}

/* ---- clients ---- */

static struct ap2_nqptp_client *client_get(struct ap2_nqptp *nq, const char *name,
                                           uint64_t now)
{
    struct ap2_nqptp_client *slot = NULL;
    for (int i = 0; i < AP2_NQPTP_MAX_CLIENTS; i++) {
        struct ap2_nqptp_client *cl = &nq->c[i];
        if (cl->name[0] && strcmp(cl->name, name) == 0) return cl;
        if (!cl->name[0] && !slot) slot = cl;
    }
    if (!slot) {
        /* full: reuse the longest-idle client that follows nothing */
        for (int i = 0; i < AP2_NQPTP_MAX_CLIENTS; i++) {
            struct ap2_nqptp_client *cl = &nq->c[i];
            if (cl->following || now - cl->seen_ns < AP2_NQPTP_IDLE_REUSE_NS) continue;
            if (!slot || cl->seen_ns < slot->seen_ns) slot = cl;
        }
        if (!slot) {
            LOG_WARN("[NQPTP] %d clients already; ignoring %s", AP2_NQPTP_MAX_CLIENTS, name);
            return NULL;
        }
        LOG_INFO("[NQPTP] reusing the slot of idle client %s for %s", slot->name, name);
        if (slot->map) munmap(slot->map, sizeof(*slot->map));
        memset(slot, 0, sizeof(*slot));
    }

    struct ap2_nqptp_smi *map = smi_attach(name);
    if (!map) return NULL;
    snprintf(slot->name, sizeof(slot->name), "%s", name);
    slot->map = map;
    slot->seen_ns = slot->stopped_ns = now;
    LOG_INFO("[NQPTP] client %s attached", name);
    return slot;
}

static void client_set_follow(struct ap2_nqptp *nq, struct ap2_nqptp_client *cl,
                              struct in_addr ip)
{
    if (cl->following && cl->follow.s_addr == ip.s_addr) return;
    client_unfollow(nq, cl);
    cl->follow = ip;
    if (!ip.s_addr) return;
    cl->following = nq->ops.follow(nq->ops.ud, ip, true);
    if (cl->following) {
        LOG_INFO("[NQPTP] %s: following the clock of %s", cl->name, inet_ntoa(ip));
    } else {
        LOG_WARN("[NQPTP] %s: cannot follow %s", cl->name, inet_ntoa(ip));
    }
}

static void client_unfollow(struct ap2_nqptp *nq, struct ap2_nqptp_client *cl)
{
    if (!cl->following) return;
    nq->ops.follow(nq->ops.ud, cl->follow, false);
    cl->following = false;
}

static void client_publish(struct ap2_nqptp *nq, struct ap2_nqptp_client *cl, uint64_t now)
{
    struct ap2_nqptp_smi_set s = {0};
    struct ap2_nqptp_clock c;
    if (cl->following && nq->ops.clock(nq->ops.ud, cl->follow, &c) && c.clock_id) {
        ap2_nqptp_to_client_clock(&c, now, nq->clock_offset_ns, &s);
        /* the client moves an anchor on the old clock by this */
        if (cl->last_clock_id && cl->last_clock_id != s.master_clock_id) {
            cl->change_offset = cl->last_offset - s.local_to_master_time_offset;
            LOG_INFO("[NQPTP] %s: clock %016" PRIx64 " -> %016" PRIx64 " (change %" PRId64 " ns)",
                     cl->name, cl->last_clock_id, s.master_clock_id,
                     (int64_t)cl->change_offset);
        }
        cl->last_clock_id = s.master_clock_id;
        cl->last_offset = s.local_to_master_time_offset;
        s.clock_change_offset = cl->change_offset;
    }
    smi_write(cl->map, &s);
}

/* ---- shared memory ---- */

static void smi_write(struct ap2_nqptp_smi *map, const struct ap2_nqptp_smi_set *s)
{
    map->main = *s;
    __sync_synchronize();
    map->secondary = *s;
    __sync_synchronize();
}

static struct ap2_nqptp_smi *smi_map(int fd)
{
    void *p = mmap(NULL, sizeof(struct ap2_nqptp_smi), PROT_READ | PROT_WRITE, MAP_SHARED,
                   fd, 0);
    return p == MAP_FAILED ? NULL : (struct ap2_nqptp_smi *)p;
}

/*
 * Map the client's object, creating it if needed. An existing one is reused:
 * clients map once, so a restarted daemon must write the same object. A new
 * one must not be visible before it is full size (a reader would fault), so
 * on Linux it is built under a temporary name and renamed into place.
 */
static struct ap2_nqptp_smi *smi_attach(const char *name)
{
    struct ap2_nqptp_smi *map = NULL;
    int fd = shm_open(name, O_RDWR, 0);
    if (fd >= 0) {
        struct stat st;
        if (fstat(fd, &st) == 0 && st.st_size >= (off_t)sizeof(*map)) map = smi_map(fd);
        close(fd);
        if (map) {
            map->version = AP2_NQPTP_SMI_VERSION;
            __sync_synchronize();
            return map;
        }
    }

#ifdef __linux__
    char path[sizeof(AP2_NQPTP_SHM_DIR) + AP2_NQPTP_NAME_MAX + 1];
    char tmp[sizeof(path) + 32];
    snprintf(path, sizeof(path), AP2_NQPTP_SHM_DIR "%s", name);
    snprintf(tmp, sizeof(tmp), AP2_NQPTP_SHM_DIR "/.%s.%d.tmp", name + 1, (int)getpid());
    fd = open(tmp, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
#else
    shm_unlink(name);   /* too small to reuse */
    fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0644);
#endif
    if (fd < 0) {
        LOG_ERROR("[NQPTP] cannot create the clock for %s: %s", name, strerror(errno));
        return NULL;
    }
    /* readable by clients of other users, whatever the umask */
    fchmod(fd, 0644);
    if (ftruncate(fd, sizeof(*map)) != 0 || !(map = smi_map(fd))) {
        LOG_ERROR("[NQPTP] cannot size the clock for %s: %s", name, strerror(errno));
        close(fd);
#ifdef __linux__
        unlink(tmp);
#else
        shm_unlink(name);
#endif
        return NULL;
    }
    close(fd);
    memset(map, 0, sizeof(*map));
    map->version = AP2_NQPTP_SMI_VERSION;
    __sync_synchronize();
#ifdef __linux__
    if (rename(tmp, path) != 0) {
        LOG_ERROR("[NQPTP] cannot publish the clock for %s: %s", name, strerror(errno));
        unlink(tmp);
        munmap(map, sizeof(*map));
        return NULL;
    }
#endif
    return map;
}

/* ---- sockets ---- */

/* Returns the bound fd, or -errno. */
static int bind_loopback(int family, int port)
{
    int fd = socket(family, SOCK_DGRAM, 0);
    if (fd < 0) return -errno;
    int rc;
    if (family == AF_INET6) {
        int on = 1;
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &on, sizeof(on));
        struct sockaddr_in6 a = {.sin6_family = AF_INET6, .sin6_port = htons(port),
                                 .sin6_addr = IN6ADDR_LOOPBACK_INIT};
        rc = bind(fd, (struct sockaddr *)&a, sizeof(a));
    } else {
        struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port)};
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        rc = bind(fd, (struct sockaddr *)&a, sizeof(a));
    }
    if (rc != 0) {
        int err = errno;
        close(fd);
        return -err;
    }
    return fd;
}
