/* Link-test entry: references the public entry points of BOTH profiles (SPP server + A2DP source/AVRCP)
 * and the Bluedroid core so --gc-sections keeps what a real firmware would keep. Never run.
 * SPDX-License-Identifier: MIT */
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_spp_api.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"

static void spp_cb(esp_spp_cb_event_t e, esp_spp_cb_param_t *p) { (void)e; (void)p; }
static void gap_cb(esp_bt_gap_cb_event_t e, esp_bt_gap_cb_param_t *p) { (void)e; (void)p; }
static void a2d_cb(esp_a2d_cb_event_t e, esp_a2d_cb_param_t *p) { (void)e; (void)p; }
static void ct_cb(esp_avrc_ct_cb_event_t e, esp_avrc_ct_cb_param_t *p) { (void)e; (void)p; }
static void tg_cb(esp_avrc_tg_cb_event_t e, esp_avrc_tg_cb_param_t *p) { (void)e; (void)p; }

int main(void)
{
    esp_bd_addr_t peer = {0};
    uint8_t buf[16] = {0};
#ifdef LT_CORE
    esp_bluedroid_init(); esp_bluedroid_enable();
    esp_bt_gap_register_callback(gap_cb);
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
    esp_bt_gap_ssp_confirm_reply(peer, true);
    esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
    esp_bt_gap_remove_bond_device(peer);
    esp_bt_gap_get_bond_device_num();
    esp_bt_gap_set_device_name("openfw");
    esp_bluedroid_disable(); esp_bluedroid_deinit();
#endif
#ifdef LT_SPP
    esp_spp_register_callback(spp_cb);
    esp_spp_cfg_t c = { .mode = ESP_SPP_MODE_CB, .enable_l2cap_ertm = true, .tx_buffer_size = 0 };
    esp_spp_enhanced_init(&c);
    esp_spp_start_srv(ESP_SPP_SEC_AUTHENTICATE, ESP_SPP_ROLE_SLAVE, 0, "xrec");
    esp_spp_write(1, 16, buf);
    esp_spp_disconnect(1); esp_spp_stop_srv(); esp_spp_deinit();
#endif
#ifdef LT_A2DP
    esp_a2d_register_callback(a2d_cb);
    esp_a2d_source_init();
    esp_a2d_source_connect(peer);
    esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
    esp_a2d_audio_buff_t *ab = 0; esp_a2d_source_audio_data_send(0, ab);
    esp_a2d_source_disconnect(peer); esp_a2d_source_deinit();
    esp_avrc_ct_register_callback(ct_cb); esp_avrc_ct_init();
    esp_avrc_tg_register_callback(tg_cb); esp_avrc_tg_init();
    esp_avrc_tg_deinit(); esp_avrc_ct_deinit();
#endif
    (void)spp_cb; (void)gap_cb; (void)a2d_cb; (void)ct_cb; (void)tg_cb;
    return 0;
}
