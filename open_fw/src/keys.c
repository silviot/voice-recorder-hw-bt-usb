/*
 * Smartlink SL6801 Bare-Metal SARADC Key Ladder (0x40096000) & Rec Switch (PB6)
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include "sl6801_regs.h"
#include "openfw.h"

#define KEYS_QUEUE_SIZE       16u
#define KEYS_QUEUE_MASK       (KEYS_QUEUE_SIZE - 1u)
#define KEYS_DEBOUNCE_SAMPLES 8u

static key_event_t       g_key_q[KEYS_QUEUE_SIZE];
static volatile uint32_t g_key_q_head        = 0u;
static volatile uint32_t g_key_q_tail        = 0u;
static volatile uint8_t  g_key_overflow      = 0u;
static volatile uint8_t  g_key_resync_ignore = 0u;
static volatile uint8_t  g_keys_scan_en      = 0u;
static volatile uint8_t  g_keys_exp_paused   = 0u;
static volatile uint8_t  g_keys_ext_paused   = 0u; /* keys_pause_scan() (headset probe) */
static volatile key_id_t g_key_stable        = KEY_NONE;
static volatile uint32_t g_key_stable_since  = 0u;
static key_id_t          g_key_cand          = KEY_NONE;
static uint8_t           g_key_cand_count    = 0u;

static inline __attribute__((always_inline)) void keys_barrier(void)
{
#ifdef OPENFW_HOST_MMIO
    __asm__ volatile ("" ::: "memory");
#else
    __asm__ volatile ("dmb" ::: "memory");
#endif
}

static inline __attribute__((always_inline)) key_id_t keys_decode_inline(uint16_t adc)
{
    if (adc < 336u) {
        return KEY_LEFT;
    }
    if (adc < 848u) {
        return KEY_RIGHT;
    }
    if (adc < 1424u) {
        return KEY_VOL_UP;
    }
    if (adc < 1824u) {
        return KEY_NONE;
    }
    if (adc < 2176u) {
        return KEY_VOL_DOWN;
    }
    if (adc < 2560u) {
        return KEY_BACK;
    }
    if (adc < 2960u) {
        return KEY_ENTER;
    }
    return KEY_NONE;
}

static inline __attribute__((always_inline)) void keys_push_event(uint32_t t_ms,
                                                                  key_id_t key,
                                                                  uint8_t pressed)
{
    uint32_t head = g_key_q_head;
    uint32_t tail = g_key_q_tail;
    if ((uint32_t)(head - tail) >= KEYS_QUEUE_SIZE) {
        g_key_overflow = 1u;
        return;
    }
    g_key_q[head & KEYS_QUEUE_MASK].t_ms    = t_ms;
    g_key_q[head & KEYS_QUEUE_MASK].key     = key;
    g_key_q[head & KEYS_QUEUE_MASK].pressed = pressed;
    keys_barrier();
    g_key_q_head = head + 1u;
}

static void keys_handle_overflow(void)
{
    if (!g_key_overflow) {
        return;
    }
    uint32_t pm;
    OPENFW_IRQ_SAVE(pm);
    if (g_key_overflow) {
        g_key_q_tail   = g_key_q_head;
        g_key_overflow = 0u;
        if (g_key_stable != KEY_NONE) {
            g_key_resync_ignore = 1u;
        }
    }
    OPENFW_IRQ_RESTORE(pm);
}

void keys_init(void)
{
    g_keys_scan_en = 0u;
    keys_barrier();

    /* PD0 = Analog mode (mux=15, floating) for kadc_ch1 resistor ladder */
    openfw_mmio_clr32(GPIOD_BASE + 0x24u, 0x3u);
    gpio_set_mux(GPIO_PORT_D, 0u, 15u);

    /* PB6 = GPIO input for Record slide switch */
    gpio_set_mux(GPIO_PORT_B, 6u, 0u);

    /* Enable codec analog bias for PB11 3.5 mm jack detect (matches stock /dev/exti ioctl 0x81) */
    openfw_mmio_set32(CRU_RST_SET1_ADDR, (1u << 5));
    openfw_mmio_set32(CRU_RST_STAT1_ADDR, (1u << 5));
    for (uint32_t wait = 0u;
         wait < 100000u && (openfw_mmio_rd32(CRU_RST_STAT1_ADDR) & (1u << 5)) == 0u;
         wait++) {
    }
    openfw_mmio_set32(CRU_CLK_EN_AUDIO_ADDR, 1u);
    openfw_mmio_set32(CODEC_GLB_ADDR, 1u);
    openfw_mmio_set32(CODEC_DAC_ANA_CFG0_ADDR, 0x200u);
    openfw_mmio_set32(CODEC_ANA_CTRL0_ADDR, 0x22u);

    /* PB11 = GPIO input with pull-up for Earphone jack detect (active-low: 0=inserted) */
    gpio_set_mux(GPIO_PORT_B, 11u, 0u);
    openfw_mmio_rmw32(GPIOB_BASE + 0x24u, 3u << 22, 2u << 22);
    openfw_mmio_clr32(GPIOB_BASE + 0x2Cu, 3u << 22);

    /* Release SARADC reset & enable clock (CRU bank 2 bit 20 = ID 0x54) */
    openfw_mmio_set32(CRU_SARADC_RST_ADDR, 1u << 20);
    openfw_mmio_set32(CRU_SARADC_CLK_ADDR, 1u << 20);
    for (uint32_t wait = 0u;
         wait < 100000u && (openfw_mmio_rd32(CRU_SARADC_CLK_ADDR) & (1u << 20)) == 0u;
         wait++) {
    }

    /* Configure continuous polling scan on ch1 (0x40096034) */
    openfw_mmio_wr32(SARADC_CTRL_ADDR,    0x80180000u | (1u << 1));
    openfw_mmio_wr32(SARADC_START_ADDR,   0u);
    openfw_mmio_wr32(SARADC_IE_ADDR,      0u);
    openfw_mmio_wr32(SARADC_REG10_ADDR,   0u);
    openfw_mmio_wr32(SARADC_REG18_ADDR,   0u);
    openfw_mmio_wr32(SARADC_CH1_THR_ADDR, 0x0FFF0030u);
    mdelay(5u);
    openfw_mmio_wr32(SARADC_START_ADDR,   0x0000A800u);
    mdelay(5u);

    /* Reset scanner & queue with stable=KEY_NONE so a key held at boot still fires */
    g_key_q_head        = 0u;
    g_key_q_tail        = 0u;
    g_key_overflow      = 0u;
    g_key_resync_ignore = 0u;
    g_keys_exp_paused   = 0u;
    g_key_stable        = KEY_NONE;
    g_key_stable_since  = 0u;
    g_key_cand          = KEY_NONE;
    g_key_cand_count    = 0u;
    keys_barrier();
    g_keys_scan_en      = 1u;
}

OPENFW_RAMFUNC
void keys_scan_tick(uint32_t now_ms)
{
    if (!g_keys_scan_en) {
        return;
    }
    if (g_exp_running != 0u || g_keys_ext_paused != 0u) {
        g_keys_exp_paused = 1u;
        return;
    }

    uint16_t adc = (uint16_t)(openfw_mmio_rd32(SARADC_CH1_DATA_ADDR) & 0x0FFFu);
    key_id_t raw = keys_decode_inline(adc);

    if (g_keys_exp_paused) {
        g_keys_exp_paused  = 0u;
        g_key_stable_since = now_ms;
        keys_barrier();
        g_key_stable       = raw;
        g_key_cand         = raw;
        g_key_cand_count   = 0u;
        g_key_overflow     = 1u;
        return;
    }

    if (raw == g_key_stable) {
        g_key_cand       = raw;
        g_key_cand_count = 0u;
        return;
    }

    if (raw == g_key_cand) {
        g_key_cand_count++;
    } else {
        g_key_cand       = raw;
        g_key_cand_count = 1u;
    }

    if (g_key_cand_count >= KEYS_DEBOUNCE_SAMPLES) {
        key_id_t old       = g_key_stable;
        g_key_stable_since = now_ms;
        keys_barrier();
        g_key_stable       = raw;
        g_key_cand_count   = 0u;

        if (g_key_resync_ignore) {
            /* The old key belongs to the previous input context. A direct
             * transition to another debounced key is a fresh press. */
            old = KEY_NONE;
            g_key_resync_ignore = 0u;
        }

        if (old != KEY_NONE) {
            keys_push_event(now_ms, old, 0u);
        }
        if (raw != KEY_NONE) {
            keys_push_event(now_ms, raw, 1u);
        }
    }
}

int keys_event_pending(void)
{
    keys_handle_overflow();
    return (g_key_q_head != g_key_q_tail) ? 1 : 0;
}

int keys_peek_event(key_event_t *out)
{
    keys_handle_overflow();
    uint32_t tail = g_key_q_tail;
    uint32_t head = g_key_q_head;
    if (head == tail) {
        return 0;
    }
    keys_barrier();
    if (out) {
        *out = g_key_q[tail & KEYS_QUEUE_MASK];
    }
    return 1;
}

int keys_pop_event(key_event_t *out)
{
    keys_handle_overflow();
    uint32_t tail = g_key_q_tail;
    uint32_t head = g_key_q_head;
    if (head == tail) {
        return 0;
    }
    keys_barrier();
    if (out) {
        *out = g_key_q[tail & KEYS_QUEUE_MASK];
    }
    keys_barrier();
    g_key_q_tail = tail + 1u;
    return 1;
}

static key_id_t keys_reset_queue(int ignore_held)
{
    uint32_t pm;
    OPENFW_IRQ_SAVE(pm);
    g_key_q_tail        = g_key_q_head;
    g_key_overflow      = 0u;
    key_id_t held = g_key_stable;
    g_key_resync_ignore = ignore_held && held != KEY_NONE;
    OPENFW_IRQ_RESTORE(pm);
    return held;
}

key_id_t keys_flush_events(void)
{
    return keys_reset_queue(0);
}

void keys_ignore_held(void)
{
    (void)keys_reset_queue(1);
}

key_id_t keys_state(uint32_t *since_ms)
{
    uint32_t pm;
    OPENFW_IRQ_SAVE(pm);
    if (g_key_overflow) {
        g_key_q_tail   = g_key_q_head;
        g_key_overflow = 0u;
        if (g_key_stable != KEY_NONE) {
            g_key_resync_ignore = 1u;
        }
    }
    key_id_t st = g_key_resync_ignore ? KEY_NONE : g_key_stable;
    uint32_t sc = g_key_stable_since;
    OPENFW_IRQ_RESTORE(pm);
    if (since_ms) {
        *since_ms = sc;
    }
    return st;
}

/* Pause/resume debounced scanning (e.g. while a diagnostic reconfigures the
 * SARADC). Resuming resyncs exactly like the end of an SRAM experiment: the
 * key held at that moment is ignored until released. */
void keys_pause_scan(int pause)
{
    keys_barrier();
    g_keys_ext_paused = pause ? 1u : 0u;
    keys_barrier();
}

uint16_t keys_read_adc(void)
{
    return (uint16_t)(openfw_mmio_rd32(SARADC_CH1_DATA_ADDR) & 0x0FFFu);
}

int keys_read_rec_switch(void)
{
    return (gpio_read(GPIO_PORT_B, 6u) == 0) ? 1 : 0;
}

int keys_read_hp_detect(void)
{
    return (gpio_read(GPIO_PORT_B, 11u) == 0) ? 1 : 0;
}

key_id_t keys_decode(uint16_t adc)
{
    return keys_decode_inline(adc);
}

const char *keys_name(key_id_t key)
{
    switch (key) {
    case KEY_NONE:     return "NONE";
    case KEY_LEFT:     return "PREV";
    case KEY_RIGHT:    return "NEXT";
    case KEY_VOL_UP:   return "VOL+";
    case KEY_VOL_DOWN: return "VOL-";
    case KEY_BACK:     return "BACK";
    case KEY_ENTER:    return "PLAY";
    default:           return "UNK";
    }
}
