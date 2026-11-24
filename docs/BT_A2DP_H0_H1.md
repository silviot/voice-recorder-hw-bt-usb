# BT headphones mode: H0 + H1 implementation record (audio core)

Branch `feat/bt-a2dp`, on top of `4c83b3d`. Implements milestones H0 and H1 of `docs/BT_A2DP_DESIGN.md` (rev 3, kept on
`feat/freertos-bluedroid`): the external-codec build, the Bluedroid queue patch, and the audio core (SBC wrapper,
pacer, PCM ring and resampler, output-sink abstraction) with host tests and a bit-exactness check on the QEMU model.
This file is the live log for this work: fold it into the design's live log section when the branches merge.
Paths are relative to `open_fw/`.

## What is in place

| Part | Files | Notes |
|---|---|---|
| Ext-codec config | `bt_port/sdkconfig.defaults`, `bt_port/sdkconfig.h`, `bt_port/bt_port_prefix.h`, `tools/gen_bluedroid_srcs.py` | `CONFIG_BT_A2DP_USE_EXTERNAL_CODEC`, `SEP_NUM_MAX=2`, `SBC_ENC_INCLUDED`, 48 kHz default rate, queue cap 3 (= 4 packets). Generator KEEP list for `external/sbc/encoder/srce`; `bt/bluedroid_srcs.mk` regenerates unchanged. |
| Bluedroid patch 0034 | `btc_a2dp_source_ext_codec.c`, `btc_a2dp_source.h` | Queue cap `#ifndef`, enqueue/deq/flush counters, `ext_txq_stat`, `ext_txq_drop`, `ext_can_send`. |
| Bluedroid patch 0035 | `sbc_types.h`, `oi_cpu_dep.h` | SBC 32-bit types as `int32_t` (were `long`: 64-bit on the host). |
| Link roots | `bt/bt_link_roots.c` | Design §5.3 roots (the `*_by_handle` AVRCP variants: the plain names are deprecated and warn) plus the `hp_tx_bd_*` entry points, which keep `SBC_Encoder_Init` in the image as `check_bt_link_roots.sh` requires. |
| SBC wrapper | `bt_port/hp_sbc.c/.h` | Config from the AVDTP SBC CIE, frame length, per-frame bitpool. `static SBC_ENC_PARAMS` lands in `.bt_bss`. |
| PCM ring + resampler | `bt_port/hp_sink.c/.h`, `bt_port/hp_rs_taps.h` (`tools/gen_hp_rs_taps.py`) | SPSC ring (4096 stereo / 8192 mono frames in 16 KiB), flush index + generation, seqlock chunk copy, exact 3x polyphase (3 x 24 taps), general 64-phase x 16-tap path with linear phase interpolation (up and 48 -> 44.1 tables), 32.32 phase step, mono <-> stereo, Q15 gain, mute, a second reader ("tap") for output mode BOTH. |
| Pacer | `bt_port/hp_tx.c/.h` | Rev 3 §4.3-4.6: elapsed and credit clamps, `HP_MAX_PKT_PER_TICK`, headroom rule, fixed fpp per STARTED period, can_send + alloc before touching PCM, speech bitpool, flush/drop with RTP `ts` rewind, packet-record FIFO with flush epochs, `queued_frames = ring + unheard_real + lat_term`, rewind snapshot frozen until the next flush, link-end add-back, rebase on every stop/STARTED, stall watchdog. |
| Bluedroid binding | `bt_port/hp_tx_bd.c/.h` | Ops on `esp_a2d_*` + patch 0034, osi_alarm tick in BTC, stop protocol of rev 3 R-10 (`stop_req` / `tick_stopped`, bounded wait), NOCP observer on the stream's ACL handle. Called by bt_hp (H5); not wired to a session yet. |
| NOCP hook | `bt_port/bt_ctrl.c/.h` | `bt_ctrl_set_nocp_observer()`: called from `rom_nocp` (bt_ctrl task) with the packets completed per handle. |
| Output sink | `include/audio_out.h`, `src/audio_out.c`, `src/player.c`, `src/audio_dac.c` | `aout_*` API of §3.1. BT=0: macros onto `audio_dac_*`, default image byte-identical (FIRM CRC-16 0xC786, `cmp` equal). BT=1: routes DAC / BT / BOTH per file; output-mode setting SPEAKER / HEADPHONES / BOTH; demand start on write/unmute; `aout_stop` drops the ring and `audio_tx_q`; `audio_dac_borrow_buf()`. The player calls `aout_*` (still always stereo, `ch = 2`); the UI and the player's BT hooks are H6. |
| Self-test | `bt_port/hp_selftest.c/.h`, `src/rtos/rtos_selftest.c`, `tools/run_qemu.py` | `T_HPCORE`: CRC32 of SBC (bitpool 35 <-> 24) + resampler (3x and 44.1 -> 48) output, equal to the host golden `0x3F0E9D61` on the Cortex-M4 model (`make test-qemu-bt`). |

## Host results (`make test-bt-hp`)

- Resampler (2 s, 1 kHz, mono and stereo): 16 -> 48 kHz 80.6 dB; 44.1 -> 48 69.4 dB; 16 -> 44.1 71.9 dB;
  48 -> 44.1 78.9 dB; 8 -> 48 82.9 dB; 22.05 -> 44.1 68.3 dB. DC gain 0.99977-1.00000, rate error < 0.002 %.
- Ring stress (2 threads, random flushes, 300 M frames; again under TSan): 0 order violations; the seqlock
  re-check discarded ~170 k racing chunk copies.
- Pacer, 60 s virtual time: 44.1 kHz fpp 7/10/11 -0.004 / -0.009 / -0.014 %, 48 kHz -0.009 / 0.000 / -0.022 %,
  0 pads, queue <= 2 (cap 4). 1 s BTC stall: 2 s after it 1.000-1.012x nominal, worst 200 ms window 77-80 frames
  (bound 97-108), burst <= 2. Bench override 30 ms / 1 packet lowered to 15 ms; 5 % tick jitter: 0 pads.
- SBC round trip through the vendored decoder: 16 kHz mono -> 48 kHz at bitpool 24 68.0 dB; 48 kHz bitpool 35
  67.2 dB; 44.1 kHz 70.1 dB; fpp unchanged across a 35 <-> 24 switch; RTP timestamps contiguous across flushes.
- Flush: 40 old frames after a seek without the txq drop (bound 60), 10 with it (bound 20).
- Position: `heard = written - queued` never stepped back while playing; 0 drift after 5 pause/unpause cycles;
  0 right after a flush; remote-suspend snapshot equal to the live value; link-loss snapshot adds back <= 4 packets.
- `queued_frames` vs ground truth (16 kHz on a 48 kHz link, slow link): within one packet (426 source frames).
- Stall watchdog fires at 810 ms (800 ms setting) with NOCP stopped, never on a slow healthy link.
- Output mode BOTH at 16 kHz: DAC vs headset offset mean -8 ms, worst 23 ms.

## Deviations from the design (and why)

1. **Patch numbers 0034/0035, not 0029.** Rev 3 §5.2: 0029-0033 are taken on `feat/freertos-bluedroid`; this
   branch's README reserves them so the lines merge without renumbering. Patch A's stat accessor fills a struct
   (`btc_a2dp_source_ext_txq_stat_t`: packets and SBC frames per counter, `rej_pkts`, `qlen`) instead of three
   out-pointers, and adds `btc_a2dp_source_ext_can_send()` for the rev 3 pre-check (the stack's `tx_flush` is
   static).
2. **New patch 0035 (not in the design).** The vendored SBC encoder and decoder type 32-bit values as `long`. On the
   LP64 host the encoder's analysis filter then walks off `s32X` (ASan) and the decoder misparses frames, so no host
   round trip was possible. Same width on target.
3. **`sdkconfig.h` edited by hand.** `kconfiglib` and an ESP-IDF checkout are not on this machine; the two lines
   are exactly what the generator would add (`CONFIG_BT_A2DP_USE_EXTERNAL_CODEC 1`, `CONFIG_BT_A2DP_SEP_NUM_MAX 2`).
   Regenerate with `tools/gen_bluedroid_config.py` when the prerequisite exists.
4. **Resampler goes straight to the polyphase design** (the stage-2 spec); the linear stage 1 was skipped. It meets
   the stage-2 gates (> 60 / > 55 dB) and is table-driven, so host and ARM agree bit for bit (`T_HPCORE`).
5. **Rewind snapshot ends at the first real packet after a later STARTED**, not only at the next flush. Otherwise
   an idle suspend while paused (snapshot ~0) followed by unpause without a seek would keep `queued_frames` at the
   stale value for the whole rest of the file.
6. **Output mode BOTH** (not in the design): the ring has a second, app-owned reader that feeds the DAC; it trails
   the headset's ring reader by `(q_bt - ring) - q_dac`, capped at half the ring. With the 16 KiB DAC buffer
   borrowed for the ring the DAC cannot run, so BOTH needs a heap ring (degrades to headset-only otherwise,
   counted). Best effort above 16 kHz: a 32 KiB ring covers 85 ms per half at 48 kHz stereo, less than the 150 ms
   latency estimate, so there the DAC runs early by the rest.
7. **QEMU bit-exactness** runs inside the existing RTOS self-test (`T_HPCORE`, every `BT=1 RTOS_SELFTEST=1` image)
   instead of a separate `HP_SELFTEST` image and log line. Its 4 KiB scratch comes from the heap: the BT static
   region has only ~33 KB of heap_5 left above `.bt_bss` (floor 32 KiB, `sl6801_firm.ld`); this work leaves it
   304 B larger than before (internal encoder statics gone, ours added).
8. **`a2d_drop` accounting removed** with rev 3 (R-9); `down_sbc` clamps at 10 x fpp.

## Open items for H5 (the session)

- Wire `hp_tx_bd_*` into bt_hp: AUDIO_CFG -> `hp_tx_bd_on_audio_cfg`, a STARTED it asked for ->
  `hp_tx_bd_started`, SUSPEND/CLOSE/link loss -> `hp_tx_bd_stopped(reason)`, delay report, ACL handle for NOCP,
  `HP_TX_EVT_STALL` -> `BTA_DmCloseACL`; teardown with `hp_tx_bd_stop_wait(100, ...)` then `hp_tx_bd_deinit`.
- `aout_bt_attach()` with the ring storage (`audio_dac_borrow_buf` for HEADPHONES, heap for BOTH).
- "START failed for good" must make `aout_is_busy()` false (rev 3 §4.5): not implemented in the sink yet.
- Player H6 hooks: mono pass-through (`aout_set_src_channels(1)`), PA and jack routing with `aout_is_bt()`,
  `aout_stop()` on power-off, UI for the output mode (English strings with `/* TODO(i18n) */`).
