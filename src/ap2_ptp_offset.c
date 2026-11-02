/*
 * AirPlay 2 PTP - tracking the offset of a remote clock from one-way Syncs
 *
 * See ap2_ptp_offset.h.
 *
 * Copyright (C) 2024-2026 Music Assistant Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ap2_ptp_offset.h"

void ap2_ptp_offset_fold(struct ap2_ptp_offset *o, int64_t sample, uint64_t rx_ns)
{
    if (!o->valid) {
        *o = (struct ap2_ptp_offset){
            .valid = true, .ns = sample, .peak_ns = sample, .last_rx_ns = rx_ns };
        return;
    }
    uint64_t elapsed = rx_ns > o->last_rx_ns ? rx_ns - o->last_rx_ns : 0;
    o->last_rx_ns = rx_ns;
    int64_t below = o->ns - sample;
    if (o->high_since_ns && rx_ns - o->high_since_ns >= AP2_PTP_OFFSET_STEP_TIME_NS)
        o->high_since_ns = 0;
    if (sample - o->peak_ns > AP2_PTP_OFFSET_JUMP_NS) {
        if (!o->high_since_ns) {
            o->high_since_ns = rx_ns ? rx_ns : 1;
            o->high_ns = sample;
        } else {
            o->ns = o->peak_ns = sample < o->high_ns ? sample : o->high_ns;
            o->high_since_ns = 0;
        }
    } else if (below <= 0) {
        o->ns = sample;
        if (sample > o->peak_ns) o->peak_ns = sample;
    } else {
        int64_t drift = (int64_t)(elapsed / 1000 * AP2_PTP_OFFSET_MAX_DRIFT_PPM / 1000);
        o->ns -= below < drift ? below : drift;
    }

    if (below < AP2_PTP_OFFSET_STEP_NS) {
        o->low_since_ns = 0;
    } else if (!o->low_since_ns) {
        o->low_since_ns = rx_ns ? rx_ns : 1;
        o->low_best_ns = sample;
    } else {
        if (sample > o->low_best_ns) o->low_best_ns = sample;
        if (rx_ns - o->low_since_ns >= AP2_PTP_OFFSET_STEP_TIME_NS) {
            o->ns = o->peak_ns = o->low_best_ns;
            o->low_since_ns = 0;
        }
    }
}
