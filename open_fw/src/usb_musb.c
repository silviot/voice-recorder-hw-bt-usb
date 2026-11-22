/*
 * Smartlink SL6801 Bare-Metal Mentor MUSB Full-Speed USB MSC Driver (0x40040000)
 *
 * Implements clean-room Full-Speed USB 2.0 Device enumeration (EP0) and
 * Bulk-Only Transport (EP1 IN/OUT, 64 B packets) over `usb_scsi.c`, running
 * purely in polled mode without relying on BootROM's closed MSC handlers.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include "sl6801_regs.h"
#include "openfw.h"
#include "openfw_exp.h"
#include "openfw_usb_msc.h"

typedef enum {
    EP0_IDLE = 0,
    EP0_TX   = 1,
} ep0_state_t;

static usb_msc_state_t g_msc;
static int             g_usb_active = 0;
static uint32_t        g_rx_pkts    = 0u;
static uint32_t        g_tx_pkts    = 0u;
static uint64_t        g_rx_bytes   = 0u;
static uint64_t        g_tx_bytes   = 0u;
static uint8_t         g_diag_sd_mounted = 0u;
static uint8_t         g_diag_rec_active = 0u;
static uint8_t            g_diag_mic_gain   = 20u;
static usb_audio_cfg_cb_t g_audio_cfg_cb    = 0;
static uint8_t            g_usb_reboot_mode = 0u;
static uint32_t        g_usb_reboot_deadline = 0u;

static ep0_state_t     g_ep0_state      = EP0_IDLE;
static const uint8_t  *g_ep0_tx_ptr     = 0;
static uint16_t        g_ep0_tx_rem     = 0u;
static uint8_t         g_ep0_zlp        = 0u;
static uint8_t         g_pending_addr   = 0u;
static uint8_t         g_set_addr_pend  = 0u;
static uint8_t         g_usb_cfg        = 0u;
static uint8_t         g_ep0_reply[2];

static const uint8_t g_dev_desc[18] = {
    18u, 0x01u,             /* bLength, bDescriptorType = DEVICE */
    0x00u, 0x02u,           /* bcdUSB = 2.00 */
    0x00u, 0x00u, 0x00u,    /* Class/SubClass/Proto (per-interface) */
    64u,                    /* bMaxPacketSize0 */
    0x09u, 0x12u,           /* idVendor  = 0x1209 (pid.codes open-source VID) */
    0x01u, 0x00u,           /* idProduct = 0x0001 */
    0x00u, 0x01u,           /* bcdDevice = 1.00 */
    1u, 2u, 3u,             /* iManufacturer, iProduct, iSerialNumber */
    1u,                     /* bNumConfigurations */
};

static const uint8_t g_cfg_desc[32] = {
    /* Configuration Descriptor (9 B) */
    9u, 0x02u, 32u, 0u, 1u, 1u, 0u, 0x80u, 250u,
    /* Interface 0: MSC SCSI Bulk-Only Transport (9 B) */
    9u, 0x04u, 0u, 0u, 2u, 0x08u, 0x06u, 0x50u, 0u,
    /* Endpoint 1 IN (Bulk, 64 B) (7 B) */
    7u, 0x05u, 0x81u, 0x02u, 64u, 0u, 0u,
    /* Endpoint 1 OUT (Bulk, 64 B) (7 B) */
    7u, 0x05u, 0x01u, 0x02u, 64u, 0u, 0u,
};

static const uint8_t g_str0_lang[4] = { 4u, 0x03u, 0x09u, 0x04u };

static const uint8_t g_str1_mfr[16] = {
    16u, 0x03u,
    'C', 0, 'a', 0, 's', 0, 's', 0, 'i', 0, 'n', 0, 'i', 0,
};

static const uint8_t g_str2_prod[30] = {
    30u, 0x03u,
    'C', 0, 'a', 0, 's', 0, 's', 0, 'i', 0, 'n', 0, 'i', 0,
    ' ', 0, 'O', 0, 'p', 0, 'e', 0, 'n', 0, 'F', 0, 'W', 0,
};

static const uint8_t g_str3_ser[18] = {
    18u, 0x03u,
    'O', 0, 'P', 0, 'E', 0, 'N', 0, 'F', 0, 'W', 0, '0', 0, '1', 0,
};

static void musb_fifo_read(uint8_t ep, uint8_t *dst, uint16_t len)
{
    uint32_t addr = MUSB_FIFO_ADDR(ep);
    uint16_t i = 0u;
    if (((uintptr_t)dst & 3u) == 0u) {
        uint32_t *d32 = (uint32_t *)(void *)dst;
        while ((uint16_t)(i + 4u) <= len) {
            *d32++ = openfw_mmio_rd32(addr);
            i = (uint16_t)(i + 4u);
        }
    } else {
        while ((uint16_t)(i + 4u) <= len) {
            uint32_t w = openfw_mmio_rd32(addr);
            dst[i + 0u] = (uint8_t)(w);
            dst[i + 1u] = (uint8_t)(w >> 8);
            dst[i + 2u] = (uint8_t)(w >> 16);
            dst[i + 3u] = (uint8_t)(w >> 24);
            i = (uint16_t)(i + 4u);
        }
    }
    while (i < len) {
        dst[i++] = openfw_mmio_rd8(addr);
    }
}

static void musb_fifo_write(uint8_t ep, const uint8_t *src, uint16_t len)
{
    uint32_t addr = MUSB_FIFO_ADDR(ep);
    uint16_t i = 0u;
    if (((uintptr_t)src & 3u) == 0u) {
        const uint32_t *s32 = (const uint32_t *)(const void *)src;
        while ((uint16_t)(i + 4u) <= len) {
            openfw_mmio_wr32(addr, *s32++);
            i = (uint16_t)(i + 4u);
        }
    } else {
        while ((uint16_t)(i + 4u) <= len) {
            uint32_t w = (uint32_t)src[i + 0u]
                       | ((uint32_t)src[i + 1u] << 8)
                       | ((uint32_t)src[i + 2u] << 16)
                       | ((uint32_t)src[i + 3u] << 24);
            openfw_mmio_wr32(addr, w);
            i = (uint16_t)(i + 4u);
        }
    }
    while (i < len) {
        openfw_mmio_wr8(addr, src[i++]);
    }
}

static void musb_flush_ep(uint8_t ep)
{
    openfw_mmio_wr8(MUSB_INDEX_ADDR, ep & 0x0Fu);
    if (ep == 0u) {
        uint16_t csr0 = openfw_mmio_rd16(MUSB_CSR0_ADDR);
        if (csr0 & 0x0003u) {
            openfw_mmio_wr16(MUSB_CSR0_ADDR, csr0 | 0x0100u); /* FlushFIFO */
        }
        return;
    }
    openfw_mmio_wr16(MUSB_TXCSR_ADDR, openfw_mmio_rd16(MUSB_TXCSR_ADDR) | 0x0008u); /* TXCSR.FlushFIFO */
    for (int t = 0; t < 10000 && (openfw_mmio_rd16(MUSB_TXCSR_ADDR) & 0x0001u); t++) {
    }
    openfw_mmio_wr16(MUSB_RXCSR_ADDR, openfw_mmio_rd16(MUSB_RXCSR_ADDR) | 0x0010u); /* RXCSR.FlushFIFO */
    for (int t = 0; t < 10000 && (openfw_mmio_rd16(MUSB_RXCSR_ADDR) & 0x0001u); t++) {
    }
}

static void musb_configure_ep1_msc(void)
{
    openfw_mmio_wr8(MUSB_INDEX_ADDR,   1u);
    openfw_mmio_wr16(MUSB_TXMAXP_ADDR, 64u);
    openfw_mmio_wr16(MUSB_TXCSR_ADDR,  0x0048u); /* ClrDataTog | FlushFIFO */
    openfw_mmio_wr16(MUSB_RXMAXP_ADDR, 64u);
    openfw_mmio_wr16(MUSB_RXCSR_ADDR,  0x0090u); /* ClrDataTog | FlushFIFO */
}

static void ep0_send_next_chunk(void)
{
    uint16_t chunk = (g_ep0_tx_rem > 64u) ? 64u : g_ep0_tx_rem;
    if (chunk > 0u) {
        musb_fifo_write(0u, g_ep0_tx_ptr, chunk);
        g_ep0_tx_ptr += chunk;
        g_ep0_tx_rem  = (uint16_t)(g_ep0_tx_rem - chunk);
    }
    uint16_t csr_flags = 0x0002u; /* TxPktRdy */
    if (g_ep0_tx_rem == 0u) {
        if (chunk == 64u && g_ep0_zlp) {
            /* Short exact-64B-multiple descriptor: defer DataEnd until trailing ZLP */
            g_ep0_zlp = 0u;
        } else {
            csr_flags |= 0x0008u; /* DataEnd */
            g_ep0_state = EP0_IDLE;
            g_ep0_zlp   = 0u;
        }
    }
    openfw_mmio_wr16(MUSB_CSR0_ADDR, csr_flags);
}

static void ep0_start_in(const uint8_t *data, uint16_t data_len, uint16_t req_len)
{
    uint16_t xfer = (data_len < req_len) ? data_len : req_len;
    g_ep0_tx_ptr = data;
    g_ep0_tx_rem = xfer;
    g_ep0_zlp    = (xfer > 0u && xfer < req_len && (xfer & 63u) == 0u) ? 1u : 0u;
    g_ep0_state  = EP0_TX;
    openfw_mmio_wr16(MUSB_CSR0_ADDR, 0x0040u); /* ServicedRxPktRdy */
    ep0_send_next_chunk();
}

static void ep0_handle_setup(const uint8_t setup[8])
{
    uint8_t  bmReq   = setup[0];
    uint8_t  bReq    = setup[1];
    uint16_t wValue  = (uint16_t)setup[2] | ((uint16_t)setup[3] << 8);
    uint16_t wIndex  = (uint16_t)setup[4] | ((uint16_t)setup[5] << 8);
    uint16_t wLength = (uint16_t)setup[6] | ((uint16_t)setup[7] << 8);

    /* USB Mass Storage class-specific requests (Interface 0) */
    if ((bmReq & 0x7Fu) == 0x21u && wIndex == 0u) {
        if (bReq == 0xFEu && bmReq == 0xA1u && wLength >= 1u) { /* GET_MAX_LUN */
            g_ep0_reply[0] = 0u;
            ep0_start_in(g_ep0_reply, 1u, wLength);
            return;
        }
        if (bReq == 0xFFu && bmReq == 0x21u && wLength == 0u) { /* BULK_ONLY_RESET */
            usb_msc_reset(&g_msc);
            musb_configure_ep1_msc();
            openfw_mmio_wr8(MUSB_INDEX_ADDR, 0u);
            openfw_mmio_wr16(MUSB_CSR0_ADDR, 0x0048u);
            return;
        }
    }

    if ((bmReq & 0x60u) != 0x00u) {
        openfw_mmio_wr16(MUSB_CSR0_ADDR, 0x0060u); /* ServicedRxPktRdy | SendStall */
        return;
    }

    switch (bReq) {
    case 0x06u: /* GET_DESCRIPTOR */ {
        uint8_t desc_type = (uint8_t)(wValue >> 8);
        uint8_t desc_idx  = (uint8_t)(wValue & 0xFFu);
        if (desc_type == 0x01u) {
            ep0_start_in(g_dev_desc, (uint16_t)sizeof(g_dev_desc), wLength);
            return;
        }
        if (desc_type == 0x02u) {
            ep0_start_in(g_cfg_desc, (uint16_t)sizeof(g_cfg_desc), wLength);
            return;
        }
        if (desc_type == 0x03u) {
            if (desc_idx == 0u) {
                ep0_start_in(g_str0_lang, (uint16_t)sizeof(g_str0_lang), wLength);
                return;
            }
            if (desc_idx == 1u) {
                ep0_start_in(g_str1_mfr, (uint16_t)sizeof(g_str1_mfr), wLength);
                return;
            }
            if (desc_idx == 2u) {
                ep0_start_in(g_str2_prod, (uint16_t)sizeof(g_str2_prod), wLength);
                return;
            }
            if (desc_idx == 3u) {
                ep0_start_in(g_str3_ser, (uint16_t)sizeof(g_str3_ser), wLength);
                return;
            }
        }
        openfw_mmio_wr16(MUSB_CSR0_ADDR, 0x0060u);
        break;
    }

    case 0x05u: /* SET_ADDRESS (deferred until after status stage completes) */
        g_pending_addr  = (uint8_t)(wValue & 0x7Fu);
        g_set_addr_pend = 1u;
        openfw_mmio_wr16(MUSB_CSR0_ADDR, 0x0048u);
        break;

    case 0x09u: /* SET_CONFIGURATION */
        if ((wValue & 0xFFu) > 1u) {
            openfw_mmio_wr16(MUSB_CSR0_ADDR, 0x0060u); /* ServicedRxPktRdy | SendStall */
            break;
        }
        g_usb_cfg = (uint8_t)(wValue & 0xFFu);
        if (g_usb_cfg == 1u) {
            musb_configure_ep1_msc();
        }
        usb_msc_reset(&g_msc);
        openfw_mmio_wr8(MUSB_INDEX_ADDR, 0u);
        openfw_mmio_wr16(MUSB_CSR0_ADDR, 0x0048u);
        break;

    case 0x08u: /* GET_CONFIGURATION */
        g_ep0_reply[0] = g_usb_cfg;
        ep0_start_in(g_ep0_reply, 1u, wLength);
        break;

    case 0x00u: /* GET_STATUS */
        g_ep0_reply[0] = 0u;
        g_ep0_reply[1] = 0u;
        ep0_start_in(g_ep0_reply, 2u, wLength);
        break;

    case 0x01u: /* CLEAR_FEATURE */
        if ((bmReq & 0x1Fu) == 0x02u && wValue == 0u) {
            uint8_t ep = (uint8_t)(wIndex & 0x0Fu);
            if (ep == 1u) {
                openfw_mmio_wr8(MUSB_INDEX_ADDR, 1u);
                if (wIndex & 0x80u) {
                    openfw_mmio_wr16(MUSB_TXCSR_ADDR, (uint16_t)((openfw_mmio_rd16(MUSB_TXCSR_ADDR) & ~0x0030u) | 0x0040u)); /* Clear SendStall + ClrDataTog */
                } else {
                    openfw_mmio_wr16(MUSB_RXCSR_ADDR, (uint16_t)((openfw_mmio_rd16(MUSB_RXCSR_ADDR) & ~0x0060u) | 0x0080u)); /* Clear SendStall + ClrDataTog */
                }
                openfw_mmio_wr8(MUSB_INDEX_ADDR, 0u);
                openfw_mmio_wr16(MUSB_CSR0_ADDR, 0x0048u);
                break;
            }
            if (ep == 0u) {
                openfw_mmio_wr16(MUSB_CSR0_ADDR, 0x0048u);
                break;
            }
        }
        openfw_mmio_wr16(MUSB_CSR0_ADDR, 0x0060u); /* ServicedRxPktRdy | SendStall */
        break;

    case 0x0Bu: /* SET_INTERFACE */
        openfw_mmio_wr16(MUSB_CSR0_ADDR, 0x0048u);
        break;

    default:
        openfw_mmio_wr16(MUSB_CSR0_ADDR, 0x0060u);
        break;
    }
}

static void ep0_service(void)
{
    openfw_mmio_wr8(MUSB_INDEX_ADDR, 0u);
    uint16_t csr = openfw_mmio_rd16(MUSB_CSR0_ADDR);

    if (csr & 0x0004u) { /* SentStall */
        openfw_mmio_wr16(MUSB_CSR0_ADDR, 0x0000u);
        g_ep0_state  = EP0_IDLE;
        g_ep0_tx_rem = 0u;
        g_ep0_zlp    = 0u;
        return;
    }
    if (csr & 0x0010u) { /* SetupEnd */
        openfw_mmio_wr16(MUSB_CSR0_ADDR, 0x0080u); /* ServicedSetupEnd */
        g_ep0_state     = EP0_IDLE;
        g_ep0_tx_rem    = 0u;
        g_ep0_zlp       = 0u;
        g_set_addr_pend = 0u;
    }

    if (g_set_addr_pend) {
        openfw_mmio_wr8(MUSB_FADDR_ADDR, g_pending_addr);
        g_set_addr_pend = 0u;
    }

    if (g_ep0_state == EP0_TX && !(openfw_mmio_rd16(MUSB_CSR0_ADDR) & 0x0002u)) {
        ep0_send_next_chunk();
        return;
    }

    if (openfw_mmio_rd16(MUSB_CSR0_ADDR) & 0x0001u) { /* RxPktRdy */
        uint16_t cnt = openfw_mmio_rd16(MUSB_COUNT0_ADDR) & 0x007Fu;
        if (cnt == 8u) {
            uint8_t setup[8] __attribute__((aligned(4)));
            musb_fifo_read(0u, setup, 8u);
            ep0_handle_setup(setup);
        } else {
            uint8_t junk[64] __attribute__((aligned(4)));
            if (cnt > 64u) {
                cnt = 64u;
            }
            if (cnt > 0u) {
                musb_fifo_read(0u, junk, cnt);
            }
            openfw_mmio_wr16(MUSB_CSR0_ADDR, 0x0048u);
        }
    }
}

static void usb_phy_power_on(void)
{
    /* 1. PLL0 runs at 192 MHz; enable 48 MHz USB tap (clk 0x09) */
    openfw_mmio_set32(CRU_CLK_SRC_GATE_ADDR, 1u << 6);

    /* 2. Release CRU reset ID 0x23 (bank 1 bit 3) + 2 ms settle */
    openfw_mmio_set32(CRU_RST_SET1_ADDR,  1u << 3);
    openfw_mmio_set32(CRU_RST_STAT1_ADDR, 1u << 3);
    {
        uint64_t t0 = us_timer_now();
        while (!(openfw_mmio_rd32(CRU_RST_STAT1_ADDR) & (1u << 3))) {
            if ((uint32_t)(us_timer_now() - t0) >= 10000u) {
                break;
            }
        }
    }
    mdelay(2u);

    /* 3. Enable USB clock (clk 0x12) & SYSCTL USB bits (0 & 6) */
    openfw_mmio_set32(CRU_CLK_EN_USB_ADDR,  1u);
    openfw_mmio_set32(SYSCTL_USB_CTRL_ADDR, (1u << 0) | (1u << 6));

    /* 4. PB22 & PB23 = Analog (mux 15) for DP/DM */
    gpio_set_mux(GPIO_PORT_B, 22u, 15u);
    gpio_set_mux(GPIO_PORT_B, 23u, 15u);
}

void usb_phy_det_arm(void)
{
    usb_phy_power_on();
    openfw_mmio_wr32(USB_PHY_INT_STATUS_ADDR, 0x7u); /* W1C */
    openfw_mmio_rmw32(USB_PHY_CTRL_ADDR, 0x000000F0u, 0x0000011Fu);
    openfw_mmio_set32(USB_PHY_INTE_ADDR, 0x3u);
}

int usb_phy_det_poll_host(void)
{
    uint32_t st = openfw_mmio_rd32(USB_PHY_INT_STATUS_ADDR);
    if (st & 0x1u) {
        openfw_mmio_wr32(USB_PHY_INT_STATUS_ADDR, st | 0x7u);
        openfw_mmio_clr32(USB_PHY_INTE_ADDR, 0x3u);
        openfw_mmio_clr32(USB_PHY_CTRL_ADDR, 0x103u);
        return 1;
    }
    return 0;
}

uint16_t usb_musb_frame(void)
{
    return openfw_mmio_rd16(MUSB_FRAME_ADDR);
}

static inline uint32_t musb_get_le32(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static inline void musb_put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static inline void musb_put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

#if OPENFW_ENABLE_USB_EXP
static void pack_exp_result(uint8_t out[32], const openfw_exp_result_t *res)
{
    musb_put_le32(&out[0],  res->magic);
    musb_put_le32(&out[4],  res->rc);
    musb_put_le32(&out[8],  res->elapsed_ms);
    musb_put_le32(&out[12], res->out_len);
    musb_put_le32(&out[16], res->log_widx);
    musb_put_le32(&out[20], res->run_count);
    musb_put_le32(&out[24], res->arena_crc_and_size);
    musb_put_le32(&out[28], res->arena_base);
}
#endif

static int musb_vendor_scsi_cb(void *user, const uint8_t cdb[16],
                               uint8_t *io_buf, uint16_t *io_len, int is_out)
{
    (void)user;
    if (!cdb || !io_buf || !io_len) {
        return -1;
    }

    switch (cdb[1]) {
    case 0x01u: /* GET_INFO (IN 32 B) */ {
        if (is_out) {
            return -1;
        }
        uint8_t info[32];
        info[0] = 'O';
        info[1] = 'F';
        info[2] = 'W';
        info[3] = '1';
        musb_put_le32(&info[4],  millis());
        musb_put_le32(&info[8],  pmu_read_vbat_mv());
        info[12] = (uint8_t)pmu_get_charge_state();
        info[13] = g_diag_sd_mounted;
        info[14] = g_diag_rec_active;
        info[15] = g_diag_mic_gain;
        musb_put_le32(&info[16], g_openfw_log.write_idx);
        musb_put_le32(&info[20], g_openfw_log.dropped_bytes);
        musb_put_le32(&info[24], OPENFW_LOG_SIZE);
        musb_put_le32(&info[28], g_prev_fault_code);

        uint16_t n = (*io_len < 32u) ? *io_len : 32u;
        for (uint16_t i = 0u; i < n; i++) {
            io_buf[i] = info[i];
        }
        *io_len = n;
        return 0;
    }

    case 0x02u: /* READ_LOG (IN 1..64 B) */ {
        if (is_out) {
            return -1;
        }
        uint32_t offset = musb_get_le32(&cdb[2]);
        uint32_t req    = musb_get_le32(&cdb[6]);
        if (req < 1u) {
            req = 1u;
        }
        if (req > 64u) {
            req = 64u;
        }
        if (req > *io_len) {
            req = *io_len;
        }
        for (uint32_t i = 0u; i < req; i++) {
            io_buf[i] = (uint8_t)g_openfw_log.buf[(offset + i) & (OPENFW_LOG_SIZE - 1u)];
        }
        *io_len = (uint16_t)req;
        return 0;
    }

    case 0x03u: /* GET_RTC (IN 8 B) */ {
        if (is_out) {
            return -1;
        }
        rtc_datetime_t dt;
        rtc_get(&dt);
        uint8_t raw[8];
        musb_put_le16(&raw[0], dt.year);
        raw[2] = dt.month;
        raw[3] = dt.day;
        raw[4] = dt.hour;
        raw[5] = dt.minute;
        raw[6] = dt.second;
        raw[7] = dt.weekday;

        uint16_t n = (*io_len < 8u) ? *io_len : 8u;
        for (uint16_t i = 0u; i < n; i++) {
            io_buf[i] = raw[i];
        }
        *io_len = n;
        return 0;
    }

    case 0x04u: /* SET_RTC (OUT 8 B) */ {
        if (!is_out || *io_len != 8u) {
            return -1;
        }
        uint16_t year    = (uint16_t)io_buf[0] | ((uint16_t)io_buf[1] << 8);
        uint8_t  month   = io_buf[2];
        uint8_t  day     = io_buf[3];
        uint8_t  hour    = io_buf[4];
        uint8_t  minute  = io_buf[5];
        uint8_t  second  = io_buf[6];
        uint8_t  weekday = io_buf[7];

        static const uint8_t k_dim[12] = { 31u, 28u, 31u, 30u, 31u, 30u, 31u, 31u, 30u, 31u, 30u, 31u };
        if (year < 2000u || year > 2099u ||
            month < 1u || month > 12u ||
            weekday > 6u ||
            hour > 23u || minute > 59u || second > 59u) {
            return -1;
        }
        uint8_t max_day = k_dim[month - 1u];
        if (month == 2u && ((year % 4u) == 0u && ((year % 100u) != 0u || (year % 400u) == 0u))) {
            max_day = 29u;
        }
        if (day < 1u || day > max_day) {
            return -1;
        }

        rtc_datetime_t dt;
        dt.year    = year;
        dt.month   = month;
        dt.day     = day;
        dt.weekday = weekday;
        dt.hour    = hour;
        dt.minute  = minute;
        dt.second  = second;
        rtc_set(&dt);
        return 0;
    }

#if OPENFW_ENABLE_USB_EXP
    case 0x05u: /* EXP_WRITE (OUT 1..64 B) */ {
        if (!is_out) {
            return -1;
        }
        uint32_t offset = musb_get_le32(&cdb[2]);
        uint32_t len    = musb_get_le32(&cdb[6]);
        if (len == 0u || len != *io_len) {
            return -1;
        }
        return openfw_exp_write(offset, io_buf, len);
    }

    case 0x06u: /* EXP_RUN (IN 32 B) */ {
        if (is_out) {
            return -1;
        }
        uint32_t entry_off = musb_get_le32(&cdb[2]);
        uint32_t arg       = musb_get_le32(&cdb[6]);
        openfw_exp_result_t res;
        if (openfw_exp_run(entry_off, arg, &res) != 0) {
            return -1;
        }
        uint8_t raw[32];
        pack_exp_result(raw, &res);
        uint16_t n = (*io_len < 32u) ? *io_len : 32u;
        for (uint16_t i = 0u; i < n; i++) {
            io_buf[i] = raw[i];
        }
        *io_len = n;
        return 0;
    }

    case 0x07u: /* MEM_READ (IN 1..64 B) */ {
        if (is_out) {
            return -1;
        }
        uint32_t addr = musb_get_le32(&cdb[2]);
        uint32_t req  = musb_get_le32(&cdb[6]);
        if (req < 1u) {
            req = 1u;
        }
        if (req > 64u) {
            req = 64u;
        }
        if (req > *io_len) {
            req = *io_len;
        }
        if (openfw_exp_mem_read(addr, io_buf, req) != 0) {
            return -1;
        }
        *io_len = (uint16_t)req;
        return 0;
    }
#endif /* OPENFW_ENABLE_USB_EXP */

    case 0x08u: /* REBOOT (0 B, deferred after CSW) */ {
        if (is_out) {
            return -1;
        }
        uint32_t mode = musb_get_le32(&cdb[2]);
        if (mode > 3u) {
            return -1;
        }
#if OPENFW_UNATTENDED_DEV
        if (mode == 3u) { /* Disallow soft power-off while in unattended dev mode */
            return -1;
        }
#endif
        g_usb_reboot_mode     = (uint8_t)(mode + 1u);
        g_usb_reboot_deadline = millis() + 250u;
        *io_len               = 0u;
        return 0;
    }

#if OPENFW_ENABLE_USB_EXP
    case 0x09u: /* EXP_STATUS (IN 32 B) */ {
        if (is_out) {
            return -1;
        }
        openfw_exp_result_t res;
        openfw_exp_get_last_result(&res);
        uint8_t raw[32];
        pack_exp_result(raw, &res);
        uint16_t n = (*io_len < 32u) ? *io_len : 32u;
        for (uint16_t i = 0u; i < n; i++) {
            io_buf[i] = raw[i];
        }
        *io_len = n;
        return 0;
    }
#endif /* OPENFW_ENABLE_USB_EXP */

    case 0x0Au: /* LOG_RESET (0 B) */ {
        if (is_out) {
            return -1;
        }
        openfw_log_reset();
        *io_len = 0u;
        return 0;
    }

    case 0x0Bu: /* AUDIO_CFG (IN 8 B / OUT 4 B) */ {
        uint8_t  cfg[4]   = { 0u, g_diag_mic_gain, 1u, 0u };
        int16_t  rms_x10  = -960;
        uint16_t clips    = 0u;
        if (is_out) {
            if (*io_len != 4u) {
                return -1;
            }
            if ((io_buf[0] != 0xFFu && io_buf[0] > 2u) ||
                (io_buf[1] != 0xFFu && io_buf[1] > 31u) ||
                (io_buf[2] != 0xFFu && io_buf[2] != 1u && io_buf[2] != 2u) ||
                (io_buf[3] != 0xFFu && io_buf[3] > 1u)) {
                return -1;
            }
            for (int i = 0; i < 4; i++) {
                cfg[i] = io_buf[i];
            }
            if (io_buf[1] != 0xFFu) {
                g_diag_mic_gain = io_buf[1];
            }
            if (g_audio_cfg_cb && g_audio_cfg_cb(1, cfg, &rms_x10, &clips) != 0) {
                return -1;
            }
            return 0;
        }
        if (g_audio_cfg_cb) {
            (void)g_audio_cfg_cb(0, cfg, &rms_x10, &clips);
        }
        uint8_t raw[8];
        raw[0] = cfg[0];
        raw[1] = cfg[1];
        raw[2] = cfg[2];
        raw[3] = cfg[3];
        musb_put_le16(&raw[4], (uint16_t)rms_x10);
        musb_put_le16(&raw[6], clips);
        uint16_t n = (*io_len < 8u) ? *io_len : 8u;
        for (uint16_t i = 0u; i < n; i++) {
            io_buf[i] = raw[i];
        }
        *io_len = n;
        return 0;
    }

    default:
        return -1;
    }
}

void usb_msc_set_audio_cfg_cb(usb_audio_cfg_cb_t cb)
{
    g_audio_cfg_cb = cb;
}

void usb_msc_set_diag_info(uint8_t sd_mounted, uint8_t rec_active, uint8_t mic_gain)
{
    g_diag_sd_mounted = sd_mounted;
    g_diag_rec_active = rec_active;
    g_diag_mic_gain   = mic_gain;
}

int usb_msc_start(blkdev_t *dev)
{
    if (!dev || g_usb_active) {
        return g_usb_active ? 0 : -1;
    }

    usb_msc_init_state(&g_msc, dev);
    usb_msc_set_vendor_cb(&g_msc, musb_vendor_scsi_cb, 0);
    g_rx_pkts       = 0u;
    g_tx_pkts       = 0u;
    g_rx_bytes      = 0u;
    g_tx_bytes      = 0u;
    g_ep0_state     = EP0_IDLE;
    g_ep0_tx_rem    = 0u;
    g_ep0_zlp       = 0u;
    g_set_addr_pend = 0u;
    g_pending_addr  = 0u;
    g_usb_cfg       = 0u;
    g_usb_reboot_mode     = 0u;
    g_usb_reboot_deadline = 0u;

    usb_phy_power_on();

    /* 5. PHY wrapper: disarm plug-detect IRQ, Device mode (bit 12) + preserve/force analog bias bit 4 (0x1010) */
    openfw_mmio_clr32(USB_PHY_INTE_ADDR, 0x03u);
    openfw_mmio_wr32(USB_PHY_INT_STATUS_ADDR, 0x07u);
    openfw_mmio_rmw32(USB_PHY_CTRL_ADDR, 0x1F3u, 0x1010u);
    mdelay(50u);

    /* 6. Disconnect, flush EP0..3, clear pending flags, Force Full-Speed */
    openfw_mmio_wr8(MUSB_POWER_ADDR, (uint8_t)(openfw_mmio_rd8(MUSB_POWER_ADDR) & ~0x40u));
    for (uint8_t ep = 0u; ep < 4u; ep++) {
        musb_flush_ep(ep);
    }
    mdelay(2u);
    (void)openfw_mmio_rd8(MUSB_INTRUSB_ADDR);
    (void)openfw_mmio_rd16(MUSB_INTRRX_ADDR);
    (void)openfw_mmio_rd16(MUSB_INTRTX_ADDR);
    openfw_mmio_wr8(MUSB_TESTMODE_ADDR, (uint8_t)(openfw_mmio_rd8(MUSB_TESTMODE_ADDR) | 0x20u)); /* Force_FS */

    /* 7. Polled MUSB interrupt masks */
    openfw_mmio_wr8(MUSB_INTRUSBE_ADDR, 0x07u);   /* Suspend | Resume | Reset */
    openfw_mmio_wr16(MUSB_INTRTXE_ADDR, 0x0003u); /* EP0 + EP1 TX */
    openfw_mmio_wr16(MUSB_INTRRXE_ADDR, 0x0002u); /* EP1 RX */
    musb_configure_ep1_msc();

    /* 8. Assert D+ pull-up (SoftConn = 1) */
    openfw_mmio_wr8(MUSB_FADDR_ADDR, 0u);
    openfw_mmio_wr8(MUSB_POWER_ADDR, (uint8_t)(openfw_mmio_rd8(MUSB_POWER_ADDR) | 0x40u));
    mdelay(3u);

    g_usb_active = 1;
    return 0;
}

static int musb_sd_async_start(void *ctx, uint32_t lba, uint32_t count, void *buf)
{
    return sd_read_async_start((sd_card_t *)ctx, lba, count, buf);
}

static int musb_sd_async_wait(void *ctx)
{
    return sd_read_async_wait((sd_card_t *)ctx);
}

static void musb_sd_async_abort(void *ctx)
{
    sd_read_async_abort((sd_card_t *)ctx);
}

void usb_msc_enable_sd_async(sd_card_t *card)
{
    if (!card || !card->initialized) {
        usb_msc_set_async_read(&g_msc, 0);
        return;
    }
    usb_msc_async_read_t ops;
    ops.read_start = musb_sd_async_start;
    ops.read_wait  = musb_sd_async_wait;
    ops.read_abort = musb_sd_async_abort;
    ops.ctx        = card;
    usb_msc_set_async_read(&g_msc, &ops);
}

void usb_msc_stop(void)
{
    if (!g_usb_active) {
        return;
    }
    g_usb_active = 0;
    usb_msc_reset(&g_msc);

    openfw_mmio_wr8(MUSB_INTRUSBE_ADDR, 0u);
    openfw_mmio_wr16(MUSB_INTRTXE_ADDR, 0u);
    openfw_mmio_wr16(MUSB_INTRRXE_ADDR, 0u);
    openfw_mmio_wr8(MUSB_POWER_ADDR, (uint8_t)(openfw_mmio_rd8(MUSB_POWER_ADDR) & ~0x40u)); /* Drop D+ pull-up */
    for (uint8_t ep = 0u; ep < 4u; ep++) {
        musb_flush_ep(ep);
    }
    (void)openfw_mmio_rd8(MUSB_INTRUSB_ADDR);
    (void)openfw_mmio_rd16(MUSB_INTRRX_ADDR);
    (void)openfw_mmio_rd16(MUSB_INTRTX_ADDR);

    openfw_mmio_clr32(SYSCTL_USB_CTRL_ADDR, 1u << 0);
    openfw_mmio_clr32(CRU_CLK_EN_USB_ADDR,  1u);
}

void usb_msc_poll(void)
{
    if (!g_usb_active) {
        return;
    }

    uint8_t  intr_usb = openfw_mmio_rd8(MUSB_INTRUSB_ADDR);
    uint16_t intr_tx  = openfw_mmio_rd16(MUSB_INTRTX_ADDR);
    uint16_t intr_rx  = openfw_mmio_rd16(MUSB_INTRRX_ADDR);

    if (intr_usb & 0x04u) { /* USB Bus Reset */
        openfw_mmio_wr8(MUSB_FADDR_ADDR, 0u);
        g_set_addr_pend = 0u;
        g_ep0_state     = EP0_IDLE;
        g_ep0_tx_rem    = 0u;
        g_ep0_zlp       = 0u;
        g_usb_cfg       = 0u;
        usb_msc_reset(&g_msc);
        musb_configure_ep1_msc();
    }

    if ((intr_tx & 0x0001u) || (openfw_mmio_wr8(MUSB_INDEX_ADDR, 0u), (openfw_mmio_rd16(MUSB_CSR0_ADDR) & 0x0015u) != 0u)) {
        ep0_service();
    }

    /* EP1 Bulk OUT RX (capped to 16 packets per poll so WRITE(10) cannot starve EP0/UI/WDT) */
    openfw_mmio_wr8(MUSB_INDEX_ADDR, 1u);
    if ((intr_rx & 0x0002u) || (openfw_mmio_rd16(MUSB_RXCSR_ADDR) & 0x0001u)) {
        uint32_t rx_budget = 16u;
        while (rx_budget-- > 0u && (openfw_mmio_rd16(MUSB_RXCSR_ADDR) & 0x0001u)) {
            uint16_t cnt = openfw_mmio_rd16(MUSB_RXCOUNT_ADDR) & 0x03FFu;
            uint8_t  pkt[64] __attribute__((aligned(4)));
            if (cnt > 64u) {
                cnt = 64u;
            }
            if (cnt > 0u) {
                musb_fifo_read(1u, pkt, cnt);
            }
            openfw_mmio_wr16(MUSB_RXCSR_ADDR, (uint16_t)(openfw_mmio_rd16(MUSB_RXCSR_ADDR) & ~0x0001u)); /* Clear RxPktRdy */
            usb_msc_on_rx_packet(&g_msc, pkt, (uint8_t)cnt);
            g_rx_pkts++;
            g_rx_bytes += (uint64_t)cnt;
        }
    }

    /* EP1 Bulk IN TX (drain up to 16 packets while FIFO accepts, matching RX burst) */
    openfw_mmio_wr8(MUSB_INDEX_ADDR, 1u);
    {
        uint32_t tx_budget = 16u;
        while (tx_budget-- > 0u && usb_msc_need_tx(&g_msc) &&
               !(openfw_mmio_rd16(MUSB_TXCSR_ADDR) & 0x0001u)) {
            uint8_t pkt[64] __attribute__((aligned(4)));
            uint8_t n = usb_msc_get_tx_packet(&g_msc, pkt);
            if (n > 0u) {
                musb_fifo_write(1u, pkt, n);
            }
            openfw_mmio_wr16(MUSB_TXCSR_ADDR, (uint16_t)(openfw_mmio_rd16(MUSB_TXCSR_ADDR) | 0x0001u)); /* Set TxPktRdy */
            g_tx_pkts++;
            g_tx_bytes += (uint64_t)n;
        }
    }

    if (g_usb_reboot_mode != 0u && !usb_msc_need_tx(&g_msc) &&
        !(openfw_mmio_rd16(MUSB_TXCSR_ADDR) & 0x0001u) &&
        (int32_t)(millis() - g_usb_reboot_deadline) >= 0) {
        uint8_t mode = g_usb_reboot_mode;
        g_usb_reboot_mode = 0u;
        usb_msc_stop();
        if (mode == 2u) {
            pmu_reboot_to_usb_bootloader();
        } else if (mode == 3u) {
            pmu_reboot_to_sd_ota();
        } else if (mode == 4u) {
            lcd_shutdown();
            pmu_soft_poweroff();
        } else {
            system_reboot();
        }
    }
}

int usb_msc_is_active(void)
{
    return g_usb_active;
}

void usb_msc_get_stats(uint32_t *rx_pkts, uint32_t *tx_pkts)
{
    if (rx_pkts) {
        *rx_pkts = g_rx_pkts;
    }
    if (tx_pkts) {
        *tx_pkts = g_tx_pkts;
    }
}

void usb_msc_get_bytes(uint64_t *rx_bytes, uint64_t *tx_bytes)
{
    if (rx_bytes) {
        *rx_bytes = g_rx_bytes;
    }
    if (tx_bytes) {
        *tx_bytes = g_tx_bytes;
    }
}
