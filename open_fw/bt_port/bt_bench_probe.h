/* bt_bench_probe: racy reads of Bluedroid's RFCOMM / L2CAP TX state for the phone-mode throughput sampler (bench
 * images only, make BT=1 BENCH=1; docs/BT_SYNC_THROUGHPUT.md). Kept apart from bt_bench_fw.c because Bluedroid's
 * internal headers define bt_status_t, which clashes with open_fw's (same reason as bt_xrec_spp_mtu.c).
 * SPDX-License-Identifier: MIT */
#ifndef BT_BENCH_PROBE_H
#define BT_BENCH_PROBE_H

#include <stdint.h>

typedef struct {
    uint8_t  ok;            /* the SPP handle resolved to an RFCOMM port in use */
    uint8_t  peer_fc;       /* RFCOMM tx.peer_fc: sending stopped (no peer credit, or peer flow control) */
    uint16_t credit_tx;     /* RFCOMM credits the peer has granted us and we have not used */
    uint32_t txq_bytes;     /* bytes in the RFCOMM port TX queue (accepted from BTC, not yet given to L2CAP) */
    uint16_t xmit_window;   /* L2CAP: controller ACL buffers free from the host's view (1005 count minus unacked) */
    uint8_t  l2c_ok;        /* xmit_window is valid */
} bt_bench_probe_t;

/* Any task (the bench sampler, timer task). No locks: plain loads from Bluedroid's control blocks with bounded
 * indexes; values can be torn or stale, which a time-weighted sample tolerates. */
void bt_bench_probe_read(uint32_t spp_handle, bt_bench_probe_t *o);

#endif /* BT_BENCH_PROBE_H */
