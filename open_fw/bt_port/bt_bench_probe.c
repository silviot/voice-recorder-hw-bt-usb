/* bt_bench_probe: see bt_bench_probe.h. Bench images only.
 * SPDX-License-Identifier: MIT */
#include <stdint.h>
#include <string.h>
#include "common/bt_target.h"
#include "bta/bta_jv_api.h"
#include "stack/port_api.h"
#include "rfc_int.h"
#include "port_int.h"
#include "l2c_int.h"
#include "bt_bench_probe.h"

#if (defined L2C_DYNAMIC_MEMORY) && (L2C_DYNAMIC_MEMORY == TRUE)
#define L2C_CB_UP() (l2c_cb_ptr != NULL)
#else
#define L2C_CB_UP() 1
#endif
#if RFC_DYNAMIC_MEMORY == TRUE
#define RFC_CB_UP() (rfc_cb_ptr != NULL)
#else
#define RFC_CB_UP() 1
#endif

void bt_bench_probe_read(uint32_t spp_handle, bt_bench_probe_t *o)
{
    memset(o, 0, sizeof(*o));
    if (L2C_CB_UP()) {
        o->xmit_window = *(volatile UINT16 *)&l2cb.controller_xmit_window;
        o->l2c_ok = 1u;
    }
    if (spp_handle == 0u || !RFC_CB_UP()) {
        return;
    }
    UINT16 port = BTA_JvRfcommGetPortHdl(spp_handle);   /* table lookup in bta_jv_cb, no lock */
    if (port == 0u || port > MAX_RFC_PORTS) {
        return;
    }
    tPORT *p = &rfc_cb.port.port[port - 1u];
    if (!*(volatile BOOLEAN *)&p->in_use) {
        return;
    }
    o->ok = 1u;
    o->credit_tx = *(volatile UINT16 *)&p->credit_tx;
    o->txq_bytes = *(volatile UINT32 *)&p->tx.queue_size;
    o->peer_fc = *(volatile BOOLEAN *)&p->tx.peer_fc ? 1u : 0u;
}
