#include <arpa/inet.h>
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ap2_nqptp.h"
#include "cross_log.h"

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
    ap2_nqptp_to_client_clock(&c, 5 * S, (int64_t)S, &s);
    assert(s.master_clock_id == 0xabc);
    assert(s.local_time == 4 * S);
    assert((int64_t)s.local_to_master_time_offset == 8 * (int64_t)S);
    assert(s.master_clock_start_time == 3 * S);
    /* The relation survives the change of domain. */
    assert(5 * S + (uint64_t)c.offset_ns == s.local_time + s.local_to_master_time_offset);

    /* Senders whose clock is behind ours have a negative offset. */
    c.offset_ns = -3 * (int64_t)S;
    ap2_nqptp_to_client_clock(&c, 5 * S, -2 * (int64_t)S, &s);
    assert(s.local_time == 7 * S);
    assert((int64_t)s.local_to_master_time_offset == -5 * (int64_t)S);
}

int main(void)
{
    test_parse_accepts_shairport_messages();
    test_parse_rejects_malformed_messages();
    test_clock_is_expressed_on_the_client_clock();
    printf("test_ap2_nqptp: all tests passed\n");
    return 0;
}
