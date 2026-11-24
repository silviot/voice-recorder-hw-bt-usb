/* Adapter that feeds bt_ctrl's H4 replies to Bluedroid's REAL hci_packet_parser.c (linked into test_bt_ctrl), so the
 * mandatory-success proof of plan M1 runs the parser code controller.c uses at start-up. Compiled with the Bluedroid
 * include path and bt_port_prefix.h (host build; tests/bt_host/portmacro.h stands in for the ARM port header).
 *
 * Framing: hci_hal_h4.c:466-469 strips the H4 type byte (offset++, len--) before the event reaches the parser.
 * An assert() inside the parser raises SIGABRT; the test catches it with siglongjmp and calls shim_free_last().
 * SPDX-License-Identifier: MIT */
#include "common/bt_defs.h"
#include "stack/bt_types.h"
#include "stack/hcimsgs.h"
#include "hci/hci_layer.h"
#include "hci/hci_packet_parser.h"

#include "bt_ctrl_parser_shim.h"

/* controller.c:183-184 (file-local there) */
#define SHIM_SUPPORTED_COMMANDS_ARRAY_SIZE 64
#define SHIM_MAX_FEATURES_CLASSIC_PAGE_COUNT 3

static BT_HDR *s_last;

/* osi_free() -> osi_free_func() (osi/allocator.h:123); the parsers free the response on their success path. */
void osi_free_func(void *ptr)
{
    if (ptr == s_last) {
        s_last = NULL;
    }
    free(ptr);
}

void shim_free_last(void)
{
    free(s_last);
    s_last = NULL;
}

static BT_HDR *mk(const uint8_t *h4, uint16_t len)
{
    BT_HDR *p = (BT_HDR *)calloc(1u, sizeof(BT_HDR) + len);
    memcpy(p->data, h4, len);
    p->offset = 1u;                         /* hci_hal_h4.c:467-468 */
    p->len = (uint16_t)(len - 1u);
    s_last = p;
    return p;
}

void shim_parse_generic(const uint8_t *h4, uint16_t len)
{
    hci_packet_parser_get_interface()->parse_generic_command_complete(mk(h4, len));
}

void shim_parse_buffer_size(const uint8_t *h4, uint16_t len, uint16_t *acl_len, uint16_t *acl_cnt, uint8_t *sco_len,
                            uint16_t *sco_cnt)
{
    hci_packet_parser_get_interface()->parse_read_buffer_size_response(mk(h4, len), acl_len, acl_cnt, sco_len, sco_cnt);
}

void shim_parse_local_version(const uint8_t *h4, uint16_t len, uint8_t *hci_ver, uint16_t *hci_rev, uint8_t *lmp_ver,
                              uint16_t *manufacturer, uint16_t *lmp_subver)
{
    bt_version_t v;
    memset(&v, 0, sizeof(v));
    hci_packet_parser_get_interface()->parse_read_local_version_info_response(mk(h4, len), &v);
    *hci_ver = v.hci_version;
    *hci_rev = v.hci_revision;
    *lmp_ver = v.lmp_version;
    *manufacturer = v.manufacturer;
    *lmp_subver = v.lmp_subversion;
}

void shim_parse_bd_addr(const uint8_t *h4, uint16_t len, uint8_t addr[6])
{
    bt_bdaddr_t a;
    memset(&a, 0, sizeof(a));
    hci_packet_parser_get_interface()->parse_read_bd_addr_response(mk(h4, len), &a);
    memcpy(addr, a.address, 6u);
}

void shim_parse_supported_cmds(const uint8_t *h4, uint16_t len, uint8_t out[64])
{
    hci_packet_parser_get_interface()->parse_read_local_supported_commands_response(
        mk(h4, len), out, SHIM_SUPPORTED_COMMANDS_ARRAY_SIZE);
}

void shim_parse_ext_features(const uint8_t *h4, uint16_t len, uint8_t *page, uint8_t *max_page, uint8_t pages[3][8])
{
    bt_device_features_t f[SHIM_MAX_FEATURES_CLASSIC_PAGE_COUNT];
    memset(f, 0, sizeof(f));
    hci_packet_parser_get_interface()->parse_read_local_extended_features_response(
        mk(h4, len), page, max_page, f, SHIM_MAX_FEATURES_CLASSIC_PAGE_COUNT);
    for (int i = 0; i < SHIM_MAX_FEATURES_CLASSIC_PAGE_COUNT; i++) {
        memcpy(pages[i], f[i].as_array, 8u);
    }
}

int shim_max_feature_pages(void)
{
    return SHIM_MAX_FEATURES_CLASSIC_PAGE_COUNT;
}

uint32_t shim_l2cap_host_fc_acl_bufs(void)
{
    return L2CAP_HOST_FC_ACL_BUFS;
}

uint32_t shim_l2cap_host_fc_acl_len(void)
{
    return L2CAP_HOST_FC_ACL_DATA_LEN;
}
