/*
 * AirPlay 2 PTP - NQPTP compatibility for the PTP daemon
 *
 * shairport-sync gets its sender's clock from nqptp, which needs UDP 319/320
 * like the daemon. This is nqptp's client interface (SMI version 11):
 *
 *   control  "<smi> T [<ip> ...]", "<smi> B|E|P", no reply.
 *            T follows the first ip (none: nothing), B play, E stop, P pause.
 *   clock    shm object <smi> (struct ap2_nqptp_smi).
 *
 * Parsing the control messages and filling the shm object, nothing more yet.
 * Copyright (C) 2024-2026 Music Assistant Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __AP2_NQPTP_H_
#define __AP2_NQPTP_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <netinet/in.h>

/* Linux POSIX shm directory (new objects are renamed into place here) */
#ifndef AP2_NQPTP_SHM_DIR
#define AP2_NQPTP_SHM_DIR "/dev/shm"
#endif

#define AP2_NQPTP_SMI_VERSION   11
#define AP2_NQPTP_NAME_MAX      63                /* including the leading "/" */
#define AP2_NQPTP_MSG_MAX       4096

/* Nanoseconds on the client's raw clock (CLOCK_MONOTONIC_RAW / CLOCK_UPTIME_RAW). */
struct ap2_nqptp_smi_set {
    uint64_t master_clock_id;              /* 0: no clock */
    uint64_t local_time;
    uint64_t local_to_master_time_offset;
    uint64_t clock_change_offset;          /* old - new offset at the last clock change */
    uint64_t master_clock_start_time;
};

/* Written main, barrier, secondary; readers accept main only if it matches. */
struct ap2_nqptp_smi {
    uint16_t version;
    struct ap2_nqptp_smi_set main;
    struct ap2_nqptp_smi_set secondary;
};

_Static_assert(offsetof(struct ap2_nqptp_smi, main) == 8, "SMI main at +8");
_Static_assert(offsetof(struct ap2_nqptp_smi, secondary) == 48, "SMI secondary at +48");
_Static_assert(sizeof(struct ap2_nqptp_smi) == 88, "SMI version 11 is 88 bytes");

/* A followed clock on the host clock (host_clock.h). */
struct ap2_nqptp_clock {
    uint64_t clock_id;
    int64_t offset_ns;     /* host ns -> master ns */
    uint64_t lock_ns;      /* host ns of its first offset sample */
};

enum ap2_nqptp_parse_result {
    AP2_NQPTP_OK = 0,
    AP2_NQPTP_ERR_EMPTY,
    AP2_NQPTP_ERR_NAME,
    AP2_NQPTP_ERR_COMMAND,
    AP2_NQPTP_ERR_ADDRESS,     /* T with a non-IPv4 sender */
    AP2_NQPTP_ERR_ARGS,        /* arguments after B, E or P */
};

struct ap2_nqptp_msg {
    char name[AP2_NQPTP_NAME_MAX + 1];
    char cmd;                  /* T, B, E or P */
    bool have_ip;
    struct in_addr ip;         /* T: first address, IPv4-mapped unmapped */
};

/*
 * Parse one control datagram (NUL-terminated or not).
 *
 * :param out: the message; on AP2_NQPTP_ERR_ADDRESS name and cmd are still set.
 */
enum ap2_nqptp_parse_result ap2_nqptp_parse(const char *buf, size_t len,
                                            struct ap2_nqptp_msg *out);

/* "/" then 1..62 printable characters, no other "/". */
bool ap2_nqptp_name_valid(const char *name);

/* Convert a host-clock sample to the client's raw clock (host = raw +
 * clock_offset_ns). Fills everything but clock_change_offset. */
void ap2_nqptp_to_client_clock(const struct ap2_nqptp_clock *c, uint64_t host_now_ns,
                               int64_t clock_offset_ns, struct ap2_nqptp_smi_set *out);

#endif /* __AP2_NQPTP_H_ */
