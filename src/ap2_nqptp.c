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
