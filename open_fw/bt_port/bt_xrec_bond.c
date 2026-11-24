/* Bond-store policy helpers for the phone-mode session (bt_port/bt_xrec_bluedroid.c, plan M4 "XREC traffic only on
 * MITM-authenticated links"). They read and edit the BTC config (the bonds Bluedroid persists through bt_store) directly
 * under btc_config_lock(), so the app task gets a synchronous answer instead of waiting for BTA DM:
 *
 *   - bt_xrec_plat_bond_mitm(): the stored link key type of a bonded peer is an authenticated combination key (P-192
 *     0x05 or P-256 0x08). A bond's existence alone proves nothing: Bluedroid stores a Just Works key
 *     (btc_dm_auth_cmpl_evt) before the app sees AUTH_CMPL, and the app's removal of it is deferred by BTA DM until the
 *     ACL is down (bta_dm_remove_device), which a BT stop, a power loss or a lost AUTH_CMPL can prevent.
 *   - bt_xrec_plat_bond_purge(): drop a peer's bond from the BTC config at once (btc_storage_remove_bonded_device(),
 *     the same call BTC makes when BTA reports the unpair), so the next bond-file write cannot carry it.
 *
 * Both take btc_config's mutex (the BTC task holds it for every config access), never BTA/BTM state. Kept in its own
 * file because Bluedroid's internal headers define bt_status_t, which clashes with open_fw's (include/openfw.h).
 * SPDX-License-Identifier: MIT */
#include <stdint.h>
#include <string.h>
#include "common/bt_target.h"
#include "common/bt_defs.h"
#include "stack/hcidefs.h"
#include "btc/btc_config.h"
#include "btc/btc_storage.h"
#include "btc/btc_util.h"
#include "device/bdaddr.h"

int bt_xrec_plat_bond_mitm(const uint8_t bda[6]);
void bt_xrec_plat_bond_purge(const uint8_t bda[6]);

int bt_xrec_plat_bond_mitm(const uint8_t bda[6])
{
    bt_bdaddr_t a;
    bdstr_t bdstr;
    memcpy(a.address, bda, 6u);
    bdaddr_to_string(&a, bdstr, sizeof(bdstr));
    int type = -1;
    btc_config_lock();
    bool have_key = btc_config_exist(bdstr, BTC_STORAGE_LINK_KEY_STR);
    bool have_type = btc_config_get_int(bdstr, BTC_STORAGE_LINK_KEY_TYPE_STR, &type);
    btc_config_unlock();
    return have_key && have_type && (type == HCI_LKEY_TYPE_AUTH_COMB || type == HCI_LKEY_TYPE_AUTH_COMB_P_256);
}

void bt_xrec_plat_bond_purge(const uint8_t bda[6])
{
    bt_bdaddr_t a;
    memcpy(a.address, bda, 6u);
    (void)btc_storage_remove_bonded_device(&a);      /* removes the keys and flushes (nvs_commit -> bt_store) */
}
