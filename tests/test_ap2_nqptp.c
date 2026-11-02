#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ap2_nqptp.h"
#include "cross_log.h"

static log_level test_log_level = lSILENCE;
log_level *loglevel = &test_log_level;

#define S 1000000000ULL

/* ---- a fake engine: which senders are followed, and their clocks ---- */

#define MAX_FAKE 8
static struct { struct in_addr ip; int refs; } followed[MAX_FAKE];
static struct { struct in_addr ip; struct ap2_nqptp_clock c; } clocks[MAX_FAKE];
static int nclocks;
static int follow_calls;
static bool refuse_follow;

static struct in_addr ip4(const char *s)
{
    struct in_addr a;
    assert(inet_pton(AF_INET, s, &a) == 1);
    return a;
}

static int refs_of(const char *s)
{
    struct in_addr a = ip4(s);
    for (int i = 0; i < MAX_FAKE; i++)
        if (followed[i].ip.s_addr == a.s_addr) return followed[i].refs;
    return 0;
}

static bool fake_follow(void *ud, struct in_addr ip, bool on)
{
    (void)ud;
    follow_calls++;
    if (on && refuse_follow) return false;
    int free_i = -1;
    for (int i = 0; i < MAX_FAKE; i++) {
        if (followed[i].ip.s_addr == ip.s_addr && followed[i].refs) {
            followed[i].refs += on ? 1 : -1;
            assert(followed[i].refs >= 0);
            return true;
        }
        if (!followed[i].refs && free_i < 0) free_i = i;
    }
    assert(on);   /* every stop matches a start */
    followed[free_i].ip = ip;
    followed[free_i].refs = 1;
    return true;
}

static bool fake_clock(void *ud, struct in_addr ip, struct ap2_nqptp_clock *out)
{
    (void)ud;
    for (int i = 0; i < nclocks; i++)
        if (clocks[i].ip.s_addr == ip.s_addr) { *out = clocks[i].c; return true; }
    return false;
}

static void set_clock(const char *ip, uint64_t id, int64_t offset, uint64_t lock)
{
    struct in_addr a = ip4(ip);
    for (int i = 0; i < nclocks; i++)
        if (clocks[i].ip.s_addr == a.s_addr) {
            clocks[i].c = (struct ap2_nqptp_clock){id, offset, lock};
            return;
        }
    clocks[nclocks].ip = a;
    clocks[nclocks++].c = (struct ap2_nqptp_clock){id, offset, lock};
}

static void fake_reset(void)
{
    memset(followed, 0, sizeof(followed));
    nclocks = 0;
    follow_calls = 0;
    refuse_follow = false;
}

static const struct ap2_nqptp_ops fake_ops = {
    .ud = NULL, .follow = fake_follow, .clock = fake_clock,
};

/* ---- helpers ---- */

/* Short names: macOS caps shm names near 31 characters. */
static char names[8][32];

static const char *name(int i)
{
    snprintf(names[i], sizeof(names[i]), "/apnq-%d-%d", (int)getpid(), i);
    return names[i];
}

static void unlink_names(void)
{
    for (int i = 0; i < 8; i++) shm_unlink(name(i));
}

/* A service without sockets: messages go straight to ap2_nqptp_handle. */
static void svc_init(struct ap2_nqptp *nq, int64_t clock_offset_ns)
{
    fake_reset();
    unlink_names();
    memset(nq, 0, sizeof(*nq));
    nq->sock4 = nq->sock6 = -1;
    nq->ops = fake_ops;
    nq->clock_offset_ns = clock_offset_ns;
}

static void say(struct ap2_nqptp *nq, uint64_t now, const char *fmt, const char *n,
                const char *arg)
{
    char buf[256];
    int len = snprintf(buf, sizeof(buf), fmt, n, arg ? arg : "");
    struct ap2_nqptp_msg m;
    enum ap2_nqptp_parse_result pr = ap2_nqptp_parse(buf, (size_t)len + 1, &m);
    ap2_nqptp_handle(nq, &m, pr, now);
}

/* Read the object the way shairport-sync does: map read-only, copy both
 * records, accept main only when secondary matches. */
static struct ap2_nqptp_smi read_smi(const char *n)
{
    int fd = shm_open(n, O_RDONLY, 0);
    assert(fd >= 0);
    struct stat st;
    assert(fstat(fd, &st) == 0 && st.st_size >= (off_t)sizeof(struct ap2_nqptp_smi));
    void *p = mmap(NULL, sizeof(struct ap2_nqptp_smi), PROT_READ, MAP_SHARED, fd, 0);
    assert(p != MAP_FAILED);
    close(fd);
    struct ap2_nqptp_smi s;
    memcpy(&s, p, sizeof(s));
    assert(memcmp(&s.main, &s.secondary, sizeof(s.main)) == 0);
    munmap(p, sizeof(s));
    return s;
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

/* ---- the service ---- */

static void test_bare_t_creates_an_empty_version_11_object(void)
{
    struct ap2_nqptp nq;
    svc_init(&nq, 0);
    say(&nq, 1 * S, "%s T%s", name(0), NULL);
    struct ap2_nqptp_smi s = read_smi(name(0));
    assert(s.version == AP2_NQPTP_SMI_VERSION);
    assert(s.main.master_clock_id == 0);
    assert(follow_calls == 0);
    ap2_nqptp_close(&nq);
}

static void test_t_publishes_the_followed_clock_once_locked(void)
{
    struct ap2_nqptp nq;
    svc_init(&nq, 1000);
    say(&nq, 1 * S, "%s T %s", name(0), "192.0.2.10");
    say(&nq, 1 * S, "%s B%s", name(0), NULL);
    assert(refs_of("192.0.2.10") == 1);
    /* Followed but no offset yet: no clock. */
    assert(read_smi(name(0)).main.master_clock_id == 0);

    set_clock("192.0.2.10", 0x1111, 500, 2 * S);
    ap2_nqptp_tick(&nq, 3 * S);
    struct ap2_nqptp_smi s = read_smi(name(0));
    assert(s.main.master_clock_id == 0x1111);
    assert(s.main.local_time == 3 * S - 1000);
    assert(s.main.local_to_master_time_offset == 1500);
    assert(s.main.master_clock_start_time == 2 * S - 1000);
    assert(s.main.clock_change_offset == 0);
    ap2_nqptp_close(&nq);
    /* Closing stops following and leaves the client a "no clock" object. */
    assert(refs_of("192.0.2.10") == 0);
    assert(read_smi(name(0)).main.master_clock_id == 0);
}

static void test_t_replaces_the_followed_sender(void)
{
    struct ap2_nqptp nq;
    svc_init(&nq, 0);
    say(&nq, 1 * S, "%s T %s", name(0), "192.0.2.1");
    assert(refs_of("192.0.2.1") == 1);
    say(&nq, 1 * S, "%s T %s", name(0), "192.0.2.2");
    assert(refs_of("192.0.2.1") == 0 && refs_of("192.0.2.2") == 1);
    /* Repeating the same sender is not another registration. */
    int calls = follow_calls;
    say(&nq, 1 * S, "%s T %s", name(0), "192.0.2.2");
    assert(follow_calls == calls && refs_of("192.0.2.2") == 1);
    /* A bare T follows nothing. */
    say(&nq, 1 * S, "%s T%s", name(0), NULL);
    assert(refs_of("192.0.2.2") == 0);
    ap2_nqptp_close(&nq);
    assert(follow_calls == calls + 1);
}

static void test_ipv6_sender_reads_no_clock(void)
{
    struct ap2_nqptp nq;
    svc_init(&nq, 0);
    set_clock("192.0.2.1", 0x1, 0, 0);
    say(&nq, 1 * S, "%s T %s", name(0), "192.0.2.1");
    ap2_nqptp_tick(&nq, 2 * S);
    assert(read_smi(name(0)).main.master_clock_id == 0x1);
    say(&nq, 3 * S, "%s T %s", name(0), "2001:db8::1");
    assert(refs_of("192.0.2.1") == 0);
    assert(read_smi(name(0)).main.master_clock_id == 0);
    ap2_nqptp_close(&nq);
}

static void test_clock_change_offset_is_old_minus_new(void)
{
    struct ap2_nqptp nq;
    svc_init(&nq, 0);
    set_clock("192.0.2.1", 0xa, 100, 0);
    say(&nq, 1 * S, "%s T %s", name(0), "192.0.2.1");
    say(&nq, 1 * S, "%s B%s", name(0), NULL);
    assert(read_smi(name(0)).main.clock_change_offset == 0);

    /* The sender's grandmaster changes (e.g. it joined a group). A client
     * holding an anchor on the old clock subtracts this to move it over. */
    set_clock("192.0.2.1", 0xb, 40, 2 * S);
    ap2_nqptp_tick(&nq, 2 * S);
    struct ap2_nqptp_smi s = read_smi(name(0));
    assert(s.main.master_clock_id == 0xb);
    assert((int64_t)s.main.clock_change_offset == 60);
    /* It stays until the next change. */
    ap2_nqptp_tick(&nq, 3 * S);
    assert((int64_t)read_smi(name(0)).main.clock_change_offset == 60);

    /* A new sender is a clock change too, even across a gap with no clock. */
    set_clock("192.0.2.2", 0xc, 100, 4 * S);
    say(&nq, 4 * S, "%s T%s", name(0), NULL);
    assert(read_smi(name(0)).main.master_clock_id == 0);
    say(&nq, 4 * S, "%s T %s", name(0), "192.0.2.2");
    s = read_smi(name(0));
    assert(s.main.master_clock_id == 0xc);
    assert((int64_t)s.main.clock_change_offset == -60);
    ap2_nqptp_close(&nq);
}

static void test_stopped_client_releases_its_sender_after_the_grace(void)
{
    struct ap2_nqptp nq;
    svc_init(&nq, 0);
    set_clock("192.0.2.1", 0x1, 0, 0);
    say(&nq, 10 * S, "%s T %s", name(0), "192.0.2.1");
    say(&nq, 10 * S, "%s B%s", name(0), NULL);

    /* Paused: the sender's clock keeps running and stays followed. */
    say(&nq, 11 * S, "%s P%s", name(0), NULL);
    ap2_nqptp_tick(&nq, 20 * S);
    assert(refs_of("192.0.2.1") == 1);

    /* Stopped: still followed within the grace, released after it. */
    say(&nq, 20 * S, "%s E%s", name(0), NULL);
    ap2_nqptp_tick(&nq, 20 * S + AP2_NQPTP_STOP_GRACE_NS);
    assert(refs_of("192.0.2.1") == 1);
    assert(read_smi(name(0)).main.master_clock_id == 0x1);
    ap2_nqptp_tick(&nq, 20 * S + AP2_NQPTP_STOP_GRACE_NS + 1);
    assert(refs_of("192.0.2.1") == 0);
    assert(read_smi(name(0)).main.master_clock_id == 0);

    /* Playing again follows the same sender again. */
    say(&nq, 30 * S, "%s B%s", name(0), NULL);
    assert(refs_of("192.0.2.1") == 1);
    assert(read_smi(name(0)).main.master_clock_id == 0x1);
    ap2_nqptp_close(&nq);
}

static void test_named_sender_that_never_plays_is_released(void)
{
    struct ap2_nqptp nq;
    svc_init(&nq, 0);
    say(&nq, 10 * S, "%s T %s", name(0), "192.0.2.1");
    ap2_nqptp_tick(&nq, 10 * S + AP2_NQPTP_STOP_GRACE_NS + 1);
    assert(refs_of("192.0.2.1") == 0);
    ap2_nqptp_close(&nq);
}

static void test_clients_are_isolated(void)
{
    struct ap2_nqptp nq;
    svc_init(&nq, 0);
    set_clock("192.0.2.1", 0x1, 10, 0);
    set_clock("192.0.2.2", 0x2, 20, 0);
    say(&nq, 1 * S, "%s T %s", name(0), "192.0.2.1");
    say(&nq, 1 * S, "%s T %s", name(1), "192.0.2.2");
    /* One sender playing to two receivers on this host. */
    say(&nq, 1 * S, "%s T %s", name(2), "192.0.2.1");
    for (int i = 0; i < 3; i++) say(&nq, 1 * S, "%s B%s", name(i), NULL);
    assert(refs_of("192.0.2.1") == 2 && refs_of("192.0.2.2") == 1);
    assert(read_smi(name(0)).main.master_clock_id == 0x1);
    assert(read_smi(name(1)).main.master_clock_id == 0x2);
    assert(read_smi(name(2)).main.master_clock_id == 0x1);

    /* One of them leaving the sender leaves the other following it. */
    say(&nq, 2 * S, "%s T%s", name(2), NULL);
    assert(refs_of("192.0.2.1") == 1);
    ap2_nqptp_tick(&nq, 2 * S);
    assert(read_smi(name(0)).main.master_clock_id == 0x1);
    assert(read_smi(name(2)).main.master_clock_id == 0);
    ap2_nqptp_close(&nq);
}

static void test_refused_follow_reads_no_clock(void)
{
    struct ap2_nqptp nq;
    svc_init(&nq, 0);
    refuse_follow = true;
    set_clock("192.0.2.1", 0x1, 0, 0);
    say(&nq, 1 * S, "%s T %s", name(0), "192.0.2.1");
    say(&nq, 1 * S, "%s B%s", name(0), NULL);
    assert(read_smi(name(0)).main.master_clock_id == 0);
    /* A later start retries. */
    refuse_follow = false;
    say(&nq, 2 * S, "%s B%s", name(0), NULL);
    assert(read_smi(name(0)).main.master_clock_id == 0x1);
    ap2_nqptp_close(&nq);
}

/* A client maps its object once, so a restarted daemon must write into the
 * object the client already holds rather than a fresh one. */
static void test_existing_object_is_reattached_not_replaced(void)
{
    struct ap2_nqptp nq;
    svc_init(&nq, 0);
    int fd = shm_open(name(0), O_CREAT | O_RDWR, 0644);
    assert(fd >= 0 && ftruncate(fd, sizeof(struct ap2_nqptp_smi)) == 0);
    struct ap2_nqptp_smi *held = mmap(NULL, sizeof(*held), PROT_READ | PROT_WRITE,
                                      MAP_SHARED, fd, 0);
    assert(held != MAP_FAILED);
    close(fd);

    set_clock("192.0.2.1", 0x77, 0, 0);
    say(&nq, 1 * S, "%s T %s", name(0), "192.0.2.1");
    assert(held->version == AP2_NQPTP_SMI_VERSION);
    assert(held->main.master_clock_id == 0x77);
    ap2_nqptp_close(&nq);
    /* Left in place, telling its holder there is no clock. */
    assert(held->main.master_clock_id == 0);
    munmap(held, sizeof(*held));
}

static void test_too_small_object_is_replaced(void)
{
    struct ap2_nqptp nq;
    svc_init(&nq, 0);
    int fd = shm_open(name(0), O_CREAT | O_RDWR, 0644);
    assert(fd >= 0 && ftruncate(fd, 16) == 0);
    close(fd);
    say(&nq, 1 * S, "%s T%s", name(0), NULL);
    assert(read_smi(name(0)).version == AP2_NQPTP_SMI_VERSION);
    ap2_nqptp_close(&nq);
}

static void test_full_table_reuses_only_idle_clients(void)
{
    struct ap2_nqptp nq;
    svc_init(&nq, 0);
    char n[32];
    for (int i = 0; i < AP2_NQPTP_MAX_CLIENTS; i++) {
        snprintf(n, sizeof(n), "/apnq-%d-f%d", (int)getpid(), i);
        say(&nq, 1 * S, "%s T %s", n, i == 0 ? NULL : "192.0.2.1");
    }
    const char *extra = name(1);
    /* Everyone is recent: nobody to evict. */
    say(&nq, 2 * S, "%s T%s", extra, NULL);
    assert(shm_open(extra, O_RDONLY, 0) < 0 && errno == ENOENT);

    /* The client following nothing goes idle; it is the one reused. The
     * others keep their senders through the grace by playing. */
    for (int i = 1; i < AP2_NQPTP_MAX_CLIENTS; i++) {
        snprintf(n, sizeof(n), "/apnq-%d-f%d", (int)getpid(), i);
        say(&nq, 1 * S + AP2_NQPTP_IDLE_REUSE_NS, "%s B%s", n, NULL);
    }
    say(&nq, 2 * S + AP2_NQPTP_IDLE_REUSE_NS, "%s T%s", extra, NULL);
    assert(read_smi(extra).version == AP2_NQPTP_SMI_VERSION);
    assert(refs_of("192.0.2.1") == AP2_NQPTP_MAX_CLIENTS - 1);

    ap2_nqptp_close(&nq);
    for (int i = 0; i < AP2_NQPTP_MAX_CLIENTS; i++) {
        snprintf(n, sizeof(n), "/apnq-%d-f%d", (int)getpid(), i);
        shm_unlink(n);
    }
}

/* The control port as shairport-sync reaches it: a datagram to localhost. */
static void test_control_port(void)
{
    fake_reset();
    unlink_names();
    struct ap2_nqptp nq;
    assert(ap2_nqptp_open(&nq, &fake_ops, 0) == 0);
    struct ap2_nqptp other;
    assert(ap2_nqptp_open(&other, &fake_ops, 0) == EADDRINUSE);

    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(AP2_NQPTP_CTRL_PORT)};
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    char msg[64];
    int len = snprintf(msg, sizeof(msg), "%s T 192.0.2.5", name(0));
    assert(sendto(s, msg, (size_t)len + 1, 0, (struct sockaddr *)&to, sizeof(to)) == len + 1);
    close(s);
    ap2_nqptp_receive(&nq, nq.sock4, 1 * S);
    assert(refs_of("192.0.2.5") == 1);
    assert(read_smi(name(0)).version == AP2_NQPTP_SMI_VERSION);
    ap2_nqptp_close(&nq);
    assert(nq.sock4 == -1 && nq.sock6 == -1);
}

int main(void)
{
    test_parse_accepts_shairport_messages();
    test_parse_rejects_malformed_messages();
    test_clock_is_expressed_on_the_client_clock();
    test_bare_t_creates_an_empty_version_11_object();
    test_t_publishes_the_followed_clock_once_locked();
    test_t_replaces_the_followed_sender();
    test_ipv6_sender_reads_no_clock();
    test_clock_change_offset_is_old_minus_new();
    test_stopped_client_releases_its_sender_after_the_grace();
    test_named_sender_that_never_plays_is_released();
    test_clients_are_isolated();
    test_refused_follow_reads_no_clock();
    test_existing_object_is_reattached_not_replaced();
    test_too_small_object_is_replaced();
    test_full_table_reuses_only_idle_clients();
    test_control_port();
    unlink_names();
    printf("test_ap2_nqptp: all tests passed\n");
    return 0;
}
