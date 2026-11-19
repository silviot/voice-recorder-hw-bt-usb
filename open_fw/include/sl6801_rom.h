/*
 * Smartlink SL6801 Internal 384 KB Mask ROM (0x000000..0x05FFFF) Bindings
 *
 * Verified Thumb-2 entry points baked into the SL6801 silicon Mask ROM.
 * Usable by standalone open-source FIRM images with zero stock FIRM code.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef SL6801_ROM_H
#define SL6801_ROM_H

#include <stdint.h>
#include <stddef.h>

#ifdef OPENFW_HOST_MMIO

int      rom_printf(const char *fmt, ...);
void     rom_gpio_config(uint32_t cfg);
int      rom_gpio_read(uint32_t cfg);
void     rom_gpio_write(uint32_t cfg, int val);
size_t   rom_strlen(const char *s);
void    *rom_memcpy(void *dst, const void *src, size_t n);
void    *rom_memset(void *dst, int val, size_t n);
int      rom_rst_is_released(uint32_t rst_id);
void     rom_rst_release(uint32_t rst_id);
void     rom_rst_hold(uint32_t rst_id);
void     rom_clk_enable(uint32_t clk_id);
void     rom_clk_disable(uint32_t clk_id);
void     rom_clk_div(uint32_t clk_id, uint32_t div);
void     rom_clk_set_src(uint32_t clk_id, uint32_t src);
void     rom_clk_set_rate(uint32_t clk_id, uint32_t hz);
uint64_t rom_us_timer_get(void);
void     rom_us_delay(uint32_t us);
void     rom_wdt_reset(uint32_t timeout);

#else

#define ROM_FN(addr, ret, args) ((ret (*) args)((uintptr_t)(addr) | 1u))

/* Debug UART0 printf (115200 bps, PD0 TX) */
#define rom_printf          ROM_FN(0x000002B0, int,      (const char *fmt, ...))

/* GPIO helpers (pin_cfg = (port<<16)|(pin<<11)|(mux<<7)|(init<<6)|(drv<<4)|pull_idx) */
#define rom_gpio_config     ROM_FN(0x000007AC, void,     (uint32_t cfg))
#define rom_gpio_read       ROM_FN(0x000007EC, int,      (uint32_t cfg))
#define rom_gpio_write      ROM_FN(0x0000080E, void,     (uint32_t cfg, int val))

/* Standard libc primitives */
#define rom_strlen          ROM_FN(0x00000AA2, size_t,   (const char *s))
#define rom_memcpy          ROM_FN(0x00000B2A, void *,   (void *dst, const void *src, size_t n))
#define rom_memset          ROM_FN(0x00000BAA, void *,   (void *dst, int val, size_t n))

/* CRU Clock & Reset helpers */
#define rom_rst_is_released ROM_FN(0x00002518, int,      (uint32_t rst_id))
#define rom_rst_release     ROM_FN(0x00002564, void,     (uint32_t rst_id))
#define rom_rst_hold        ROM_FN(0x00002570, void,     (uint32_t rst_id))
#define rom_clk_enable      ROM_FN(0x000027A0, void,     (uint32_t clk_id))
#define rom_clk_disable     ROM_FN(0x00002A7C, void,     (uint32_t clk_id))
#define rom_clk_div         ROM_FN(0x00002D58, void,     (uint32_t clk_id, uint32_t div))
#define rom_clk_set_src     ROM_FN(0x00003118, void,     (uint32_t clk_id, uint32_t src))
#define rom_clk_set_rate    ROM_FN(0x00003904, void,     (uint32_t clk_id, uint32_t hz))

/* 64-bit System Microsecond Timer, Delay & WDT */
#define rom_us_timer_get    ROM_FN(0x0000B6D8, uint64_t, (void))
#define rom_us_delay        ROM_FN(0x0000B730, void,     (uint32_t us))
#define rom_wdt_reset       ROM_FN(0x0000B7B0, void,     (uint32_t timeout))

#endif

/* RivieraWaves RW-BT Classic BR/EDR Mask ROM Controller (0x020000..0x0467FF) */
#define rom_lc_con_cmp_evt       ROM_FN(0x00029F82, void,     (uint16_t dest_id, uint8_t status))
#define rom_lc_pca_init          ROM_FN(0x0002D960, void,     (uint8_t mode, uint32_t step))
#define rom_rwbt_rx_isr          ROM_FN(0x0002F388, void,     (void))
#define rom_rwbt_cscnt_isr       ROM_FN(0x0002F3A4, void,     (void))
#define rom_co_list_init         ROM_FN(0x000303A4, void,     (void *list))
#define rom_co_list_push_back    ROM_FN(0x000303AC, void,     (void *list, void *hdr))
#define rom_co_list_pop_front    ROM_FN(0x000303CC, void *,   (void *list))
#define rom_em_buf_init          ROM_FN(0x0003052A, void,     (void))
#define rom_em_buf_tx_alloc      ROM_FN(0x000305B8, uint8_t *,(void))
#define rom_em_buf_rx_free       ROM_FN(0x000305EC, void,     (void *p))
#define rom_lm_slot_to_time      ROM_FN(0x0003062C, uint32_t, (uint32_t slots))
#define rom_rwbt_env_flags_clear ROM_FN(0x00030656, void,     (void))
#define rom_hci_conhdl_type      ROM_FN(0x0003075C, uint8_t,  (uint16_t h))
#define rom_hci_conhdl_to_task   ROM_FN(0x0003076C, uint16_t, (uint16_t h))
#define rom_ke_task_schedule     ROM_FN(0x000307D4, void,     (void))
#define rom_ke_task_create       ROM_FN(0x0003089C, uint8_t,  (uint8_t type, const void *desc))
#define rom_ke_state_set         ROM_FN(0x0003092C, void,     (uint16_t task_id, uint8_t state))
#define rom_ke_msg_alloc         ROM_FN(0x000309EC, void *,   (uint16_t id, uint16_t dst, uint16_t src, uint16_t len))
#define rom_ke_msg_send          ROM_FN(0x00030A30, void,     (const void *param))
#define rom_ke_msg_send_basic    ROM_FN(0x00030A60, void,     (uint16_t id, uint16_t dst, uint16_t src))
#define rom_ke_msg_free          ROM_FN(0x00030A9E, void,     (const void *msg))
#define rom_ke_timer_schedule    ROM_FN(0x00030B8C, void,     (void))
#define rom_ke_event_init        ROM_FN(0x00030DA4, void,     (void))
#define rom_ke_event_schedule    ROM_FN(0x00030DB4, void,     (void))
#define rom_ke_event_set         ROM_FN(0x00030DC0, void,     (uint8_t ev))
#define rom_ke_event_callback_set ROM_FN(0x00030DCC, uint8_t, (uint8_t ev, void (*cb)(void)))
#define rom_ke_event_clear       ROM_FN(0x00030DE4, void,     (uint8_t ev))
#define rom_lm_con_info_init     ROM_FN(0x00031188, void,     (void))
#define rom_lc_pkt_mask1         ROM_FN(0x000311CC, uint16_t, (uint16_t pkt, uint8_t m, uint8_t f))
#define rom_lc_pkt_mask2         ROM_FN(0x00031214, uint16_t, (uint16_t pkt, uint8_t m1, uint8_t f1, uint8_t m2, uint8_t f2, uint8_t m3, uint8_t f3))
#define rom_lm_ssp_init          ROM_FN(0x00031464, void,     (uint8_t *rand_out_16b))
#define rom_lc_feat_check        ROM_FN(0x0003175C, uint32_t, (uint8_t *feats, uint8_t bit))
#define rom_lm_sec_p192_init     ROM_FN(0x00031DE4, void,     (void))
#define rom_lm_acl_env_init      ROM_FN(0x000328F8, void,     (void))
#define rom_hci_cmd_cmp_evt_send ROM_FN(0x00032E40, void,     (uint16_t id, uint16_t dst, uint16_t src, uint8_t len))
#define rom_lc_sco_init          ROM_FN(0x000338DE, void,     (void))
#define rom_ld_acl_con_setup1    ROM_FN(0x00036550, void,     (uint8_t link_id))
#define rom_ld_acl_con_setup2    ROM_FN(0x00036804, void,     (uint8_t link_id, uint8_t a, uint8_t b))
#define rom_lm_nb_sync_init      ROM_FN(0x00038D18, void,     (void))
#define rom_ld_afh_init          ROM_FN(0x00039554, void,     (void))
#define rom_ld_util_cs_init      ROM_FN(0x0003A42C, void,     (uint8_t cs_idx, const uint8_t bd[6], const uint8_t bch[5]))
#define rom_ld_acl_con_setup3    ROM_FN(0x0003A774, void,     (uint8_t link_id))
#define rom_ld_bd_addr_set       ROM_FN(0x0003BA30, void,     (const uint8_t bd_addr[6]))
#define rom_ld_acl_txpwr_init    ROM_FN(0x0003BD28, void,     (uint8_t link_id, uint8_t a, uint8_t b))
#define rom_ld_sched_init        ROM_FN(0x0003D23C, void,     (void))
#define rom_bt_int_disable       ROM_FN(0x0003DE80, void,     (void))
#define rom_bt_int_restore       ROM_FN(0x0003DE8C, void,     (void))
#define rom_ld_sco_audio_init    ROM_FN(0x0003DFCC, void,     (void))

#endif /* SL6801_ROM_H */
