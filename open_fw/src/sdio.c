/*
 * Smartlink SL6801 Bare-Metal 1-Bit SDIO / microSD Driver (0x40003000)
 *
 * Uses raw polling PIO on Port D (PD5=CLK, PD4=CMD, PD6=D0) with single-block
 * CMD17/CMD24 transfers so BootROM SRAM (0x00804A04) is untouched and the
 * factory firmware's CMD18/CMD12 multi-block stall is completely avoided.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include "sl6801_regs.h"
#include "sl6801_rom.h"
#include "openfw.h"

#define SDIO_CMD_START      0x80000000u
#define SDIO_CMD_UPD_CLK    0x80200000u

#define SD_CMD0_GO_IDLE     0xA400u
#define SD_CMD2_ALL_CID     0x25C2u
#define SD_CMD3_REL_ADDR    0x2543u
#define SD_CMD7_SELECT      0x2547u
#define SD_CMD8_IF_COND     0x2548u
#define SD_CMD9_SEND_CSD    0x25C9u
#define SD_CMD12_STOP       0x454Cu
#define SD_CMD13_STATUS     0x254Du
#define SD_CMD16_SETLEN     0x2550u
#define SD_CMD17_READ1       0x2351u
#define SD_CMD18_READ_MULTI  0x2352u
#define SD_CMD24_WRITE1      0x2758u
#define SD_CMD25_WRITE_MULTI 0x2759u
#define SD_ACMD41_OPCOND     0x2569u
#define SD_CMD55_APPCMD      0x2577u

#define SDIO_CMD_TIMEOUT_US  100000u
#define SDIO_DAT_TIMEOUT_US  500000u
#define SDIO_MAX_MULTI_BLKS  64u

static int sdio_read_one_sector(sd_card_t *card, uint32_t lba, uint8_t *dst);

static void sdio_reset_fifo(void)
{
    openfw_mmio_set32(SDIO_CTRL_ADDR, 0x2u);
    uint64_t t0 = us_timer_now();
    while (openfw_mmio_rd32(SDIO_CTRL_ADDR) & 0x2u) {
        if ((uint32_t)(us_timer_now() - t0) >= SDIO_CMD_TIMEOUT_US) {
            break;
        }
    }
}

static int sdio_wait_cmd_accept(void)
{
    uint64_t t0 = us_timer_now();
    while (openfw_mmio_rd32(SDIO_CMD_ADDR) & SDIO_CMD_START) {
        if ((uint32_t)(us_timer_now() - t0) >= SDIO_CMD_TIMEOUT_US) {
            return -1;
        }
    }
    return 0;
}

static int sdio_update_clk(void)
{
    if (sdio_wait_cmd_accept() != 0) {
        return -1;
    }
    openfw_mmio_wr32(SDIO_CMD_ADDR, SDIO_CMD_UPD_CLK);
    return sdio_wait_cmd_accept();
}

static void sdio_set_clk_div(uint32_t div)
{
    rom_clk_disable(CRU_CLK_SDIO);
    rom_clk_div(CRU_CLK_SDIO, div);
    rom_us_delay(100u);
    rom_clk_enable(CRU_CLK_SDIO);
}

static int sdio_send_cmd(uint32_t cmd_word, uint32_t arg, int ignore_rcrc)
{
    if (sdio_wait_cmd_accept() != 0) {
        return -1;
    }

    openfw_mmio_wr32(SDIO_RINTSTS_ADDR, 0xFFFFFFFFu);
    openfw_mmio_wr32(SDIO_ARG_ADDR,     arg);
    openfw_mmio_wr32(SDIO_CMD_ADDR,     SDIO_CMD_START | (cmd_word & 0xFFFFu));

    uint64_t t0 = us_timer_now();
    while (!(openfw_mmio_rd32(SDIO_RINTSTS_ADDR) & SDIO_INT_CMD_DONE)) {
        if ((uint32_t)(us_timer_now() - t0) >= SDIO_CMD_TIMEOUT_US) {
            openfw_mmio_wr32(SDIO_RINTSTS_ADDR, 0xFFFFFFFFu);
            return -1;
        }
    }

    uint32_t st = openfw_mmio_rd32(SDIO_RINTSTS_ADDR);
    openfw_mmio_wr32(SDIO_RINTSTS_ADDR, 0x00000344u);

    if (st & (SDIO_INT_RTO | SDIO_INT_DRTO)) {
        return -1;
    }
    if (!ignore_rcrc && (st & SDIO_INT_RCRC)) {
        return -1;
    }
    return 0;
}

static int sdio_send_acmd41(uint32_t arg)
{
    if (sdio_send_cmd(SD_CMD55_APPCMD, 0u, 0) != 0) {
        return -1;
    }
    return sdio_send_cmd(SD_ACMD41_OPCOND, arg, 1);
}

static uint32_t parse_csd_sectors(void)
{
    /* Standard DesignWare dw_mmc R2 packing: RESP3=[127:96], RESP2=[95:64], RESP1=[63:32] */
    uint32_t csd3 = openfw_mmio_rd32(SDIO_RESP3_ADDR);
    uint32_t csd2 = openfw_mmio_rd32(SDIO_RESP2_ADDR);
    uint32_t csd1 = openfw_mmio_rd32(SDIO_RESP1_ADDR);
    uint32_t csd_ver = (csd3 >> 30) & 0x3u;
    if (csd_ver == 1u) {
        uint32_t c_size = ((csd2 & 0x3Fu) << 16) | ((csd1 >> 16) & 0xFFFFu);
        return (c_size + 1u) * 1024u;
    }
    return 0u;
}

int sd_init(sd_card_t *card)
{
    if (!card) {
        return -1;
    }
    card->initialized  = 0;
    card->is_sdhc      = 0;
    card->rca          = 0u;
    card->sector_count = 0u;
    card->clk_div      = 0u;
    card->async_active = 0u;
    card->async_last_desc = 0u;
    card->async_t0_us  = 0u;

    /* 1-bit SDIO pins on Port D: PD5=CLK, PD4=CMD (PU), PD6=D0 (PU) */
    rom_gpio_config(0x00032D90u);
    rom_gpio_config(0x0003259Bu);
    rom_gpio_config(0x0003359Bu);

    rom_clk_set_src(CRU_CLK_SDIO, CRU_CLK_SRC_HOSC24M);
    rom_rst_hold(CRU_RST_SDIO);
    mdelay(5u);
    rom_rst_release(CRU_RST_SDIO);

    /* Initial 375 kHz identification clock (24 MHz / 64) */
    sdio_set_clk_div(64u);

    /* Soft reset controller + FIFO + DMA, then lock 1-bit polling mode */
    openfw_mmio_set32(SDIO_CTRL_ADDR, 0x7u);
    {
        uint64_t t0 = us_timer_now();
        while (openfw_mmio_rd32(SDIO_CTRL_ADDR) & 0x7u) {
            if ((uint32_t)(us_timer_now() - t0) >= SDIO_CMD_TIMEOUT_US) {
                return -1;
            }
        }
    }
    openfw_mmio_rmw32(SDIO_CTRL_ADDR, 0xC0000008u, 0x40000000u);
    if (sdio_update_clk() != 0) {
        return -1;
    }

    openfw_mmio_wr32(SDIO_RINTSTS_ADDR, 0xFFFFFFFFu);
    openfw_mmio_wr32(SDIO_FIFOTH_ADDR,  0x20070008u);
    openfw_mmio_wr32(SDIO_TMOUT_ADDR,   0x00FFFFFFu);
    openfw_mmio_set32(SDIO_BMOD_ADDR,   0x2u);
    openfw_mmio_wr32(SDIO_INTMASK_ADDR, 0xFFFFFF40u);

    /* Card identification: CMD0 -> CMD8 -> ACMD41 -> CMD2 -> CMD3 */
    if (sdio_send_cmd(SD_CMD0_GO_IDLE, 0u, 1) != 0) {
        return -1;
    }
    mdelay(2u);

    uint32_t acmd41_arg = 0x00100000u;
    if (sdio_send_cmd(SD_CMD8_IF_COND, 0x000001AAu, 0) == 0 &&
        (openfw_mmio_rd32(SDIO_RESP0_ADDR) & 0xFFu) == 0xAAu) {
        acmd41_arg = 0x40100000u; /* SD v2.0 HCS=1 */
    }

    int ready = 0;
    uint32_t ocr = 0u;
    for (int tries = 0; tries < 250; tries++) {
        wdt_feed();
        if (sdio_send_acmd41(acmd41_arg) == 0) {
            ocr = openfw_mmio_rd32(SDIO_RESP0_ADDR);
            if (ocr & (1u << 31)) {
                ready = 1;
                break;
            }
        }
        mdelay(4u);
    }
    if (!ready) {
        return -1;
    }

    int is_sdhc = (ocr & (1u << 30)) ? 1 : 0;

    if (sdio_send_cmd(SD_CMD2_ALL_CID, 0u, 1) != 0) {
        return -1;
    }
    if (sdio_send_cmd(SD_CMD3_REL_ADDR, 0x00010000u, 0) != 0) {
        return -1;
    }
    uint16_t rca = (uint16_t)(openfw_mmio_rd32(SDIO_RESP0_ADDR) >> 16);
    if (rca == 0u) {
        return -1;
    }

    uint32_t sectors = 0u;
    if (sdio_send_cmd(SD_CMD9_SEND_CSD, (uint32_t)rca << 16, 1) == 0) {
        sectors = parse_csd_sectors();
    }

    if (sdio_send_cmd(SD_CMD7_SELECT, (uint32_t)rca << 16, 0) != 0) {
        return -1;
    }
    if (!is_sdhc) {
        if (sdio_send_cmd(SD_CMD16_SETLEN, 512u, 0) != 0) {
            return -1;
        }
    }

    card->initialized  = 1;
    card->is_sdhc      = is_sdhc;
    card->rca          = rca;
    card->sector_count = sectors;

    /* Auto-negotiate highest reliable 1-bit SDIO clock: 24 MHz -> 12 MHz -> 6 MHz */
    static const uint32_t k_divs[3] = { 1u, 2u, 4u };
    uint8_t probe_sec[512];
    int clk_ok = 0;
    for (uint32_t di = 0u; di < 3u; di++) {
        sdio_set_clk_div(k_divs[di]);
        if (sdio_update_clk() != 0) {
            continue;
        }
        if (sdio_read_one_sector(card, 0u, probe_sec) == 0) {
            card->clk_div = (uint8_t)k_divs[di];
            clk_ok = 1;
            break;
        }
        sdio_reset_fifo();
    }
    if (!clk_ok) {
        card->initialized = 0;
        return -1;
    }
    return 0;
}

static int sdio_wait_card_ready(sd_card_t *card)
{
    /* Wait for card programming completion (R1 bit 8 = READY_FOR_DATA) */
    uint64_t p0 = us_timer_now();
    uint32_t fast_polls = 0u;
    while (1) {
        if (sdio_send_cmd(SD_CMD13_STATUS, (uint32_t)card->rca << 16, 0) == 0 &&
            (openfw_mmio_rd32(SDIO_RESP0_ADDR) & (1u << 8))) {
            return 0;
        }
        if ((uint32_t)(us_timer_now() - p0) >= SDIO_DAT_TIMEOUT_US) {
            return -1;
        }
        if (fast_polls < 16u) {
            fast_polls++;
        } else {
            rom_us_delay(50u);
        }
    }
}

static int sdio_fifo_read_words(uint8_t *dst, uint32_t words, uint64_t t0)
{
    int aligned = (((uintptr_t)dst & 3u) == 0u);
    uint32_t *dst32 = (uint32_t *)(void *)dst;
    for (uint32_t w = 0u; w < words; w++) {
        while (openfw_mmio_rd32(SDIO_STATUS_ADDR) & SDIO_STAT_FIFO_EMP) {
            if (openfw_mmio_rd32(SDIO_RINTSTS_ADDR) & SDIO_INT_DATA_ERRS) {
                openfw_mmio_wr32(SDIO_RINTSTS_ADDR, 0xFFFFFFFFu);
                return -1;
            }
            if ((uint32_t)(us_timer_now() - t0) >= SDIO_DAT_TIMEOUT_US) {
                openfw_mmio_wr32(SDIO_RINTSTS_ADDR, 0xFFFFFFFFu);
                return -1;
            }
        }
        uint32_t word = openfw_mmio_rd32(SDIO_FIFO_ADDR);
        if (aligned) {
            dst32[w] = word;
        } else {
            dst[w * 4u + 0u] = (uint8_t)(word);
            dst[w * 4u + 1u] = (uint8_t)(word >> 8);
            dst[w * 4u + 2u] = (uint8_t)(word >> 16);
            dst[w * 4u + 3u] = (uint8_t)(word >> 24);
        }
    }
    return 0;
}

static int sdio_fifo_write_words(const uint8_t *src, uint32_t words, uint64_t t0)
{
    int aligned = (((uintptr_t)src & 3u) == 0u);
    const uint32_t *src32 = (const uint32_t *)(const void *)src;
    for (uint32_t w = 0u; w < words; w++) {
        while (openfw_mmio_rd32(SDIO_STATUS_ADDR) & SDIO_STAT_FIFO_FULL) {
            if (openfw_mmio_rd32(SDIO_RINTSTS_ADDR) & SDIO_INT_DATA_ERRS) {
                openfw_mmio_wr32(SDIO_RINTSTS_ADDR, 0xFFFFFFFFu);
                return -1;
            }
            if ((uint32_t)(us_timer_now() - t0) >= SDIO_DAT_TIMEOUT_US) {
                openfw_mmio_wr32(SDIO_RINTSTS_ADDR, 0xFFFFFFFFu);
                return -1;
            }
        }
        uint32_t word = aligned ? src32[w]
            : ((uint32_t)src[w * 4u + 0u]
             | ((uint32_t)src[w * 4u + 1u] << 8)
             | ((uint32_t)src[w * 4u + 2u] << 16)
             | ((uint32_t)src[w * 4u + 3u] << 24));
        openfw_mmio_wr32(SDIO_FIFO_ADDR, word);
    }
    return 0;
}

static int sdio_read_one_sector(sd_card_t *card, uint32_t lba, uint8_t *dst)
{
    uint32_t arg = card->is_sdhc ? lba : (lba << 9);

    sdio_reset_fifo();
    openfw_mmio_wr32(SDIO_BLKSIZ_ADDR,  512u);
    openfw_mmio_wr32(SDIO_BYTCNT_ADDR,  512u);
    openfw_mmio_wr32(SDIO_RINTSTS_ADDR, 0xFFFFFFFFu);

    if (sdio_send_cmd(SD_CMD17_READ1, arg, 0) != 0) {
        return -1;
    }

    uint64_t t0 = us_timer_now();
    if (sdio_fifo_read_words(dst, 128u, t0) != 0) {
        sdio_reset_fifo();
        return -1;
    }

    while (!(openfw_mmio_rd32(SDIO_RINTSTS_ADDR) & SDIO_INT_DTO)) {
        if ((uint32_t)(us_timer_now() - t0) >= SDIO_DAT_TIMEOUT_US) {
            openfw_mmio_wr32(SDIO_RINTSTS_ADDR, 0xFFFFFFFFu);
            sdio_reset_fifo();
            return -1;
        }
    }
    uint32_t st = openfw_mmio_rd32(SDIO_RINTSTS_ADDR);
    openfw_mmio_wr32(SDIO_RINTSTS_ADDR, 0xFFFFFFFFu);
    if (st & SDIO_INT_DATA_ERRS) {
        sdio_reset_fifo();
        return -1;
    }
    return 0;
}

typedef struct {
    volatile uint32_t des0;
    uint32_t          des1;
    uint32_t          des2;
    uint32_t          des3;
} sdio_idmac_desc_t;

#define IDMAC_DES0_OWN (1u << 31)
#define IDMAC_DES0_CH  (1u << 4)
#define IDMAC_DES0_FS  (1u << 3)
#define IDMAC_DES0_LD  (1u << 2)
#define IDMAC_DES0_DIC (1u << 1)

#ifndef OPENFW_HOST_MMIO
static sdio_idmac_desc_t g_idmac_desc[2] __attribute__((aligned(16)));
#endif

static void sdio_reset_dma_and_fifo(void)
{
    openfw_mmio_set32(SDIO_CTRL_ADDR, 0x6u); /* FIFO_RESET | DMA_RESET */
    uint64_t t0 = us_timer_now();
    while (openfw_mmio_rd32(SDIO_CTRL_ADDR) & 0x6u) {
        if ((uint32_t)(us_timer_now() - t0) >= SDIO_CMD_TIMEOUT_US) {
            break;
        }
    }
}

void sd_read_async_abort(sd_card_t *card)
{
    if (!card || !card->async_active) {
        return;
    }
    card->async_active = 0u;
    openfw_mmio_wr32(SDIO_IDMAC_BMOD_ADDR, 0u);
    openfw_mmio_clr32(SDIO_CTRL_ADDR, SDIO_CTRL_USE_IDMAC);
    (void)sdio_send_cmd(SD_CMD12_STOP, 0u, 1);
    sdio_reset_dma_and_fifo();
    openfw_mmio_wr32(SDIO_RINTSTS_ADDR, 0xFFFFFFFFu);
    openfw_mmio_wr32(SDIO_IDSTS_ADDR,   0xFFFFFFFFu);
}

int sd_read_async_start(sd_card_t *card, uint32_t lba, uint32_t count, void *buf)
{
#ifdef OPENFW_HOST_MMIO
    (void)card; (void)lba; (void)count; (void)buf;
    return -2; /* 64-bit host pointer width cannot feed 32-bit IDMAC */
#else
    if (!card || !card->initialized || !buf) {
        return -1;
    }
    if (card->async_active) {
        sd_read_async_abort(card);
    }
    if (count < 2u || count > 16u || (((uintptr_t)buf & 3u) != 0u)) {
        return -2; /* Caller falls back to synchronous PIO */
    }
    if (card->sector_count > 3u && lba + count > card->sector_count - 3u) {
        return -2; /* Near end-of-disk: use single-block CMD17 */
    }

    uint32_t total_bytes = count * 512u;
    uint32_t buf_addr    = (uint32_t)(uintptr_t)buf;
    if (total_bytes <= 4096u) {
        g_idmac_desc[0].des0 = IDMAC_DES0_OWN | IDMAC_DES0_CH | IDMAC_DES0_FS | IDMAC_DES0_LD;
        g_idmac_desc[0].des1 = total_bytes;
        g_idmac_desc[0].des2 = buf_addr;
        g_idmac_desc[0].des3 = (uint32_t)(uintptr_t)&g_idmac_desc[0];
        card->async_last_desc = 0u;
    } else {
        g_idmac_desc[0].des0 = IDMAC_DES0_OWN | IDMAC_DES0_CH | IDMAC_DES0_FS | IDMAC_DES0_DIC;
        g_idmac_desc[0].des1 = 4096u;
        g_idmac_desc[0].des2 = buf_addr;
        g_idmac_desc[0].des3 = (uint32_t)(uintptr_t)&g_idmac_desc[1];
        g_idmac_desc[1].des0 = IDMAC_DES0_OWN | IDMAC_DES0_CH | IDMAC_DES0_LD;
        g_idmac_desc[1].des1 = total_bytes - 4096u;
        g_idmac_desc[1].des2 = buf_addr + 4096u;
        g_idmac_desc[1].des3 = (uint32_t)(uintptr_t)&g_idmac_desc[1];
        card->async_last_desc = 1u;
    }
    __asm__ volatile ("dsb" ::: "memory");

    sdio_reset_dma_and_fifo();
    openfw_mmio_set32(SDIO_CTRL_ADDR, SDIO_CTRL_USE_IDMAC);
    openfw_mmio_wr32(SDIO_IDMAC_BMOD_ADDR, SDIO_IDMAC_BMOD_SWR);
    {
        uint64_t t0 = us_timer_now();
        while (openfw_mmio_rd32(SDIO_IDMAC_BMOD_ADDR) & SDIO_IDMAC_BMOD_SWR) {
            if ((uint32_t)(us_timer_now() - t0) >= SDIO_CMD_TIMEOUT_US) {
                openfw_mmio_clr32(SDIO_CTRL_ADDR, SDIO_CTRL_USE_IDMAC);
                return -1;
            }
        }
    }
    openfw_mmio_wr32(SDIO_IDSTS_ADDR,      0xFFFFFFFFu);
    openfw_mmio_wr32(SDIO_IDINTEN_ADDR,    SDIO_IDSTS_RI | SDIO_IDSTS_TI);
    openfw_mmio_wr32(SDIO_DBADDR_ADDR,     (uint32_t)(uintptr_t)&g_idmac_desc[0]);
    openfw_mmio_wr32(SDIO_IDMAC_BMOD_ADDR, SDIO_IDMAC_BMOD_DE | SDIO_IDMAC_BMOD_FB);

    openfw_mmio_wr32(SDIO_BLKSIZ_ADDR,  512u);
    openfw_mmio_wr32(SDIO_BYTCNT_ADDR,  total_bytes);
    openfw_mmio_wr32(SDIO_RINTSTS_ADDR, 0xFFFFFFFFu);

    uint32_t arg = card->is_sdhc ? lba : (lba << 9);
    if (sdio_send_cmd(SD_CMD18_READ_MULTI, arg, 0) != 0) {
        openfw_mmio_wr32(SDIO_IDMAC_BMOD_ADDR, 0u);
        openfw_mmio_clr32(SDIO_CTRL_ADDR, SDIO_CTRL_USE_IDMAC);
        sdio_reset_dma_and_fifo();
        return -1;
    }
    openfw_mmio_wr32(SDIO_PLDMND_ADDR, 1u);

    card->async_active = 1u;
    card->async_t0_us  = (uint32_t)us_timer_now();
    return 0;
#endif
}

int sd_read_async_poll(sd_card_t *card)
{
#ifdef OPENFW_HOST_MMIO
    (void)card;
    return -1;
#else
    if (!card || !card->async_active) {
        return 1;
    }
    uint32_t st    = openfw_mmio_rd32(SDIO_RINTSTS_ADDR);
    uint32_t idsts = openfw_mmio_rd32(SDIO_IDSTS_ADDR);
    if ((st & SDIO_INT_DATA_ERRS) ||
        (idsts & (SDIO_IDSTS_FBE | SDIO_IDSTS_DU | SDIO_IDSTS_CES))) {
        sd_read_async_abort(card);
        return -1;
    }
    if ((st & SDIO_INT_DTO) &&
        ((g_idmac_desc[card->async_last_desc].des0 & IDMAC_DES0_OWN) == 0u)) {
        card->async_active = 0u;
        openfw_mmio_wr32(SDIO_RINTSTS_ADDR,    0xFFFFFFFFu);
        openfw_mmio_wr32(SDIO_IDSTS_ADDR,      0xFFFFFFFFu);
        openfw_mmio_wr32(SDIO_IDMAC_BMOD_ADDR, 0u);
        openfw_mmio_clr32(SDIO_CTRL_ADDR,      SDIO_CTRL_USE_IDMAC);
        int stop_rc = sdio_send_cmd(SD_CMD12_STOP, 0u, 1);
        sdio_reset_fifo();
        __asm__ volatile ("dsb" ::: "memory");
        return (stop_rc == 0) ? 1 : -1;
    }
    if ((uint32_t)((uint32_t)us_timer_now() - card->async_t0_us) >= SDIO_DAT_TIMEOUT_US) {
        sd_read_async_abort(card);
        return -1;
    }
    return 0;
#endif
}

int sd_read_async_wait(sd_card_t *card)
{
    if (!card || !card->async_active) {
        return 0;
    }
    while (1) {
        wdt_feed();
        int rc = sd_read_async_poll(card);
        if (rc > 0) {
            return 0;
        }
        if (rc < 0) {
            return -1;
        }
    }
}

int sd_flush(sd_card_t *card)
{
    if (card && card->async_active) {
        sd_read_async_abort(card);
    }
    return 0;
}

static int sdio_read_multi(sd_card_t *card, uint32_t lba, uint32_t count, uint8_t *dst)
{
    uint32_t arg = card->is_sdhc ? lba : (lba << 9);

    sdio_reset_fifo();
    openfw_mmio_wr32(SDIO_BLKSIZ_ADDR,  512u);
    openfw_mmio_wr32(SDIO_BYTCNT_ADDR,  count * 512u);
    openfw_mmio_wr32(SDIO_RINTSTS_ADDR, 0xFFFFFFFFu);

    if (sdio_send_cmd(SD_CMD18_READ_MULTI, arg, 0) != 0) {
        return -1;
    }

    uint64_t t0 = us_timer_now();
    if (sdio_fifo_read_words(dst, count * 128u, t0) != 0) {
        (void)sdio_send_cmd(SD_CMD12_STOP, 0u, 1);
        sdio_reset_fifo();
        return -1;
    }

    while (!(openfw_mmio_rd32(SDIO_RINTSTS_ADDR) & SDIO_INT_DTO)) {
        if ((uint32_t)(us_timer_now() - t0) >= SDIO_DAT_TIMEOUT_US) {
            openfw_mmio_wr32(SDIO_RINTSTS_ADDR, 0xFFFFFFFFu);
            (void)sdio_send_cmd(SD_CMD12_STOP, 0u, 1);
            sdio_reset_fifo();
            return -1;
        }
    }
    uint32_t st = openfw_mmio_rd32(SDIO_RINTSTS_ADDR);
    openfw_mmio_wr32(SDIO_RINTSTS_ADDR, 0xFFFFFFFFu);
    int stop_rc = sdio_send_cmd(SD_CMD12_STOP, 0u, 1);
    sdio_reset_fifo();
    if (stop_rc != 0 || (st & SDIO_INT_DATA_ERRS)) {
        return -1;
    }
    return 0;
}

static int sdio_write_one_sector(sd_card_t *card, uint32_t lba, const uint8_t *src)
{
    uint32_t arg = card->is_sdhc ? lba : (lba << 9);

    sdio_reset_fifo();
    openfw_mmio_wr32(SDIO_BLKSIZ_ADDR,  512u);
    openfw_mmio_wr32(SDIO_BYTCNT_ADDR,  512u);
    openfw_mmio_wr32(SDIO_RINTSTS_ADDR, 0xFFFFFFFFu);

    if (sdio_send_cmd(SD_CMD24_WRITE1, arg, 0) != 0) {
        return -1;
    }

    uint64_t t0 = us_timer_now();
    if (sdio_fifo_write_words(src, 128u, t0) != 0) {
        sdio_reset_fifo();
        return -1;
    }

    while (!(openfw_mmio_rd32(SDIO_RINTSTS_ADDR) & SDIO_INT_DTO)) {
        if ((uint32_t)(us_timer_now() - t0) >= SDIO_DAT_TIMEOUT_US) {
            openfw_mmio_wr32(SDIO_RINTSTS_ADDR, 0xFFFFFFFFu);
            sdio_reset_fifo();
            return -1;
        }
    }
    uint32_t st = openfw_mmio_rd32(SDIO_RINTSTS_ADDR);
    openfw_mmio_wr32(SDIO_RINTSTS_ADDR, 0xFFFFFFFFu);
    if (st & SDIO_INT_DATA_ERRS) {
        sdio_reset_fifo();
        return -1;
    }
    return sdio_wait_card_ready(card);
}

static int sdio_write_multi(sd_card_t *card, uint32_t lba, uint32_t count, const uint8_t *src)
{
    uint32_t arg = card->is_sdhc ? lba : (lba << 9);

    sdio_reset_fifo();
    openfw_mmio_wr32(SDIO_BLKSIZ_ADDR,  512u);
    openfw_mmio_wr32(SDIO_BYTCNT_ADDR,  count * 512u);
    openfw_mmio_wr32(SDIO_RINTSTS_ADDR, 0xFFFFFFFFu);

    if (sdio_send_cmd(SD_CMD25_WRITE_MULTI, arg, 0) != 0) {
        return -1;
    }

    uint64_t t0 = us_timer_now();
    if (sdio_fifo_write_words(src, count * 128u, t0) != 0) {
        (void)sdio_send_cmd(SD_CMD12_STOP, 0u, 1);
        sdio_reset_fifo();
        return -1;
    }

    while (!(openfw_mmio_rd32(SDIO_RINTSTS_ADDR) & SDIO_INT_DTO)) {
        if ((uint32_t)(us_timer_now() - t0) >= SDIO_DAT_TIMEOUT_US) {
            openfw_mmio_wr32(SDIO_RINTSTS_ADDR, 0xFFFFFFFFu);
            (void)sdio_send_cmd(SD_CMD12_STOP, 0u, 1);
            sdio_reset_fifo();
            return -1;
        }
    }
    uint32_t st = openfw_mmio_rd32(SDIO_RINTSTS_ADDR);
    openfw_mmio_wr32(SDIO_RINTSTS_ADDR, 0xFFFFFFFFu);

    int stop_rc = sdio_send_cmd(SD_CMD12_STOP, 0u, 1);
    if (stop_rc != 0 || (st & SDIO_INT_DATA_ERRS)) {
        sdio_reset_fifo();
        return -1;
    }
    return sdio_wait_card_ready(card);
}

int sd_read_blocks(sd_card_t *card, uint32_t lba, uint32_t count, void *buf)
{
    if (!card || !card->initialized || (!buf && count > 0u)) {
        return -1;
    }
    if (card->async_active) {
        sd_read_async_abort(card);
    }
    uint8_t *dst = (uint8_t *)buf;
    uint32_t done = 0u;
    while (done < count) {
        wdt_feed();
        uint32_t rem = count - done;
        uint32_t cur_lba = lba + done;
        int near_end = (card->sector_count > 3u && cur_lba + rem > card->sector_count - 3u);
        if (rem >= 2u && !near_end) {
            uint32_t chunk = (rem > SDIO_MAX_MULTI_BLKS) ? SDIO_MAX_MULTI_BLKS : rem;
            if (sdio_read_multi(card, cur_lba, chunk, dst + (done * 512u)) != 0) {
                return -1;
            }
            done += chunk;
        } else {
            if (sdio_read_one_sector(card, cur_lba, dst + (done * 512u)) != 0) {
                return -1;
            }
            done++;
        }
    }
    return 0;
}

int sd_write_blocks(sd_card_t *card, uint32_t lba, uint32_t count, const void *buf)
{
    if (!card || !card->initialized || (!buf && count > 0u)) {
        return -1;
    }
    (void)sd_flush(card);
    const uint8_t *src = (const uint8_t *)buf;
    uint32_t done = 0u;
    while (done < count) {
        wdt_feed();
        uint32_t rem = count - done;
        uint32_t cur_lba = lba + done;
        int near_end = (card->sector_count > 3u && cur_lba + rem > card->sector_count - 3u);
        if (rem >= 2u && !near_end) {
            uint32_t chunk = (rem > SDIO_MAX_MULTI_BLKS) ? SDIO_MAX_MULTI_BLKS : rem;
            if (sdio_write_multi(card, cur_lba, chunk, src + (done * 512u)) != 0) {
                return -1;
            }
            done += chunk;
        } else {
            if (sdio_write_one_sector(card, cur_lba, src + (done * 512u)) != 0) {
                return -1;
            }
            done++;
        }
    }
    return 0;
}

static int sd_blk_read(void *ctx, uint32_t lba, uint32_t count, void *buf)
{
    return sd_read_blocks((sd_card_t *)ctx, lba, count, buf);
}

static int sd_blk_write(void *ctx, uint32_t lba, uint32_t count, const void *buf)
{
    return sd_write_blocks((sd_card_t *)ctx, lba, count, buf);
}

blkdev_t sd_as_blkdev(sd_card_t *card)
{
    blkdev_t dev;
    dev.read_sectors  = sd_blk_read;
    dev.write_sectors = sd_blk_write;
    dev.total_sectors = card ? card->sector_count : 0u;
    dev.ctx           = card;
    return dev;
}
