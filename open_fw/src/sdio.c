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
#define SD_CMD17_READ1      0x2351u
#define SD_CMD24_WRITE1     0x2758u
#define SD_ACMD41_OPCOND    0x2569u
#define SD_CMD55_APPCMD     0x2577u

#define SDIO_CMD_TIMEOUT_US 100000u
#define SDIO_DAT_TIMEOUT_US 500000u

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

    /* Ramp to 6 MHz (24 MHz / 4) for stable 1-bit data transfers */
    sdio_set_clk_div(4u);
    if (sdio_update_clk() != 0) {
        return -1;
    }

    card->initialized  = 1;
    card->is_sdhc      = is_sdhc;
    card->rca          = rca;
    card->sector_count = sectors;
    return 0;
}

static int sdio_read_one_sector(sd_card_t *card, uint32_t lba, uint8_t *dst)
{
    uint32_t arg = card->is_sdhc ? lba : (lba << 9);

    openfw_mmio_wr32(SDIO_BLKSIZ_ADDR,  512u);
    openfw_mmio_wr32(SDIO_BYTCNT_ADDR,  512u);
    openfw_mmio_wr32(SDIO_RINTSTS_ADDR, 0xFFFFFFFFu);

    if (sdio_send_cmd(SD_CMD17_READ1, arg, 0) != 0) {
        return -1;
    }

    uint64_t t0 = us_timer_now();
    for (uint32_t w = 0u; w < 128u; w++) {
        while (openfw_mmio_rd32(SDIO_STATUS_ADDR) & SDIO_STAT_FIFO_EMP) {
            if (openfw_mmio_rd32(SDIO_RINTSTS_ADDR) & (SDIO_INT_RTO | SDIO_INT_DRTO)) {
                openfw_mmio_wr32(SDIO_RINTSTS_ADDR, 0xFFFFFFFFu);
                return -1;
            }
            if ((uint32_t)(us_timer_now() - t0) >= SDIO_DAT_TIMEOUT_US) {
                openfw_mmio_wr32(SDIO_RINTSTS_ADDR, 0xFFFFFFFFu);
                return -1;
            }
        }
        uint32_t word = openfw_mmio_rd32(SDIO_FIFO_ADDR);
        dst[w * 4u + 0u] = (uint8_t)(word);
        dst[w * 4u + 1u] = (uint8_t)(word >> 8);
        dst[w * 4u + 2u] = (uint8_t)(word >> 16);
        dst[w * 4u + 3u] = (uint8_t)(word >> 24);
    }

    while (!(openfw_mmio_rd32(SDIO_RINTSTS_ADDR) & SDIO_INT_DTO)) {
        if ((uint32_t)(us_timer_now() - t0) >= SDIO_DAT_TIMEOUT_US) {
            openfw_mmio_wr32(SDIO_RINTSTS_ADDR, 0xFFFFFFFFu);
            return -1;
        }
    }
    uint32_t st = openfw_mmio_rd32(SDIO_RINTSTS_ADDR);
    openfw_mmio_wr32(SDIO_RINTSTS_ADDR, 0xFFFFFFFFu);
    if (st & (SDIO_INT_RCRC | SDIO_INT_RTO | SDIO_INT_DRTO)) {
        return -1;
    }
    return 0;
}

static int sdio_write_one_sector(sd_card_t *card, uint32_t lba, const uint8_t *src)
{
    uint32_t arg = card->is_sdhc ? lba : (lba << 9);

    openfw_mmio_wr32(SDIO_BLKSIZ_ADDR,  512u);
    openfw_mmio_wr32(SDIO_BYTCNT_ADDR,  512u);
    openfw_mmio_wr32(SDIO_RINTSTS_ADDR, 0xFFFFFFFFu);

    if (sdio_send_cmd(SD_CMD24_WRITE1, arg, 0) != 0) {
        return -1;
    }

    uint64_t t0 = us_timer_now();
    for (uint32_t w = 0u; w < 128u; w++) {
        while (openfw_mmio_rd32(SDIO_STATUS_ADDR) & SDIO_STAT_FIFO_FULL) {
            if (openfw_mmio_rd32(SDIO_RINTSTS_ADDR) & (SDIO_INT_RTO | SDIO_INT_DRTO)) {
                openfw_mmio_wr32(SDIO_RINTSTS_ADDR, 0xFFFFFFFFu);
                return -1;
            }
            if ((uint32_t)(us_timer_now() - t0) >= SDIO_DAT_TIMEOUT_US) {
                openfw_mmio_wr32(SDIO_RINTSTS_ADDR, 0xFFFFFFFFu);
                return -1;
            }
        }
        uint32_t word = (uint32_t)src[w * 4u + 0u]
                      | ((uint32_t)src[w * 4u + 1u] << 8)
                      | ((uint32_t)src[w * 4u + 2u] << 16)
                      | ((uint32_t)src[w * 4u + 3u] << 24);
        openfw_mmio_wr32(SDIO_FIFO_ADDR, word);
    }

    while (!(openfw_mmio_rd32(SDIO_RINTSTS_ADDR) & SDIO_INT_DTO)) {
        if ((uint32_t)(us_timer_now() - t0) >= SDIO_DAT_TIMEOUT_US) {
            openfw_mmio_wr32(SDIO_RINTSTS_ADDR, 0xFFFFFFFFu);
            return -1;
        }
    }
    openfw_mmio_wr32(SDIO_RINTSTS_ADDR, 0xFFFFFFFFu);

    /* Wait for card programming completion (R1 bit 8 = READY_FOR_DATA) */
    uint64_t p0 = us_timer_now();
    while (1) {
        if (sdio_send_cmd(SD_CMD13_STATUS, (uint32_t)card->rca << 16, 0) == 0 &&
            (openfw_mmio_rd32(SDIO_RESP0_ADDR) & (1u << 8))) {
            return 0;
        }
        if ((uint32_t)(us_timer_now() - p0) >= SDIO_DAT_TIMEOUT_US) {
            return -1;
        }
        rom_us_delay(100u);
    }
}

int sd_read_blocks(sd_card_t *card, uint32_t lba, uint32_t count, void *buf)
{
    if (!card || !card->initialized || (!buf && count > 0u)) {
        return -1;
    }
    uint8_t *dst = (uint8_t *)buf;
    for (uint32_t i = 0u; i < count; i++) {
        wdt_feed();
        if (sdio_read_one_sector(card, lba + i, dst + (i * 512u)) != 0) {
            return -1;
        }
    }
    return 0;
}

int sd_write_blocks(sd_card_t *card, uint32_t lba, uint32_t count, const void *buf)
{
    if (!card || !card->initialized || (!buf && count > 0u)) {
        return -1;
    }
    const uint8_t *src = (const uint8_t *)buf;
    for (uint32_t i = 0u; i < count; i++) {
        wdt_feed();
        if (sdio_write_one_sector(card, lba + i, src + (i * 512u)) != 0) {
            return -1;
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
