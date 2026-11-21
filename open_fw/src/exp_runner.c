/*
 * Smartlink SL6801 OpenFW Dynamic SRAM Experiment Loader, Runner & Telemetry Bridge
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include "sl6801_regs.h"
#include "openfw.h"
#include "openfw_exp.h"
#include "openfw_crypto.h"

#ifdef OPENFW_HOST_MMIO
static uint8_t g_exp_arena[OPENFW_EXP_ARENA_SIZE];
static uint8_t g_exp_out[OPENFW_EXP_OUT_SIZE];
static openfw_host_exp_fn_t g_host_runner = 0;
#else
__attribute__((section(".exp_arena"), used, aligned(4096)))
uint8_t g_exp_arena[OPENFW_EXP_ARENA_SIZE];
__attribute__((section(".exp_out"), used, aligned(4096)))
uint8_t g_exp_out[OPENFW_EXP_OUT_SIZE];
#endif

static uint32_t            g_exp_out_len     = 0u;
static uint32_t            g_exp_loaded_size = 0u;
static uint32_t            g_exp_run_count   = 0u;
static openfw_exp_api_t    g_exp_api;
static int                 g_exp_api_ready   = 0;
static openfw_exp_result_t g_last_res;

static exfat_vol_t        *g_exp_vol         = 0;
static int                *g_exp_sd_mounted  = 0;

static void (*volatile g_dmac_ch0_cb)(void) = 0;

#ifndef OPENFW_HOST_MMIO
__attribute__((section(".ramtext")))
#endif
void DMAC_CH0_IRQHandler(void)
{
    void (*cb)(void) = g_dmac_ch0_cb;
    if (cb) {
        cb();
    } else {
        uint32_t st = openfw_mmio_rd32(DMAC_CH_IRQ_STAT_ADDR(0u));
        openfw_mmio_wr32(DMAC_CH_IRQ_STAT_ADDR(0u), st);
        openfw_mmio_clr32(DMAC_CH_CTL_ADDR(0u), DMAC_CTL_CH_EN);
        openfw_mmio_wr32(DMAC_CH_IRQ_EN_ADDR(0u), 0u);
        nvic_disable_irq(DMAC_IRQ_BASE);
    }
}

void openfw_exp_set_dmac_ch0_irq(void (*cb)(void))
{
    if (!cb) {
        nvic_disable_irq(DMAC_IRQ_BASE);
        OPENFW_DSB();
        OPENFW_ISB();
        openfw_mmio_wr32(DMAC_CH_IRQ_EN_ADDR(0u), 0u);
        openfw_mmio_clr32(DMAC_CH_CTL_ADDR(0u), DMAC_CTL_CH_EN);
        openfw_mmio_wr32(DMAC_CH_IRQ_STAT_ADDR(0u),
                         openfw_mmio_rd32(DMAC_CH_IRQ_STAT_ADDR(0u)));
        nvic_clear_pending(DMAC_IRQ_BASE);
        g_dmac_ch0_cb = 0;
    } else {
        g_dmac_ch0_cb = cb;
    }
}

void openfw_exp_init_api(const openfw_exp_api_t *api)
{
    if (api) {
        g_exp_api = *api;
    }
    g_exp_api.magic       = OPENFW_EXP_ABI_MAGIC;
    g_exp_api.arena_base  = g_exp_arena;
    g_exp_api.arena_size  = OPENFW_EXP_ARENA_SIZE;
    g_exp_api.out_buf     = g_exp_out;
    g_exp_api.out_size    = OPENFW_EXP_OUT_SIZE;
    g_exp_api.out_len_ptr = &g_exp_out_len;
    if (!g_exp_api.wdt_feed)         g_exp_api.wdt_feed         = wdt_feed;
    if (!g_exp_api.millis)           g_exp_api.millis           = millis;
    if (!g_exp_api.us_timer_now)     g_exp_api.us_timer_now     = us_timer_now;
    if (!g_exp_api.udelay)           g_exp_api.udelay           = udelay;
    if (!g_exp_api.mdelay)           g_exp_api.mdelay           = mdelay;
    if (!g_exp_api.log_reset)        g_exp_api.log_reset        = openfw_log_reset;
    if (!g_exp_api.log_putc)         g_exp_api.log_putc         = openfw_log_putc;
    if (!g_exp_api.log_puts)         g_exp_api.log_puts         = openfw_log_puts;
    if (!g_exp_api.log_u32)          g_exp_api.log_u32          = openfw_log_u32;
    if (!g_exp_api.log_hex)          g_exp_api.log_hex          = openfw_log_hex;
    if (!g_exp_api.sd_write_file)    g_exp_api.sd_write_file    = openfw_exp_sd_write_file;
    if (!g_exp_api.sd_dump_log)      g_exp_api.sd_dump_log      = openfw_exp_sd_dump_log;
    if (!g_exp_api.set_dmac_ch0_irq) g_exp_api.set_dmac_ch0_irq = openfw_exp_set_dmac_ch0_irq;
    g_exp_api_ready = 1;
}

void openfw_exp_bind_fs(exfat_vol_t *vol, int *sd_mounted_ptr)
{
    g_exp_vol        = vol;
    g_exp_sd_mounted = sd_mounted_ptr;
}

#ifdef OPENFW_HOST_MMIO
void openfw_exp_set_host_runner(openfw_host_exp_fn_t fn)
{
    g_host_runner = fn;
}
#endif

int openfw_exp_write(uint32_t off, const uint8_t *data, uint32_t len)
{
    if (off > OPENFW_EXP_ARENA_SIZE || len > (OPENFW_EXP_ARENA_SIZE - off)) {
        return -1;
    }
    if (len > 0u && !data) {
        return -1;
    }
    if (off == 0u) {
        for (uint32_t i = 0u; i < OPENFW_EXP_ARENA_SIZE; i++) {
            g_exp_arena[i] = 0u;
        }
        g_exp_loaded_size = 0u;
    }
    for (uint32_t i = 0u; i < len; i++) {
        g_exp_arena[off + i] = data[i];
    }
    if ((off + len) > g_exp_loaded_size) {
        g_exp_loaded_size = off + len;
    }
    return 0;
}

int openfw_exp_run(uint32_t entry_off, uint32_t arg, openfw_exp_result_t *out)
{
    if (g_exp_loaded_size == 0u || (entry_off & ~1u) >= OPENFW_EXP_ARENA_SIZE) {
        return -1;
    }
    if (!g_exp_api_ready) {
        openfw_exp_init_api(0);
    }

    g_exp_out_len = 0u;
    g_exp_api.arena_base  = g_exp_arena;
    g_exp_api.arena_size  = OPENFW_EXP_ARENA_SIZE;
    g_exp_api.out_buf     = g_exp_out;
    g_exp_api.out_size    = OPENFW_EXP_OUT_SIZE;
    g_exp_api.out_len_ptr = &g_exp_out_len;

    wdt_feed();
    uint32_t t0 = millis();
    uint32_t rc = 0u;

#ifdef OPENFW_HOST_MMIO
    if (!g_host_runner) {
        return -1;
    }
    g_exp_deadline_ms = t0 + OPENFW_EXP_TIMEOUT_MS;
    g_exp_running     = 1u;
    rc = g_host_runner(&g_exp_api, entry_off, arg);
#else
    typedef uint32_t (*exp_entry_fn_t)(const openfw_exp_api_t *, uint32_t);
    uintptr_t fn_addr = ((uintptr_t)&g_exp_arena[entry_off & ~1u]) | 1u;
    g_exp_deadline_ms = t0 + OPENFW_EXP_TIMEOUT_MS;
    g_exp_running     = 1u;
    __asm__ volatile ("dsb\nisb" ::: "memory");
    exp_entry_fn_t fn = (exp_entry_fn_t)fn_addr;
    rc = fn(&g_exp_api, arg);
#endif

    g_exp_running = 0u;
    openfw_exp_set_dmac_ch0_irq(0);
    wdt_feed();

    uint32_t elapsed = (uint32_t)(millis() - t0);
    if (g_exp_out_len > OPENFW_EXP_OUT_SIZE) {
        g_exp_out_len = OPENFW_EXP_OUT_SIZE;
    }

    uint16_t crc16 = openfw_crc16_ccitt_false(g_exp_arena, g_exp_loaded_size);
    g_exp_run_count++;

    g_last_res.magic              = OPENFW_EXP_RES_MAGIC;
    g_last_res.rc                 = rc;
    g_last_res.elapsed_ms         = elapsed;
    g_last_res.out_len            = g_exp_out_len;
    g_last_res.log_widx           = g_openfw_log.write_idx;
    g_last_res.run_count          = g_exp_run_count;
    g_last_res.arena_crc_and_size = ((uint32_t)crc16 << 16) | (g_exp_loaded_size & 0xFFFFu);
    g_last_res.arena_base         = (uint32_t)(uintptr_t)g_exp_arena;

    if (out) {
        *out = g_last_res;
    }
    return 0;
}

void openfw_exp_get_last_result(openfw_exp_result_t *out)
{
    if (!out) {
        return;
    }
    *out = g_last_res;
    if (out->magic != OPENFW_EXP_RES_MAGIC) {
        out->magic              = OPENFW_EXP_RES_MAGIC;
        out->rc                 = 0u;
        out->elapsed_ms         = 0u;
        out->out_len            = g_exp_out_len;
        out->log_widx           = g_openfw_log.write_idx;
        out->run_count          = g_exp_run_count;
        out->arena_crc_and_size = (g_exp_loaded_size & 0xFFFFu);
        out->arena_base         = (uint32_t)(uintptr_t)g_exp_arena;
    }
}

int openfw_exp_mem_read(uint32_t addr, uint8_t *dst, uint32_t len)
{
    if (!dst || len == 0u || len > 64u || addr > (0xFFFFFFFFu - len)) {
        return -1;
    }
#ifdef OPENFW_HOST_MMIO
    if (addr >= OPENFW_EXP_ARENA_ADDR &&
        addr < (OPENFW_EXP_ARENA_ADDR + OPENFW_EXP_ARENA_SIZE) &&
        len <= (OPENFW_EXP_ARENA_ADDR + OPENFW_EXP_ARENA_SIZE - addr)) {
        uint32_t off = addr - OPENFW_EXP_ARENA_ADDR;
        for (uint32_t i = 0u; i < len; i++) {
            dst[i] = g_exp_arena[off + i];
        }
        return 0;
    }
    if (addr >= OPENFW_EXP_OUT_ADDR &&
        addr < (OPENFW_EXP_OUT_ADDR + OPENFW_EXP_OUT_SIZE) &&
        len <= (OPENFW_EXP_OUT_ADDR + OPENFW_EXP_OUT_SIZE - addr)) {
        uint32_t off = addr - OPENFW_EXP_OUT_ADDR;
        for (uint32_t i = 0u; i < len; i++) {
            dst[i] = g_exp_out[off + i];
        }
        return 0;
    }
    if (addr >= 0x40000000u && (addr & 3u) == 0u && (len & 3u) == 0u) {
        for (uint32_t i = 0u; i < len; i += 4u) {
            uint32_t w = openfw_mmio_rd32(addr + i);
            dst[i + 0u] = (uint8_t)(w);
            dst[i + 1u] = (uint8_t)(w >> 8);
            dst[i + 2u] = (uint8_t)(w >> 16);
            dst[i + 3u] = (uint8_t)(w >> 24);
        }
        return 0;
    }
    for (uint32_t i = 0u; i < len; i++) {
        dst[i] = 0u;
    }
    return 0;
#else
    if ((addr & 3u) == 0u && (len & 3u) == 0u) {
        for (uint32_t i = 0u; i < len; i += 4u) {
            uint32_t w = *(volatile uint32_t *)(uintptr_t)(addr + i);
            dst[i + 0u] = (uint8_t)(w);
            dst[i + 1u] = (uint8_t)(w >> 8);
            dst[i + 2u] = (uint8_t)(w >> 16);
            dst[i + 3u] = (uint8_t)(w >> 24);
        }
    } else {
        for (uint32_t i = 0u; i < len; i++) {
            dst[i] = *(volatile uint8_t *)(uintptr_t)(addr + i);
        }
    }
    return 0;
#endif
}

static int exp_sd_open_for_overwrite(const char *dir, const char *name, exfat_writer_t *w)
{
    if (!g_exp_vol || !g_exp_sd_mounted || !(*g_exp_sd_mounted) || !name || name[0] == '\0') {
        return -1;
    }

    const char *use_dir = (dir && dir[0] != '\0') ? dir : "/";
    int is_root = (use_dir[0] == '/' && use_dir[1] == '\0');
    if (!is_root) {
        if (exfat_mkdir(g_exp_vol, use_dir) != 0) {
            return -1;
        }
    }

    char path[96];
    int pos = 0;
    if (use_dir[0] != '/') {
        path[pos++] = '/';
    }
    for (int i = 0; use_dir[i] != '\0' && pos < 60; i++) {
        path[pos++] = use_dir[i];
    }
    if (pos == 0 || path[pos - 1] != '/') {
        path[pos++] = '/';
    }
    for (int i = 0; name[i] != '\0' && pos < (int)(sizeof(path) - 1); i++) {
        path[pos++] = name[i];
    }
    path[pos] = '\0';

    exfat_entry_t existing;
    if (exfat_find_path(g_exp_vol, path, &existing) == 0) {
        if (existing.is_dir) {
            return -1;
        }
        if (exfat_delete_file(g_exp_vol, use_dir, name) != 0) {
            return -1;
        }
    }

    rtc_datetime_t dt;
    rtc_get(&dt);
    exfat_set_timestamp(g_exp_vol, rtc_to_dos_timestamp(&dt));

    return exfat_create_file(g_exp_vol, use_dir, name, w);
}

int openfw_exp_sd_write_file(const char *dir, const char *name, const void *data, uint32_t len)
{
    if (len > 0u && !data) {
        return -1;
    }

    exfat_writer_t w;
    if (exp_sd_open_for_overwrite(dir, name, &w) != 0) {
        return -1;
    }
    if (len > 0u && exfat_write(&w, data, len) != 0) {
        (void)exfat_close_file(&w);
        return -1;
    }
    return exfat_close_file(&w);
}

int openfw_exp_sd_dump_log(const char *name)
{
    const char *fname = (name && name[0] != '\0') ? name : "exp.log";
    if (g_openfw_log.magic != OPENFW_LOG_MAGIC || g_openfw_log.size != OPENFW_LOG_SIZE) {
        return -1;
    }

    exfat_writer_t w;
    if (exp_sd_open_for_overwrite("/", fname, &w) != 0) {
        return -1;
    }

    uint32_t widx = g_openfw_log.write_idx;
    int rc = 0;
    if (widx <= OPENFW_LOG_SIZE) {
        if (widx > 0u) {
            rc = exfat_write(&w, g_openfw_log.buf, widx);
        }
    } else {
        uint32_t start     = widx & (OPENFW_LOG_SIZE - 1u);
        uint32_t first_len = OPENFW_LOG_SIZE - start;
        rc = exfat_write(&w, &g_openfw_log.buf[start], first_len);
        if (rc == 0 && start > 0u) {
            rc = exfat_write(&w, &g_openfw_log.buf[0], start);
        }
    }
    if (rc != 0) {
        (void)exfat_close_file(&w);
        return -1;
    }
    return exfat_close_file(&w);
}
