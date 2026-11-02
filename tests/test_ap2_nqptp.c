#include <arpa/inet.h>
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ap2_nqptp.h"
#include "cross_log.h"
#include "host_clock.h"

static log_level test_log_level = lSILENCE;
log_level *loglevel = &test_log_level;

#define S 1000000000ULL

static struct in_addr ip4(const char *s)
{
    struct in_addr a;
    assert(inet_pton(AF_INET, s, &a) == 1);
    return a;
}

/* ---- parsing ---- */

static enum ap2_nqptp_parse_result parse(const char *s, struct ap2_nqptp_msg *m)
{
    return ap2_nqptp_parse(s, strlen(s), m);
}

static void test_parse_accepts_shairport_messages(void)
{
    struct ap2_nqptp_msg m;
    /* shairport-sync sends the terminating NUL with every message. */
    const char with_nul[] = "/ma-1 T 192.0.2.7";
    assert(ap2_nqptp_parse(with_nul, sizeof(with_nul), &m) == AP2_NQPTP_OK);
    assert(strcmp(m.name, "/ma-1") == 0 && m.cmd == 'T' && m.have_ip);
    assert(m.ip.s_addr == ip4("192.0.2.7").s_addr);

    assert(parse("/nqptp T", &m) == AP2_NQPTP_OK && m.cmd == 'T' && !m.have_ip);
    assert(parse("/nqptp B", &m) == AP2_NQPTP_OK && m.cmd == 'B');
    assert(parse("/nqptp E", &m) == AP2_NQPTP_OK && m.cmd == 'E');
    assert(parse("/nqptp P\n", &m) == AP2_NQPTP_OK && m.cmd == 'P');

    /* Only the first address is followed. */
    assert(parse("/x T 192.0.2.1 192.0.2.2 192.0.2.3", &m) == AP2_NQPTP_OK);
    assert(m.ip.s_addr == ip4("192.0.2.1").s_addr);

    /* An IPv4-mapped IPv6 address is the IPv4 sender. */
    assert(parse("/x T ::ffff:192.0.2.9", &m) == AP2_NQPTP_OK && m.have_ip);
    assert(m.ip.s_addr == ip4("192.0.2.9").s_addr);

    /* Bytes after the NUL are not part of the message. */
    const char trailing[] = "/x B\0garbage";
    assert(ap2_nqptp_parse(trailing, sizeof(trailing), &m) == AP2_NQPTP_OK);
}

static void test_parse_rejects_malformed_messages(void)
{
    struct ap2_nqptp_msg m;
    assert(parse("", &m) == AP2_NQPTP_ERR_EMPTY);
    assert(parse("   ", &m) == AP2_NQPTP_ERR_EMPTY);
    /* nqptp's old, unnamed form: no interface to publish to. */
    assert(parse("T 192.0.2.1", &m) == AP2_NQPTP_ERR_NAME);
    assert(parse("/ T", &m) == AP2_NQPTP_ERR_NAME);
    assert(parse("/a/b T", &m) == AP2_NQPTP_ERR_NAME);
    assert(parse("/.. T", &m) == AP2_NQPTP_ERR_NAME);
    assert(parse("/. T", &m) == AP2_NQPTP_ERR_NAME);
    char long_name[80];
    memset(long_name, 'a', sizeof(long_name));
    long_name[0] = '/';
    long_name[64] = '\0';   /* 64 characters: one too many */
    assert(!ap2_nqptp_name_valid(long_name));
    long_name[63] = '\0';
    assert(ap2_nqptp_name_valid(long_name));

    assert(parse("/x", &m) == AP2_NQPTP_ERR_COMMAND);
    assert(parse("/x X", &m) == AP2_NQPTP_ERR_COMMAND);
    assert(parse("/x TB", &m) == AP2_NQPTP_ERR_COMMAND);
    assert(parse("/x B 192.0.2.1", &m) == AP2_NQPTP_ERR_ARGS);
    assert(parse("/x T not-an-ip", &m) == AP2_NQPTP_ERR_ADDRESS);

    /* A sender reached over IPv6 cannot be followed, but the client is known,
     * so it can be told there is no clock. */
    assert(parse("/x T fe80::1", &m) == AP2_NQPTP_ERR_ADDRESS);
    assert(strcmp(m.name, "/x") == 0 && m.cmd == 'T' && !m.have_ip);

    /* Longer than any datagram shairport-sync sends: cut, not overrun. */
    static char big[AP2_NQPTP_MSG_MAX * 2];
    memset(big, 'a', sizeof(big));
    big[0] = '/';
    assert(ap2_nqptp_parse(big, sizeof(big), &m) == AP2_NQPTP_ERR_NAME);
}

/* ---- clock domains ---- */

static void test_clock_is_expressed_on_the_client_clock(void)
{
    /* host = raw + 1 s. The sender's clock is host + 7 s, so raw + 8 s. */
    struct ap2_nqptp_clock c = {.clock_id = 0xabc, .offset_ns = 7 * (int64_t)S,
                                .lock_ns = 4 * S};
    struct ap2_nqptp_smi_set s;
    ap2_nqptp_to_client_clock(&c, 5 * S, (int64_t)S, 0, &s);
    assert(s.master_clock_id == 0xabc);
    assert(s.local_time == 4 * S);
    assert((int64_t)s.local_to_master_time_offset == 8 * (int64_t)S);
    assert(s.master_clock_start_time == 3 * S);
    /* The relation survives the change of domain. */
    assert(5 * S + (uint64_t)c.offset_ns == s.local_time + s.local_to_master_time_offset);

    /* Senders whose clock is behind ours have a negative offset. */
    c.offset_ns = -3 * (int64_t)S;
    ap2_nqptp_to_client_clock(&c, 5 * S, -2 * (int64_t)S, 0, &s);
    assert(s.local_time == 7 * S);
    assert((int64_t)s.local_to_master_time_offset == -5 * (int64_t)S);
}

static void test_clock_conversion_undoes_the_rate_correction(void)
{
    /* +50 ppm after 10 hours of raw clock: host - raw is 1.8 s more than the
     * offset, which a client reading the raw clock would otherwise be late by. */
    const int64_t ppb = 50000, off = (int64_t)S;
    const uint64_t raw = 36000 * S, lock_raw = 35000 * S;
    const uint64_t host = raw + (uint64_t)host_clock_rate_term_ns((int64_t)raw, ppb) + off;
    const uint64_t lock = lock_raw + (uint64_t)host_clock_rate_term_ns((int64_t)lock_raw, ppb) + off;
    assert(host - raw == S + 1800000000);
    struct ap2_nqptp_clock c = {.clock_id = 0xabc, .offset_ns = 7 * (int64_t)S, .lock_ns = lock};
    struct ap2_nqptp_smi_set s;
    ap2_nqptp_to_client_clock(&c, host, off, ppb, &s);
    /* exact up to rounding of the rate term */
    assert(llabs((int64_t)(s.local_time - raw)) <= 1);
    assert(llabs((int64_t)(s.master_clock_start_time - lock_raw)) <= 1);
    /* the sender's clock is still where the host clock says it is */
    assert(host + (uint64_t)c.offset_ns == s.local_time + s.local_to_master_time_offset);

    /* a slow clock corrects the other way */
    const uint64_t host_slow = raw + (uint64_t)host_clock_rate_term_ns((int64_t)raw, -ppb) + off;
    ap2_nqptp_to_client_clock(&c, host_slow, off, -ppb, &s);
    assert(llabs((int64_t)(s.local_time - raw)) <= 1);

    /* the largest rate --clock-rate-ppb takes, after a year of uptime */
    const int64_t max_ppb = 2000000;
    const uint64_t year = 365ull * 86400 * S;
    const uint64_t host_max = year + (uint64_t)host_clock_rate_term_ns((int64_t)year, max_ppb) + off;
    ap2_nqptp_to_client_clock(&c, host_max, off, max_ppb, &s);
    assert(llabs((int64_t)(s.local_time - year)) <= 1);
}

int main(void)
{
    test_parse_accepts_shairport_messages();
    test_parse_rejects_malformed_messages();
    test_clock_is_expressed_on_the_client_clock();
    test_clock_conversion_undoes_the_rate_correction();
    printf("test_ap2_nqptp: all tests passed\n");
    return 0;
}
