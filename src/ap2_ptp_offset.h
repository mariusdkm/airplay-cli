/*
 * AirPlay 2 PTP - tracking the offset of a remote clock from one-way Syncs
 *
 * AirPlay's PTP clocks only send Sync/Follow_Up, so each sample is
 * preciseOriginTimestamp - local receive time: the true offset minus however
 * long the Sync was underway. That delay is never negative, but over Wi-Fi it
 * is often tens of milliseconds (power-saving receivers get queued frames at
 * each ~102 ms beacon), so a sample can only ever read too low. The estimate
 * therefore follows the highest samples, as nqptp does: a higher one is taken
 * at once, a lower one only pulls it down as fast as a clock can drift. A real
 * step back shows as samples that all stay far below, and is taken once that
 * has lasted long enough to rule out a stretch of late Syncs.
 *
 * A sample can also read too high, from a bad correction field, so one more
 * than AP2_PTP_OFFSET_JUMP_NS above the highest level taken so far waits for a
 * second sample that also reads that high, and the lower of the two is taken.
 * Climbing back from the estimate's own drift down needs no confirmation. A step back smaller than
 * AP2_PTP_OFFSET_STEP_NS is only followed at the drift rate; nqptp follows
 * those even slower (negative jitter clamped to 2.5 ms, weighted 1/256).
 *
 * Copyright (C) 2024-2026 Music Assistant Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __AP2_PTP_OFFSET_H_
#define __AP2_PTP_OFFSET_H_

#include <stdbool.h>
#include <stdint.h>

/* Fastest drift between two clocks the estimate follows downwards. */
#define AP2_PTP_OFFSET_MAX_DRIFT_PPM  100
/* Samples further below the estimate than this, for this long without one
 * closer to it, are a step back of the remote clock, not late Syncs. */
#define AP2_PTP_OFFSET_STEP_NS        20000000LL         /* 20 ms */
#define AP2_PTP_OFFSET_STEP_TIME_NS   2000000000ULL      /* 2 s */
/* A sample this far above the highest level taken so far needs a second one
 * within STEP_TIME that reads as high. */
#define AP2_PTP_OFFSET_JUMP_NS        1000000LL          /* 1 ms */

struct ap2_ptp_offset {
    bool valid;                       /* at least one sample folded in */
    int64_t ns;                       /* local -> remote clock (add to local ns) */
    int64_t peak_ns;                  /* highest level taken since the last step back */
    uint64_t last_rx_ns;              /* local receive time of the last sample */
    uint64_t low_since_ns;            /* first of a run of samples far below; 0: none */
    int64_t low_best_ns;              /* highest sample of that run */
    uint64_t high_since_ns;           /* a jump up waiting to be confirmed; 0: none */
    int64_t high_ns;                  /* the sample of that jump */
};

/*
 * Fold one offset sample into the estimate. Clear `valid` (or the whole
 * struct) to start over, e.g. on a new grandmaster.
 *
 * @param o       the estimate
 * @param sample  preciseOriginTimestamp (+ corrections) minus `rx_ns`
 * @param rx_ns   local time the Sync was received
 */
void ap2_ptp_offset_fold(struct ap2_ptp_offset *o, int64_t sample, uint64_t rx_ns);

#endif
