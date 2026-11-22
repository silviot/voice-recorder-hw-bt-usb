/*
 * Smartlink SL6801 Dual-Core CPU1 Dedicated LVGL/LCD Rendering Engine
 *
 * Boots Cortex-M4F CPU1 via the hardware mailbox (0x30000600..0x30000608) and
 * CRU reset ID 0, matching stock firmware's ViewTask split:
 *   - CPU0 owns audio DMA, DSP, SDIO/exFAT, USB MSC, Classic BT, PMU, and keys.
 *   - CPU1 owns LVGL widget binding, software rasterization, animations, and
 *     the 8080 parallel ST7735S LCDC bus (0x40095000).
 *
 * Lock-free ABA-free 2-slot double-buffered command handoff allows CPU0 to
 * publish UI state snapshots in microseconds without blocking on LCD blits.
 * Automatically falls back to synchronous single-core rendering on host builds,
 * QEMU (where CPU1 is not emulated), or if CPU1 faults.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stddef.h>
#include "sl6801_regs.h"
#include "sl6801_rom.h"
#include "openfw.h"
#include "openfw_ui.h"
#include "ui/ui_port.h"

#define CPU1_CMD_RENDER        1u
#define CPU1_CMD_REC_LIVE      2u
#define CPU1_MAX_FILES         32u

typedef struct {
    ui_screen_id_t  scr;
    uint8_t         kind;
    uint8_t         has_files;
    uint8_t         has_spec_pcm;
    uint8_t         spec_ch;
    uint8_t         spec_vor_wait;
    uint16_t        spec_frames;
    uint16_t        file_count_copied;
    ui_model_t      model;
    union {
        ui_file_entry_t files[CPU1_MAX_FILES];
        int16_t         spec_pcm[UI_SPEC_FFT_N * 2u];
    } payload;
} cpu1_ui_slot_t;

volatile uint32_t g_cpu1_alive        = 0u;
volatile uint32_t g_cpu1_faulted      = 0u;
volatile uint32_t g_cpu1_fault_pc     = 0u;
volatile uint32_t g_cpu1_fault_cfsr   = 0u;
volatile uint32_t g_cpu1_wfe_ok       = 0u;
volatile uint32_t g_cpu1_wfe_wakes    = 0u;
volatile uint32_t g_ui_last_render_us = 0u;
volatile uint32_t g_ui_max_render_us  = 0u;
volatile uint32_t g_ui_frames         = 0u;
volatile uint32_t g_ui_last_load_us   = 0u;
volatile uint32_t g_ui_last_draw_us   = 0u;
volatile uint32_t g_ui_last_blit_us   = 0u;
volatile uint32_t g_ui_heap_used      = 0u;

#ifndef OPENFW_HOST_MMIO

#define CPU1_STACK_WORDS 768u /* 3 KiB stack for LVGL + EEZ rendering */

static uint32_t       g_cpu1_stack[CPU1_STACK_WORDS] __attribute__((aligned(8)));
static cpu1_ui_slot_t s_slots[2];

static volatile uint32_t s_req_word          = 0u;    /* (seq << 1) | slot_idx */
static volatile uint32_t s_ack_seq           = 0u;
static volatile uint32_t s_cpu1_reading_slot = 0xFFu; /* 0, 1, or 0xFF (idle) */
static volatile uint32_t s_cpu1_animating    = 0u;
static volatile uint32_t s_cpu1_stop_req     = 0u;
static volatile uint32_t s_cpu1_stopped      = 0u;
static volatile uint32_t s_mbox_probe_done   = 0u;
static volatile uint8_t  s_spec_reset_pend   = 0u;
static volatile uint8_t  s_spec_marker_pend  = 0u;
static ui_spectrogram_t *volatile s_spec_ptr = NULL;
static uint8_t           s_cpu1_active       = 0u;
static uint32_t          s_last_seq          = 0u;
static uint32_t          s_last_slot         = 0u;
static uint32_t          s_stall_ack_seen    = 0u;
static uint64_t          s_stall_since_us    = 0ull;

static inline void cpu1_mbox_drain_and_clear(void)
{
    while ((MBOX_CH_CNT(0u) & 7u) >= 2u) {
        (void)MBOX_CH_DATA(0u);
        (void)MBOX_CH_DATA(0u);
    }
    MBOX_CPU1_IRQ_STAT = 0x01u;
    NVIC_ICPR(IRQ_IPC_MBOX_CPU1 >> 5) = (1u << (IRQ_IPC_MBOX_CPU1 & 31u));
}

static inline void cpu1_ring_mbox(void)
{
    if ((MBOX_CH_CNT(0u) & 7u) <= 2u) {
        MBOX_CH_DATA(0u) = 1u;
        MBOX_CH_DATA(0u) = 1u;
        OPENFW_DSB();
    }
}

static void cpu1_trampoline(void) __attribute__((naked, used));
static void cpu1_entry_c(void) __attribute__((noreturn, used));
static void cpu1_fault_asm(void) __attribute__((naked, used));
static void cpu1_fault_c(const uint32_t *frame, uint32_t ipsr) __attribute__((noreturn, used));

__attribute__((section(".data"), aligned(512), used))
static uint32_t g_cpu1_isr_vector[16] = {
    (uint32_t)(uintptr_t)&g_cpu1_stack[CPU1_STACK_WORDS], /*  0: Initial MSP */
    (uint32_t)(uintptr_t)cpu1_trampoline,                 /*  1: Reset */
    (uint32_t)(uintptr_t)cpu1_fault_asm,                  /*  2: NMI */
    (uint32_t)(uintptr_t)cpu1_fault_asm,                  /*  3: HardFault */
    (uint32_t)(uintptr_t)cpu1_fault_asm,                  /*  4: MemManage */
    (uint32_t)(uintptr_t)cpu1_fault_asm,                  /*  5: BusFault */
    (uint32_t)(uintptr_t)cpu1_fault_asm,                  /*  6: UsageFault */
    0u, 0u, 0u, 0u,
    (uint32_t)(uintptr_t)cpu1_fault_asm,                  /* 11: SVCall */
    (uint32_t)(uintptr_t)cpu1_fault_asm,                  /* 12: DebugMon */
    0u,
    (uint32_t)(uintptr_t)cpu1_fault_asm,                  /* 14: PendSV */
    (uint32_t)(uintptr_t)cpu1_fault_asm,                  /* 15: SysTick */
};

OPENFW_FASTFUNC __attribute__((used, noreturn))
static void cpu1_fault_c(const uint32_t *frame, uint32_t ipsr)
{
    (void)ipsr;
    __asm__ volatile ("cpsid i" ::: "memory");
    uint32_t faddr = (uint32_t)(uintptr_t)frame;
    uint32_t pc    = 0xDEAD57ACu;
    if ((faddr & 3u) == 0u &&
        faddr >= (uint32_t)(uintptr_t)&g_cpu1_stack[0] &&
        faddr <= (uint32_t)(uintptr_t)&g_cpu1_stack[CPU1_STACK_WORDS] - 28u) {
        pc = frame[6];
    }
    g_cpu1_fault_pc     = pc;
    g_cpu1_fault_cfsr   = SCB_CFSR;
    s_cpu1_animating    = 0u;
    s_cpu1_reading_slot = 0xFFu;
    g_cpu1_faulted      = 1u;
    s_cpu1_stopped      = 1u;
    OPENFW_DSB();
    while (1) {
        OPENFW_NOP();
    }
}

OPENFW_FASTFUNC __attribute__((naked, used))
static void cpu1_fault_asm(void)
{
    __asm__ volatile (
        "tst lr, #4\n"
        "ite eq\n"
        "mrseq r0, msp\n"
        "mrsne r0, psp\n"
        "mrs r1, ipsr\n"
        "ldr r2, =0x30000604\n"
        "ldr r2, [r2]\n"
        "mov sp, r2\n"
        "b.w cpu1_fault_c\n"
    );
}

/* Exact SL6801 CPU1 boot trampoline (mirrors stock FIRM 0x00CE7370) */
OPENFW_FASTFUNC __attribute__((naked, used))
static void cpu1_trampoline(void)
{
    __asm__ volatile (
        "ldr r1, =0x30000604\n"
        "ldr r0, [r1]\n"
        "mov sp, r0\n"
        "ldr r0, =0xE000ED88\n"
        "ldr r1, [r0]\n"
        "orr r1, r1, #0x00F00000\n"
        "str r1, [r0]\n"
        "dsb\n"
        "isb\n"
        "ldr r3, =0x40000060\n"
        "movs r2, #8\n"
        "str r2, [r3]\n"
        "ldr r0, =0x30000608\n"
        "ldr r0, [r0]\n"
        "bx r0\n"
    );
}

OPENFW_FASTFUNC __attribute__((noreturn, used))
static void cpu1_entry_c(void)
{
    __asm__ volatile ("cpsid i" ::: "memory");
    SCB_VTOR  = (uint32_t)(uintptr_t)g_cpu1_isr_vector;
    SCB_SHCSR |= SCB_SHCSR_FAULTS_EN;
    SCB_SCR   |= SCB_SCR_SEVONPEND;
    OPENFW_DSB();
    OPENFW_ISB();

    ui_port_init();

    /* Prepare IPC Mailbox ch0 RX IRQ on CPU1 and run boot self-test for WFE wake */
    MBOX_CPU1_IRQ_EN = (MBOX_CPU1_IRQ_EN & ~0x02u) | 0x01u;
    cpu1_mbox_drain_and_clear();
    OPENFW_DSB();

    g_cpu1_alive = 1u;
    OPENFW_DSB();

    {
        uint64_t t_mb = us_timer_now();
        while ((MBOX_CH_CNT(0u) & 7u) < 2u && (us_timer_now() - t_mb) < 2000ull) {
            OPENFW_NOP();
        }
        uint32_t irq_bit = (1u << (IRQ_IPC_MBOX_CPU1 & 31u));
        uint32_t ispr    = NVIC_ISPR(IRQ_IPC_MBOX_CPU1 >> 5);
        cpu1_mbox_drain_and_clear();
        g_cpu1_wfe_ok     = (ispr & irq_bit) ? 1u : 0u;
        s_mbox_probe_done = 1u;
        OPENFW_DSB();
    }

    uint32_t last_tick_ms = millis();

    while (1) {
        if (s_cpu1_stop_req != 0u) {
            s_cpu1_animating    = 0u;
            s_cpu1_reading_slot = 0xFFu;
            s_cpu1_stopped      = 1u;
            OPENFW_DSB();
            /* Halt completely bus-silent (IRQs are masked) before CPU0 asserts reset */
            while (1) {
                __asm__ volatile ("wfi" ::: "memory");
            }
        }

        uint32_t w   = s_req_word;
        uint32_t seq = w >> 1;
        if (seq != s_ack_seq) {
            uint32_t idx = w & 1u;
            s_cpu1_reading_slot = idx;
            OPENFW_DSB();
            /* Verify CPU0 did not advance s_req_word before seeing s_cpu1_reading_slot */
            if (s_req_word != w) {
                s_cpu1_reading_slot = 0xFFu;
                OPENFW_DSB();
                continue;
            }

            const cpu1_ui_slot_t *slot = &s_slots[idx];
            uint64_t t0 = us_timer_now();
            ui_spectrogram_t *sp = slot->model.spec ? slot->model.spec : s_spec_ptr;
            if (s_spec_reset_pend && sp) {
                s_spec_reset_pend  = 0u;
                s_spec_marker_pend = 0u;
                ui_spec_reset(sp);
            }
            if (slot->has_spec_pcm && sp) {
                ui_spec_push_pcm(sp,
                                 (slot->spec_frames > 0u) ? slot->payload.spec_pcm : (const int16_t *)0,
                                 slot->spec_frames, slot->spec_ch, (int)slot->spec_vor_wait);
            }
            if (s_spec_marker_pend && sp) {
                s_spec_marker_pend = 0u;
                ui_spec_drop_marker(sp);
            }
            if (slot->kind == CPU1_CMD_REC_LIVE) {
                ui_rec_update_live_sync(&slot->model);
            } else {
                ui_render_screen_sync(slot->scr, &slot->model);
            }
            uint32_t dt_us = (uint32_t)(us_timer_now() - t0);
            g_ui_last_render_us = dt_us;
            if (dt_us > g_ui_max_render_us) {
                g_ui_max_render_us = dt_us;
            }
            g_ui_frames++;
            {
                lv_mem_monitor_t mon;
                lv_mem_monitor(&mon);
                g_ui_heap_used = (uint32_t)(mon.total_size - mon.free_size);
            }
            s_cpu1_animating = (uint32_t)ui_port_is_animating();

            OPENFW_DSB();
            s_ack_seq           = seq;
            s_cpu1_reading_slot = 0xFFu;
            OPENFW_DSB();
            last_tick_ms = millis();
            continue;
        }

        if (!s_cpu1_animating && g_cpu1_wfe_ok != 0u) {
            /* True zero-power Cortex-M4 WFE sleep woken in <1 us by CPU0 IPC Mailbox ch0 */
            cpu1_mbox_drain_and_clear();
            OPENFW_DSB();
            if ((s_req_word >> 1) != s_ack_seq || s_cpu1_stop_req != 0u) {
                continue;
            }
            __asm__ volatile ("wfe" ::: "memory");
            g_cpu1_wfe_wakes++;
            continue;
        }

        uint32_t now_ms   = millis();
        uint32_t tick_gap = s_cpu1_animating ? 1u : 100u;
        if ((uint32_t)(now_ms - last_tick_ms) >= tick_gap) {
            last_tick_ms = now_ms;
            ui_port_task_handler();
            s_cpu1_animating = (uint32_t)ui_port_is_animating();
        } else {
            /* Keep SRAM bus nearly idle when static (~10 us wake latency) and light during anims */
            uint32_t nops = s_cpu1_animating ? 64u : 1024u;
            for (volatile uint32_t d = 0u; d < nops; d++) {
                OPENFW_NOP();
            }
        }
    }
}

static void cpu1_submit_slot(uint8_t kind, ui_screen_id_t scr, const ui_model_t *m,
                             const int16_t *spec_pcm, uint32_t spec_frames,
                             uint8_t spec_ch, int spec_vor_wait, int has_spec_pcm)
{
    uint64_t now_us = us_timer_now();
    /* Guard against a stalled CPU1 (e.g. missed WFE wake): if prior work went
     * unacknowledged for >250 ms, park CPU1 cleanly and fall back to CPU0. */
    if (s_ack_seq == s_last_seq) {
        s_stall_since_us = 0ull;
    } else if (s_stall_since_us == 0ull || s_ack_seq != s_stall_ack_seen) {
        s_stall_ack_seen = s_ack_seq;
        s_stall_since_us = now_us;
    } else if ((now_us - s_stall_since_us) > 250000ull) {
        cpu1_ui_stop();
        if (s_spec_reset_pend && m->spec) {
            s_spec_reset_pend  = 0u;
            s_spec_marker_pend = 0u;
            ui_spec_reset(m->spec);
        }
        if (has_spec_pcm && m->spec) {
            ui_spec_push_pcm(m->spec, spec_pcm, spec_frames, spec_ch, spec_vor_wait);
        }
        if (s_spec_marker_pend && m->spec) {
            s_spec_marker_pend = 0u;
            ui_spec_drop_marker(m->spec);
        }
        if (kind == CPU1_CMD_REC_LIVE) {
            ui_rec_update_live_sync(m);
        } else {
            ui_render_screen_sync(scr, m);
        }
        return;
    }

    uint32_t next = s_last_slot ^ 1u;
    uint64_t t0   = now_us;
    while (s_cpu1_reading_slot == next) {
        if (g_cpu1_faulted != 0u || (us_timer_now() - t0) > 100000ull) {
            cpu1_ui_stop();
            if (s_spec_reset_pend && m->spec) {
                s_spec_reset_pend  = 0u;
                s_spec_marker_pend = 0u;
                ui_spec_reset(m->spec);
            }
            if (has_spec_pcm && m->spec) {
                ui_spec_push_pcm(m->spec, spec_pcm, spec_frames, spec_ch, spec_vor_wait);
            }
            if (s_spec_marker_pend && m->spec) {
                s_spec_marker_pend = 0u;
                ui_spec_drop_marker(m->spec);
            }
            if (kind == CPU1_CMD_REC_LIVE) {
                ui_rec_update_live_sync(m);
            } else {
                ui_render_screen_sync(scr, m);
            }
            return;
        }
        OPENFW_NOP();
    }

    cpu1_ui_slot_t *slot = &s_slots[next];
    slot->scr          = scr;
    slot->kind         = kind;
    slot->has_spec_pcm = (uint8_t)(has_spec_pcm ? 1u : 0u);
    slot->spec_ch      = spec_ch;
    slot->spec_vor_wait = (uint8_t)(spec_vor_wait ? 1u : 0u);
    slot->spec_frames  = 0u;
    slot->model        = *m;

    if (kind == CPU1_CMD_RENDER &&
        (scr == UI_SCR_FILES || scr == UI_SCR_DELETE) &&
        m->files != NULL && m->file_count > 0) {
        uint32_t n = (uint32_t)m->file_count;
        if (n > CPU1_MAX_FILES) {
            n = CPU1_MAX_FILES;
        }
        for (uint32_t i = 0u; i < n; i++) {
            slot->payload.files[i] = m->files[i];
        }
        slot->has_files         = 1u;
        slot->file_count_copied = (uint16_t)n;
        slot->model.files       = slot->payload.files;
        slot->model.file_count  = (int)n;
    } else {
        slot->has_files         = 0u;
        slot->file_count_copied = 0u;
        slot->model.files       = NULL;
        if (has_spec_pcm && spec_pcm != NULL && spec_frames > 0u) {
            uint32_t nf = (spec_frames > UI_SPEC_FFT_N) ? UI_SPEC_FFT_N : spec_frames;
            uint32_t ns = nf * ((spec_ch >= 2u) ? 2u : 1u);
            for (uint32_t i = 0u; i < ns; i++) {
                slot->payload.spec_pcm[i] = spec_pcm[i];
            }
            slot->spec_frames = (uint16_t)nf;
        }
    }

    uint32_t seq = (s_last_seq + 1u) & 0x7FFFFFFFu;
    if (seq == 0u) {
        seq = 1u;
    }
    s_last_seq  = seq;
    s_last_slot = next;

    OPENFW_DSB();
    s_req_word = (seq << 1) | next;
    OPENFW_DSB();
    if (g_cpu1_wfe_ok != 0u) {
        cpu1_ring_mbox();
    }
}

int cpu1_ui_start(void)
{
    if (s_cpu1_active) {
        return 1;
    }

    /* Enable SL6801 Hardware IPC Mailbox (0x40070000) ch0 CPU0 -> CPU1 for WFE wakeups */
    rom_rst_release(CRU_RST_IPC_MBOX);
    rom_clk_enable(CRU_CLK_IPC_MBOX);
    MBOX_CFG         = (MBOX_CFG & ~0x10u) | 0x01u;
    MBOX_CPU0_IRQ_EN &= ~0x03u;
    MBOX_CPU1_IRQ_EN = (MBOX_CPU1_IRQ_EN & ~0x02u) | 0x01u;
    while ((MBOX_CH_CNT(0u) & 7u) >= 2u) {
        (void)MBOX_CH_DATA(0u);
        (void)MBOX_CH_DATA(0u);
    }
    MBOX_CPU0_IRQ_STAT = 0xFFFFFFFFu;
    MBOX_CPU1_IRQ_STAT = 0xFFFFFFFFu;

    /* Reset all shared IPC state before releasing CPU1 (preserve lifetime stats) */
    rom_rst_hold(CRU_RST_CPU1);
    g_cpu1_alive        = 0u;
    g_cpu1_faulted      = 0u;
    g_cpu1_fault_pc     = 0u;
    g_cpu1_fault_cfsr   = 0u;
    g_cpu1_wfe_ok       = 0u;
    s_req_word          = 0u;
    s_ack_seq           = 0u;
    s_cpu1_reading_slot = 0xFFu;
    s_cpu1_animating    = 0u;
    s_cpu1_stop_req     = 0u;
    s_cpu1_stopped      = 0u;
    s_mbox_probe_done   = 0u;
    s_last_seq          = 0u;
    s_last_slot         = 0u;
    s_stall_ack_seen    = 0u;
    s_stall_since_us    = 0ull;

    CPU1_BOOT_TRAMP   = ((uint32_t)(uintptr_t)cpu1_trampoline) | 1u;
    CPU1_BOOT_SP      = (uint32_t)(uintptr_t)&g_cpu1_stack[CPU1_STACK_WORDS];
    CPU1_BOOT_ENTRY   = ((uint32_t)(uintptr_t)cpu1_entry_c) | 1u;
    SYSCTL_CPU1_READY = 0u;
    OPENFW_DSB();
    OPENFW_ISB();

    rom_rst_release(CRU_RST_CPU1);

    /* Wait up to 60 ms for CPU1 to initialize LVGL and assert g_cpu1_alive */
    uint64_t t0 = us_timer_now();
    while (g_cpu1_alive == 0u && g_cpu1_faulted == 0u) {
        if ((us_timer_now() - t0) >= 60000ull) {
            break;
        }
        OPENFW_WFI();
    }

    if (g_cpu1_alive != 0u && g_cpu1_faulted == 0u) {
        /* Trigger 2-word ch0 doorbell so CPU1 self-verifies NVIC IRQ 55 -> SEVONPEND WFE */
        cpu1_ring_mbox();
        uint64_t t_p = us_timer_now();
        while (s_mbox_probe_done == 0u && g_cpu1_faulted == 0u &&
               (us_timer_now() - t_p) < 5000ull) {
            OPENFW_NOP();
        }
        s_cpu1_active = 1u;
        return 1;
    }

    /* CPU1 did not boot (e.g. single-core QEMU) — park cleanly and use CPU0 fallback */
    rom_rst_hold(CRU_RST_CPU1);
    s_cpu1_active = 0u;
    return 0;
}

void cpu1_ui_stop(void)
{
    if (s_cpu1_active) {
        s_cpu1_stop_req = 1u;
        OPENFW_DSB();
        if (g_cpu1_wfe_ok != 0u) {
            cpu1_ring_mbox();
        }
        uint64_t t0 = us_timer_now();
        while (s_cpu1_stopped == 0u && (us_timer_now() - t0) < 50000ull) {
            OPENFW_NOP();
        }
        s_cpu1_active       = 0u;
        s_cpu1_animating    = 0u;
        s_cpu1_reading_slot = 0xFFu;
    }
    rom_rst_hold(CRU_RST_CPU1);
    OPENFW_DSB();
}

int cpu1_ui_is_active(void)
{
    if (s_cpu1_active && g_cpu1_faulted != 0u) {
        cpu1_ui_stop();
    }
    return (int)s_cpu1_active;
}

int cpu1_ui_wfe_active(void)
{
    return (cpu1_ui_is_active() && g_cpu1_wfe_ok != 0u) ? 1 : 0;
}

void cpu1_ui_sync(uint32_t timeout_ms)
{
    if (!cpu1_ui_is_active()) {
        return;
    }
    uint64_t t0      = us_timer_now();
    uint64_t lim_us  = (uint64_t)timeout_ms * 1000ull;
    while (s_ack_seq != (s_req_word >> 1) || s_cpu1_reading_slot != 0xFFu) {
        if (g_cpu1_faulted != 0u) {
            cpu1_ui_stop();
            return;
        }
        if ((us_timer_now() - t0) >= lim_us) {
            return;
        }
        /* Yield AHB/XIP/APB bus completely to CPU1 until next 1 ms SysTick */
        OPENFW_WFI();
    }
}

void ui_spec_reset_async(ui_spectrogram_t *sp)
{
    if (!sp) {
        return;
    }
    if (cpu1_ui_is_active()) {
        s_spec_ptr         = sp;
        s_spec_marker_pend = 0u;
        s_spec_reset_pend  = 1u;
        OPENFW_DSB();
        return;
    }
    ui_spec_reset(sp);
}

void ui_spec_drop_marker_async(ui_spectrogram_t *sp)
{
    if (!sp) {
        return;
    }
    if (cpu1_ui_is_active()) {
        s_spec_ptr         = sp;
        s_spec_marker_pend = 1u;
        OPENFW_DSB();
        return;
    }
    ui_spec_drop_marker(sp);
}

void ui_render_screen(ui_screen_id_t scr, const ui_model_t *m)
{
    if (!m) {
        return;
    }
    if (cpu1_ui_is_active()) {
        cpu1_submit_slot(CPU1_CMD_RENDER, scr, m, NULL, 0u, 0u, 0, 0);
        return;
    }
    if (s_spec_reset_pend && m->spec) {
        s_spec_reset_pend  = 0u;
        s_spec_marker_pend = 0u;
        ui_spec_reset(m->spec);
    }
    if (s_spec_marker_pend && m->spec) {
        s_spec_marker_pend = 0u;
        ui_spec_drop_marker(m->spec);
    }
    uint64_t t0 = us_timer_now();
    ui_render_screen_sync(scr, m);
    uint32_t dt_us = (uint32_t)(us_timer_now() - t0);
    g_ui_last_render_us = dt_us;
    if (dt_us > g_ui_max_render_us) {
        g_ui_max_render_us = dt_us;
    }
    g_ui_frames++;
}

void ui_rec_update_live(const ui_model_t *m)
{
    if (!m) {
        return;
    }
    if (cpu1_ui_is_active()) {
        /* Only post 20 Hz live meter/spec updates when CPU1 is idle so we never
         * overwrite a pending full-screen switch. */
        if (s_ack_seq == (s_req_word >> 1) && s_cpu1_reading_slot == 0xFFu) {
            cpu1_submit_slot(CPU1_CMD_REC_LIVE, UI_SCR_REC, m, NULL, 0u, 0u, 0, 0);
        }
        return;
    }
    ui_rec_update_live_sync(m);
}

void ui_rec_push_live(const ui_model_t *m, const int16_t *pcm, uint32_t frames,
                      uint8_t channels, int vor_paused, int full_render)
{
    if (!m) {
        return;
    }
    if (cpu1_ui_is_active()) {
        if (full_render) {
            cpu1_submit_slot(CPU1_CMD_RENDER, UI_SCR_REC, m, pcm, frames, channels, vor_paused, 1);
        } else if (s_ack_seq == (s_req_word >> 1) && s_cpu1_reading_slot == 0xFFu) {
            cpu1_submit_slot(CPU1_CMD_REC_LIVE, UI_SCR_REC, m, pcm, frames, channels, vor_paused, 1);
        }
        return;
    }
    if (m->spec) {
        ui_spec_push_pcm(m->spec, pcm, frames, channels, vor_paused);
    }
    if (full_render) {
        ui_render_screen_sync(UI_SCR_REC, m);
    } else {
        ui_rec_update_live_sync(m);
    }
}

void ui_dispatch_tick(void)
{
    if (cpu1_ui_is_active()) {
        return; /* CPU1 drives LVGL timers & 125 Hz animations autonomously */
    }
    ui_port_task_handler();
}

int ui_dispatch_is_animating(void)
{
    if (cpu1_ui_is_active()) {
        return (s_cpu1_animating != 0u || s_ack_seq != (s_req_word >> 1)) ? 1 : 0;
    }
    return ui_port_is_animating();
}

#else /* OPENFW_HOST_MMIO: synchronous host fallback */

int cpu1_ui_start(void)                  { return 0; }
void cpu1_ui_stop(void)                  { }
int cpu1_ui_is_active(void)              { return 0; }
int cpu1_ui_wfe_active(void)             { return 0; }
void cpu1_ui_sync(uint32_t timeout_ms)   { (void)timeout_ms; }

void ui_render_screen(ui_screen_id_t scr, const ui_model_t *m)
{
    ui_render_screen_sync(scr, m);
    g_ui_frames++;
}

void ui_rec_update_live(const ui_model_t *m)
{
    ui_rec_update_live_sync(m);
}

void ui_rec_push_live(const ui_model_t *m, const int16_t *pcm, uint32_t frames,
                      uint8_t channels, int vor_paused, int full_render)
{
    if (m && m->spec) {
        ui_spec_push_pcm(m->spec, pcm, frames, channels, vor_paused);
    }
    if (full_render) {
        ui_render_screen_sync(UI_SCR_REC, m);
    } else {
        ui_rec_update_live_sync(m);
    }
}

void ui_spec_reset_async(ui_spectrogram_t *sp)
{
    ui_spec_reset(sp);
}

void ui_spec_drop_marker_async(ui_spectrogram_t *sp)
{
    ui_spec_drop_marker(sp);
}

void ui_dispatch_tick(void)
{
    ui_port_task_handler();
}

int ui_dispatch_is_animating(void)
{
    return ui_port_is_animating();
}

#endif /* OPENFW_HOST_MMIO */
