/* bt_xrec_plat_spp_peer_mtu(): negotiated RFCOMM MTU of an open SPP connection, for xrec_proto's get_mtu (LS batch and
 * GETS chunk sizing, as the legacy bt_spp_get_mtu, src/bt_spp.c).
 *
 * The esp_spp API does not report it (btc_spp.c keeps rfc_srv_open.peer_mtu in its private slot), so it is read the
 * way btc_spp itself resolves a connection: BTA_JvRfcommGetPortHdl() (bta_jv_api.c) then PORT_GetQueueStatus()
 * (port_api.c: p_port->peer_mtu). Called from the SPP callback, i.e. in BTC context like btc_spp's own
 * BTA_JvRfcommGetPortHdl() calls, never from the app task. Kept in its own file because Bluedroid's internal headers
 * define bt_status_t, which clashes with open_fw's (include/openfw.h).
 * SPDX-License-Identifier: MIT */
#include <stdint.h>
#include <string.h>
#include "common/bt_target.h"
#include "osi/thread.h"
#include "bta/bta_jv_api.h"
#include "stack/port_api.h"
#include "stack/btu.h"
#include "stack/btm_api.h"
#include "btm_int.h"

uint16_t bt_xrec_plat_spp_peer_mtu(uint32_t handle);
void bt_xrec_on_enc_change(const uint8_t bda[6], uint8_t encr_enable);
void bt_xrec_plat_enc_watch_set(int enable);
void bt_xrec_plat_drop_acl(const uint8_t bda[6]);

static uint8_t s_drop_bdas[4][6];
static uint8_t s_drop_idx;

static void on_btm_enc_change(BD_ADDR bd_addr, UINT8 enc_mode)
{
    bt_xrec_on_enc_change(bd_addr, enc_mode);
}

static void enc_watch_work(void *ctx)
{
    int enable = (uintptr_t)ctx != 0u;
    btm_cb.api.p_enc_change_callback = enable ? on_btm_enc_change : NULL;
    if (!enable) {
        memset(s_drop_bdas, 0, sizeof(s_drop_bdas));
    }
}

static void drop_acl_work(void *ctx)
{
    uint8_t bda[6];
    memcpy(bda, s_drop_bdas[(uintptr_t)ctx & 3u], 6u);
    (void)btm_remove_acl(bda, BT_TRANSPORT_BR_EDR);
}

uint16_t bt_xrec_plat_spp_peer_mtu(uint32_t handle)
{
    UINT16 port = BTA_JvRfcommGetPortHdl(handle);
    tPORT_STATUS st;
    if (port == 0xFFFFu || PORT_GetQueueStatus(port, &st) != PORT_SUCCESS) {
        return 0u;
    }
    return st.mtu_size;
}

void bt_xrec_plat_enc_watch_set(int enable)
{
    osi_thread_t *t = btu_get_current_thread();
    if (t == NULL) {
        if (!enable) {
            memset(s_drop_bdas, 0, sizeof(s_drop_bdas));
            s_drop_idx = 0u;
        }
        return;
    }
    if (!enable) {
        s_drop_idx = 0u;
    }
    (void)osi_thread_post(t, enc_watch_work, (void *)(uintptr_t)(enable ? 1u : 0u), 0, 0u);
}

void bt_xrec_plat_drop_acl(const uint8_t bda[6])
{
    if (bda == NULL) {
        return;
    }
    osi_thread_t *t = btu_get_current_thread();
    if (t == NULL) {
        return;
    }
    unsigned slot = (unsigned)(s_drop_idx++ & 3u);
    memcpy(s_drop_bdas[slot], bda, 6u);
    (void)osi_thread_post(t, drop_acl_work, (void *)(uintptr_t)slot, 0, 0u);
}
