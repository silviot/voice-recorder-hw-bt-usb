/*
 * Smartlink SL6801 OpenFW Clean-Room RW-BT Controller, H4 HCI, SSP Responder
 * and Classic SPP / +XREC v5 Bridge
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include "sl6801_regs.h"
#include "sl6801_rom.h"
#include "openfw.h"
#include "openfw_exp.h"
#include "openfw_crypto.h"
#include "openfw_bt_spp.h"
#include "openfw_xrec.h"

#define TASK_LM                 0x38u
#define TASK_LC                 0x39u
#define TASK_LD                 0x3Bu
#define TASK_HCI                0x3Cu
#define KE_MSG_DEFAULT_HANDLER  0xFFFFu

#define KE_MSG_CONSUMED         0
#define KE_MSG_NO_FREE          1

#define HCI_ACL_MSG_TYPE        0x02u
#define HCI_EVT_MSG_TYPE        0x04u
#define LC_HCI_ACL_DATA_RX      0xE47Du

#define EM_BASE_ADDR            0x40030000u
#define EM_SIZE_BYTES           0x8000u

typedef void (*rwip_eif_cb_t)(uint8_t status);

struct rwip_eif_api {
    void (*read)(uint8_t *buf, uint32_t len, rwip_eif_cb_t cb);
    void (*write)(uint8_t *buf, uint32_t len, rwip_eif_cb_t cb);
    void (*flow_on)(void);
    uint8_t (*flow_off)(void);
};

struct co_list_hdr {
    struct co_list_hdr *next;
};

struct co_list {
    struct co_list_hdr *first;
    struct co_list_hdr *last;
};

struct ke_msg {
    struct co_list_hdr hdr;
    uint8_t  hci_type;
    int8_t   hci_hdr_offset;
    uint16_t hci_len;
    uint16_t id;
    uint16_t dest_id;
    uint16_t src_id;
    uint16_t param_len;
    uint8_t  param[];
};

struct hci_acl_data_pkt {
    uint16_t conhdl;
    uint8_t  pb_bc_flag;
    uint8_t  _pad0;
    uint16_t length;
    uint16_t _pad1;
    uint8_t *buf_ptr;
};

struct hci_tl_env_tag {
    const struct rwip_eif_api *ext_if;
    uint8_t  tx_curr_type;
    uint8_t  rx_state;
    uint8_t  tx_state;
    uint8_t  nb_h2c_cmd_pkts;
    uint8_t  _pad0[0x68];
    struct co_list evt_queue;
    struct co_list acl_queue;
    uint8_t *acl_tx_frag_ptr;
    uint16_t acl_tx_rem_len;
    uint16_t acl_tx_hdr;
    uint16_t host_nb_acl_pkts;
    uint16_t host_unacked_pkts[7];
    uint16_t host_acl_data_pkt_len;
    uint8_t  _pad1[0x3D];
    uint8_t  host_flow_ctrl_en;
    uint32_t _pad2;
    uint8_t  evt_msk[8];
    uint32_t _pad3;
};

struct ke_msg_handler {
    uint16_t id;
    uint16_t pad;
    int (*func)(uint16_t msgid, void const *param, uint16_t dest_id, uint16_t src_id);
};

struct ke_state_handler {
    const struct ke_msg_handler *msg_table;
    uint16_t msg_cnt;
    uint16_t pad;
};

struct ke_task_desc {
    const struct ke_state_handler *state_handler;
    const struct ke_state_handler *default_handler;
    uint8_t *state;
    uint16_t state_max;
    uint16_t idx_max;
};

#define HCI_TL_ENV   ((struct hci_tl_env_tag *)0x00803C28u)
#define HCI_EVT_Q    ((void *)(0x00803C28u + 0x70u))
#define HCI_ACL_Q    ((void *)(0x00803C28u + 0x78u))

/* ============================================================================
 * Bare-Metal Memory Helpers (no libc calls)
 * ============================================================================ */
static void bt_memzero(void *dst, uint32_t n)
{
    volatile uint8_t *d = (volatile uint8_t *)dst;
    for (uint32_t i = 0u; i < n; i++) {
        d[i] = 0u;
    }
}

static void bt_memcpy(void *dst, const void *src, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0u; i < n; i++) {
        d[i] = s[i];
    }
}

static void bt_memmove(void *dst, const void *src, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d < s) {
        for (uint32_t i = 0u; i < n; i++) {
            d[i] = s[i];
        }
    } else if (d > s) {
        for (uint32_t i = n; i > 0u; i--) {
            d[i - 1u] = s[i - 1u];
        }
    }
}

/* ============================================================================
 * Nested PRIMASK Lock, IRQ 26 ISR Top/Bottom-Half, Static 8 KiB Heap & PRNG
 * ============================================================================ */
static volatile uint32_t g_bt_irq_pending    = 0u;
static volatile uint32_t g_bt_irq_count      = 0u;
static volatile uint32_t g_irq_nest          = 0u;
static volatile uint32_t g_irq_saved_primask = 0u;
static uint32_t          g_prng_state        = 0x6D2B79F5u;

#define BT_HEAP_WORDS 2048u
typedef struct {
    uint32_t size_flags; /* aligned total block size including 8B header; bit 0 = used */
    uint32_t tag;
} bt_blk_hdr_t;

static uint32_t g_bt_heap[BT_HEAP_WORDS] __attribute__((aligned(8)));

__attribute__((section(".ramtext")))
void RWBT_IRQHandler(void)
{
    uint32_t stat = RWBT_INTSTAT;
    if (!stat) {
        return;
    }
    RWBT_INTACK = stat;
    g_bt_irq_pending |= stat;
    g_bt_irq_count++;
}

static void bt_irq_lock(void)
{
    uint32_t primask;
    __asm__ volatile ("mrs %0, primask\n cpsid i" : "=r"(primask) :: "memory");
    if (g_irq_nest == 0u) {
        g_irq_saved_primask = primask;
    }
    g_irq_nest++;
}

static void bt_irq_unlock(void)
{
    if (g_irq_nest > 0u) {
        g_irq_nest--;
        if (g_irq_nest == 0u) {
            uint32_t pm = g_irq_saved_primask;
            __asm__ volatile ("msr primask, %0" :: "r"(pm) : "memory");
        }
    }
}

static void bt_irq_bh_poll(void)
{
    uint32_t primask, stat;
    __asm__ volatile ("mrs %0, primask\n cpsid i" : "=r"(primask) :: "memory");
    stat = g_bt_irq_pending;
    g_bt_irq_pending = 0u;
    __asm__ volatile ("msr primask, %0" :: "r"(primask) : "memory");

    if (stat & (1u << 0)) {
        rom_rwbt_cscnt_isr();
    }
    if (stat & (1u << 1)) {
        rom_rwbt_rx_isr();
    }
    if (stat & (1u << 10)) {
        rom_ke_event_set(1u);
    }
}

static uint32_t bt_rand32(void)
{
    uint32_t x = g_prng_state ^ (uint32_t)us_timer_now();
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    if (x == 0u) {
        x = 0x13579BDFu;
    }
    g_prng_state = x;
    return x;
}

static uint32_t bt_rand80(void)
{
    return bt_rand32() % 80u;
}

static void bt_heap_init(void)
{
    bt_blk_hdr_t *blk = (bt_blk_hdr_t *)g_bt_heap;
    blk->size_flags = (uint32_t)(BT_HEAP_WORDS * sizeof(uint32_t));
    blk->tag        = 0u;
}

static void *bt_malloc(uint32_t size, uint8_t type)
{
    uint32_t aligned = ((size + 7u) & ~7u) + (uint32_t)sizeof(bt_blk_hdr_t);
    if (aligned < 16u) {
        aligned = 16u;
    }

    bt_irq_lock();
    uint8_t *base = (uint8_t *)g_bt_heap;
    uint8_t *end  = base + (BT_HEAP_WORDS * sizeof(uint32_t));
    uint8_t *cur  = base;

    while (cur + sizeof(bt_blk_hdr_t) <= end) {
        bt_blk_hdr_t *blk = (bt_blk_hdr_t *)cur;
        uint32_t bsz = blk->size_flags & ~7u;
        if (bsz < sizeof(bt_blk_hdr_t) || cur + bsz > end) {
            break;
        }
        if ((blk->size_flags & 1u) == 0u) {
            /* Coalesce adjacent free blocks forward */
            while (cur + bsz + sizeof(bt_blk_hdr_t) <= end) {
                bt_blk_hdr_t *nxt = (bt_blk_hdr_t *)(cur + bsz);
                uint32_t nsz = nxt->size_flags & ~7u;
                if ((nxt->size_flags & 1u) != 0u || nsz < sizeof(bt_blk_hdr_t)) {
                    break;
                }
                bsz += nsz;
                blk->size_flags = bsz;
            }
            if (bsz >= aligned) {
                uint32_t rem = bsz - aligned;
                if (rem >= 16u) {
                    bt_blk_hdr_t *split = (bt_blk_hdr_t *)(cur + aligned);
                    split->size_flags = rem;
                    split->tag        = 0u;
                    blk->size_flags   = aligned | 1u;
                } else {
                    blk->size_flags   = bsz | 1u;
                }
                blk->tag = type;
                bt_irq_unlock();
                return cur + sizeof(bt_blk_hdr_t);
            }
        }
        cur += bsz;
    }

    bt_irq_unlock();
    return 0;
}

static void bt_free(void *ptr)
{
    if (!ptr) {
        return;
    }
    uint8_t *p    = (uint8_t *)ptr;
    uint8_t *base = (uint8_t *)g_bt_heap;
    uint8_t *end  = base + (BT_HEAP_WORDS * sizeof(uint32_t));
    if (p < base + sizeof(bt_blk_hdr_t) || p >= end) {
        return;
    }
    bt_irq_lock();
    bt_blk_hdr_t *blk = (bt_blk_hdr_t *)(p - sizeof(bt_blk_hdr_t));
    blk->size_flags &= ~1u;
    bt_irq_unlock();
}

/* ============================================================================
 * Clean-Room ke_event Scheduler & RW-BT Active-Link / RF Hook Implementations
 * ============================================================================ */
static void bt_ke_event_schedule(void)
{
    volatile uint32_t *ev_field = (volatile uint32_t *)0x008041E4u;
    void (*const *cbs)(void)    = (void (*const *)(void))0x008041E8u;

    for (uint32_t iter = 0u; iter < 64u; iter++) {
        uint32_t mask = *ev_field & 0x3Fu;
        if (mask == 0u) {
            break;
        }
        for (int b = 5; b >= 0; b--) {
            if (mask & (1u << b)) {
                if (cbs[b]) {
                    cbs[b]();
                } else {
                    bt_irq_lock();
                    *ev_field &= ~(1u << b);
                    bt_irq_unlock();
                }
                break;
            }
        }
    }
}

static void bt_ke_event_set(uint8_t ev)
{
    if (ev <= 5u) {
        bt_irq_lock();
        *(volatile uint32_t *)0x008041E4u |= (1u << ev);
        bt_irq_unlock();
    }
}

static void hook_noop(void) {}
static uint32_t hook_ret0(void) { return 0u; }

static void cs_set_common(uint8_t *cs, uint8_t a_val)
{
    cs[0x0A] = (uint8_t)((cs[0x0A] & 0x03u) | a_val);
    cs[0x14] = *(volatile uint8_t *)0x00802CF0u;
    volatile uint32_t *w30 = (volatile uint32_t *)(cs + 0x30);
    *w30 = (*w30 & ~0x78000000u) | 0x10000000u;
}

static void hook_bb0_2_3(uint8_t *cs) { cs_set_common(cs, 0x40u); }
static void hook_bb0_4_6(uint8_t *cs) { cs_set_common(cs, 0x58u); }
static void hook_bb0_5(uint8_t *cs)   { cs[0x14] = *(volatile uint8_t *)0x00802CF0u; }

static void hook_2904_00(void *cs, uint8_t link_id)
{
    (void)cs;
    volatile uint8_t *env = (volatile uint8_t *)0x0080296Cu + link_id;
    if (env[0x110] == 0u) {
        env[0x14F] = 2u;
        rom_ld_acl_txpwr_init(link_id, 2u, 1u);
    }
}

static void hook_21bc_acl_con_cmp(uint16_t dest_id)
{
    uint32_t link_id = (uint32_t)dest_id >> 8;
    uint8_t *lc      = (uint8_t *)0x00801384u + 0x208u * link_id;

    if (lc[0x55] == 0u) {
        rom_lc_con_cmp_evt(dest_id, 0u);
        lc[0x55] = 1u;
        rom_ld_acl_con_setup1(lc[0x41]);
    }
    lc[0x52] = 1u;

    uint16_t pkt = *(uint16_t *)(lc + 0x22);
    uint16_t m1  = rom_lc_pkt_mask1(pkt, 0x7Fu, lc[0x6C]);
    uint16_t m2  = rom_lc_pkt_mask2(pkt, 0xFAu, lc[0x6F], 0xDBu, lc[0x70], 0xFDu, lc[0x71]);
    *(uint16_t *)(lc + 0x22) = (uint16_t)(m1 | m2);

    rom_ld_acl_con_setup3(lc[0x41]);
    rom_ke_msg_send_basic(0xE485u, dest_id, dest_id);

    uint32_t has_feat = rom_lc_feat_check(lc + 0x6C, 0x12u);
    rom_ld_acl_con_setup2(lc[0x41], 0u, has_feat ? 0u : 1u);

    lc[0x204] = 0u;
    rom_ke_msg_send_basic(0xE4FAu, dest_id, dest_id);
    rom_ke_state_set(dest_id, 5u); /* LC_CONNECTED */
    rom_ke_msg_send_basic(0xE4EEu, dest_id, dest_id);
}

static void hook_3bbc_7_set_evt_mask(const uint8_t *mask)
{
    static const uint8_t mandatory[8] = { 0x00u, 0x60u, 0x04u, 0x00u, 0xF8u, 0x07u, 0x40u, 0xE2u };
    volatile uint8_t *dst = (volatile uint8_t *)0x00803D04u;
    for (int i = 0; i < 8; i++) {
        dst[i] = (uint8_t)(mask[i] | mandatory[i]);
    }
}

static uint8_t rf_txpwr_inc(uint8_t link_id)
{
    volatile uint8_t *cs = (volatile uint8_t *)(0x400302B0u + (uint32_t)link_id * 0x5Cu);
    uint8_t max_pwr = *(volatile uint8_t *)0x00803C0Cu;
    uint8_t cur     = cs[0x14];
    if (cur < max_pwr) {
        uint8_t nxt = (uint8_t)(cur + 1u);
        if ((nxt & 1u) != 0u || nxt == max_pwr) {
            cs[0x14] = nxt;
        }
        return 0u;
    }
    return 1u;
}

static uint8_t rf_txpwr_dec(uint8_t link_id)
{
    volatile uint8_t *cs = (volatile uint8_t *)(0x400302B0u + (uint32_t)link_id * 0x5Cu);
    uint8_t cur = cs[0x14];
    if (cur > 2u) {
        uint8_t nxt = (uint8_t)(cur - 1u);
        cs[0x14] = ((nxt & 1u) == 0u) ? (uint8_t)(cur - 2u) : nxt;
        return 0u;
    }
    return 1u;
}

static uint8_t rf_txpwr_epc(uint8_t link_id, uint8_t req)
{
    if (req == 0u) {
        return rf_txpwr_dec(link_id) ? 0x3Fu : 0x15u;
    }
    if (req == 1u) {
        return rf_txpwr_inc(link_id) ? 0x2Au : 0x15u;
    }
    if (req == 2u) {
        volatile uint8_t *cs = (volatile uint8_t *)(0x400302B0u + (uint32_t)link_id * 0x5Cu);
        cs[0x14] = *(volatile uint8_t *)0x00803C0Cu;
        return 0x2Au;
    }
    return 0u;
}

static int8_t rf_rssi_convert(uint16_t raw)
{
    return (int8_t)((((raw >> 3) << 1) - 0x40u) & 0xFEu);
}

/* ============================================================================
 * TASK_HCI (0x3C), Event Repacking & H4 Transport Engine (0x00803C28)
 * ============================================================================ */
static void openfw_hci_tx_done_cb(uint8_t status)
{
    (void)status;
    rom_ke_event_set(5u);
}

static void openfw_hci_tx_start(uint8_t type, uint8_t *buf, uint16_t len)
{
    buf[-1] = type;
    HCI_TL_ENV->tx_state = 0u;
    HCI_TL_ENV->ext_if->write(buf - 1, (uint32_t)len + 1u, openfw_hci_tx_done_cb);
}

static void openfw_hci_tx_next(void)
{
    struct hci_tl_env_tag *env = HCI_TL_ENV;
    struct ke_msg *msg = (struct ke_msg *)env->evt_queue.first;
    if (msg != 0) {
        env->tx_curr_type = HCI_EVT_MSG_TYPE;
        openfw_hci_tx_start(HCI_EVT_MSG_TYPE, msg->param + msg->hci_hdr_offset, msg->hci_len);
        return;
    }
    uint16_t max_frag = env->host_acl_data_pkt_len;
    if (env->acl_tx_rem_len != 0u) {
        env->acl_tx_hdr = (uint16_t)((env->acl_tx_hdr & 0x3FFFu) | 0x1000u);
    } else {
        msg = (struct ke_msg *)env->acl_queue.first;
        if (!msg) {
            return;
        }
        struct hci_acl_data_pkt *rx = (struct hci_acl_data_pkt *)msg->param;
        if (!rx->buf_ptr) {
            return;
        }
        env->acl_tx_hdr      = (uint16_t)((rx->conhdl & 0x0FFFu) | ((uint16_t)rx->pb_bc_flag << 12));
        env->acl_tx_frag_ptr = rx->buf_ptr;
        env->acl_tx_rem_len  = rx->length;
    }
    uint16_t frag = env->acl_tx_rem_len;
    if (frag > max_frag) {
        env->acl_tx_rem_len = (uint16_t)(frag - max_frag);
        frag = max_frag;
    } else {
        env->acl_tx_rem_len = 0u;
    }
    uint8_t *p = env->acl_tx_frag_ptr;
    env->acl_tx_frag_ptr = p + frag;
    p[-4] = (uint8_t)(env->acl_tx_hdr & 0xFFu);
    p[-3] = (uint8_t)(env->acl_tx_hdr >> 8);
    p[-2] = (uint8_t)(frag & 0xFFu);
    p[-1] = (uint8_t)(frag >> 8);
    env->tx_curr_type = HCI_ACL_MSG_TYPE;
    openfw_hci_tx_start(HCI_ACL_MSG_TYPE, p - 4, (uint16_t)(frag + 4u));
}

static void openfw_hci_tx_kick(void)
{
    if (HCI_TL_ENV->tx_state == 1u) {
        openfw_hci_tx_next();
    }
}

static void openfw_hci_tx_evt_cb(void)
{
    rom_ke_event_clear(5u);
    bt_irq_lock();
    struct hci_tl_env_tag *env = HCI_TL_ENV;
    uint8_t type  = env->tx_curr_type;
    env->tx_state = 1u;
    if (type == HCI_EVT_MSG_TYPE) {
        struct ke_msg *msg = (struct ke_msg *)rom_co_list_pop_front(&env->evt_queue);
        if (msg) {
            rom_ke_msg_free(msg);
        }
    } else if (env->acl_tx_rem_len == 0u) {
        struct ke_msg *msg = (struct ke_msg *)rom_co_list_pop_front(&env->acl_queue);
        if (msg) {
            struct hci_acl_data_pkt *rx = (struct hci_acl_data_pkt *)msg->param;
            rom_em_buf_rx_free(rx->buf_ptr);
            rom_ke_msg_free(msg);
        }
    }
    openfw_hci_tx_next();
    bt_irq_unlock();
}

static uint16_t pack_cmd_cmp(uint8_t *msg, uint16_t opcode, uint8_t plen)
{
    msg[0x0B] = 0x0Eu;
    msg[0x0C] = (uint8_t)(plen + 3u);
    msg[0x0D] = 1u;
    msg[0x0E] = (uint8_t)(opcode & 0xFFu);
    msg[0x0F] = (uint8_t)(opcode >> 8);
    if (HCI_TL_ENV->nb_h2c_cmd_pkts < 7u) {
        HCI_TL_ENV->nb_h2c_cmd_pkts++;
    }
    return (uint16_t)(plen + 5u);
}

static uint16_t pack_cmd_status(uint8_t *msg, uint16_t opcode)
{
    uint8_t status = msg[0x10];
    msg[0x0B] = 0x0Fu;
    msg[0x0C] = 0x04u;
    msg[0x0D] = status;
    msg[0x0E] = 1u;
    msg[0x0F] = (uint8_t)(opcode & 0xFFu);
    msg[0x10] = (uint8_t)(opcode >> 8);
    if (HCI_TL_ENV->nb_h2c_cmd_pkts < 7u) {
        HCI_TL_ENV->nb_h2c_cmd_pkts++;
    }
    return 6u;
}

static uint16_t pack_std_evt(uint8_t *msg, uint8_t evt_code, uint8_t plen)
{
    msg[0x0E] = evt_code;
    msg[0x0F] = plen;
    return (uint16_t)(plen + 2u);
}

static uint16_t rd_le16_unaligned(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd_le32_unaligned(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static int openfw_hci_default_handler(uint16_t msgid, void const *param,
                                      uint16_t dest_id, uint16_t src_id)
{
    (void)dest_id;
    (void)src_id;
    uint8_t *p   = (uint8_t *)(uintptr_t)param;
    uint8_t *msg = p - 0x10;

    if (msgid == LC_HCI_ACL_DATA_RX) {
        msg[0x04] = HCI_ACL_MSG_TYPE;
        msg[0x05] = (uint8_t)(int8_t)-4;
        rom_co_list_push_back(HCI_ACL_Q, msg);
        openfw_hci_tx_kick();
        return KE_MSG_NO_FREE;
    }

    int8_t   hdr_off = -2;
    uint16_t hci_len = 0u;

    switch (msgid) {
    case 0xE079u: hdr_off = -5; hci_len = pack_cmd_cmp(msg, 0x0C01u, 1u); break;
    case 0xE07Au: hdr_off = -5; hci_len = pack_cmd_cmp(msg, 0x0C03u, 1u); break;
    case 0xE083u: hdr_off = -5; hci_len = pack_cmd_cmp(msg, 0x0C13u, 1u); break;
    case 0xE08Au: hdr_off = -5; hci_len = pack_cmd_cmp(msg, 0x0C1Au, 1u); break;
    case 0xE094u: hdr_off = -5; hci_len = pack_cmd_cmp(msg, 0x0C24u, 1u); break;
    case 0xE0B1u: hdr_off = -5; hci_len = pack_cmd_cmp(msg, 0x0C56u, 1u); break;
    case 0xE0C0u: hdr_off = -5; hci_len = pack_cmd_cmp(msg, 0x1009u, 7u); break;
    case 0xE06Au: hdr_off = -5; hci_len = pack_cmd_cmp(msg, 0x040Bu, 7u); break;
    case 0xE06Bu: hdr_off = -5; hci_len = pack_cmd_cmp(msg, 0x040Cu, 7u); break;
    case 0xE06Cu: hdr_off = -5; hci_len = pack_cmd_cmp(msg, 0x040Du, 7u); break;
    case 0xE06Du: hdr_off = -5; hci_len = pack_cmd_cmp(msg, 0x040Eu, 7u); break;
    case 0xE06Fu: hdr_off = -5; hci_len = pack_cmd_cmp(msg, 0x042Bu, 7u); break;
    case 0xE070u: hdr_off = -5; hci_len = pack_cmd_cmp(msg, 0x042Cu, 7u); break;
    case 0xE071u: hdr_off = -5; hci_len = pack_cmd_cmp(msg, 0x042Du, 7u); break;

    case 0xE0C9u:
    case 0xE43Bu: hdr_off = -5; hci_len = pack_cmd_status(msg, 0x0409u); break;
    case 0xE43Au: hdr_off = -5; hci_len = pack_cmd_status(msg, 0x0406u); break;

    case 0xE0D2u:
    case 0xE456u: {
        uint16_t conhdl = rd_le16_unaligned(p + 2);
        p[1] = (uint8_t)conhdl;
        p[2] = (uint8_t)(conhdl >> 8);
        bt_memmove(p + 3, p + 5, 6u);
        p[9]  = p[11];
        p[10] = p[12];
        hci_len = pack_std_evt(msg, 0x03u, 11u);
        break;
    }
    case 0xE457u: hci_len = pack_std_evt(msg, 0x04u, 10u); break;
    case 0xE458u: {
        uint16_t conhdl = rd_le16_unaligned(p + 2);
        p[1] = (uint8_t)conhdl;
        p[2] = (uint8_t)(conhdl >> 8);
        p[3] = p[4];
        hci_len = pack_std_evt(msg, 0x05u, 4u);
        break;
    }
    case 0xE459u: {
        uint16_t conhdl = rd_le16_unaligned(p + 2);
        p[1] = (uint8_t)conhdl;
        p[2] = (uint8_t)(conhdl >> 8);
        hci_len = pack_std_evt(msg, 0x06u, 3u);
        break;
    }
    case 0xE45Au: {
        uint16_t conhdl = rd_le16_unaligned(p + 2);
        p[1] = (uint8_t)conhdl;
        p[2] = (uint8_t)(conhdl >> 8);
        p[3] = p[4];
        hci_len = pack_std_evt(msg, 0x08u, 4u);
        break;
    }
    case 0xE45Du: {
        uint16_t conhdl = rd_le16_unaligned(p + 2);
        p[1] = (uint8_t)conhdl;
        p[2] = (uint8_t)(conhdl >> 8);
        bt_memmove(p + 3, p + 4, 8u);
        hci_len = pack_std_evt(msg, 0x0Bu, 11u);
        break;
    }
    case 0xE45Eu: {
        uint16_t conhdl = rd_le16_unaligned(p + 2);
        uint16_t cid    = rd_le16_unaligned(p + 6);
        uint16_t sub    = rd_le16_unaligned(p + 8);
        p[1] = (uint8_t)conhdl; p[2] = (uint8_t)(conhdl >> 8);
        p[3] = p[4];
        p[4] = (uint8_t)cid;    p[5] = (uint8_t)(cid >> 8);
        p[6] = (uint8_t)sub;    p[7] = (uint8_t)(sub >> 8);
        hci_len = pack_std_evt(msg, 0x0Cu, 8u);
        break;
    }
    case 0xE462u: {
        uint8_t n = p[0];
        if (n > 0u) {
            uint16_t conhdl = rd_le16_unaligned(p + 2);
            uint16_t nb     = rd_le16_unaligned(p + 4);
            p[1] = (uint8_t)conhdl; p[2] = (uint8_t)(conhdl >> 8);
            p[3] = (uint8_t)nb;     p[4] = (uint8_t)(nb >> 8);
        }
        hci_len = pack_std_evt(msg, 0x13u, (uint8_t)(1u + 4u * n));
        break;
    }
    case 0xE464u: hci_len = pack_std_evt(msg, 0x16u, 6u);  break;
    case 0xE465u: hci_len = pack_std_evt(msg, 0x17u, 6u);  break;
    case 0xE466u: hci_len = pack_std_evt(msg, 0x18u, 23u); break;
    case 0xE468u: hci_len = pack_std_evt(msg, 0x1Bu, 3u);  break;
    case 0xE472u: hci_len = pack_std_evt(msg, 0x31u, 6u);  break;
    case 0xE473u: hci_len = pack_std_evt(msg, 0x32u, 9u);  break;
    case 0xE474u: {
        uint32_t pk = rd_le32_unaligned(p + 8);
        p[6] = (uint8_t)pk;
        p[7] = (uint8_t)(pk >> 8);
        p[8] = (uint8_t)(pk >> 16);
        p[9] = (uint8_t)(pk >> 24);
        hci_len = pack_std_evt(msg, 0x33u, 10u);
        break;
    }
    case 0xE477u: hci_len = pack_std_evt(msg, 0x36u, 7u);  break;
    case 0xE478u: hci_len = pack_std_evt(msg, 0x38u, 4u);  break;
    default:
        return KE_MSG_CONSUMED;
    }

    msg[0x04] = HCI_EVT_MSG_TYPE;
    msg[0x05] = (uint8_t)hdr_off;
    *(uint16_t *)(msg + 0x06) = hci_len;
    rom_co_list_push_back(HCI_EVT_Q, msg);
    openfw_hci_tx_kick();
    return KE_MSG_NO_FREE;
}

static uint8_t g_openfw_hci_state[1];
static const struct ke_msg_handler g_openfw_hci_default_tbl[] = {
    { KE_MSG_DEFAULT_HANDLER, 0u, openfw_hci_default_handler },
};
static const struct ke_state_handler g_openfw_hci_default_state = {
    g_openfw_hci_default_tbl, 1u, 0u
};
static const struct ke_task_desc g_openfw_task_desc_hci = {
    0, &g_openfw_hci_default_state, g_openfw_hci_state, 1u, 1u
};

struct hci_cmd_map {
    uint16_t opcode;
    uint16_t msg_id;
    uint8_t  min_len;
};

static const struct hci_cmd_map g_openfw_cmd_tbl[] = {
    { 0x0C01u, 0xE01Cu,   8u },
    { 0x0C13u, 0xE025u, 248u },
    { 0x0C1Au, 0xE02Cu,   1u },
    { 0x0C24u, 0xE034u,   3u },
    { 0x0C52u, 0xE04Du, 241u },
    { 0x0C56u, 0xE04Fu,   1u },
    { 0x1001u, 0xE058u,   0u },
    { 0x1005u, 0xE05Cu,   0u },
    { 0x1009u, 0xE05Du,   0u },
    { 0x0401u, 0xE000u,   5u },
    { 0x0405u, 0xE004u,  14u },
    { 0x0406u, 0xE400u,   4u },
    { 0x0409u, 0xE006u,   7u },
    { 0x040Bu, 0xE008u,  22u },
    { 0x040Cu, 0xE009u,   6u },
    { 0x040Du, 0xE00Au,  23u },
    { 0x040Eu, 0xE00Bu,   6u },
    { 0x042Bu, 0xE011u,   9u },
    { 0x042Cu, 0xE012u,   6u },
    { 0x042Du, 0xE013u,   6u },
};

static int openfw_hci_send_cmd(uint16_t opcode, uint8_t param_len, const uint8_t *param)
{
    struct hci_tl_env_tag *env = HCI_TL_ENV;
    if (opcode != 0x0C35u) {
        if (env->nb_h2c_cmd_pkts == 0u) {
            return 0;
        }
        env->nb_h2c_cmd_pkts--;
    }
    if (opcode == 0x0C03u) {
        rom_hci_cmd_cmp_evt_send(0xE07Au, TASK_HCI, TASK_LM, 0u);
        return 1;
    }
    for (uint32_t i = 0u; i < (uint32_t)(sizeof(g_openfw_cmd_tbl) / sizeof(g_openfw_cmd_tbl[0])); i++) {
        if (g_openfw_cmd_tbl[i].opcode != opcode) {
            continue;
        }
        uint16_t msg_id  = g_openfw_cmd_tbl[i].msg_id;
        uint16_t dest_id = (uint16_t)(msg_id >> 10);
        if (param_len == 0u && g_openfw_cmd_tbl[i].min_len == 0u) {
            rom_ke_msg_send_basic(msg_id, dest_id, TASK_HCI);
            return 1;
        }
        uint16_t alloc_len = (param_len > g_openfw_cmd_tbl[i].min_len)
                           ? param_len : g_openfw_cmd_tbl[i].min_len;
        uint8_t *buf = (uint8_t *)rom_ke_msg_alloc(msg_id, dest_id, TASK_HCI, alloc_len);
        if (!buf) {
            return 0;
        }
        bt_memzero(buf, alloc_len);
        if (param_len > 0u && param) {
            bt_memcpy(buf, param, param_len);
        }
        if (dest_id == TASK_LC) {
            uint16_t conhdl  = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
            uint16_t lc_task = rom_hci_conhdl_to_task(conhdl);
            if (lc_task == 0xFFu) {
                rom_ke_msg_free((struct ke_msg *)(buf - 0x10));
                return 0;
            }
            ((struct ke_msg *)(buf - 0x10))->dest_id = lc_task;
        }
        rom_ke_msg_send(buf);
        return 1;
    }
    return 0;
}

static int openfw_hci_send_acl(uint16_t conhdl, uint8_t pb_bc_flag, uint16_t len, const uint8_t *data)
{
    uint16_t hdl = (uint16_t)(conhdl & 0x0FFFu);
    if (len > 0x03FDu) {
        len = 0x03FDu;
    }
    if (pb_bc_flag <= 3u && rom_hci_conhdl_type(hdl) != 1u) {
        return 0;
    }
    uint16_t dest_id = rom_hci_conhdl_to_task(hdl);
    if (dest_id == 0xFFu) {
        return 0;
    }
    uint8_t *em_buf = rom_em_buf_tx_alloc();
    if (!em_buf) {
        return 0;
    }
    bt_memcpy(em_buf, data, len);
    struct hci_acl_data_pkt *msg =
        (struct hci_acl_data_pkt *)rom_ke_msg_alloc(0xE426u, dest_id, TASK_HCI, (uint16_t)sizeof(*msg));
    if (!msg) {
        return 0;
    }
    msg->conhdl     = hdl;
    msg->pb_bc_flag = pb_bc_flag;
    msg->_pad0      = 0u;
    msg->length     = len;
    msg->_pad1      = 0u;
    msg->buf_ptr    = em_buf;
    rom_ke_msg_send(msg);
    return 1;
}

/* ============================================================================
 * Clean-Room RW-BT Controller Initialization (100% BootROM + OpenFW Hooks)
 * ============================================================================ */
static const uint8_t g_pwr_t_802d80[0x15] = {
    0x00u, 0x00u, 0x01u, 0x01u, 0x01u, 0x01u, 0xFFu, 0xFFu, 0xFFu, 0xFFu,
    0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu
};
static const uint8_t g_pwr_t_802cf4[0x46] = {
    0x1Eu, 0x00u, 0x3Cu, 0x00u, 0x1Du, 0x00u, 0x3Cu, 0x00u, 0x00u, 0x01u,
    0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu,
    0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu,
    0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu,
    0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu,
    0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu,
    0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu
};
static const uint8_t g_pwr_t_802e75[0x10] = {
    0xFFu, 0xFFu, 0xFFu, 0x00u, 0x00u, 0xFFu, 0xFFu, 0xFFu,
    0xFFu, 0xFFu, 0x00u, 0x00u, 0xFFu, 0xFFu, 0x00u, 0x00u
};
static const uint8_t g_pwr_t_802d95[0x15] = {
    0x00u, 0x00u, 0x01u, 0x08u, 0x03u, 0x03u, 0x10u, 0x07u, 0x00u, 0xFFu,
    0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu
};
static const uint8_t g_pwr_t_802d3a[0x46] = {
    0x1Eu, 0x00u, 0x3Cu, 0x00u, 0x05u, 0x00u, 0x3Cu, 0x00u, 0x00u, 0x01u,
    0x1Eu, 0x00u, 0x3Cu, 0x00u, 0x05u, 0x00u, 0x3Cu, 0x00u, 0x00u, 0x02u,
    0x14u, 0x00u, 0x3Cu, 0x00u, 0x05u, 0x00u, 0x3Cu, 0x00u, 0x01u, 0x02u,
    0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu,
    0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu,
    0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu,
    0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu
};
static const uint8_t g_pwr_t_802e85[0x10] = {
    0xFFu, 0xFFu, 0xFFu, 0x00u, 0x01u, 0xFFu, 0xFFu, 0xFFu,
    0x02u, 0xFFu, 0x01u, 0x02u, 0xFFu, 0xFFu, 0x01u, 0x02u
};

static void eif_read(uint8_t *buf, uint32_t len, rwip_eif_cb_t cb)
{
    (void)buf;
    (void)len;
    (void)cb;
}

static void eif_write(uint8_t *buf, uint32_t len, rwip_eif_cb_t cb);

static void eif_flow_on(void) {}
static uint8_t eif_flow_off(void) { return 1u; }

static const struct rwip_eif_api g_openfw_eif_api = {
    eif_read, eif_write, eif_flow_on, eif_flow_off
};

static void rwbt_install_kernel_hooks(void)
{
    REG32(0x00803C1Cu) = (uint32_t)(uintptr_t)bt_irq_lock;
    REG32(0x00803C20u) = (uint32_t)(uintptr_t)bt_irq_unlock;
    REG32(0x00803C24u) = (uint32_t)(uintptr_t)bt_rand80;
    REG32(0x008041CCu) = (uint32_t)(uintptr_t)bt_malloc;
    REG32(0x008041D0u) = (uint32_t)(uintptr_t)bt_free;

    REG32(0x008041D4u) = 0x00030B59u; /* ke_timer_hw_set */
    REG32(0x008041D8u) = 0x00030BF9u; /* ke_timer_set */
    REG32(0x008041DCu) = 0x00030CA9u; /* ke_timer_clear */
    REG32(0x008041E0u) = 0x00030D19u; /* ke_timer_active */

    REG32(0x008041A8u) = 0x00030A01u; /* ke_msg_alloc */
    REG32(0x008041ACu) = 0x00030A3Du; /* ke_msg_send */
    REG32(0x008041B0u) = 0x00030A7Du; /* ke_msg_forward */

    REG32(0x008040D0u) = 0x000304BDu; /* EM buf pool alloc */
    REG32(0x008040D4u) = 0x000304E9u; /* EM buf pool free */
    REG32(0x00804124u) = 0x000305D9u; /* EM RX buf alloc */

    bt_memzero((void *)0x008041B4u, 24u);
    bt_memzero((void *)0x0080415Cu, 0x4Cu);

    rom_ke_event_init();
    REG32(0x00804200u) = (uint32_t)(uintptr_t)bt_ke_event_schedule;
    REG32(0x00804204u) = (uint32_t)(uintptr_t)bt_ke_event_set;

    rom_ke_event_callback_set(0u, (void (*)(void))(uintptr_t)0x000307D5u); /* ke_task_schedule */
    rom_ke_event_callback_set(1u, (void (*)(void))(uintptr_t)0x00030B8Du); /* ke_timer_schedule */
    rom_ke_event_callback_set(5u, openfw_hci_tx_evt_cb);
}

static void rwbt_init_hci_layer(void)
{
    uint32_t *hci_hooks = (uint32_t *)0x00803BBCu;
    hci_hooks[0] = (uint32_t)(uintptr_t)hook_ret0;
    hci_hooks[1] = (uint32_t)(uintptr_t)hook_ret0;
    hci_hooks[2] = (uint32_t)(uintptr_t)hook_ret0;
    hci_hooks[3] = (uint32_t)(uintptr_t)hook_ret0;
    hci_hooks[4] = (uint32_t)(uintptr_t)hook_ret0;
    hci_hooks[5] = (uint32_t)(uintptr_t)hook_ret0;
    hci_hooks[6] = (uint32_t)(uintptr_t)hook_ret0;
    hci_hooks[7] = (uint32_t)(uintptr_t)hook_3bbc_7_set_evt_mask;

    bt_memzero(HCI_TL_ENV, sizeof(*HCI_TL_ENV));
    HCI_TL_ENV->ext_if                = &g_openfw_eif_api;
    HCI_TL_ENV->tx_state              = 1u; /* idle */
    HCI_TL_ENV->nb_h2c_cmd_pkts       = 7u;
    HCI_TL_ENV->host_acl_data_pkt_len = 0xFFFFu;

    static const uint8_t def_msk[8] = { 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu };
    bt_memcpy(HCI_TL_ENV->evt_msk, def_msk, 8u);
    hook_3bbc_7_set_evt_mask(def_msk);

    rom_co_list_init(HCI_EVT_Q);
    rom_co_list_init(HCI_ACL_Q);
    rom_ke_task_create(TASK_HCI, &g_openfw_task_desc_hci);
    rom_ke_state_set(TASK_HCI, 0u);
}

static void rwbt_init_ld_layer(const uint8_t bd_addr[6])
{
    uint32_t *bb0 = (uint32_t *)0x00802BB0u;
    bb0[0] = 0x00036485u;
    bb0[1] = 0x000364EDu;
    bb0[2] = (uint32_t)(uintptr_t)hook_bb0_2_3;
    bb0[3] = (uint32_t)(uintptr_t)hook_bb0_2_3;
    bb0[4] = (uint32_t)(uintptr_t)hook_bb0_4_6;
    bb0[5] = (uint32_t)(uintptr_t)hook_bb0_5;
    bb0[6] = (uint32_t)(uintptr_t)hook_bb0_4_6;
    bb0[7] = 0x00034B5Du;

    uint32_t *ldh = (uint32_t *)0x00802904u;
    ldh[0]  = (uint32_t)(uintptr_t)hook_2904_00; /* +0x00 */
    ldh[1]  = (uint32_t)(uintptr_t)hook_ret0;    /* +0x04 */
    ldh[2]  = (uint32_t)(uintptr_t)hook_ret0;    /* +0x08 */
    ldh[3]  = (uint32_t)(uintptr_t)hook_ret0;    /* +0x0C */
    ldh[4]  = 0x0002E649u;                       /* +0x10 */
    ldh[5]  = 0x00039E01u;                       /* +0x14 */
    ldh[6]  = 0x0003A7EDu;                       /* +0x18 */
    ldh[7]  = 0x0002DE05u;                       /* +0x1C */
    ldh[8]  = 0x0002E041u;                       /* +0x20 */
    ldh[9]  = 0x0002E2A5u;                       /* +0x24 */
    ldh[10] = 0x0002E4C5u;                       /* +0x28 */
    ldh[11] = 0x0003A8B9u;                       /* +0x2C */
    ldh[12] = 0x0003C779u;                       /* +0x30 */
    ldh[13] = 0x0003AE9Du;                       /* +0x34 */
    ldh[14] = 0x0003C119u;                       /* +0x38 */
    ldh[15] = 0x0003A439u;                       /* +0x3C */
    ldh[16] = 0x0003A781u;                       /* +0x40 */
    ldh[17] = (uint32_t)(uintptr_t)hook_ret0;    /* +0x44 */
    ldh[18] = 0x0003B379u;                       /* +0x48 */
    ldh[19] = (uint32_t)(uintptr_t)hook_ret0;    /* +0x4C */
    ldh[20] = (uint32_t)(uintptr_t)hook_ret0;    /* +0x50 */
    ldh[21] = 0x0003DAA9u;                       /* +0x54 */
    ldh[22] = 0x000366E9u;                       /* +0x58 */
    ldh[23] = 0x0003AB65u;                       /* +0x5C */

    uint32_t *sco = (uint32_t *)0x008030CCu;
    sco[0] = (uint32_t)(uintptr_t)hook_ret0;
    sco[1] = 0x0003E791u;
    sco[2] = 0x0003E181u;

    /* Install rwip_rf vtable @ 0x00803BDC */
    uint32_t *rf = (uint32_t *)0x00803BDCu;
    rf[0]  = (uint32_t)(uintptr_t)hook_noop;       /* +0x00 */
    rf[1]  = (uint32_t)(uintptr_t)hook_noop;       /* +0x04 */
    rf[2]  = (uint32_t)(uintptr_t)rf_txpwr_inc;    /* +0x08 */
    rf[3]  = (uint32_t)(uintptr_t)rf_txpwr_dec;    /* +0x0C */
    rf[4]  = (uint32_t)(uintptr_t)rf_txpwr_epc;    /* +0x10 */
    rf[5]  = (uint32_t)(uintptr_t)hook_ret0;       /* +0x14 */
    rf[6]  = (uint32_t)(uintptr_t)hook_ret0;       /* +0x18 */
    rf[7]  = (uint32_t)(uintptr_t)rf_rssi_convert; /* +0x1C */
    rf[8]  = (uint32_t)(uintptr_t)hook_noop;       /* +0x20 */
    rf[9]  = (uint32_t)(uintptr_t)hook_ret0;       /* +0x24 */
    rf[10] = (uint32_t)(uintptr_t)hook_noop;       /* +0x28 */
    rf[11] = (uint32_t)(uintptr_t)hook_noop;       /* +0x2C */
    REG8(0x00803BDCu + 0x30u) = 0x12u;             /* txpwr_max */

    bt_memzero((void *)0x0080296Cu, 0x1C4u);
    bt_memzero((void *)0x00802C04u, 0x28u);
    bt_memzero((void *)0x00802CF4u, 0x374u);
    bt_memzero((void *)0x008030D8u, 0x148u);
    bt_memzero((void *)0x00803220u, 0x6Cu);
    bt_memzero((void *)0x00802BD0u, 8u);
    REG8(0x00802BD1u) = 1u; /* Blocker #2 (0x00D5711C, read by BootROM) */

    bt_memzero((void *)0x00802B48u, 0x68u);
    bt_memzero((void *)0x00802208u, 0x614u);
    bt_memzero((void *)0x00802C2Cu, 0xC0u);
    bt_memzero((void *)0x0080281Cu, 0xE8u);
    bt_memzero((void *)0x00802BD8u, 0x12u);

    /* Blocker #1: 37 non-zero SRAM constants from stock ld_init prelude (0x00D56D00) */
    REG16(0x00802B46u) = 0x001Cu;
    REG16(0x00802B44u) = 0x0064u;
    REG16(0x00802964u) = 0x0032u;
    REG8(0x00803C14u)  = 0x00u;
    REG8(0x00803C15u)  = 0x0Fu;
    REG8(0x00803C16u)  = 0x27u;
    REG8(0x00803C17u)  = 0x06u;
    REG8(0x00802B42u)  = 0x1Eu;
    REG8(0x00802966u)  = 0x07u;
    REG8(0x00802967u)  = 0x0Fu;
    REG8(0x00802968u)  = 0x02u;
    REG8(0x00802969u)  = 0x03u;
    REG8(0x0080296Au)  = 0x1Cu;
    REG8(0x0080296Bu)  = 0x03u;
    static const uint8_t k_2b36[12] = {
        0x01u, 0x13u, 0x12u, 0x11u, 0x10u, 0x0Eu, 0x0Du, 0x0Cu, 0x0Bu, 0x0Au, 0x09u, 0x08u
    };
    for (uint32_t i = 0u; i < 12u; i++) {
        REG8(0x00802B36u + i) = k_2b36[i];
    }
    REG8(0x00804208u)  = 0x02u;
    REG8(0x00804209u)  = 0x0Au;
    REG8(0x0080420Au)  = 0x02u;
    REG8(0x0080420Bu)  = 0x0Eu;
    REG8(0x00804222u)  = 0x03u;
    REG8(0x00804223u)  = 0x16u;
    REG8(0x0080422Au)  = 0x03u;
    REG8(0x0080422Bu)  = 0x16u;
    REG8(0x0080422Cu)  = 0x02u;
    REG8(0x0080422Du)  = 0x12u;
    REG8(0x0080422Eu)  = 0x1Eu;

    REG8(0x00802CF0u)  = 0x12u;
    REG16(0x00803C12u) = 0x14u;
    REG8(0x00802B34u)  = 1u;

    rom_ke_task_create(TASK_LD, (const void *)0x00800608u);
    rom_ld_bd_addr_set(bd_addr);

    for (uint32_t a = EM_BASE_ADDR; a < EM_BASE_ADDR + 0x20u; a += 2u) {
        REG16(a) = 0x6000u;
    }
    for (uint32_t a = 0x00802AF3u; a <= 0x00802B02u; a++) {
        REG8(a) = 0xFFu;
    }

    rom_em_buf_init();
    rom_ld_sched_init();

    for (uint8_t cs = 0u; cs < 10u; cs++) {
        rom_ld_util_cs_init(cs, (const uint8_t *)0x00802C14u, (const uint8_t *)0x00802BEBu);
    }

    REG16(0x00802B48u + 0x40u) = 0x2000u;
    REG16(0x00802B48u + 0x4Eu) = 0x0038u;
    REG8(0x00802B48u  + 0x49u) = 0u;
    REG8(0x00802B48u  + 0x4Du) = 0u;
    REG8(0x00802B48u  + 0x65u) = 1u;
    REG8(0x00802B48u  + 0x66u) = 0u;
    REG8(0x00802B48u  + 0x67u) = 0u;

    REG8(0x0080296Cu  + 0x197u) = 0u;
    REG8(0x0080296Cu  + 0x19Du) = 0u;
    REG8(0x0080296Cu  + 0x19Eu) = 0u;
    REG32(0x0080296Cu + 0x1A4u) = 0x1Bu;
    REG32(0x0080296Cu + 0x1A8u) = 0x070004FFu;

    REG8(0x00802C04u + 0x19u) = 1u;
    REG8(0x00802C04u + 0x1Au) = 0u;
    REG8(0x00802C04u + 0x1Bu) = 0xFAu;
    REG8(0x00802C04u + 0x1Cu) = 0x0Au;
    REG8(0x00802C04u + 0x1Du) = 0u;
    REG32(0x00802C04u + 0x20u) = 0u;

    REG8(0x008030D8u + 0x113u) = 1u;

    bt_memzero((void *)0x400313A8u, 0xF0u);
    RWBT_REG(0x0C0u) = 0x014313A8u;

    /* Copy TX power & RSSI tables */
    bt_memcpy((void *)0x00802CF4u, g_pwr_t_802cf4, (uint32_t)sizeof(g_pwr_t_802cf4));
    bt_memcpy((void *)0x00802D3Au, g_pwr_t_802d3a, (uint32_t)sizeof(g_pwr_t_802d3a));
    bt_memcpy((void *)0x00802D80u, g_pwr_t_802d80, (uint32_t)sizeof(g_pwr_t_802d80));
    bt_memcpy((void *)0x00802D95u, g_pwr_t_802d95, (uint32_t)sizeof(g_pwr_t_802d95));
    bt_memcpy((void *)0x00802E75u, g_pwr_t_802e75, (uint32_t)sizeof(g_pwr_t_802e75));
    bt_memcpy((void *)0x00802E85u, g_pwr_t_802e85, (uint32_t)sizeof(g_pwr_t_802e85));
    REG8(0x00802CF4u + 0x1A1u) = 1u;
    REG8(0x00802CF4u + 0x1A2u) = 2u;

    /* Local LMP features (BR/EDR + SSP + eSCO) */
    REG32(0x00802C04u + 0x00u) = 0xFACDFE7Fu;
    REG32(0x00802C04u + 0x04u) = 0x877BFDDBu;
    REG32(0x00802C0Cu) = 0u;
    REG32(0x00802C10u) = 0u;

    rom_ld_sco_audio_init();
    rom_ld_afh_init();

    RWBT_REG(0x000u) = (RWBT_REG(0x000u) & ~0x01407000u) | 0x00200007u;
    RWBT_REG(0x1E4u) = 0x00001590u;
    RWBT_REG(0x00Cu) = 0xFFFF0003u;
    RWBT_REG(0x1B0u) = 0x0000801Eu;
    REG8(0x40020118u) |= 0x08u;
    RWBT_REG(0x000u) |= 0x00000100u; /* RWBTEN */
}

static void rwbt_init_lc_layer(void)
{
    uint32_t *lc_hooks = (uint32_t *)0x008021BCu;
    lc_hooks[0]  = (uint32_t)(uintptr_t)hook_21bc_acl_con_cmp; /* 0x008021BC */
    lc_hooks[1]  = 0x000290C3u;
    lc_hooks[2]  = 0x0002154Du;
    lc_hooks[3]  = 0x0002D7C1u;
    lc_hooks[4]  = 0x0002AA6Du;
    lc_hooks[5]  = 0x0002B701u;
    lc_hooks[6]  = 0x00029891u;
    lc_hooks[7]  = 0x000336E9u;
    lc_hooks[12] = 0x0003381Du; /* 0x008021EC */
    lc_hooks[13] = 0x000338D9u; /* 0x008021F0 */

    rom_ke_task_create(TASK_LC, (const void *)0x008005F8u);
    for (uint32_t i = 0u; i < 7u; i++) {
        rom_ke_state_set((uint16_t)((i << 8) | TASK_LC), 0u);
        bt_memzero((void *)(0x00801384u + i * 0x208u), 0x208u);
    }

    rom_lc_pca_init(1u, 2u);
    rom_lc_sco_init();
}

static void rwbt_init_lm_layer(void)
{
    bt_memzero((void *)0x008038F4u, 0x220u);
    bt_memzero((void *)0x008032F0u, 0x54u);

    REG32(0x00803BB8u) = 0x00038BCDu;
    REG32(0x00803344u) = 0x00031979u;
    REG32(0x00803BACu) = 0x00037C05u;
    REG32(0x00803BA0u) = 0x000374DDu;
    REG32(0x00803BA4u) = 0x000375B9u;

    rom_rwbt_env_flags_clear();

    REG8(0x008038F4u  + 0x218u) = 1u;
    REG8(0x008038F4u  + 0x21Au) = 1u;
    REG16(0x008032F0u + 0x006u) = 0x1FA0u;
    REG16(0x008032F0u + 0x050u) = 5u;

    rom_lm_ssp_init((uint8_t *)0x008032FAu);
    REG8(0x008032F0u + 0x002u) = 7u;
    REG8(0x008032F0u + 0x003u) = 0x10u;

    rom_lm_nb_sync_init();
    rom_lm_acl_env_init();
    rom_lm_con_info_init();
    rom_lm_sec_p192_init();

    REG16(0x008038F4u + 0x00Au) = 0x0080u;
    REG16(0x008038F4u + 0x012u) = 0x0080u;
    REG16(0x008038F4u + 0x00Cu) = 0x0400u;
    REG16(0x008038F4u + 0x014u) = 0x0400u;
    REG32(0x008038F4u + 0x004u) = rom_lm_slot_to_time(0x0400u);
    REG32(0x008038F4u + 0x000u) = rom_lm_slot_to_time(0x0400u);
    REG16(0x008038F4u + 0x20Eu) = 0x8B33u; /* GIAC LAP */
    REG8(0x008038F4u  + 0x210u) = 0x9Eu;

    rom_ke_task_create(TASK_LM, (const void *)0x00800660u);
    rom_ke_state_set(TASK_LM, 0u);
}

static void rwbt_controller_init(const uint8_t bd_addr[6])
{
    nvic_disable_irq(IRQ_BT);
    nvic_clear_pending(IRQ_BT);
    g_bt_irq_pending = 0u;
    g_bt_irq_count   = 0u;
    g_irq_nest       = 0u;

    bt_memzero((void *)EM_BASE_ADDR, EM_SIZE_BYTES);

    RWBT_CNTL |= (1u << 29);
    for (uint32_t t = 0u; t < 200000u && (RWBT_CNTL & (1u << 29)); t++) {
    }

    bt_heap_init();
    rwbt_install_kernel_hooks();
    rwbt_init_hci_layer();
    rwbt_init_ld_layer(bd_addr);
    rwbt_init_lc_layer();
    rwbt_init_lm_layer();

    RWBT_TIMING      = (RWBT_TIMING & 0x8000F000u) | 0x1B460846u;
    RWBT_REG(0x02Cu) = 0x000001FFu;

    nvic_clear_pending(IRQ_BT);
    nvic_enable_irq(IRQ_BT);
}

/* ============================================================================
 * Host Controller Bring-Up, SSP Link Key Cache, Classic SPP & +XREC v5 Bridge
 * ============================================================================ */
typedef struct {
    uint8_t valid;
    uint8_t bd[6];
    uint8_t key[16];
} bt_link_key_slot_t;

typedef enum {
    HCI_ST_IDLE = 0,
    HCI_ST_WAIT_RESET,
    HCI_ST_WAIT_NAME,
    HCI_ST_WAIT_COD,
    HCI_ST_WAIT_SSP,
    HCI_ST_WAIT_SCAN,
    HCI_ST_READY
} hci_init_stage_t;

static bt_spp_t           g_spp;
static xrec_ctx_t         g_xrec;
static exfat_vol_t       *g_vol = 0;
static int                g_sd_mounted = 0;

static uint8_t            g_active = 0u;
static hci_init_stage_t   g_hci_stage = HCI_ST_IDLE;
static uint8_t            g_bd_addr[6];
static char               g_dev_name[16];
static uint16_t           g_conhdl = 0u;
static uint8_t            g_acl_connected = 0u;
static uint32_t           g_pair_deadline_ms = 0u;
static uint8_t            g_pending_reboot_mode = 0u; /* 0=none, 1=reboot, 2=sd_ota, 3=dfu */
static uint32_t           g_reboot_deadline_ms = 0u;
static uint32_t           g_rx_acl_pkts = 0u;
static uint32_t           g_tx_acl_pkts = 0u;
static bt_link_key_slot_t g_key_cache[4];
static uint8_t            g_key_next = 0u;

/* 1-PDU staging slot so transient EM TX buffer pressure never drops an ACL frame */
static uint16_t           g_pending_tx_len = 0u;
static uint16_t           g_pending_tx_hdl = 0u;
static uint8_t            g_pending_tx_pdu[BT_SPP_TX_MAX_PDU];

static int mem_eq6(const uint8_t a[6], const uint8_t b[6])
{
    for (int i = 0; i < 6; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

static const bt_link_key_slot_t *find_link_key(const uint8_t bd[6])
{
    for (int i = 0; i < 4; i++) {
        if (g_key_cache[i].valid && mem_eq6(g_key_cache[i].bd, bd)) {
            return &g_key_cache[i];
        }
    }
    return 0;
}

static void save_link_keys_to_sd(void)
{
    if (!g_sd_mounted || !g_vol) {
        return;
    }
    exfat_entry_t existing;
    if (exfat_find_path(g_vol, "/bt_keys.bin", &existing) == 0 && !existing.is_dir) {
        (void)exfat_delete_file(g_vol, "/", "bt_keys.bin");
    }
    rtc_datetime_t dt;
    rtc_get(&dt);
    exfat_set_timestamp(g_vol, rtc_to_dos_timestamp(&dt));

    exfat_writer_t w;
    if (exfat_create_file(g_vol, "/", "bt_keys.bin", &w) == 0) {
        uint8_t blob[96];
        blob[0] = 'B';
        blob[1] = 'L';
        blob[2] = 'K';
        blob[3] = '1';
        for (uint32_t i = 0u; i < 4u; i++) {
            uint32_t off = 4u + i * 23u;
            blob[off] = g_key_cache[i].valid ? 1u : 0u;
            bt_memcpy(&blob[off + 1u], g_key_cache[i].bd, 6u);
            bt_memcpy(&blob[off + 7u], g_key_cache[i].key, 16u);
        }
        (void)exfat_write(&w, blob, (uint32_t)sizeof(blob));
        (void)exfat_close_file(&w);
    }
}

static void load_link_keys_from_sd(void)
{
    bt_memzero(g_key_cache, sizeof(g_key_cache));
    g_key_next = 0u;

    if (!g_sd_mounted || !g_vol) {
        return;
    }
    exfat_entry_t ent;
    if (exfat_find_path(g_vol, "/bt_keys.bin", &ent) != 0 || ent.is_dir || ent.size != 96u) {
        return;
    }
    uint8_t blob[96];
    uint32_t nread = 0u;
    if (exfat_read_file(g_vol, &ent, 0u, blob, (uint32_t)sizeof(blob), &nread) != 0 ||
        nread != 96u ||
        blob[0] != 'B' || blob[1] != 'L' || blob[2] != 'K' || blob[3] != '1') {
        return;
    }
    int found_empty = 0;
    for (uint32_t i = 0u; i < 4u; i++) {
        uint32_t off = 4u + i * 23u;
        g_key_cache[i].valid = (blob[off] != 0u) ? 1u : 0u;
        bt_memcpy(g_key_cache[i].bd, &blob[off + 1u], 6u);
        bt_memcpy(g_key_cache[i].key, &blob[off + 7u], 16u);
        if (!found_empty && !g_key_cache[i].valid) {
            g_key_next  = (uint8_t)i;
            found_empty = 1;
        }
    }
}

static void store_link_key(const uint8_t bd[6], const uint8_t key[16])
{
    int slot = -1;
    for (int i = 0; i < 4; i++) {
        if (g_key_cache[i].valid && mem_eq6(g_key_cache[i].bd, bd)) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        slot = g_key_next;
        g_key_next = (uint8_t)((g_key_next + 1u) & 3u);
    }
    g_key_cache[slot].valid = 1u;
    bt_memcpy(g_key_cache[slot].bd, bd, 6u);
    bt_memcpy(g_key_cache[slot].key, key, 16u);
    save_link_keys_to_sd();
}

static void hci_send_scan_enable(uint8_t mode)
{
    (void)openfw_hci_send_cmd(0x0C1Au, 1u, &mode);
}

static void hci_advance_init(uint16_t opcode)
{
    if (opcode == 0x0C03u && g_hci_stage == HCI_ST_WAIT_RESET) {
        uint8_t name_param[248];
        bt_memzero(name_param, sizeof(name_param));
        for (int i = 0; i < 15 && g_dev_name[i] != '\0'; i++) {
            name_param[i] = (uint8_t)g_dev_name[i];
        }
        g_hci_stage = HCI_ST_WAIT_NAME;
        (void)openfw_hci_send_cmd(0x0C13u, 248u, name_param);
    } else if (opcode == 0x0C13u && g_hci_stage == HCI_ST_WAIT_NAME) {
        static const uint8_t cod[3] = { 0x04u, 0x04u, 0x20u }; /* Audio / Portable Recorder */
        g_hci_stage = HCI_ST_WAIT_COD;
        (void)openfw_hci_send_cmd(0x0C24u, 3u, cod);
    } else if (opcode == 0x0C24u && g_hci_stage == HCI_ST_WAIT_COD) {
        uint8_t ssp = 1u;
        g_hci_stage = HCI_ST_WAIT_SSP;
        (void)openfw_hci_send_cmd(0x0C56u, 1u, &ssp);
    } else if (opcode == 0x0C56u && g_hci_stage == HCI_ST_WAIT_SSP) {
        g_hci_stage = HCI_ST_WAIT_SCAN;
        hci_send_scan_enable(0x02u); /* Page Scan only until user opens 60s pairing */
    } else if (opcode == 0x0C1Au && g_hci_stage == HCI_ST_WAIT_SCAN) {
        g_hci_stage = HCI_ST_READY;
        openfw_log_puts("BT HCI READY\n");
    }
}

static void handle_hci_event(const uint8_t *evt, uint32_t len)
{
    if (len < 2u) {
        return;
    }
    uint8_t evt_code = evt[0];

    switch (evt_code) {
    case 0x0Eu: /* Command Complete */
        if (len >= 5u) {
            uint16_t opcode = (uint16_t)evt[3] | ((uint16_t)evt[4] << 8);
            hci_advance_init(opcode);
        }
        break;

    case 0x04u: /* Connection Request */
        if (len >= 12u) {
            uint8_t accept[7];
            bt_memcpy(accept, &evt[2], 6u);
            accept[6] = 0x01u; /* Remain slave */
            (void)openfw_hci_send_cmd(0x0409u, 7u, accept);
        }
        break;

    case 0x03u: /* Connection Complete */
        if (len >= 13u && evt[2] == 0u && evt[11] == 0x01u) {
            uint16_t hdl = (uint16_t)evt[3] | ((uint16_t)evt[4] << 8);
            g_conhdl        = hdl;
            g_acl_connected = 1u;
            bt_spp_reset_link(&g_spp, hdl);
            xrec_on_disconnect(&g_xrec);
            openfw_log_puts("BT ACL CONN hdl=");
            openfw_log_hex(hdl);
            openfw_log_putc('\n');
        }
        break;

    case 0x05u: /* Disconnection Complete */
        if (len >= 6u) {
            uint16_t hdl = (uint16_t)evt[3] | ((uint16_t)evt[4] << 8);
            openfw_log_puts("BT DISC reason=");
            openfw_log_hex(evt[5]);
            openfw_log_putc('\n');
            if (!g_acl_connected || hdl == g_conhdl) {
                g_acl_connected  = 0u;
                g_conhdl         = 0u;
                g_pending_tx_len = 0u;
                bt_spp_reset_link(&g_spp, 0u);
                xrec_on_disconnect(&g_xrec);
            }
        }
        break;

    case 0x16u: /* PIN Code Request */
        if (len >= 8u) {
            uint8_t rep[23];
            bt_memzero(rep, sizeof(rep));
            bt_memcpy(rep, &evt[2], 6u);
            rep[6]  = 4u;
            rep[7]  = '0';
            rep[8]  = '0';
            rep[9]  = '0';
            rep[10] = '0';
            (void)openfw_hci_send_cmd(0x040Du, 23u, rep);
        }
        break;

    case 0x17u: /* Link Key Request */
        if (len >= 8u) {
            const bt_link_key_slot_t *slot = find_link_key(&evt[2]);
            if (slot) {
                uint8_t rep[22];
                bt_memcpy(rep, &evt[2], 6u);
                bt_memcpy(&rep[6], slot->key, 16u);
                (void)openfw_hci_send_cmd(0x040Bu, 22u, rep);
            } else {
                (void)openfw_hci_send_cmd(0x040Cu, 6u, &evt[2]);
            }
        }
        break;

    case 0x18u: /* Link Key Notification */
        if (len >= 24u) {
            store_link_key(&evt[2], &evt[8]);
        }
        break;

    case 0x31u: /* IO Capability Request */
        if (len >= 8u) {
            uint8_t rep[9];
            bt_memcpy(rep, &evt[2], 6u);
            rep[6] = 0x03u; /* NoInputNoOutput (Just Works SSP) */
            rep[7] = 0x00u; /* OOB not present */
            rep[8] = 0x04u; /* MITM not required - General Bonding */
            (void)openfw_hci_send_cmd(0x042Bu, 9u, rep);
        }
        break;

    case 0x33u: /* User Confirmation Request */
        if (len >= 8u) {
            (void)openfw_hci_send_cmd(0x042Cu, 6u, &evt[2]);
        }
        break;

    default:
        break;
    }
}

static void eif_write(uint8_t *buf, uint32_t len, rwip_eif_cb_t cb)
{
    if (buf && len >= 2u) {
        if (buf[0] == HCI_EVT_MSG_TYPE) {
            handle_hci_event(&buf[1], len - 1u);
        } else if (buf[0] == HCI_ACL_MSG_TYPE && len >= 5u) {
            uint16_t hdr_flags = (uint16_t)buf[1] | ((uint16_t)buf[2] << 8);
            uint16_t acl_len   = (uint16_t)buf[3] | ((uint16_t)buf[4] << 8);
            uint16_t conhdl    = (uint16_t)(hdr_flags & 0x0FFFu);
            uint8_t  pb_flag   = (uint8_t)((hdr_flags >> 12) & 0x03u);
            if ((5u + acl_len) <= len) {
                g_rx_acl_pkts++;
                bt_spp_on_acl_rx(&g_spp, conhdl, pb_flag, &buf[5], acl_len);
            }
        }
    }
    if (cb) {
        cb(0u);
    }
}

static void spp_rx_cb(void *user, const uint8_t *data, uint16_t len)
{
    xrec_on_rx((xrec_ctx_t *)user, data, len);
}

static void spp_disc_cb(void *user)
{
    xrec_on_disconnect((xrec_ctx_t *)user);
}

static int xrec_cb_send(void *user, const uint8_t *data, uint16_t len)
{
    (void)user;
    return bt_spp_send(&g_spp, data, len);
}

static void xrec_cb_get_stat(void *user, xrec_stat_t *out)
{
    (void)user;
    uint32_t mv = pmu_read_vbat_mv();
    uint32_t pct = 0u;
    if (mv > 3350u) {
        pct = (mv >= 4150u) ? 100u : ((mv - 3350u) * 100u) / 800u;
    }
    out->voltage_mv    = (uint16_t)mv;
    out->bat_pct       = (uint8_t)pct;
    out->charge_status = (uint8_t)pmu_get_charge_state();
    out->vbus          = (uint8_t)(pmu_is_vbus_present() ? 1u : 0u);
    out->media_status  = recorder_is_saving() ? 0x21u : 0x10u;
    out->rec_sec       = recorder_is_saving() ? recorder_get_seconds() : 0u;
    out->free_mb       = 0u;
    out->total_mb      = 0u;
    if (g_sd_mounted && g_vol) {
        (void)exfat_stat_fs(g_vol, &out->free_mb, &out->total_mb);
    }
}

static void xrec_cb_get_rtc(void *user, xrec_rtc_t *out)
{
    (void)user;
    rtc_datetime_t dt;
    rtc_get(&dt);
    out->year    = dt.year;
    out->month   = dt.month;
    out->day     = dt.day;
    out->hour    = dt.hour;
    out->minute  = dt.minute;
    out->second  = dt.second;
    out->weekday = dt.weekday;
}

static int xrec_cb_set_rtc(void *user, const xrec_rtc_t *rt)
{
    (void)user;
    rtc_datetime_t dt;
    dt.year    = rt->year;
    dt.month   = rt->month;
    dt.day     = rt->day;
    dt.hour    = rt->hour;
    dt.minute  = rt->minute;
    dt.second  = rt->second;
    dt.weekday = rt->weekday;
    rtc_set(&dt);
    if (g_sd_mounted && g_vol) {
        rtc_datetime_t now_dt;
        rtc_get(&now_dt);
        exfat_set_timestamp(g_vol, rtc_to_dos_timestamp(&now_dt));
    }
    return 0;
}

static void xrec_cb_entropy(void *user, uint8_t out[16])
{
    (void)user;
    uint32_t seed[4];
    seed[0] = (uint32_t)us_timer_now();
    seed[1] = (uint32_t)(us_timer_now() >> 32);
    seed[2] = pmu_read_vbat_mv() ^ ((uint32_t)keys_read_adc() << 16);
    seed[3] = bt_rand32();
    uint8_t digest[32];
    openfw_sha256((const uint8_t *)seed, (uint32_t)sizeof(seed), digest);
    bt_memcpy(out, digest, 16u);
}

static void xrec_cb_on_bound_key(void *user, const uint8_t key[32])
{
    (void)user;
    openfw_log_puts("BT XREC PAIRED\n");
    g_pair_deadline_ms = 0u;
    if (g_hci_stage == HCI_ST_READY) {
        hci_send_scan_enable(0x02u); /* Disable Inquiry Scan once paired */
    }
    if (!g_sd_mounted || !g_vol) {
        return;
    }
    exfat_entry_t existing;
    if (exfat_find_path(g_vol, "/cassini.key", &existing) == 0 && !existing.is_dir) {
        (void)exfat_delete_file(g_vol, "/", "cassini.key");
    }
    rtc_datetime_t dt;
    rtc_get(&dt);
    exfat_set_timestamp(g_vol, rtc_to_dos_timestamp(&dt));

    exfat_writer_t w;
    if (exfat_create_file(g_vol, "/", "cassini.key", &w) == 0) {
        uint8_t blob[36];
        blob[0] = 'C';
        blob[1] = 'S';
        blob[2] = 'K';
        blob[3] = '5';
        bt_memcpy(&blob[4], key, 32u);
        (void)exfat_write(&w, blob, (uint32_t)sizeof(blob));
        (void)exfat_close_file(&w);
    }
}

static void xrec_cb_on_reboot(void *user)
{
    (void)user;
    g_pending_reboot_mode = 1u;
    g_reboot_deadline_ms  = millis() + 250u;
}

static void xrec_cb_on_ota(void *user)
{
    (void)user;
    g_pending_reboot_mode = 2u;
    g_reboot_deadline_ms  = millis() + 250u;
}

static int xrec_cb_exp_write(void *user, uint32_t off, const uint8_t *data, uint16_t len)
{
    (void)user;
    return openfw_exp_write(off, data, len);
}

static int xrec_cb_exp_run(void *user, uint32_t entry_off, uint32_t arg,
                           uint32_t *rc, uint32_t *elapsed_ms,
                           uint32_t *out_len, uint32_t *log_widx)
{
    (void)user;
    openfw_exp_result_t res;
    if (openfw_exp_run(entry_off, arg, &res) != 0) {
        return -1;
    }
    if (rc)         *rc         = res.rc;
    if (elapsed_ms) *elapsed_ms = res.elapsed_ms;
    if (out_len)    *out_len    = res.out_len;
    if (log_widx)   *log_widx   = res.log_widx;
    return 0;
}

static uint32_t xrec_cb_read_log(void *user, uint32_t off, uint8_t *dst,
                                 uint32_t max_len, uint32_t *out_widx)
{
    (void)user;
    if (g_openfw_log.magic != OPENFW_LOG_MAGIC || g_openfw_log.size != OPENFW_LOG_SIZE) {
        if (out_widx) {
            *out_widx = 0u;
        }
        return 0u;
    }
    uint32_t widx  = g_openfw_log.write_idx;
    uint32_t avail = (widx < OPENFW_LOG_SIZE) ? widx : OPENFW_LOG_SIZE;
    if (out_widx) {
        *out_widx = widx;
    }
    if (!dst || max_len == 0u || off >= avail) {
        return 0u;
    }
    uint32_t start = (widx <= OPENFW_LOG_SIZE) ? 0u : (widx & (OPENFW_LOG_SIZE - 1u));
    uint32_t n     = avail - off;
    if (n > max_len) {
        n = max_len;
    }
    for (uint32_t i = 0u; i < n; i++) {
        dst[i] = (uint8_t)g_openfw_log.buf[(start + off + i) & (OPENFW_LOG_SIZE - 1u)];
    }
    return n;
}

static void xrec_cb_reset_log(void *user)
{
    (void)user;
    openfw_log_reset();
}

static void xrec_cb_on_dfu(void *user)
{
    (void)user;
    g_pending_reboot_mode = 3u;
    g_reboot_deadline_ms  = millis() + 250u;
}

static int xrec_cb_audio_cfg(void *user, int set, uint8_t *dsp_profile,
                             uint8_t *gain, uint8_t *channels, uint8_t *vor)
{
    (void)user;
    if (set) {
        if (dsp_profile && *dsp_profile <= 2u) {
            recorder_set_dsp_profile((audio_dsp_profile_t)*dsp_profile);
        }
        if (gain && *gain <= 31u) {
            recorder_set_gain(*gain);
        }
        if (channels && (*channels == 1u || *channels == 2u)) {
            recorder_set_channels(*channels);
        }
        if (vor && *vor <= 1u) {
            recorder_set_vor((int)*vor);
        }
    }
    if (dsp_profile) *dsp_profile = (uint8_t)recorder_get_dsp_profile();
    if (gain)        *gain        = recorder_get_gain();
    if (channels)    *channels    = recorder_get_channels();
    if (vor)         *vor         = (uint8_t)(recorder_get_vor() ? 1u : 0u);
    return 0;
}

static int xrec_cb_rec_start(void *user, const char *opt_name, char *out_name, uint32_t out_max)
{
    (void)user;
    (void)opt_name;
    if (!out_name || out_max == 0u) {
        return -1;
    }
    out_name[0] = '\0';
    int was_saving = recorder_is_saving();
    if (recorder_start(g_vol, g_sd_mounted) != 0) {
        return -1;
    }
    if (recorder_is_saving()) {
        const char *fname = recorder_get_filename();
        uint32_t i = 0u;
        while (fname[i] != '\0' && (i + 1u) < out_max) {
            out_name[i] = fname[i];
            i++;
        }
        out_name[i] = '\0';
        return (i > 0u) ? 0 : -1;
    }
    if (!was_saving && !keys_read_rec_switch()) {
        (void)recorder_stop();
    }
    return -1;
}

static int xrec_cb_rec_stop(void *user)
{
    (void)user;
    return recorder_stop();
}

static void load_bound_key_from_sd(void)
{
    if (!g_sd_mounted || !g_vol) {
        return;
    }
    exfat_entry_t ent;
    if (exfat_find_path(g_vol, "/cassini.key", &ent) != 0 || ent.is_dir || ent.size != 36u) {
        return;
    }
    uint8_t blob[36];
    uint32_t nread = 0u;
    if (exfat_read_file(g_vol, &ent, 0u, blob, (uint32_t)sizeof(blob), &nread) == 0 &&
        nread == 36u && blob[0] == 'C' && blob[1] == 'S' && blob[2] == 'K' && blob[3] == '5') {
        xrec_set_bound_key(&g_xrec, &blob[4]);
    }
}

static void derive_local_bd_addr_and_name(void)
{
    uint32_t ef0 = 0u, ef1 = 0u;
    rom_rst_release(0x58u);
    rom_clk_enable(0x1Bu);
    if (EFUSE_WORD(3) & (1u << 31)) {
        ef0 = EFUSE_WORD(0);
        ef1 = EFUSE_WORD(1);
    } else {
        ef0 = (uint32_t)(us_timer_now() & 0xFFFFu) ^ 0xA55Au;
        ef1 = 0x37u;
    }
    /* Locally-administered unicast BR/EDR address (LSB first, g_bd_addr[5] bit 1 = 1) */
    g_bd_addr[0] = (uint8_t)(ef0 & 0xFFu);
    g_bd_addr[1] = (uint8_t)((ef0 >> 8) & 0xFFu);
    g_bd_addr[2] = (uint8_t)(ef1 & 0xFFu);
    g_bd_addr[3] = 0x55u;
    g_bd_addr[4] = 0xCAu;
    g_bd_addr[5] = 0x02u;

    static const char hex[] = "0123456789ABCDEF";
    static const char pfx[] = "Cassini_";
    for (int i = 0; i < 8; i++) {
        g_dev_name[i] = pfx[i];
    }
    g_dev_name[8]  = hex[g_bd_addr[0] >> 4];
    g_dev_name[9]  = hex[g_bd_addr[0] & 0x0Fu];
    g_dev_name[10] = '\0';
}

int bt_xrec_start(exfat_vol_t *vol, int sd_mounted)
{
    static const xrec_ops_t ops = {
        xrec_cb_send,
        xrec_cb_get_stat,
        xrec_cb_get_rtc,
        xrec_cb_set_rtc,
        xrec_cb_entropy,
        xrec_cb_on_bound_key,
        xrec_cb_on_reboot,
        xrec_cb_rec_start,
        xrec_cb_rec_stop,
        xrec_cb_on_ota,
        xrec_cb_exp_write,
        xrec_cb_exp_run,
        xrec_cb_read_log,
        xrec_cb_reset_log,
        xrec_cb_on_dfu,
        xrec_cb_audio_cfg,
    };

    g_vol              = vol;
    g_sd_mounted       = sd_mounted;
    g_conhdl           = 0u;
    g_acl_connected    = 0u;
    g_pair_deadline_ms = 0u;
    g_pending_reboot_mode = 0u;
    g_reboot_deadline_ms  = 0u;
    g_rx_acl_pkts      = 0u;
    g_tx_acl_pkts      = 0u;
    g_pending_tx_len   = 0u;
    g_prng_state      ^= (uint32_t)us_timer_now() ^ pmu_read_vbat_mv();

    derive_local_bd_addr_and_name();
    openfw_log_puts("BT start: ");
    openfw_log_puts(g_dev_name);
    openfw_log_putc('\n');

    xrec_init(&g_xrec, g_sd_mounted ? g_vol : 0, &ops, 0);
    load_bound_key_from_sd();
    load_link_keys_from_sd();

    bt_spp_init(&g_spp, spp_rx_cb, &g_xrec);
    bt_spp_set_disconnect_cb(&g_spp, spp_disc_cb, &g_xrec);

    rwbt_controller_init(g_bd_addr);
    g_hci_stage = HCI_ST_WAIT_RESET;
    g_active    = 1u;
    (void)openfw_hci_send_cmd(0x0C03u, 0u, 0); /* HCI_Reset kicks init chain */
    return 0;
}

void bt_xrec_open_pairing(uint32_t duration_ms)
{
    if (!g_active) {
        return;
    }
    uint8_t rnd[16];
    xrec_cb_entropy(0, rnd);

    uint32_t code = ((uint32_t)rnd[0] | ((uint32_t)rnd[1] << 8) | ((uint32_t)rnd[2] << 16)) % 1000000u;
    char pin[6];
    for (int i = 5; i >= 0; i--) {
        pin[i] = (char)('0' + (code % 10u));
        code /= 10u;
    }

    xrec_open_pairing(&g_xrec, pin);
    g_pair_deadline_ms = millis() + (duration_ms ? duration_ms : 60000u);
    if (g_hci_stage == HCI_ST_READY) {
        hci_send_scan_enable(0x03u); /* Inquiry Scan + Page Scan */
    }
}

void bt_xrec_poll(void)
{
    if (!g_active) {
        return;
    }

    bt_irq_bh_poll();
    bt_ke_event_schedule();

    if (g_xrec.pair_active && g_pair_deadline_ms != 0u &&
        (int32_t)(millis() - g_pair_deadline_ms) >= 0) {
        xrec_close_pairing(&g_xrec);
        g_pair_deadline_ms = 0u;
        if (g_hci_stage == HCI_ST_READY) {
            hci_send_scan_enable(0x02u);
        }
    }

    xrec_poll(&g_xrec);

    if (g_pending_tx_len > 0u) {
        if (!openfw_hci_send_acl(g_pending_tx_hdl, 0x02u, g_pending_tx_len, g_pending_tx_pdu)) {
            return;
        }
        g_tx_acl_pkts++;
        g_pending_tx_len = 0u;
    }

    uint16_t conhdl = 0u;
    uint16_t plen;
    while ((plen = bt_spp_pop_acl_tx(&g_spp, &conhdl, g_pending_tx_pdu,
                                     (uint16_t)sizeof(g_pending_tx_pdu))) > 0u) {
        if (!openfw_hci_send_acl(conhdl, 0x02u, plen, g_pending_tx_pdu)) {
            g_pending_tx_hdl = conhdl;
            g_pending_tx_len = plen;
            break;
        }
        g_tx_acl_pkts++;
    }

    if (g_pending_reboot_mode != 0u && g_pending_tx_len == 0u &&
        (int32_t)(millis() - g_reboot_deadline_ms) >= 0) {
        uint8_t mode = g_pending_reboot_mode;
        g_pending_reboot_mode = 0u;
        if (mode == 2u) {
            pmu_reboot_to_sd_ota();
        } else if (mode == 3u) {
            pmu_reboot_to_usb_bootloader();
        }
        system_reboot();
    }
}

void bt_xrec_stop(void)
{
    nvic_disable_irq(IRQ_BT);
    RWBT_INTCNTL = 0u;
    RWBT_INTACK  = 0xFFFFFFFFu;
    RWBT_CNTL   &= ~(1u << 8);
    RWBT_CNTL   |= (1u << 29);
    for (uint32_t t = 0u; t < 200000u && (RWBT_CNTL & (1u << 29)); t++) {
    }
    nvic_clear_pending(IRQ_BT);

    g_bt_irq_pending      = 0u;
    g_bt_irq_count        = 0u;
    g_active              = 0u;
    g_hci_stage           = HCI_ST_IDLE;
    g_acl_connected       = 0u;
    g_conhdl              = 0u;
    g_pending_tx_len      = 0u;
    g_pair_deadline_ms    = 0u;
    g_pending_reboot_mode = 0u;
    g_reboot_deadline_ms  = 0u;
    xrec_close_pairing(&g_xrec);
    xrec_on_disconnect(&g_xrec);
    bt_spp_reset_link(&g_spp, 0u);
}

void bt_xrec_get_status(bt_status_t *out)
{
    if (!out) {
        return;
    }
    bt_memzero(out, sizeof(*out));
    out->active        = g_active;
    out->hci_ready     = (uint8_t)(g_hci_stage == HCI_ST_READY ? 1u : 0u);
    out->acl_connected = g_acl_connected;
    out->spp_open      = (uint8_t)bt_spp_is_open(&g_spp);
    out->xrec_bound    = g_xrec.bound;
    out->xrec_auth     = g_xrec.auth;
    out->pair_active   = g_xrec.pair_active;
    bt_memcpy(out->pair_pin, g_xrec.pair_pin, 8u);
    out->rx_acl_pkts   = g_rx_acl_pkts;
    out->tx_acl_pkts   = g_tx_acl_pkts;
    out->irq_count     = g_bt_irq_count;
    bt_memcpy(out->bd_addr, g_bd_addr, 6u);
    bt_memcpy(out->dev_name, g_dev_name, 16u);
}
