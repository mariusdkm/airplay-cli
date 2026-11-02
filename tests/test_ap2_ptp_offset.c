#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "ap2_ptp_offset.h"

#define MS 1000000LL
#define SYNC_INTERVAL_NS (125 * MS)

/* Fold `n` Syncs of a remote clock at `offset` (+ `drift_ns` per Sync), each
 * read `delay(i)` late; return the largest estimate error after the first. */
static int64_t run(struct ap2_ptp_offset *o, uint64_t *now, int n, int64_t offset,
                   int64_t drift_ns, int64_t (*delay)(int))
{
    int64_t worst = 0;
    for (int i = 0; i < n; i++) {
        int64_t truth = offset + i * drift_ns;
        ap2_ptp_offset_fold(o, truth - delay(i), *now);
        *now += SYNC_INTERVAL_NS;
        int64_t error = llabs(o->ns - truth);
        if (i > 0 && error > worst) worst = error;
    }
    return worst;
}

static int64_t on_time(int i)
{
    (void)i;
    return 0;
}

/* A power-saving Wi-Fi receiver gets queued frames at each 102.4 ms beacon, so
 * a Sync every 125 ms arrives up to a beacon late in a 22.6 ms sawtooth. */
static int64_t behind_beacons(int i)
{
    return ((int64_t)i * SYNC_INTERVAL_NS) % (1024 * MS / 10);
}

static void test_late_syncs_do_not_move_the_estimate(void)
{
    struct ap2_ptp_offset o = {0};
    uint64_t now = 1000 * MS;
    int64_t worst = run(&o, &now, 480, 5000 * MS, 0, behind_beacons);
    printf("  late syncs: worst error %.3f ms\n", worst / 1e6);
    assert(worst < 3 * MS);   /* snapping to each sample was up to a beacon (~100 ms) off */
}

static void test_drift_is_followed_both_ways(void)
{
    struct ap2_ptp_offset o = {0};
    uint64_t now = 1000 * MS;
    /* 50 ppm either way: 6.25 us per 125 ms Sync */
    assert(run(&o, &now, 400, 0, 6250, on_time) == 0);
    o = (struct ap2_ptp_offset){0};
    assert(run(&o, &now, 400, 0, -6250, on_time) == 0);
}

static void test_step_forward_is_taken_at_once(void)
{
    struct ap2_ptp_offset o = {0};
    uint64_t now = 1000 * MS;
    run(&o, &now, 10, 0, 0, on_time);
    ap2_ptp_offset_fold(&o, 300 * MS, now);
    assert(o.ns == 300 * MS);
}

static void test_step_back_is_taken_once_it_persists(void)
{
    struct ap2_ptp_offset o = {0};
    uint64_t now = 1000 * MS;
    run(&o, &now, 10, 0, 0, on_time);
    uint64_t step_at = now;
    while (o.ns > -500 * MS) {
        ap2_ptp_offset_fold(&o, -1000 * MS - behind_beacons((int)((now - step_at) / SYNC_INTERVAL_NS)),
                            now);
        now += SYNC_INTERVAL_NS;
        assert(now - step_at <= AP2_PTP_OFFSET_STEP_TIME_NS + SYNC_INTERVAL_NS);
    }
    /* the least delayed Sync of the run, not the last one */
    assert(o.ns == -1000 * MS);
}

static void test_restart_takes_the_first_sample(void)
{
    struct ap2_ptp_offset o = {0};
    ap2_ptp_offset_fold(&o, 7 * MS, 1);
    o.valid = false;
    ap2_ptp_offset_fold(&o, -3 * MS, 2);
    assert(o.valid && o.ns == -3 * MS && o.low_since_ns == 0);
}

int main(void)
{
    test_late_syncs_do_not_move_the_estimate();
    test_drift_is_followed_both_ways();
    test_step_forward_is_taken_at_once();
    test_step_back_is_taken_once_it_persists();
    test_restart_takes_the_first_sample();
    printf("test_ap2_ptp_offset: all tests passed\n");
    return 0;
}
