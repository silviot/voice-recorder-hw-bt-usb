/* Link roots for the BT=1 firmware image (plan §4, step 1).
 *
 * Until the application calls Bluedroid (steps 6-7), nothing in open_fw references the stack, and
 * --gc-sections would silently drop it, producing a deceptively small image. This table, kept by
 * KEEP(*(.bt_link_roots)) in sl6801_firm.ld, references every public entry point the two modes will use,
 * so the integrated image carries the real stack. tools/check_bt_link_roots.sh verifies each name below
 * is defined in the final ELF. Linked as a plain object (not from an archive) so it is always pulled in.
 *
 * SPDX-License-Identifier: MIT */
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_spp_api.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "esp_bluedroid_hci.h"
#include "hp_tx_bd.h"

#define ROOT(fn) (const void *)(fn)

__attribute__((used, section(".bt_link_roots")))
const void *const g_bt_link_roots[] = {
    /* core */
    ROOT(esp_bluedroid_attach_hci_driver), ROOT(esp_bluedroid_detach_hci_driver),
    ROOT(esp_bluedroid_init), ROOT(esp_bluedroid_enable), ROOT(esp_bluedroid_disable), ROOT(esp_bluedroid_deinit),
    ROOT(esp_bt_gap_register_callback), ROOT(esp_bt_gap_set_scan_mode), ROOT(esp_bt_gap_set_security_param),
    ROOT(esp_bt_gap_ssp_confirm_reply), ROOT(esp_bt_gap_start_discovery), ROOT(esp_bt_gap_remove_bond_device),
    ROOT(esp_bt_gap_get_bond_device_num), ROOT(esp_bt_gap_set_device_name),
    /* phone mode: SPP */
    ROOT(esp_spp_register_callback), ROOT(esp_spp_enhanced_init), ROOT(esp_spp_start_srv), ROOT(esp_spp_write),
    ROOT(esp_spp_disconnect), ROOT(esp_spp_stop_srv), ROOT(esp_spp_deinit), ROOT(esp_spp_start_discovery),
    /* headphones mode: A2DP source + AVRCP */
    ROOT(esp_a2d_register_callback), ROOT(esp_a2d_source_init), ROOT(esp_a2d_source_connect),
    ROOT(esp_a2d_media_ctrl_by_handle), ROOT(esp_a2d_source_audio_data_send), ROOT(esp_a2d_source_disconnect_by_handle),
    ROOT(esp_a2d_source_deinit),
    /* headphones mode, external-codec source (docs/BT_A2DP_DESIGN.md §5.3) */
    ROOT(esp_a2d_audio_buff_alloc), ROOT(esp_a2d_audio_buff_free), ROOT(esp_a2d_source_register_stream_endpoint),
    ROOT(esp_a2d_source_set_pref_mcc), ROOT(esp_avrc_tg_set_psth_cmd_filter), ROOT(esp_avrc_tg_set_rn_evt_cap),
    ROOT(esp_avrc_tg_send_rn_rsp_by_handle), ROOT(esp_avrc_ct_send_get_rn_capabilities_cmd_by_handle),
    ROOT(esp_avrc_ct_send_register_notification_cmd_by_handle), ROOT(esp_avrc_ct_send_set_absolute_volume_cmd_by_handle),
    ROOT(esp_bt_gap_cancel_discovery), ROOT(esp_bt_gap_set_cod), ROOT(esp_bt_gap_read_remote_name),
    /* our SBC encoder + pacer on the ext-codec path (bt_port/hp_tx_bd.c; pulls SBC_Encoder_Init into the image until
     * the headphones session calls it, H5) */
    ROOT(hp_tx_bd_init), ROOT(hp_tx_bd_deinit), ROOT(hp_tx_bd_set_conn), ROOT(hp_tx_bd_set_acl_handle),
    ROOT(hp_tx_bd_on_audio_cfg), ROOT(hp_tx_bd_started), ROOT(hp_tx_bd_stopped), ROOT(hp_tx_bd_delay_report),
    ROOT(hp_tx_bd_stop_wait),
    ROOT(esp_avrc_ct_register_callback), ROOT(esp_avrc_ct_init), ROOT(esp_avrc_ct_deinit),
    ROOT(esp_avrc_tg_register_callback), ROOT(esp_avrc_tg_init), ROOT(esp_avrc_tg_deinit),
};
