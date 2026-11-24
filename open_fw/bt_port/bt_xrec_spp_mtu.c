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
#include "common/bt_target.h"
#include "bta/bta_jv_api.h"
#include "stack/port_api.h"

uint16_t bt_xrec_plat_spp_peer_mtu(uint32_t handle);

uint16_t bt_xrec_plat_spp_peer_mtu(uint32_t handle)
{
    UINT16 port = BTA_JvRfcommGetPortHdl(handle);
    tPORT_STATUS st;
    if (port == 0xFFFFu || PORT_GetQueueStatus(port, &st) != PORT_SUCCESS) {
        return 0u;
    }
    return st.mtu_size;
}
