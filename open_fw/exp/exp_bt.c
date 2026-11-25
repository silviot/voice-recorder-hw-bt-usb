/*
 * Experiment 6 (Classic Bluetooth RWBT + SSP + +XREC Bring-Up):
 * Runs calibrated RF init, starts BT controller + +XREC service, opens 60 s
 * pairing window, polls for `arg` seconds (default 8 s), and logs HCI ready
 * state, RWBT IRQ count, ACL connections, SPP state, and BD_ADDR.
 * Needs an image with phone mode (default BT=1): BT=0 and HCI probe images
 * pass NULL bt_xrec_* entries, and the experiment returns 0xFFFFFFFF.
 *
 * SPDX-License-Identifier: MIT
 */
#include "exp_sdk.h"

EXP_ENTRY
uint32_t exp_main(const openfw_exp_api_t *api, uint32_t arg)
{
    uint32_t secs = (arg >= 1u && arg <= 60u) ? arg : 8u;
    api->wdt_feed();
    if (!api->bt_xrec_start) {
        api->log_puts("[exp_bt] no phone mode in this image\n");
        return 0xFFFFFFFFu;
    }

    uint32_t cal = api->bt_rf_init_calibrated(0x53u, 32000000u);
    api->wdt_feed();
    (void)api->bt_xrec_start(0, 0);
    api->bt_xrec_open_pairing(60000u);

    uint32_t t0 = api->millis();
    uint32_t hci_ready_ms = 0xFFFFFFFFu;
    bt_status_t st;
    while ((uint32_t)(api->millis() - t0) < (secs * 1000u)) {
        api->wdt_feed();
        api->bt_xrec_poll();
        if (hci_ready_ms == 0xFFFFFFFFu) {
            api->bt_xrec_get_status(&st);
            if (st.hci_ready) {
                hci_ready_ms = (uint32_t)(api->millis() - t0);
            }
        }
        api->mdelay(5u);
    }

    api->bt_xrec_get_status(&st);
    api->bt_xrec_stop();

    api->log_puts("[exp_bt] rf_cal=");
    api->log_u32(cal);
    api->log_puts(" name=");
    api->log_puts(st.dev_name);
    api->log_puts(" bd=");
    for (int i = 5; i >= 0; i--) {
        static const char hex[] = "0123456789ABCDEF";
        api->log_putc(hex[st.bd_addr[i] >> 4]);
        api->log_putc(hex[st.bd_addr[i] & 0xFu]);
        if (i > 0) api->log_putc(':');
    }
    api->log_putc('\n');

    api->log_puts("[exp_bt] hci_ready=");
    api->log_u32(st.hci_ready);
    api->log_puts(" ready_ms=");
    api->log_u32(hci_ready_ms);
    api->log_puts(" irq_cnt=");
    api->log_u32(st.irq_count);
    api->log_puts(" acl=");
    api->log_u32(st.acl_connected);
    api->log_puts(" spp=");
    api->log_u32(st.spp_open);
    api->log_puts(" rx=");
    api->log_u32(st.rx_acl_pkts);
    api->log_puts(" tx=");
    api->log_u32(st.tx_acl_pkts);
    api->log_putc('\n');

    return ((uint32_t)st.hci_ready << 24) | (st.irq_count & 0x00FFFFFFu);
}
