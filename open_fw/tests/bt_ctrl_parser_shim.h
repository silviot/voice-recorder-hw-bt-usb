/* See bt_ctrl_parser_shim.c. Plain-C view of Bluedroid's hci_packet_parser.c for test_bt_ctrl.c.
 * SPDX-License-Identifier: MIT */
#ifndef BT_CTRL_PARSER_SHIM_H
#define BT_CTRL_PARSER_SHIM_H

#include <stdint.h>

void shim_free_last(void);
void shim_parse_generic(const uint8_t *h4, uint16_t len);
void shim_parse_buffer_size(const uint8_t *h4, uint16_t len, uint16_t *acl_len, uint16_t *acl_cnt, uint8_t *sco_len,
                            uint16_t *sco_cnt);
void shim_parse_local_version(const uint8_t *h4, uint16_t len, uint8_t *hci_ver, uint16_t *hci_rev, uint8_t *lmp_ver,
                              uint16_t *manufacturer, uint16_t *lmp_subver);
void shim_parse_bd_addr(const uint8_t *h4, uint16_t len, uint8_t addr[6]);
void shim_parse_supported_cmds(const uint8_t *h4, uint16_t len, uint8_t out[64]);
void shim_parse_ext_features(const uint8_t *h4, uint16_t len, uint8_t *page, uint8_t *max_page, uint8_t pages[3][8]);
int shim_max_feature_pages(void);
uint32_t shim_l2cap_host_fc_acl_bufs(void);
uint32_t shim_l2cap_host_fc_acl_len(void);

#endif /* BT_CTRL_PARSER_SHIM_H */
