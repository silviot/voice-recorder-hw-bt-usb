# Bluetooth plan: hardware results

Silicon results for `docs/BLUETOOTH_BLUEDROID_PLAN.md` (M5 experiments). Device: HW_76 (SL6801), USB-attached,
probes run with `tools/dev_cycle.py exp` on `DEV_EXP=1` images. Production firmware restored after each session.

## Session 1 (2026-11-24)

### Exp 0 — SRAM candidate regions: PASS
Image: default (non-RTOS) `DEV_EXP=1`, which uses neither region. `exp/exp_sram_fill.c` wrote an address-hash pattern
over `0x805C00..0x81A37F` (81.9 KiB) and `0x86B000..0x87FFFF` (84 KiB), 42,464 words.

| Check point | Activity since fill | Changed words |
|---|---|---|
| immediately | — | 0 / 0 |
| after exercise | legacy BT 20 s (`exp_bt.c`: RF cal, RW-BT ROM controller, discoverable, 32,021 IRQ-26 events, HCI ready), DAC DMA playback 2 s (`exp_dac.c`), mic DMA capture 5 s (0 overruns) + 3 SD file writes (`exp_sram_exercise.c`), USB probe traffic, CPU1 LVGL UI | 0 / 0 |

Not yet covered: BT ACL/connection traffic and power-mode transitions — re-check during Exp 1 stage C / Exp 5.
Consequence: the lower region is now heap_5 region 0 in RTOS builds (`sl6801_firm.ld` `RTOS_HEAP_LO`).

### RTOS + BT image self-test on silicon: 14/14 PASS
Image: `DEV_EXP=1 BT=1 RTOS_SELFTEST=1` (Bluedroid linked, not started). Read back with `exp/exp_peek.c`.
delay 50 ms, queue 100, priority-inheritance mutex, timer, notify, event group, heap, preemption 20 ms, 10 FromISR
notifications, stack watermark, self-delete reap, `.bt_data` copy (168 B), malloc failure returns NULL (no reboot),
`bt_port` glue (bt_alarm task delivers a 5 ms esp_timer exactly once after the drain).
Free heap at boot: 34,424 B with the upper region only; **118,256 B** with both regions (min 95,536 B after the
tests), identical to QEMU.

### Exp 2 — noise sources (first read; 16 KiB raw dumps, `exp/exp_entropy_dump.c`)
Quick estimators (MCV with 99 % upper bound, binary Markov, lag-1 correlation, bz2 on packed bits):

| Source | Result |
|---|---|
| mic ADC, gain 8, bits 0/1/2 | ~0.95–0.98 bits per bit each, transition probabilities 0.49–0.51, packed bits incompressible (bz2 ratio 1.19) |
| mic ADC, gain 31, bit 0 | ~0.95; bits 1–2 weaker (0.54–0.78) |
| SARADC key ladder, back-to-back | stuck/correlated (lag-1 r = 0.999) — unusable |
| SARADC key ladder, 200 µs apart | weak (bit-0 Markov 0.28) |
| timer jitter (us timer over a busy loop) | 0.22 bits/byte — unusable |

Provisional source config: mic ADC at gain 8, the 2 low bits of each sample as one sample, credited at **1 bit/sample**
(about half the measured value). At 16 kHz the 384-bit seed needs ~0.1 s after the 1024-sample start-up window.
Still required before crediting in a release: NIST SP 800-90B `ea_non_iid` on longer dumps, cold-start and quiet-room
repeats, and confirmation that the noise is electronic (it is present at low gain).

### Other observations
- eFuse words: `w0=0x1510011D w1=0x003D4000 w2=0x00000600 w3=0xC0000440`; the legacy BD_ADDR `02:CA:55:00:01:1D`
  ends with w0's low bytes — candidate stable device-identity input (plan M1 "Local identity").
- Idle key-ladder ADC 4055..4090.
- One SD multi-block write timed out and fell back to single-sector writes (existing driver behaviour, `SDW_MDTO`).
