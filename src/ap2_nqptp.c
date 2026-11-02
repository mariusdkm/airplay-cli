/*
 * AirPlay 2 PTP - NQPTP compatibility for the PTP daemon
 *
 * See ap2_nqptp.h. Written from shairport-sync's side of the interface; no
 * nqptp code is used.
 *
 * Copyright (C) 2024-2026 Music Assistant Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>

#include "ap2_nqptp.h"
#include "host_clock.h"

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

/* Solve host = raw + rate_term(raw) + clock_offset for raw by fixed-point
 * iteration. Each step shrinks the error by a factor of ppb, so it settles
 * within a few steps even at --clock-rate-ppb's limit of 2000 ppm. */
static uint64_t host_to_raw(uint64_t host_ns, int64_t clock_offset_ns, int64_t rate_ppb)
{
    const int64_t ns = (int64_t)host_ns - clock_offset_ns;
    int64_t raw = ns;
    for (int i = 0; i < 8; i++) {
        int64_t next = ns - host_clock_rate_term_ns(raw, rate_ppb);
        if (next == raw) break;
        raw = next;
    }
    return (uint64_t)raw;
}

void ap2_nqptp_to_client_clock(const struct ap2_nqptp_clock *c, uint64_t host_now_ns,
                               int64_t clock_offset_ns, int64_t rate_ppb,
                               struct ap2_nqptp_smi_set *out)
{
    /* master = host + offset. With a rate correction, host - raw grows with
     * the raw clock, so the offset to raw holds for now: publishing again
     * keeps it current. */
    out->master_clock_id = c->clock_id;
    out->local_time = host_to_raw(host_now_ns, clock_offset_ns, rate_ppb);
    out->local_to_master_time_offset =
        (uint64_t)((int64_t)host_now_ns + c->offset_ns - (int64_t)out->local_time);
    out->master_clock_start_time = host_to_raw(c->lock_ns, clock_offset_ns, rate_ppb);
}
