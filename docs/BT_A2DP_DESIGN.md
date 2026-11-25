# BT headphones mode: A2DP Source + AVRCP design (plan step 7)

Status: design **rev 3** (external review disposition in §18), nothing implemented yet. Checked against branch `feat/freertos-bluedroid` at `eb0b307`
(code read only; one throw-away script run in `/tmp/a2r_gen`, §5.1). Implements step 7 of
`docs/BLUETOOTH_BLUEDROID_PLAN.md`: plan §M4 "Headphones", the pairing policy, Exp 4 and step 7 (that file is being
edited concurrently, so this doc cites it by section, not by line). Paths are relative to `open_fw/` unless they start
with `docs/`. `BD/` means `third_party/bluedroid/host/bluedroid/`; `ASRC` means
`BD/btc/profile/std/a2dp/btc_a2dp_source.c`; `XSRC` means `BD/btc/profile/std/a2dp/btc_a2dp_source_ext_codec.c`.

Rev 2 answers the 24-point review (`/tmp/a2dp_design_critique.md`). The biggest change: **the app encodes SBC and
paces the stream itself** through Bluedroid's external-codec source path (`BTC_AV_EXT_CODEC`), instead of patching
the internal encoder's pacing. §16 lists every review point with its outcome.

**Silicon tags.** **[S]** marks an assumption only silicon can confirm. Each one has a numbered experiment in §11.
**[H]** marks something the host harness or a host unit test confirms.

**Invariant for every milestone.** All new code, config and patches build only under `BT=1`. The default image
(`BT=0`) stays byte-identical at the same source revision and toolchain. The gate is a `cmp` of the image bytes, not
a claim about token streams (§3.1).

---

## 1. Goals and non-goals

Goals (v1):
- The recorder streams to one pair of BT headphones using A2DP Source with SBC, 48 kHz preferred, 44.1 kHz fallback.
- The headset's buttons work through AVRCP Target: play/pause, next/previous, FF/REW, and volume where the headset
  sends it.
- The player feeds the headset through the same output-sink API it uses for the DAC. Seek, pause, EOF, scrub and
  replay keep the meaning they have today.
- Pairing: Just Works is accepted only inside a pairing window the user opened. The device reconnects to the last
  bonded headset on its own.
- Reuse the vendored Bluedroid A2DP/AVDTP/AVRCP code and its SBC encoder. Local patches stay small and additive.
- Use the sink's AVDTP delay report for the position estimate when the sink sends one.

Non-goals (v1):
- More than one headset at a time. The ROM refuses page scan `0C1A` while more than one ACL link is up
  (`docs/BT_ROM_HCI_MAP.md:330`).
- Phone mode and headphones mode at the same time. The two stay mutually exclusive.
- Codecs other than SBC. HFP/HSP (no microphone path).
- Recording while streaming. The REC switch suspends the stream (§6.9).

---

## 2. Architecture overview

```
 app task (prio 1)                          BTC task (prio 4, 3 KB stack)                 BTU task
 ┌──────────────────────────┐               ┌──────────────────────────────────┐          ┌──────────────────────┐
 │ player.c                 │               │ hp_tx_tick()  (osi_alarm, 20 ms) │          │ bta_av_data_path     │
 │  decode (src rate, ch)   │               │  credit pacer (clamped)          │          │  readbuf → AVDTP     │
 │  aout_write(p,n,ch) ─────┼─► SPSC ring ─►│  resample, mono→stereo, gain,pad │          │  → L2CAP → HCI       │
 │  aout_flush/queued/busy  │  (g_dac_buf)  │  SBC_Encoder (per-frame bitpool) │          │                      │
 │                          │               │  esp_a2d_source_audio_data_send ─┼─► audio_tx_q (≤ TXQ_PKT_CAP) ──►│
 │ bt_hp_poll() ◄ evt ring ◄┼───────────────┼── a2d_cb / avrc_ct_cb / tg_cb    │          └──────────────────────┘
 └──────────────────────────┘               └──────────────────────────────────┘
```

### 2.1 Why the external-codec path

Verified at `eb0b307`:
- In the internal path all pacing lives in vendored code. `ASRC:1194-1239` uses one constant
  (`MAX_OUTPUT_A2DP_SRC_FRAME_QUEUE_SZ`, 27) both as a per-tick frame cap and as a packet-queue cap (`:1229-1236`,
  `:1517-1535`). Its backlog counter is unbounded and its result is a `UINT8` (`:1196`, `:1222-1227`). Its clock is
  broken on target (`time_now_us`, `:438-449`, falls back to the `gettimeofday` stub, `bt_port/bt_port_fw_stubs.c:37`).
  Encoded audio can only be flushed through the `len == -1` callback, and pad frames cannot be told apart from real
  ones. Fixing that needs three patches to a 1780-line file plus a `bta_av_co.c` patch for the rate fallback.
- With `BTC_AV_EXT_CODEC`, `ASRC` compiles to nothing (`:42`, `:1780`). The source side is the 256-line `XSRC`, which
  only holds a queue. Pacing, queue depth, flush, pad accounting and bitpool become our code, which is pure C and
  testable on virtual time. The 48 → 44.1 kHz fallback comes from config alone (§4.1).

### 2.2 New files (all `BT=1` only)

| File | What it holds | Tested by |
|---|---|---|
| `include/audio_out.h` | Sink API. With `BT=0` a set of macros aliasing `audio_dac_*` (§3.1). | `cmp` of the BT=0 image |
| `src/audio_out.c` | Dispatch between the DAC and A2DP sinks. | `tests/test_player.c` (BT=1 variant) |
| `bt_port/hp_sink.c/.h` | SPSC ring, flush index, resampler, mono→stereo, gain, counters. Pure C. | `tests/test_hp_sink.c` |
| `bt_port/hp_tx.c/.h` | Pacer, SBC wrapper, packetiser, stack-queue accounting, stall watchdog input. Pure C with injected `now_us()`, `send()`, `txq_stat()`. | `tests/test_hp_tx.c` (virtual time) |
| `bt_port/hp_sbc.c` | Thin wrapper over `SBC_Encoder_Init`/`SBC_Encoder`, one `static SBC_ENC_PARAMS`. Built with the Bluedroid flags and prefix header. | `test_hp_tx.c`, QEMU CRC |
| `bt_port/bt_session_core.c/.h` | Shared session core split out of `bt_xrec_bluedroid.c`: shutdown deadline, fault ladder, store, entropy gate (§13 H4). | `test_phone.c`, `test_headphones.c` |
| `bt_port/bt_hp_bluedroid.c/.h` | Headphones session: discovery, pairing policy, A2DP/AVRCP callbacks, reconnect, event ring. | `tests/bt_host/test_headphones.c` (scenario H) |
| `bt_port/bt_hp_fw.c` | Firmware glue: bring-up/teardown job, bond/settings store, CoD. | QEMU gate, silicon |
| `src/ui/` additions | Headphones screens (§9). | wasm build, silicon |

---

## 3. Output-sink abstraction

### 3.1 API and the default image

The player calls exactly 8 `audio_dac_*` functions (`include/openfw.h:266-274`). The fake in
`tests/test_player.c:111-158` stubs the same 8. That set is the contract, with one change: the write call carries the
channel count, so mono can pass through and scrub can keep writing stereo (`player.c:522` writes stereo
`g_mono_tx` whatever the file's channel count).

```c
/* include/audio_out.h */
int      aout_start(uint32_t src_rate, uint8_t vol_0_31);              /* 0 = ok; never blocks (§3.2) */
int      aout_write(const int16_t *pcm, uint32_t n, uint8_t ch);      /* frames accepted, 0 = full (partial ok) */
int      aout_is_busy(void);                                          /* real (non-pad) audio still to be heard */
void     aout_flush(void);                                            /* drop everything not yet heard (best effort) */
uint32_t aout_queued_frames(void);                                    /* in SOURCE-rate frames */
void     aout_set_mute(int muted);
void     aout_set_volume(uint8_t vol_0_31, uint16_t dig_0_511);
void     aout_stop(void);                                             /* end of this file, not end of the session */
int      aout_is_bt(void);                                            /* PA, hp-detect, hp_vol vs bt_vol routing */
```

- **`BT=0`.** `audio_out.h` is macros only: `aout_write(p,n,ch)` → `audio_dac_write_stereo((p),(n))` (the player
  still expands mono before calling it when `!aout_is_bt()`), `#define aout_is_bt() 0`, and so on. Whether the
  object code is identical is not argued from the preprocessor; the H6 gate compares image bytes with `cmp`.
- **`BT=1`.** `src/audio_out.c` dispatches on one `static const aout_ops_t *g_aout`, set to the DAC sink or to the
  A2DP sink while a headphones stream is open. It switches only between files. A link drop in the middle of a file
  auto-pauses instead (§6.7).
- **Mono.** In the A2DP sink the ring keeps the source channel count. The player skips its mono expansion
  (`player.c:992-1002`) only when `aout_is_bt()`. Scrub keeps writing stereo with `ch = 2`; the sink downmixes
  `(L+R)/2` into a mono ring, or the ring is stereo for stereo files. `ch` must equal the ring's channel count or be 2
  (downmix); anything else returns 0 and counts `ch_mismatch` **[H]**.
- **Speaker and jack routing (rev 3, review R-14).** The player calls `audio_pa_set(!hp_in)` unconditionally at file
  open and at ten more sites in `openfw_play_file_ui` (`player.c`), switches `volume` between `hp_vol`/`spk_vol` on
  jack edges, and auto-pauses on unplug. With `aout_is_bt()` every `audio_pa_set` becomes `audio_pa_set(0)` (the
  class-D amp stays off while streaming), jack edges neither change `volume` nor pause, and `volume` is `bt_vol`
  (§6.6). The power-off path in `ui_poll_power_button` (`main.c`) calls `audio_dac_stop()` directly for the player
  screen; under `BT=1` it calls `aout_stop()` and then the session shutdown (§6.7).

### 3.2 A2DP sink semantics (`hp_sink.c` + `hp_tx.c`)

| Call | DAC sink today | A2DP sink |
|---|---|---|
| `start(rate, vol)` | powers the codec, about 50 ms of `mdelay` (`audio_dac.c:198-259`) | **Asynchronous.** Fails with -1 only if no stream is OPEN (works like "rate not supported", `player.c:644-646`). Otherwise resets the ring and the resampler, sets `src_rate → conn_rate`, posts a START request to `bt_hp` and returns 0. The ring accepts data at once. Until the stream is STARTED the ring does not drain, so the wait counts as queued time and the player's position stays right. No blocking wait for `MEDIA_CTRL_ACK` on the app task (critique #21). |
| `write(p, n, ch)` | at most one slot, 0 when full | Copies `min(n, free)` frames into the ring. Never blocks. **Demand start (rev 3, R-5):** if the stream is OPEN or SUSPENDED (idle suspend, remote suspend, or reconnected after a link loss) and no START is outstanding, the first accepted write posts a START to `bt_hp`. The player calls `aout_start()` only once per file (`openfw_play_file_ui`); unpause and scrub only clear mute and write, so without this the stream would never restart. |
| `is_busy()` | DMA active | Real frames remain: `ring_pcm > 0` or `unheard_real_pcm > 0` (§4.4), or less than `lat_ms` has passed since the last real frame was acked. Pad and pause-silence frames never count (critique #9). If a START fails for good (§4.5) the sink reports not busy and posts `HP_EVT_START_FAILED`, so EOF and full-ring waits cannot hang. |
| `queued_frames()` | queued slot bytes | §4.4: ring + real frames not yet heard (+ bounded latency term), in source-rate PCM frames. 0 right after `flush()`. |
| `flush()` | reset slots | Producer-side flush index (§3.3); starts a new flush epoch (§4.4); optional drop of the encoded stack queue (§4.5). |
| `set_mute(m)` | codec mute bits | The tick encodes zeros instead of ring data (ring still drains, so position advances as with the DAC). Unmute counts as demand (see `write`). |
| `set_volume(v)` | Q15 gain plus analog gain | `bt_vol` path (§6.6). |
| `stop()` | powers the codec down | **Rev 3 (R-M7):** flush (ring, new epoch) and drop `audio_tx_q`, so BACK or REC is silent at once instead of draining up to ~0.5 s; the stream stays STARTED and the tick sends silence. Suspend only on idle (§6.5). |

Position math: "heard" is played minus `queued_frames` (`player.c:317-322`), and scrub uses the same idea
(`:361-374`). Both stay correct because every term is in source-rate frames.

### 3.3 Ring, flush and concurrency

- **Storage.** The ring reuses the 16 KiB `g_dac_buf` (`audio_dac.c:46`), idle in A2DP mode, so it costs no RAM;
  main SRAM is nearly full (`sl6801_firm.ld:85`). `audio_dac.c` exports `audio_dac_borrow_buf(&len)` under `BT=1`;
  it fails while the DAC runs.
- **Layout.** Source rate, source channel count: 4096 stereo or 8192 mono frames = 256 ms of 16 kHz stereo, 512 ms
  of 16 kHz mono, about 93 ms of 44.1 kHz stereo. Exp 4d checks whether 93 ms covers SD stalls at priority 1
  **[S]**. Fallback: `HP_RING_FRAMES` from the heap (8 KB more for 44.1 kHz stereo).
- **Concurrency.** One producer (app task), one consumer (BTC tick). Free-running 32-bit `volatile uint32_t wr, rd`
  with `__DMB()` between payload and index stores. No locks.
- **Flush without the race (critique #8).** `aout_flush()` stores `flush_wr = wr` and then bumps `flush_gen`. The
  producer immediately treats `max(rd, flush_wr)` as its read index, so `queued_frames` and free space are right at
  once, and post-seek writes go after `flush_wr`. The consumer, on seeing a new `flush_gen`, sets
  `rd = max(rd, flush_wr)` (it never drops past `flush_wr`, so post-seek data survives) and resets the resampler
  history. `test_hp_sink.c` covers flush-then-write-before-consumer-runs, double flush, and wrap **[H]**, plus a
  pthread stress under TSan.

### 3.4 Rate and channel handling

- **Connection rate.** 48 kHz preferred, 44.1 kHz fallback, from SEP registration alone (§4.1). Our recordings are
  16 kHz (`recorder.c:187,222,253,372`), so 16 → 48 kHz is an exact factor of 3. If the configuration changes later
  (`ESP_A2D_AUDIO_CFG_EVT` again, for example after `esp_a2d_source_set_pref_mcc`), the tick re-runs encoder setup
  and the resampler ratio at the next frame boundary.
- **Resampler** (`hp_sink.c`), spec matched to its gates (critique #18):
  - Stage 1 (H1): linear interpolation, phase step `src/conn` in **32.32 fixed point (`uint64_t`)**: a 48 kHz file
    on a 44.1 kHz link has a step > 1, which a pure Q32 fraction would wrap (rev 3, R-6). Gate: rate accuracy ±0.01 %, DC gain 1 ± 0.001,
    1 kHz tone SNR ≥ 35 dB at 16 → 48 kHz, no click (> −40 dBFS step) at flush boundaries **[H]**.
  - Stage 2: polyphase FIR. Exact 3× case: 3 phases × 24 taps (72-tap prototype, Kaiser, cut-off 0.42 × 16 kHz).
    Other ratios: 64 phases × 16 taps, cut-off 0.40 × min(src, conn), linear interpolation between phases. Gate
    > 60 dB SNR for the 3× case and > 55 dB for 44.1 → 48 / 16 → 44.1 **[H]**. Cost: 24 MAC × 48 k/s ≈ 1.2 M MAC/s
    mono (stereo out is a copy), well under 3 % CPU **[S]** (Exp 4.0).
  - A file at the connection rate takes a copy-only path.
- **Mono SBC.** Not used in v1. Exp 4c knob through `esp_a2d_source_set_pref_mcc` (`BD/api/esp_a2dp_api.c:440-461`;
  `bta_av_co.c:2174-2300`).

### 3.5 SBC parameters, packet rate and credit budget

Frame length, joint stereo, 8 subbands, 16 blocks: `4 + 8 + ceil((8 + 16·bitpool)/8)` = `13 + 2·bitpool` bytes.
128 samples per frame: 375 frames/s at 48 kHz, 344.5 at 44.1 kHz.

| Bitpool | Frame | 44.1 kHz | 48 kHz |
|---|---|---|---|
| 53 | 119 B | 328 kbps | 357 kbps |
| 35 (v1 max) | 83 B | 229 kbps | 249 kbps |
| 28 | 69 B | 190 kbps | 207 kbps |
| 22 (speech candidate) | 57 B | 157 kbps | 171 kbps |

- **Bitpool.** Our SEPs advertise max 35; `bta_av_build_src_cfg` clamps to our caps (`bta_av_co.c:638-645`) and
  `bta_av_co_audio_codec_build_config` clamps to the peer (`:1552-1554`). The negotiated `[min, max]` arrives in
  `ESP_A2D_AUDIO_CFG_EVT`. `SBC_Encoder` reads `s16BitPool` per frame (`sbc_packing.c:91`,
  `sbc_enc_bit_alloc_ste.c:60`), so the tick may pick any bitpool in range per frame without renegotiation
  (critique #17). v1 policy: `max` for music; a lower `hp_speech_bp` (default 24, Exp 4c) when the source is
  ≤ 16 kHz, since there is no content above 8 kHz. The tick changes bitpool only at packet boundaries.
- **fpp is fixed per stream.** `fpp` is computed once, when the tick starts on STARTED, from the frame length at the
  negotiated **max** bitpool, and stays constant until the next STARTED. A lower per-packet bitpool keeps the same
  `fpp` (the packet is just shorter than `audio_mtu`). So every media packet of a STARTED period carries exactly `fpp`
  SBC frames, which the NOCP accounting in §4.4 (`acked_sbc += fpp` per NOCP) and the downstream clamp rely on. A
  reconfiguration needs a suspend, and the counters are rebased on every STARTED (§4.4), so a new `fpp` never
  mixes with old packets.
- **Packet rate (critique #16, corrected).** `audio_mtu = min(peer L2CAP MTU − 12, BTA MTU) − 1` (`bta_av_aact.c:1447`;
  `btc_av.c:53`, `:1134-1135`). At the max bitpool packets are full: `fpp = min(15, audio_mtu / frame_len(max_bp))`
  frames (15 = 4-bit header field, `bta_av_co.c:1458`). At bitpool 35, 48 kHz:

  | Peer MTU | `audio_mtu` | fpp | Packets/s | ACL bytes/packet |
  |---|---|---|---|---|
  | 672 (L2CAP default) | 659 | 7 | 53.6 | 4+12+1+581 = 598 |
  | 895 (common) | 882 | 10 | 37.5 | 847 |
  | 1005+ | 992 | 11 | 34.1 | 930 |

  Every packet fits one HCI ACL fragment (controller data length 1021, `bt_port_prefix.h`). With 4 credits
  (`bt_ctrl_cmds.h:30`) and full packets, NOCP must return on average within 4 / rate = **75-117 ms** of submission,
  not the 138 ms rev 1 used. Exp 4c measures `nocp_max_ms` against this **[S]**.

---

## 4. Transmit path (`hp_tx.c`)

### 4.1 Stream endpoints and codec negotiation (no `bta_av_co.c` patch)

Verified mechanism:
- Local capabilities are per SEP; every SEID starts as the default SBC caps, 44.1 kHz only, max bitpool 53
  (`bta_av_co.c:80-88`, `:350-361`; `btc_av.c:1965-1982`). `esp_a2d_source_register_stream_endpoint`
  (`BD/api/esp_a2dp_api.c:463`) overwrites one SEID, only while the BTC AV state is IDLE (`btc_av.c:2665-2671`), and
  only with source-valid caps: mono and a stereo mode, 8 subbands, loudness (`btc_av.c:2621-2627`).
- As initiator, at the end of `bta_av_co_audio_getconfig` (`bta_av_co.c:1021-1040`),
  `bta_av_co_audio_peer_supports_codec` (`:1772-1814`) builds a config from SEID `cur_seid`'s caps with
  `bta_av_build_src_cfg` (`:566-651` via `:2422-2428`). It starts from `btc_av_sbc_default_config` (`:106-114`) and
  prefers 48 kHz when the default rate is not in our caps (`:590-596`). If no peer sink accepts that exact config
  (`:1600-1622`), it moves to `cur_seid + 1` (`:1802-1811`).
- When we initiate, the local SEP handle is the **first** SRC SEP with codec SBC (`bta_av_get_scb_handle`,
  `bta_av_aact.c:234-245`, used at `:1980`), so SEID 0 must include every rate we might configure.
- A peer-initiated configuration is checked against any registered SEP (`bta_av_co.c:1976-1995`).

Recipe:
- `CONFIG_BT_A2DP_SEP_NUM_MAX=2` (`BD/Kconfig.in:130-135`, depends on the external codec).
- Prefix: `#define BTC_AV_SBC_DEFAULT_SAMP_FREQ A2D_SBC_IE_SAMP_FREQ_48` (already `#if !defined`, `bta_av_co.c:101-103`).
- After `esp_a2d_source_init` and before any connect (state IDLE), register:
  - SEID 0: SBC {44.1 | 48}, all channel modes, blocks 4/8/12/16, subbands 4/8, loudness + SNR, bitpool 2..35;
  - SEID 1: SBC {44.1}, same fields, bitpool 2..35 (explicitly, or it keeps bitpool 53).
  - Each `ESP_A2D_SEP_REG_STATE_EVT` that is not success is an init failure (`fault(CONFIG)`), not a warning.
- Result: SEID 0 yields 48 kHz; a sink without 48 kHz fails the match and SEID 1 yields 44.1 kHz. Every A2DP sink
  must support both rates, all channel modes, all block lengths and both subband counts (mirrored in
  `btc_av.c:2610-2619`), so in practice only the rate fallback and the bitpool clamp matter. Scenario S5 covers
  48-capable, 44.1-only and bitpool-27 peers **[H]**; Exp 4b records each headset's choice **[S]**.
- The negotiated config arrives in `ESP_A2D_AUDIO_CFG_EVT` (`btc_av.c:1197-1204`); peer caps in
  `ESP_A2D_REPORT_SNK_ALL_CODEC_CAPS_EVT` (logged on bench images).

### 4.2 Encoder setup

- `hp_sbc.c` holds one `static SBC_ENC_PARAMS` (about 1.7 KB: `as16PcmBuffer` 512 B, `s32SbBuffer` 1 KB;
  `BD/external/sbc/encoder/include/sbc_encoder.h:160-204`). Never on the BTC stack. It is placed with the Bluedroid
  statics (`.bt_bss`, carved from the RTOS heap region), not in main SRAM `.bss`, which is the tight region (§12.1).
- On each `ESP_A2D_AUDIO_CFG_EVT` (including reconfigure) bt_hp stores the config; the tick applies it at the next
  frame boundary: rate, channel mode, blocks, subbands, allocation, `[min, max]` bitpool, `SBC_Encoder_Init`, then
  overwrite `s16BitPool` (Init derives it from `u16BitRate` and clamps a negative result to 0, `SBC_Encoder_Init` in
  `sbc_encoder.c`; so the bitpool is always written **after** Init, and every per-packet bitpool change is a plain
  field write, never a re-Init).
- `SBC_MAX_NUM_FRAME` is 1 (`sbc_encoder.h:76-77`): one 128-sample frame per call.
- Portable C: `SBC_ARM_ASM_OPT` is FALSE (`sbc_encoder.h:92`), and the only `__asm` (`sbc_dct.h:28-31`) is behind it.
  Host and ARM output should match bit for bit; the QEMU CRC check proves it early (H1) **[H]**.
- Encoder globals (`EncMaxShiftCounter`, `s32LRDiff`, `sbc_encoder.c:32-37`) are fine: in ext mode we are the only user.
- `hp_sbc.c` is compiled with the Bluedroid include set and `-include bt_port_prefix.h`, so the struct layout matches
  (`SBC_ENC_INCLUDED`, `SBC_JOINT_STE_INCLUDED`, `SBC_NO_PCM_CPY_OPTION`). A `_Static_assert` on
  `sizeof(SBC_ENC_PARAMS)` in a Bluedroid-flag unit and in `hp_sbc.c` catches drift.

### 4.3 Pacer spec (runs in BTC context)

- **Timer.** `osi_alarm_new` + `osi_alarm_set_periodic(HP_TICK_MS = 20)` (`common/osi/include/osi/alarm.h:56,71`).
  Bluedroid's alarms sit on the `bt_esp_timer` shim, whose callbacks run in BTC context from a static pool
  (`bt_port/bt_esp_timer.c:1-5`; drained by `btc_task.c:336,346`, patch 0006). This is the same path the internal
  encoder's 30 ms alarm used, so CPU and stack profile in the 3 KB BTC stack are unchanged. Running in BTC also
  serialises the tick with `btc_a2dp_source_state` and `tx_flush` changes (suspend, close), so the checks in
  `esp_a2d_source_audio_data_send` (`esp_a2dp_api.c:620-640`) and `btc_a2dp_source_enqueue_audio_frame`
  (`XSRC:78-98`) cannot race them. The pool has 50 slots (`bt_esp_timer.h:7`); scenario S1 asserts headroom **[H]**.
  Not on the app task: it is starved by SD, exFAT and UI work (critique #15/#21).
- **Lifecycle.** Created once after `esp_a2d_source_init`. Started when bt_hp sees a STARTED it asked for (ack of its
  START, §4.5). Stopped on SUSPEND (local or remote), CLOSE, link loss and shutdown. Freed before
  `esp_a2d_source_deinit`.
- **Stopping without a race (rev 3, R-10).** rev 2 waited for "one delivery generation" (`bt_esp_timer_generation`),
  but that counter (`bt_alarm_generation` in `bt_port/bt_alarm.c`) only advances on set/cancel/free, never on
  callback entry or exit, so the wait would spin until the hard path; `osi_alarm_t` does not expose its
  `esp_timer_handle_t` either. Instead the tick is stopped **in BTC context**, where alarm callbacks are delivered
  (`bt_alarm_drain` from the BTC task): bt_hp sets an atomic `stop_req`; the next tick (or a BTC-context stop called
  from bt_hp's A2DP callback for SUSPEND/CLOSE, which already run in BTC) calls `osi_alarm_cancel`, rebases the
  counters (§4.4), and sets `tick_stopped`. Because cancel and delivery run on the same task, no callback can be
  mid-run after it. The app waits for `tick_stopped` with a 100 ms bound (BTC prio 4 preempts the app), then may
  `osi_alarm_free`; the existing alarm patch (README row 0029: callback re-validated under `alarm_mutex`) covers a
  free from another task. A bound overrun is a teardown fault, not a spin **[H]**.
- **Each tick:**
  All credit quantities are PCM samples per channel at the connection rate.
  1. `now = esp_timer_get_time()`; `el = min(now − last, 2 × HP_TICK_US)` (elapsed clamp, critique #2).
  2. `credit += el × conn_rate`; `credit = min(credit, CREDIT_MAX)` with
     `CREDIT_MAX = max(2 × HP_TICK_MS × conn_rate / 1000, HP_MAX_PKT_PER_TICK × fpp × 128)` samples (credit clamp).
     The second term makes `HP_MAX_PKT_PER_TICK` reachable at every fpp: with the rev 2 draft's `(fpp + 1) × 128`
     term the 40 ms term dominated (1920 samples = 15 SBC frames at 48 kHz), which allows only 1 packet per tick at
     fpp 10/11 and at fpp 7 / 44.1 kHz (1764 samples), leaving 1.6 % headroom in the last case.
  3. `room = TXQ_PKT_CAP − txq_pkts` (patch A accessor, §5.2).
  4. While `credit ≥ fpp × 128` and `room > 0` and at most `HP_MAX_PKT_PER_TICK` (2) packets this tick
     (rev 3, R-11 / missing-mode 1: **check and allocate before touching PCM**, so no holdover buffer is needed):
     - stop if the source is not STARTED or is flushing (the same checks `esp_a2d_source_audio_data_send` makes; the
       tick runs in BTC, so they cannot change between this check and the send);
     - `buf = esp_a2d_audio_buff_alloc(fpp × frame_len)`; on `NULL` (OOM, or the stack disabling) count
       `alloc_fail` and leave: nothing was pulled, the next tick retries;
     - pull `fpp × 128` samples at the connection rate from the ring through the resampler; apply gain;
       duplicate mono to stereo; pad with zeros and add the pad count to `pad_frames` (a stall or empty ring);
       record how many **real** source-rate frames this packet carries (pads and pause silence count 0, §4.4);
     - encode `fpp` frames into `buf`, setting `number_frame = fpp`, `data_len ≤ audio_mtu`, `timestamp = ts`;
     - `esp_a2d_source_audio_data_send(conn_hdl, buf)`: on `ESP_OK`, `ts += 128 × fpp` (RTP units = samples),
       `credit −= fpp × 128`, `sent_sbc += fpp` (SBC frames, §4.4), push the packet record (§4.4); on any error
       (should not happen after the pre-checks) `esp_a2d_audio_buff_free(buf)`, count `tx_lost_sbc += fpp`, keep
       the packet's real frames in `unheard_real_pcm` as "lost unheard" so a later rewind replays them and leave;
       the next tick goes on (an already STARTED stream gets no further STARTED), and refusals lasting `stall_ms`
       raise the stall event (§4.6; live log §19.6). Host tests assert `tx_lost_sbc == 0`.
  5. Remaining credit carries over (bounded by step 2).
- **Result.** At the max bitpool packets are full, about every 27 ms at 48 kHz / bitpool 35 / fpp 10. A 1 s stall
  produces at most `CREDIT_MAX` of catch-up (40-64 ms of audio: 2 ticks, or 2 packets at fpp 7-11), never a burst
  of 1.9× real time. Sustained capacity is `HP_MAX_PKT_PER_TICK × fpp × 128` samples per tick: at 20 ms and 2
  packets that is 1.87× real time in the worst case (fpp 7, 48 kHz) and 2.67× in the common one (fpp 10).
- **Per-tick encode cost.** §12.2 puts SBC at 15-19 % CPU for 375 frames/s, so **0.40-0.51 ms per frame** from SRAM.
  Worst case per tick is 2 packets × 11 frames = 22 frames ≈ **9-11 ms** on BTC, and 1.5-3× that (13-34 ms) if the
  encoder runs from XIP **[S]** (Exp 4.0 measures `cpu_sbc_pct` and the per-tick maximum). If BTC latency hurts AVRCP,
  the fallback is `HP_TICK_MS` 10 with `HP_MAX_PKT_PER_TICK` 1 (≤ 11 frames ≈ 4.4-5.6 ms per tick); lowering the
  packet cap alone at 20 ms is not allowed (see the headroom rule below).
- **Headroom rule.** `HP_MAX_PKT_PER_TICK × fpp × 128 × 1000 / HP_TICK_MS ≥ 1.2 × conn_rate` for the stream's fpp.
  At the L2CAP default MTU fpp is 7, so at 48 kHz a cap of 2 allows `HP_TICK_MS` ≤ 31 and a cap of 1 needs ≤ 15.
  bt_hp checks the rule at STARTED and, if a bench override breaks it, logs it and lowers `HP_TICK_MS` until it holds
  **[H]**.
- **Constants** (bench-overridable, §10.2): `HP_TICK_MS` 20, `TXQ_PKT_CAP` 4 (≈ 107 ms at fpp 10),
  `HP_MAX_PKT_PER_TICK` 2.

### 4.4 Queue accounting, `queued_frames` and `is_busy`

**Units.** Two units appear, and every name says which:
- `*_pcm`: PCM frames (one sample per channel). `ring_pcm` and `unheard_real_pcm` are at the **source** rate. (rev 3:
  the one-packet `hold_pcm` holdover is gone, §4.3.)
- `*_sbc`: SBC frames. One SBC frame is exactly `128` connection-rate PCM frames (8 subbands × 16 blocks).

Cumulative counters (SBC frames, `uint32_t`, wrap-safe differences):
- `sent_sbc` (tick, on `ESP_OK`), `deq_sbc` (patch A: BTU `readbuf`), `flushed_sbc` (patch A: frames freed from
  `audio_tx_q` by `btc_a2dp_source_tx_flush` or by `btc_a2dp_source_ext_txq_drop`). `a2d_drop_sbc` is **dropped in
  rev 3 (R-9):** in `bta_av_data_path` (`bta_av_aact.c`) a buffer taken from a non-empty `a2d_list` is removed
  before the `list_length(a2d_list) < 3` test and prepended back, so with one stream `a2d_list` never holds more than
  one packet and `bta_av_co_audio_drop` is unreachable. The host test asserts `a2d_list` ≤ 1 instead.
- `acked_sbc`: advanced by `fpp` for each NOCP-completed ACL packet on the stream's handle (bt_ctrl hook), but
  **saturating** at `deq_sbc`: it never passes what BTA has handed to L2CAP. Every media packet of a
  STARTED period is exactly `fpp` SBC frames and one ACL fragment (§3.5). Signalling packets on the same handle make it
  slightly early; the saturation bounds that error **[H]**.
- **Rebase (found in the rev 2 re-check).** Several stack paths discard downstream packets without any counter or
  NOCP: `bta_av_str_stopped` frees `a2d_list` without calling `bta_av_co_audio_drop` and, on a flushing stop, calls
  `L2CA_FlushChannel`; `bta_av_do_close`, the suspend-rejected path in `bta_av_suspend_cont` and the reconfigure
  paths in `bta_av_reconfig` and `bta_av_rcfg_cfm` flush L2CAP too (all in `bta_av_aact.c`). Without correction
  `deq − acked` would stay inflated for good, skewing `down_sbc` and holding the §4.6 watchdog
  precondition true. So whenever the tick stops (SUSPEND local or remote, CLOSE, stall, link loss) and again on
  every STARTED, **after the rewind snapshot below has been taken**, the tick sets `acked_sbc = deq_sbc` (everything
  downstream counts as finished: it was played, flushed, or the link is gone). With the saturation above, late NOCPs
  for flushed packets change nothing. `fpp` is also recomputed only at STARTED (§3.5). S7, S8 and S10 check that
  `down_sbc` is 0 after a suspend/resume and after a stall **[H]**.

Transport occupancy (used by the stall watchdog and for stats):
- `stack_sbc = sent_sbc − deq_sbc − flushed_sbc` (exact; in `audio_tx_q`)
- `down_sbc = clamp(deq_sbc − acked_sbc, 0, 10 × fpp)` (BTA `a2d_list` ≤ 1, L2CAP ≤ 5, controller ≤ 4 credits)

**Position (rev 3, R-3; replaces rev 2's `queued_frames` formula).** rev 2 summed every SBC frame in flight plus a
constant `lat_pcm`. That counted pause silence, pads and frames sent **before** the last `aout_flush()` (which no
longer belong to the player's `played_frames`), and kept `queued_frames ≥ lat_pcm` even right after a flush, so
repeated pause/unpause or paused seeks walked the position backwards. rev 3 counts only real post-flush frames:
- The tick keeps a FIFO of packet records `{fpp, real_src_pcm, epoch}` (≤ `TXQ_PKT_CAP` + 10 entries, 4 B each),
  pushed on `ESP_OK`, popped in order as the packet finishes: acked (NOCP), flushed from `audio_tx_q`, or written
  off by the rebase. `aout_flush()` bumps `epoch`; records of older epochs still play and still pop, but no longer
  count.
- `unheard_real_pcm` = sum of `real_src_pcm` over records of the current epoch not yet acked (popped by flush or
  rebase they are **not** heard, so they move into the rewind snapshot below instead of vanishing).
- `lat_term = min(lat_pcm(src), real_src_pcm of current-epoch records acked during the last lat_ms)`: 0 after a
  flush, at most `lat_pcm` while playing, and it decays to 0 during pause silence.
- `queued_frames = ring_pcm + unheard_real_pcm + lat_term`. After `aout_flush()` it is 0, like the DAC's.
- **Rewind snapshot.** On every stop (local or remote SUSPEND, stall, link loss, CLOSE) the BTC-side handler
  computes `rewind_pcm = queued_frames` **before** the stack's queue flush is counted as finished and before the
  rebase, and stores it; until the player's next `aout_flush()`, `queued_frames()` returns that frozen value. On a
  remote SUSPEND the stack has already freed `audio_tx_q` before the event (`btc_av.c`, the SUSPEND path of the
  STARTED state handler); those records were popped as "flushed, unheard", so they stay in the snapshot. The
  player's normal `player_pause_rewind()` (`played_frames − queued_frames`) then lands on the last heard sample
  regardless of `resume_sec`, seeks, scrub or file changes, with no special seek math.
- **Link end inflates NOCP (rev 3, review missing-mode 5).** Before a Disconnection Complete the ROM's link-end path
  frees the link's queued TX buffers and reports them as one NOCP (`docs/BT_ROM_HCI_MAP.md`, "Link-end
  finalisation" row), so up to 4 never-aired packets look acked. On a link loss (and a stall, §4.6) the snapshot
  therefore adds back the real frames of the last `min(4, credits in use at the last NOCP)` acked records: replaying
  up to ~100 ms is preferred to skipping it. The packet-record FIFO keeps acked records for `lat_ms` anyway.
- `lat_ms` = sink delay report / 10 when one arrived (`ESP_A2D_REPORT_SNK_DELAY_VALUE_EVT`, unit 1/10 ms; delivered by
  `bta_av_co.c:1520-1524` → `XSRC:100-111` while the source is ON), else `HP_LAT_MS` 150 **[S]**. `lat_pcm(src) =
  lat_ms × src / 1000`.
- `is_busy`: real audio is still pending while `ring_pcm > 0 || unheard_real_pcm > 0`, and for `lat_ms` after that
  condition first went false. Flushed records are not "unheard" for this purpose, and the rebase clears the FIFO on
  every stop, so it also clears while SUSPENDED. Pad frames never extend it, so EOF settles under congestion
  (critique #9).
- **Scrub.** `scrub_heard()` (`player.c`) takes `min(frames, queued)` per rate step; with `queued_frames` 0 after the
  flush that starts a scrub and only real grains counted afterwards, `submitted ≥ queued` holds **[H]**
  (`test_player` BT=1 variant).

This replaces rev 1's `txq_len × 128` formula, which undercounted about 10× (critique #1).

### 4.5 Flush, suspend and start

- **Flush on seek.** Producer-side flush index (§3.3) and a new epoch (§4.4). Then, if `HP_FLUSH_DROPS_TXQ` (default 1), bt_hp asks the
  tick to call `btc_a2dp_source_ext_txq_drop()` (patch A, BTC context only), which returns the dropped count in SBC
  frames, and to rewind `ts -= 128 × dropped_sbc` (RTP units are samples) so RTP timestamps stay contiguous. What is
  already downstream (≤ 10 packets, usually ≤ 2) still plays (it belongs to an older epoch, so it no longer counts
  in `queued_frames`, §4.4). There is no `len == -1` callback in this path. S8
  asserts post-flush audio reaches the peer within `(TXQ_PKT_CAP + 2) × fpp` SBC frames when the drop is off, and
  within `2 × fpp` when on **[H]**.
- **Local suspend** (`media_ctrl SUSPEND`, idle timer): the stack sets `tx_flush` and frees the queue
  (`btc_av.c:1707`; `XSRC:127-133`); on `BTA_AV_SUSPEND_EVT` it acks and clears `tx_flush` (`btc_av.c:1728-1748`;
  `XSRC:65-76,135-151`). A failed suspend clears `tx_flush` again (`btc_av.c:1744`). bt_hp stops the tick first.
- **Remote SUSPEND** (in-ear detection, power save; critique #12): same stack path, then the BTC state goes back to
  OPENED with `BTC_AV_FLAG_REMOTE_SUSPEND` and `ESP_A2D_AUDIO_STATE_EVT` SUSPEND (`btc_av.c:1750-1770`). bt_hp
  stops the tick and posts `HP_EVT_REMOTE_SUSPEND`; the player auto-pauses with the usual rewind (heard position from
  §4.4). The flag does not block our own later START (`btc_av_cb_stream_ready` checks only `PENDING_STOP`,
  `btc_av.c:2254-2274`).
- **Remote START** (partly rejected critique #12): a sink-initiated START while we are OPENED is **immediately
  suspended by the stack** (`btc_av.c:1455-1461`, "trigger suspend as remote initiated"), so it cannot mean "ready".
  bt_hp logs it and does nothing; the user's or headset's AVRCP PLAY resumes, which issues our own START. Scenario
  S8 asserts both directions **[H]**. **Rev 3 (review missing-mode 4):** the stack still enters STARTED and reports
  `ESP_A2D_AUDIO_STATE_EVT` STARTED, then SUSPEND a few ms later. bt_hp therefore starts the tick only on a STARTED
  it asked for (`start_pending` set), treats an unasked STARTED as "remote START, auto-suspend follows", and treats
  the SUSPEND that follows it as part of that, **not** as `HP_EVT_REMOTE_SUSPEND` (no false pause).
- **Media control is one-at-a-time (rev 3, review missing-mode 3).** `btc_a2dp_control_media_ctrl`
  (`btc_a2dp_control.c`) answers `ESP_A2D_MEDIA_CTRL_ACK_BUSY` while a command is un-acked, and only START,
  SUSPEND and CHECK_SRC_RDY are handled (STOP falls to the default FAILURE branch). bt_hp keeps one outstanding
  command and a one-deep "wanted" state (STARTED or SUSPENDED); a newer want replaces an older queued one, and is
  issued when the ack arrives. bt_hp never uses STOP.
- **START that fails (rev 3, missing-mode 3).** `ESP_A2D_MEDIA_CTRL_ACK_FAILURE`, or a STARTED callback with a
  failure status (the stack stays OPEN), is retried once after 500 ms; a second failure posts
  `HP_EVT_START_FAILED`: the player auto-pauses with the snapshot rewind and shows `Headphones not ready`, and
  `is_busy` stops reporting the ring as pending (§3.2), so neither a full ring nor the EOF wait can hang the player.
- **`ESP_ERR_INVALID_STATE`** from a send while not STARTED (`esp_a2dp_api.c:624`; `btc_av.c:2837-2850`) stops
  consumption until the next STARTED; the ring keeps its data, so nothing is skipped.

### 4.6 Credit-stall watchdog and dead links (critique #3)

On silicon a remote detach was not reported for 20-40 s (`docs/BT_HW_RESULTS.md`: session 4 "Fault" row, session 5
run 2 "Stale link after an idle drop" row, and the ~40 s open question at the end of session 6), and the supervision
timeout is a fixed 20 s (`0C37` stays REJ). Meanwhile the ROM holds all 4 credits.
- **Watchdog.** In STARTED, if `acked_sbc` has not advanced for `HP_STALL_MS` (default 800) while `down_sbc > 0`
  (§4.4; rebased on every STARTED, so a past suspend cannot hold this true), bt_hp stops the tick (taking the §4.4
  rewind snapshot, which counts every unacked real frame as unheard) and posts `HP_EVT_LINK_STALLED`: the player
  auto-pauses and rewinds with its normal `played − queued_frames`. (rev 2 seeked to `acked_sbc × 128 − lat_pcm`,
  which mixes a stream counter with a file offset; withdrawn, R-3.)
- **Disconnect path (rev 3, R-4).** The `0406` goes **through Bluedroid**, never straight to `bt_ctrl`
  (`bt_ctrl_send` has a single producer, hciT, and a direct command would desynchronise `hci_layer.c`'s command
  window). bt_hp calls `BTA_DmCloseACL(bda, FALSE, BT_TRANSPORT_BR_EDR)` (posts to BTU → `btm_remove_acl` →
  `btsnd_hcic_disconnect`). That is an HCI command, so it is not held back by the zero ACL credits, whereas
  `esp_a2d_source_disconnect` would only queue an AVDTP close PDU behind the stuck L2CAP queue. bt_hp then waits in
  STALLED_DISC for Disconnection Complete; it does **not** enter RECONNECTING before it, because neither `bt_ctrl`
  nor L2CAP gives the credits back and `btc_av` keeps its single connection slot until the link is down.
- **Ghost links (rev 3, R-4).** In the ROM's H2 state (`docs/BT_ROM_HCI_MAP.md` §11 H2: LC state 0) a `0406` is
  silently consumed; `bt_ctrl` answers it with `0x22` at `BT_CTRL_FWD_DEADLINE_MS` (7 s) and no Disconnection Complete
  ever comes, so the 4 credits stay held. So if no Disconnection Complete has arrived `HP_STALL_DISC_MS` (8000, just
  past the forwarding deadline) after the `0406`, or the `0406` failed, bt_hp escalates to the existing fault/recovery
  ladder (session-core restart: controller stop/start, the same path phone mode uses for a refused `0C1A`), then
  RECONNECTING. The user sees `Reconnecting…` for a few seconds; nothing plays into the void meanwhile because the
  player is already paused.
- **Tunable.** `HP_STALL_MS` must exceed the longest NOCP gap of a healthy link; Exp 4c records `nocp_max_ms` and the
  value is set to `max(800, 3 × nocp_max_ms)` **[S]**.
- **Test.** Fake-ROM variants in scenario S10: NOCP stops, no detach event, `0406` answered (normal) and `0406`
  silent (H2 ghost → escalation) **[H]**.

---

## 5. Vendored-stack changes

### 5.1 Build configuration (no source changes)

- `bt_port/sdkconfig.defaults`: `CONFIG_BT_A2DP_USE_EXTERNAL_CODEC=y`, `CONFIG_BT_A2DP_SEP_NUM_MAX=2`; regenerate
  `bt_port/sdkconfig.h` with `tools/gen_bluedroid_config.py` (needs `kconfiglib` and an ESP-IDF checkout at the
  vendored SHA via `IDF_PATH`, `third_party/bluedroid/PORTING_NOTES.md`; an H0 prerequisite). The switch maps through
  `BD/common/include/common/bluedroid_user_config.h:48-52` to `BTC_AV_EXT_CODEC`/`BTA_AV_EXT_CODEC`
  (`bt_target.h:94-96`). Today it is unset (`bt_port/sdkconfig.generated.old:71`). The diff of the regenerated
  `sdkconfig.h` must show only these two symbols **[H]**.
- `bt_port/bt_port_prefix.h`: `#define SBC_ENC_INCLUDED TRUE` (in ext mode `bt_target.h:97-102` no longer defines it
  and it falls back to FALSE at `:826-827`, which compiles every encoder file empty, `sbc_encoder.c:30`) and
  `#define BTC_AV_SBC_DEFAULT_SAMP_FREQ A2D_SBC_IE_SAMP_FREQ_48`. Both are `#ifndef`/`#if !defined` upstream.
- **`tools/gen_bluedroid_srcs.py` needs a keep-list (new finding, not in the critique).** The vendored
  `CMakeLists.txt:442-470` adds the SBC encoder sources only when `CONFIG_BT_A2DP_ENABLE AND NOT
  CONFIG_BT_A2DP_USE_EXTERNAL_CODEC`. Regenerating `bt/bluedroid_srcs.mk` in a scratch copy (`/tmp/a2r_gen`) with
  the two symbols added dropped all 8 `external/sbc/encoder/srce/*.c` files (today `bt/bluedroid_srcs.mk:191-198`).
  Fix: a `KEEP` list in the generator that always emits `external/sbc/encoder/srce/*.c`; the encoder include dir is
  already found by `find` (`bt/Makefile.bluedroid:28`, `tests/bt_host/Makefile:50`). `btc_a2dp_source.c` and
  `btc_a2dp_source_ext_codec.c` are both listed already (`:158-159`), and the sink ext file stays excluded by
  `btc_a2dp_sink` (`tools/gen_bluedroid_srcs.py:22-23`).
- `BTC_MEDIA_AA_BUF_SIZE` (patch 0002) no longer matters: the app sizes each buffer.
- The legacy PCM callback API disappears (`BTC_AV_SRC_API_REG_DATA_CB_EVT` only exists with `EXT_CODEC == FALSE`,
  `btc_av.c:3209-3214`); nothing in open_fw uses it.

### 5.2 Patches (letters here; numbers are assigned when they land, next free today is **0034**)

Each follows `third_party/bluedroid_patches/README.md`: `#if OPENFW_BLUEDROID`, one commit plus one `.patch`, README row.
rev 2 called these 0029-0032; those numbers were taken in the meantime by the r1 review fixes (README rows 0029-0033,
"Next free number: 0034"), so rev 3 uses letters (R-13).

| Patch | File | Change | Risk |
|---|---|---|---|
| A | `XSRC` | `MAX_OUTPUT_A2DP_SRC_FRAME_QUEUE_SZ` becomes `#ifndef` (`XSRC:29`; prefix sets 3, so `length > 3` admits 4 = `TXQ_PKT_CAP`, `:90`). Adds `btc_a2dp_source_ext_txq_stat(uint32_t *sent_pkts, uint32_t *deq_sbc, uint32_t *flushed_sbc)` from atomic counters updated in `btc_a2dp_source_enqueue_audio_frame` (`:95`), `btc_a2dp_source_audio_readbuf` (`:124`) and `btc_a2dp_source_tx_flush` (`:60-62`); frame counts come from each buffer's `number_frame`. Adds `btc_a2dp_source_ext_txq_drop()` (BTC context only) returning the dropped SBC frame count. rev 3: no `bta_av_co.c` hook any more (the `a2d_list` drop is unreachable, §4.4). Stop-time drops in `bta_av_str_stopped` and L2CAP flushes are not counted; the tick's rebase covers them (§4.4). Host-tested in H0 by `tests/bt_host/test_xsrc_stats.c` (§13). | low |
| B | `BD/stack/btm/btm_sec.c` | Connection-request hook at the top of `btm_sec_conn_req` (`:2725`): `openfw_conn_req_allowed(bda, dc)`; if it returns 0, `btsnd_hcic_reject_conn(bda, HCI_ERR_HOST_REJECT_DEVICE)`. A weak default allows everything. Used by both modes (§6.3). | medium (touches connection admission) |
| C | `BD/btc/profile/std/gap/btc_gap_bt.c` | In the INQ_RES path (`:325-328`), after the EIR check, call `openfw_inq_name_wanted(cod)`; if 0, set `remt_name_not_required`. Mode 0 inquiry has no EIR, so today BTA pages every result for a name. | low |
| D | `BD/btc/core/btc_dm.c` | **Required in v1 (rev 3, R-8).** `btc_dm_sp_cfm_req_evt` confirms Just Works itself (`BTA_DmConfirm(..., true)`) without any app event. A hook `openfw_just_works_allowed(bda)` (weak default: allow, so phone mode is unchanged) is consulted first; 0 → `BTA_DmConfirm(..., false)`. Headphones mode returns 1 only for the picked address inside the window and only when `ssp_block()` is clear, and arms the same `cfm_ok` slot phone mode's `on_cfm_req` arms, so a failed or vanished pairing calls `ssp_burn()`. | medium (pairing) |
| E | `BD/bta/av/bta_av_aact.c` (and/or `btc_av.c`) | **New in rev 3 (R-1).** Teardown with a frozen peer must still reach `clean_up()` in `btc_av.c`. While BTA AV is disabling, `bta_av_do_close` waits up to `BTA_AV_CLOSE_REQ_TIME_VAL` (4 s) for the peer's AVDTP close; patch 0026 detaches the ACL and `esp_bluedroid_deinit` stops `btu_thread` well before that, so `BTA_AV_DISABLE_EVT` never runs and the `btc_av` statics (`g_a2dp_on_init`, `g_a2dp_source_ongoing_deinit`, `btc_av_common.disabling`, the source state) stay set for the rest of the boot (`.bt_bss` is zeroed only in `startup.c`). Fix shape, chosen in H5 after S1 is shown red: when `bta_av_cb.disabling`, close the stream locally without waiting (no `AVDT_CloseReq` timer), so `BTA_AV_DEREG_COMP`/`DISABLE_EVT` follow in milliseconds. | medium (teardown) |

Dropped from rev 1: 0029 (clock), 0030 (internal queue cap), 0031 (`bta_av_co.c` caps and bitpool). None of them is
needed in the ext path. `PORTING_NOTES.md:64` ("≤ 5 × 1280 B") is still wrong about patch 0002 and gets corrected in H0.

### 5.3 Link roots

`bt/bt_link_roots.c:36` already roots `esp_a2d_source_audio_data_send`, which is the right call for this path
(rev 1 wanted to replace it; reversed). Add: `esp_a2d_audio_buff_alloc`, `esp_a2d_audio_buff_free`,
`esp_a2d_source_register_stream_endpoint`, `esp_a2d_source_set_pref_mcc`, `esp_avrc_tg_set_psth_cmd_filter`,
`esp_avrc_ct_send_get_rn_capabilities_cmd`,
`esp_avrc_ct_send_register_notification_cmd`, `esp_avrc_ct_send_set_absolute_volume_cmd`,
`esp_bt_gap_cancel_discovery`, `esp_bt_gap_set_cod`, `esp_bt_gap_read_remote_name` (all declared in
`BD/api/include/api/`). `tools/check_bt_link_roots.sh` checks each. rev 3 drops `esp_avrc_tg_set_rn_evt_cap` and
`esp_avrc_tg_send_rn_rsp` (no TG notifications in v1, §6.6; the latter is also `deprecated`, which breaks the
zero-warning build).

`SBC_Encoder_Init`/`SBC_Encoder` are not roots: they are internal, not ESP API, and `bt_link_roots.c` only includes
the public headers. Until `hp_sbc.c` calls them (H1), `--gc-sections` (`Makefile`, firmware link rule) drops them
from the image, so H0 checks the encoder in `libbluedroid.a` instead and H1 checks it in the ELF (§13).
**Rev 3 (review gate-1):** `tools/check_bt_link_roots.sh`, run by the `BT=1` link rule, has a hard-coded list of
Bluedroid internals that must be in the ELF, and it includes `SBC_Encoder_Init` (reached today through the internal
codec's `btc_a2dp_source.c`). H0 therefore removes `SBC_Encoder_Init` from that list (`btc_a2dp_source_startup` stays:
`XSRC` defines it too), and H1 puts it back once `hp_sbc.c` calls it; otherwise H0's own `make BT=1` fails.

### 5.4 Init order

`esp_bluedroid_enable` → `esp_avrc_ct_init` + callback → `esp_avrc_tg_init` (filter: PLAY, PAUSE, STOP, FORWARD,
BACKWARD, FAST_FORWARD, REWIND, VOL_UP, VOL_DOWN; **no RN caps**, rev 3: the vendored TG allows only
`VOLUME_CHANGE` notifications, §6.6) →
`esp_a2d_register_callback` → `esp_a2d_source_init` → wait for `ESP_A2D_PROF_STATE_EVT` init →
`esp_a2d_source_register_stream_endpoint` ×2 (§4.1) → create the tick alarm. AVRC init stays before
`esp_a2d_source_init`.

### 5.5 Teardown within the 2 s deadline (critique #14)

`bt_shutdown` (`bt_port/bt_xrec_bluedroid.c`) takes the hard path (fault record, reboot) after
`BT_XREC_SHUTDOWN_DEADLINE_MS` = 2000 (`bt_xrec_bluedroid.h`; the deadline check is in the teardown wait loop of
`bt_shutdown`, raising `BT_XREC_FAULT_DOWN_DEADLINE`). A graceful AVDTP/AVCTP close
waits on a peer that may not answer. So in headphones mode:
1. Stop the tick in BTC context and wait for `tick_stopped` (§4.3), then free the alarm.
2. `esp_avrc_tg_deinit` / `esp_avrc_ct_deinit` **first** (rev 3, R-1: AVRC must deinit before A2DP,
   `esp_avrc_api.h` and the "should deinit in advance of A2DP" checks in `btc_avrc.c`).
3. **No** `esp_a2d_source_disconnect`. Call `esp_a2d_source_deinit` and wait up to 500 ms for
   `ESP_A2D_PROF_STATE_EVT` (`ESP_A2D_DEINIT_SUCCESS`, sent by `clean_up()` in `btc_av.c`). With patch E this arrives
   even with a frozen peer. If it still times out, continue, but record `hp_av_deinit_timeout` and mark headphones mode
   unavailable until reboot (a later `esp_a2d_source_init` would fail with `ESP_ERR_INVALID_STATE`); the session
   offers a reboot instead of a silent failure.
4. `esp_bluedroid_disable`, whose patch-0026 path detaches links in about 0.63 s.
From milestone H4 on this runs in the shared session core, which owns the deadline and the fault ladder for both
modes. Before that, the H3 bench job runs the same steps directly under its own 2 s timer and logs
`teardown_ms` (§13). Scenario steps S1 and S13 run with a frozen peer and the stream STARTED and must finish under
2 s with no hard path, see `ESP_A2D_DEINIT_SUCCESS`, and then complete a **second** init/enable/`a2d_source_init`
cycle (the check that catches stuck `btc_av` statics) **[H]**.

---

## 6. Session layer `bt_hp`

Structure as in `bt_xrec_bluedroid.c`: BTC callbacks copy events into a locked event ring; the app task drains it
with `bt_hp_poll(now_ms)`; decisions are made on the app task. (rev 2 answered some AVRCP replies in the BTC
callback; rev 3 has none left to answer, §6.6. The tick start/stop and the §4.4 snapshot do run in BTC.)

### 6.1 State machine

```
OFF ─start─► IDLE (connectable to bonded only) ─auto─► RECONNECTING ─ok─► CONNECTED ─open─► OPEN ─start─► STARTED
 ▲            │  ▲                                  │fail(backoff)        │                   ▲    │suspend / remote
 │            │  └──────────────────────────────────┘                     │                   └────┤ suspend / stall
 │            └─user "Pair"─► DISCOVERING ─done─► NAMING ─pick─► CONNECTING(pair window) ─────┘    ▼
 └──stop (bt_shutdown)                                                               SUSPENDED ◄─┘
```

- `bt_hp_stream_ready()` is true in OPEN, STARTED and SUSPENDED. `aout_start` from SUSPENDED or OPEN posts a START
  and returns at once (§3.2); so does the first write or unmute after a pause (demand start, §3.2).
- Link loss from CONNECTED on posts `HP_EVT_LINK_LOST`; a credit stall posts `HP_EVT_LINK_STALLED` and passes through
  STALLED_DISC before RECONNECTING (§4.6). The player treats both like a headphone unplug (the unplug branch of
  `openfw_play_file_ui`), rewinding with the §4.4 snapshot.

### 6.2 Discovery and naming

- `esp_bt_gap_start_discovery(INQ_MODE_GENERAL, 8 /* 10.24 s */, 0)`. Inquiry mode 0: no RSSI, no EIR.
- **CoD filter** in patch C's hook: major Audio/Video (0x04) with Rendering (bit 18) or Audio (bit 21), or a minor
  class in {wearable headset, hands-free, headphones, loudspeaker, portable audio, car audio}. Others get
  `remt_name_not_required`, so BTA does not page them for a name; without the patch each out-of-range device would
  cost a full page timeout (`btc_gap_bt.c:325-328` sets the flag only from EIR) (critique #11).
- **Names.** BTA's own name pass runs after inquiry complete, one at a time. A name request during an inquiry is
  SAVED by the ROM (`docs/BT_ROM_HCI_MAP.md:80`). Scenario S2 asserts the RNR count equals the number of audio devices and that
  none is sent before inquiry complete **[H]**.
- At most 8 entries, names truncated to 24 bytes for display.
- **Before connecting** the session cancels discovery and waits for `ESP_BT_GAP_DISC_STATE_CHANGED_EVT(STOPPED)` and
  for any name request in flight. **Rev 3 (R-13b):** STOPPED does not prove the latter: during BTA's name pass
  `bta_dm_search_cancel` (`bta_dm_act.c`) calls `BTM_CancelRemoteDeviceName` (`041A`) and posts
  `BTA_DM_SEARCH_CMPL_EVT` at once, before the `0x07` Remote Name Request Complete. So `bt_ctrl` exports
  `bt_ctrl_rnr_pending()` (true from a forwarded `0419`'s CS 0 until its `0x07` or a `041A` CC; both pass through
  bt_ctrl, which would add this one flag for it; H2 work), and bt_hp issues the connect only when it is false, with a 1 s bound after which it
  connects anyway and logs `hp_rnr_wait_to`. S3 scripts a pick during the name pass **[H]**. Bluedroid does not queue a page behind an inquiry
  (`BD/stack/btm/btm_acl.c:2758-2782`). During an inquiry the ROM answers `0405` with `0x0C` at once (LM state 4,
  `docs/BT_ROM_HCI_MAP.md:82`), not SAVED as rev 1 said; it is SAVED in LM states 1-2.
- **Page scan off around `0401`/`0405`** (critique #6). What LM states 1 and 2 are is not documented; if page or
  inquiry scan maps to one of them, every `0405` would be SAVED and fail after 7 s. bt_hp turns scan off (`0C1A` =
  0x00, which the ROM always accepts) before `0401` or `0405` and restores the IDLE value after Connection Complete
  or inquiry complete. As in phone mode this goes through `esp_bt_gap_set_scan_mode` (BTA writes `0C1A` twice,
  `bta_dm_set_visibility`), never as a raw command to `bt_ctrl` (rev 3, R-4). Exp 4a/4b run both variants (`hp_pscan=0/1`) to settle it **[S]**.

### 6.3 Pairing policy and connection admission

- Headsets are NoInputNoOutput, so pairing is always Just Works. Local IO capability stays DisplayYesNo.
- **The window.** "Pair new headphones" opens a 120 s window; the user picks one device. Only that address may bond,
  only while the window is open.
- **Enforcement (rev 3, R-8).** Up front, through patch D: Bluedroid auto-confirms Just Works in
  `btc_dm_sp_cfm_req_evt` without any app event, so phone mode's `on_cfm_req` (and with it `ssp_block()` and the
  `cfm_ok` → `ssp_burn()` path) never runs for a headset. Without D, a starved entropy pool or an already-burned
  P-192 key would not stop a pairing, and a failed one would never burn the key. With D, the Just Works confirm is
  accepted only for the picked address, inside the window, with `ssp_block()` clear, and `cfm_ok` is armed so the
  existing failure handling burns the key. bt_hp also checks `ssp_block()` before it initiates a connect to an
  unbonded address. The post-hoc check stays as a second line: on `ESP_BT_GAP_AUTH_CMPL_EVT`, a bond from another
  address or outside the window is removed (`esp_bt_gap_remove_bond_device`) and the link dropped (the pattern of
  `reject_bond` in `bt_xrec_bluedroid.c`). The admission hook below keeps unknown devices from getting that far.
- **Connection-request allow-list (patch B, critique #10).** Today nothing filters incoming ACLs:
  `BTM_ALLOW_CONN_IF_NONDISCOVER` is TRUE (`bt_target.h:1300`), the ESP API passes `BTA_DM_IGNORE`
  (`btc_gap_bt.c:91`), and `connect_only_paired` needs `BTM_SEC_LINK_KEY_AUTHED` (`btm_sec.c:2738-2743`), which a
  Just Works key does not set, so it would refuse our own headset. The hook `openfw_conn_req_allowed(bda, cod)`
  consults a per-mode table:
  - headphones mode: the bonded headsets in `bt_hp_bonds.bin`, plus the picked address while the window is open;
  - phone mode: everything **except** addresses in `bt_hp_bonds.bin` (read once at mode start), so a bonded headset
    paging its last source cannot take the one link the `0C1A` rule allows. That file is a `bt_store` image (`BTS1`
    container holding Bluedroid's config INI), not an address table, so phone mode reads it with a small pure parser
    for the bonded-device section names (host-tested), **without** loading it into `bt_store`.
  Scenario steps S11 and S12 cover the headset paging us in each mode **[H]**.
- **Entropy gate, key-reuse protection, legacy PIN refusal:** the phone-mode functions (in `bt_xrec_bluedroid.c`:
  `ssp_block`, which consults `bt_xrec_plat_pairing_block`, and `ssp_burn`; the `EV_PIN_REQ` case of
  `on_gap_event`), reached for Just Works only through patch D (above). Exp 4 records any brand that needs a PIN **[S]**.
- **Discoverability.** Inquiry scan stays off. Page scan is on in IDLE and RECONNECTING (except around our own
  `0401`/`0405`, §6.2), only while at most one link exists, under the existing `0C1A` retry and fail-closed logic.

### 6.4 Bonds and the store

Phone mode deletes every non-MITM bond when it starts (`purge_unauthenticated_bonds` in `bt_xrec_bluedroid.c`,
called from `on_spp_event` on `EV_SPP_INIT`). Headset bonds are exactly that kind. So each mode has its own file: `bt_bonds.bin`
(`bt_xrec_fw.c:54`) and `bt_hp_bonds.bin`, each loaded only by its mode. The purge cannot see headset bonds,
`link_authorized` cannot authorize a headset for XREC, and the 4-bond / 24-entry limits apply per mode. Headphones
mode keeps at most 2 bonds (LRU). The same file holds the `bt_hp` keys: last-used address and `bt_vol` (§6.6).

**Store reset between modes (rev 3, R-7).** `bt_store` is one static RAM image for the whole boot, and `bonds_load`
(`bt_xrec_fw.c`) returns without touching it when the bond file is missing, empty or too big. So entering headphones
mode after phone mode before `bt_hp_bonds.bin` exists would start Bluedroid with the phone's bonds and later flush
them into `bt_hp_bonds.bin`; the phone-mode deny list would then reject the user's own phone. Every mode start
therefore resets `bt_store` (new `bt_store_reset()`, entries and data cleared, not dirty) **before** loading its own
file, and the flush callback is re-registered per mode (`bt_port_init` already replaces `s_store_flush` when called
again with a non-NULL callback, so only the reset is new). S12 runs phone → headphones (no file) → phone **[H]**.

CoD: phone mode `0x200404`; headphones mode `0x28041C` (Audio + Capturing, major AV, minor portable audio). Exp 4a
checks no headset refuses a source with this CoD **[S]**.

### 6.5 Reconnect, suspend and idle

- **Entering headphones mode with a bond.** RECONNECTING calls `esp_a2d_source_connect(last)` with backoff 0, 3, 10,
  30, 60 s, then connectable only. The 30/60 s steps straddle the ~40 s ghost link (`docs/BT_HW_RESULTS.md`, session 5
  "Stale link after an idle drop" row and the open question at the end of session 6).
  `0x0B` or `0x04` moves to the next step and logs `HP ghost?`. Exp 4g measures it **[S]**.
- **A headset that pages us.** Admitted by the allow-list; AVDTP collision handled by Bluedroid. We are then the
  piconet slave, and with role switch masked (§7) we stay slave; Exp 4g logs `linkinfo` for this case and checks
  the stream rate holds **[S]**.
- **Pause and idle.** While paused the tick sends silence. After `HP_IDLE_SUSPEND_S` (20 s) bt_hp suspends; after
  `HP_IDLE_DISCONNECT_S` (10 min) it disconnects. Play resumes through the demand start of §3.2 (the player itself
  never calls `aout_start` on unpause); after an idle disconnect the first write finds no OPEN stream, so the sink
  posts a reconnect and the player auto-pauses with `Reconnecting…` until OPEN.
- **Remote suspend** auto-pauses (§4.5); the stream stays SUSPENDED until PLAY.
- Player exit and per-file `aout_stop` never suspend; the session owns the stream.

### 6.6 AVRCP mapping and volume (critique #15)

The TG receives passthrough commands in BTC. bt_hp posts them to its ring; `bt_hp_poll` turns them into
`player_action_t` (`include/openfw_player.h:36-48`), merged where the player decodes keys (`player.c:719-746`).

| AVRCP (headset → us) | Player action |
|---|---|
| PLAY, PAUSE (press edge) | `PLAYER_ACT_PLAY_PAUSE` only if it changes the state (PLAY while paused or at EOF, PAUSE while playing); otherwise ignored. The player has only the toggle (`include/openfw_player.h`). |
| STOP | as PAUSE |
| FORWARD / BACKWARD (**press edge only**) | next / previous file, or ±seek per `skip_mode`. Repeated presses from a held key are ignored until a release. |
| FAST_FORWARD / REWIND press … release | scrub start / end via a virtual "held" bit. A held key arrives as repeated presses: each one re-arms the 2 s scrub watchdog. |
| VOL_UP / VOL_DOWN | `bt_vol` ± 4 (only when absolute volume is off) |

- **Merging into the player (rev 3, R-15).** AVRCP actions enter `openfw_play_file_ui` after the physical-key
  decode and **bypass the screen-off gate** (with the OLED off, physical keys are flushed; a headset button in a
  pocket must still work). AVRCP scrub is not a physical hold: the scrub-end test (`!keys.hold_active` or
  `keys_state(NULL) != keys.held`) would end it on the next loop pass, so a `scrub_src` field selects the end test:
  physical keys as today, or the virtual held bit plus its 2 s watchdog for AVRCP.
- **Outside the player** (home, browser): PLAY runs the home screen's Resume action (`play_named_file` on the last
  file, as ENTER does with `home_focus` 0), whatever the current focus (the default focus is Files); with no last
  file it shows the "nothing played" toast. Other keys are ignored and counted. This avoids surprise playback from a
  stray FORWARD.
- **No TG notifications in v1 (rev 3, R-2).** rev 2 registered `PLAY_STATUS_CHANGE`/`TRACK_CHANGE` and answered them
  from the BTC callback. The vendored TG cannot: `cs_rn_allowed_evt` in `btc_avrc.c` is `VOLUME_CHANGE` only, so
  `esp_avrc_tg_set_rn_evt_cap` rejects the others; `btc_avrc_tg_send_rn_rsp` handles only `VOLUME_CHANGE`
  ("todo: implement other event notifications") and drops the rest; and `btc_rc_upstreams_evt` rejects
  `GET_PLAY_STATUS`/`GET_ELEMENT_ATTR` with `AVRC_STS_BAD_CMD` although `bta_av_cfg.c` advertises TG category 1.
  Headphones need none of this for passthrough keys and absolute volume, so v1 drops it rather than patch
  `btc_avrc.c`. Exp 4e notes any headset that misbehaves without play-status notifications **[S]**. There is no
  "absolute volume reply" to send either: as source we are the volume **CT** (we send `SetAbsoluteVolume`); a
  `SetAbsoluteVolume` arriving at our TG is answered by `btc_rc_upstreams_evt` itself before the app callback.
- **Absolute volume.** On CONNECTED the CT asks for capabilities. If `EVT_VOLUME_CHANGE` is supported: register,
  send `SetAbsoluteVolume(bt_vol)`, and take the value from its response. **Re-register after every CHANGED**
  (notifications are one-shot). The Q15 gain stays at unity.
- **Without absolute volume**, the device's keys scale PCM in the tick with the 32-entry Q15 table
  (`pcm_gain_q15` in `audio_dac.c`), indexed by `round(bt_vol × 31 / 127)`.
- **Scale.** `bt_vol` is 0-127 (AVRCP). `aout_set_volume`'s `vol_0_31` argument is that same `round(bt_vol × 31 /
  127)` for the A2DP sink; the player's 0-31 VOL actions map back by `bt_vol = round(v × 127 / 31)`. The overlay goes
  through the existing `format_vol_label` (`src/ui/ui_screens.c`), which shows 0-31 as `0`-`10` with one decimal,
  with prefix `BT` instead of `HP`/`SPK`; device keys step `bt_vol` by 4.
- **Persistence (critique #20).** `bt_vol` lives in the `bt_hp` store file, not in `ui_settings_t`.
  `ui_persist_load` requires an exact size and version 1 (`src/persist.c:61-63,73-75`), so appending a field would
  reset every setting when the user moves between a `BT=0` and a `BT=1` image. The settings layout stays unchanged.
- Volume is tested separately from transport keys: scenario S9 and Exp 4e.

### 6.7 Player and main-loop integration

- `bt_hp_poll()` runs from the home loop and the player loop, behind a `BT=1` hook macro next to `OPENFW_TASK_WAIT`
  (`player.c:1081-1085`); empty with `BT=0`.
- `HP_EVT_LINK_LOST`, `_LINK_STALLED`, `_REMOTE_SUSPEND` and `_START_FAILED` auto-pause. No automatic fallback to the
  DAC (the speaker could play in public); the player shows "Headphones disconnected".
- **Every exit path stops the session (rev 3, review missing-mode 2).** Headphones mode runs in the background
  across screens, so the session shutdown must run on: power-off (`ui_poll_power_button` handles only the USB, BT
  and player screens today), entering USB mass storage (`run_usb_msc_mode`: otherwise `bt_hp` could flush
  `bt_hp_bonds.bin` while the host writes raw sectors), and entering phone mode (`run_bt_xrec_mode`: one `bt_ctrl`
  owner). OpenFW has no card-removal detection, so rev 2's "card removal" item is dropped; an SD write error during
  a store flush is logged and retried at the next flush.
- **Start failure toast.** If `aout_start` returns -1 (headphones On but no stream OPEN), the player shows
  `Headphones not connected` before returning; `main.c` already keeps `resume_sec`, but it clears `unplayed`, so
  under `BT=1` that clear is skipped for a start failure.

### 6.8 Scrub

Scrub writes stereo grains through `aout_write(…, 2)` (§3.1). In the A2DP sink the grains go through the same ring
and pacing; the scrub preview lags by `queued_frames`, which the scrub position math already subtracts.

### 6.9 Recording

The REC switch exits the player (`player.c:707-710`). In headphones mode it first suspends the stream, keeps the link
and runs the recorder. Rev 3: the suspend sits in the main loop's record-switch branch itself (step 2, "Record
slide switch", of the home loop in `main.c`), so REC from Home or the browser, where the session may still be
sending silence, suspends too. Exp 4h checks for RF noise in the microphone path **[S]**; if present, REC disconnects.

---

## 7. bt_ctrl and ROM forwarding

Opcodes are refused, or forwarded only through `BT_CTRL_EXP1_PASSED_LIST` (empty by default,
`bt_port/bt_ctrl_cmds.h:49-55`, table `bt_ctrl_cmds.c:10-95`). Each row flips to forwarded only after its silicon
experiment passes; host and bench images may forward through the gate list.

| Opcode | Needed for | Change | ROM behaviour (`docs/BT_ROM_HCI_MAP.md` §2) | Risk |
|---|---|---|---|---|
| `0401` Inquiry | discovery | gated → FWD | LM state 0 only; 1: SAVED; 2: SAVED or `0x3A`; ≥ 3: `0x0C` (`:78`) | Low-medium **[S]** |
| `0402` Inquiry Cancel | stop before paging | zero-length layout (`bt_ctrl_cmds.c:116-119`), then gate. Rev 3: `bt_ctrl_cmds_init` counts a FWD row whose `bt_ctrl_cmds_fwd_min_len()` is 0 as a validation error, so H2 changes the check to "has a `k_fwd_layout` entry" (allowing `min_len` 0) before any gate list contains `0402` | Deferred CC, no inquiry-complete (`:79`) | Medium: CC must beat the 7 s deadline **[S]** |
| `0405` Create Connection | connect/reconnect | gated → FWD | LM 1/2: SAVED; ≥ 3 (incl. inquiry = 4): `0x0C`; link exists: `0x0B` (`:82`) | Medium-high: first time as master **[S]** |
| `0408` Create Connection Cancel | user cancel/timeout | gated → FWD | CC, then Connection Complete `0x02` (`:83`) | Medium: race with completion **[H][S]** |
| `0419`/`041A` | names | already FWD | SAVED while LM state ≠ 0 | session-side (§6.2) |
| `040F` Change Packet Type | sent on **every** ACL setup (`btm_establish_continue` → `btm_set_packet_types`, `btm_acl.c`) and re-applied after every role change (`BD/stack/btm/btm_acl.c:1680-1683`); `0405` itself asks for DM/DH 1-3-5 (`l2cu_create_conn_after_switch`, `l2c_utils.c`). Phone mode already runs with it refused, so a refused `040F` is today's state, not a new one (rev 3) | stays REJ unless Exp 4b passes with Bluedroid's exact set | Requested set must equal the ROM's allowed set, else `0x11` (`:84`) | **High (critique #5):** a role switch with `040F` rejected may leave 1-slot packets, which cannot carry 229-249 kbps. Mitigation in §11 Exp 4b. |
| `0C28` Auto Flush Timeout | — | stays REJ | — | Low: the AV flush timeout is 0 (`bta_av_cfg.c:56`), which `L2CA_SetFlushTimeout` maps to "no automatic flush" (`l2c_api.c:1164-1168`) and only sends when it differs from the link's current value (`:1196-1201`). Scenario S5 asserts it is never sent **[H]**. |
| `0C37` LSTO | — | stays REJ (20 s) | deferred until LMP | Low; the stall watchdog (§4.6) covers dead links |
| `080B` Switch Role, sniff, `080D` | — | stay REJ, feature bits masked | The ROM still advertises role switch at LMP level | Medium: see `040F` |
| `1005` credits | streaming | keep 4 (`bt_ctrl_cmds.h:30`); `hp_credits` sweep in Exp 4c. `BT_CTRL_H2C_ACL_CREDITS` is a compile-time constant (used in `bt_ctrl.c` and the `1005` reply in `bt_ctrl_cmds.c`), so the sweep uses separate `BENCH=1 HP_CREDITS=n` builds, not a runtime key (rev 3) | 9 EM TX buffers | Medium: NOCP within 75-117 ms (§3.5) |

**Role switch decision (critique #5).** Before Exp 4b, either (a) write the masked feature bits into the ROM's LMP
feature table at `0x802C04` so role switch is not offered (the open item in `docs/BT_CTRL_OPCODE_TABLE.md` §3.6), or
(b) run Exp 4b on a bench image that also forwards `040F` and prove it is accepted with Bluedroid's packet-type set.
Every connect logs Role Change events, packet type and max slots (`hp_linkinfo` line, §10.2).

**Stock firmware reference (critique #13).** The stock firmware ran A2DP Source on this ROM ("music mode",
`docs/FIRMWARE_ANALYSIS.md:186`). The static read of the `0405`, `040F` and `0C1A` parameters it sent moves to H0 as a
desk task, before any silicon run.

Every flip from gated to forwarded is one commit citing the log in `docs/BT_HW_RESULTS.md`.

---

## 8. Host testing

### 8.1 Unit tests on virtual time (no stack)

- `tests/test_hp_tx.c` drives `hp_tx.c` with an injected `now_us()`, a fake `send()` that models the stack queue
  (cap, `ESP_FAIL`, `INVALID_STATE`) and drain rate, and the vendored SBC decoder (`BD/external/sbc/decoder/`, in the
  tree; excluded only from the target build) linked to decode the output. Cases:
  - rate within ±0.1 % at 44.1 and 48 kHz over 60 s virtual time, for fpp 7/10/11;
  - a 1 s tick stall, then recovery: frames sent in any 200 ms window ≤ nominal + `CREDIT_MAX` + one packet, and
    the burst after the stall reaches but never exceeds `HP_MAX_PKT_PER_TICK` packets in one tick, for fpp 7/10/11
    (critique #2);
  - headroom rule (§4.3): a bench override that breaks it is lowered, and a 5 % late tick jitter causes no pads;
  - queue cap never exceeded; the pre-checks keep `tx_lost_sbc` at 0; a `NULL` buffer allocation pulls no PCM;
  - position (rev 3): `queued_frames` is 0 right after a flush; 50 pause/unpause cycles and 50 paused seeks leave the
    heard position unchanged; pause silence, pads and pre-flush packets never count; the stop snapshot equals the
    fake's ground truth of unheard real frames, including frames the stack flushed on a remote SUSPEND;
  - tick stop in BTC context: no callback after `tick_stopped`, bound never hit;
  - resampler step > 1 (48 kHz file, 44.1 kHz link) keeps rate within ±0.01 %;
  - pad frames counted separately; `is_busy` settles at EOF while the drain is congested;
  - flush (with and without txq drop, `ts` rewound by `128 × dropped_sbc`), local suspend, remote suspend, remote
    START ignored;
  - counters across a suspend with packets in `a2d_list` and L2CAP that never get NOCP: after the next STARTED,
    `down_sbc` is 0, `is_busy` clears, and the stall watchdog does not fire;
  - per-packet bitpool change mid-stream (35 ↔ 24) decodes cleanly with the same `fpp`;
  - decode round trip SNR ≥ 30 dB at bitpool 35 for a 1 kHz tone;
  - `queued_frames` within ±1 packet of the fake's ground truth, including downstream and NOCP.
- `test_hp_tx.c` uses a fake `send()`, so it does not exercise patch A. That is done by
  `tests/bt_host/test_xsrc_stats.c` (H0 gate): it links the patched `XSRC` with stubbed
  `bta_av_ci_src_data_ready`, drives enqueue, `readbuf`, `btc_a2dp_source_set_tx_flush` and `ext_txq_drop`, and
  checks every counter, the queue cap of 4, and the drop's return value **[H]**.
- `tests/test_hp_sink.c`: ring, flush index (§3.3), padding, resampler gates (§3.4), mono and downmix paths, TSan
  producer/consumer stress.
- `tests/test_player.c` gets a `BT=1` build variant (today `Makefile:260` builds it without BT)
  with an `aout` A2DP-like fake: partial writes, large latency, async start, flush semantics, scrub with `ch = 2`
  into a mono ring (critique #19); rev 3 adds unpause after a remote/idle suspend (demand start), a START that never
  succeeds (EOF and full-ring waits end), PA never on with `aout_is_bt()`, jack edges ignored, and AVRCP scrub
  surviving with no physical key held.

### 8.2 Scenario H (`tests/bt_host/test_headphones.c`)

Timing is not measured here: the harness clock is real `CLOCK_MONOTONIC` (`tests/bt_host/host_glue.c:42-47`), which
is fine for framing and protocol checks (critique #22). Pacing accuracy lives in §8.1.

**Fake ROM additions** (`tests/fake_rom.c/.h`): `0401` with N mode-0 results and per-device CoD; `0402` deferred CC;
`0405` with page-timeout `0x04`, ghost `0x0B`, and `0x0C` while inquiring (state 4); `0405` SAVED in "LM state 1/2"
(no answer, `0x22` at 7 s); `0408` including the race; an ACL rate model (BR / 2-DH5 / 3-DH5, scripted stalls); a
"NOCP stops, no detach" variant; optional peer-initiated role switch.

**Fake headphone peer.** `fake_peer` is link-level only today (`tests/fake_rom.h:159-167`). Before writing
`fake_hp.c` (L2CAP server, SDP records 0x110B/0x110E/0x110C, AVDTP acceptor, AVCTP), milestone H5 evaluates Bumble
as the AVDTP peer (plan M3(a)); whichever is chosen must offer configurable SBC caps (44.1 / 48 / both, max bitpool), a remote
SUSPEND and START, a delay report, passthrough press/release/repeat, and absolute-volume registration.

Steps are numbered S1-S13 so they do not collide with milestones H0-H7. The "Needs" column says which peer a step
requires: **link** = the existing link-level `fake_peer`, **AV** = the AVDTP/AVCTP peer from milestone H5.

| Step | Needs | Checks |
|---|---|---|
| S1 | AV | Enable, then disable with a frozen peer and the stream STARTED: < 2 s, no hard path, `ESP_A2D_DEINIT_SUCCESS` seen, 0 leaks, alarm pool headroom, then a second full cycle succeeds (patch E; shown red first). (S1a, no peer: enable/disable only, runs from H3.) |
| S2 | link | Discovery: 5 devices, 2 audio. RNR count 2, none before inquiry complete. Cancel works. Page scan off during inquiry. |
| S3 | link | Pick device: discovery cancelled, then `0405`; a scripted `0405` during inquiry gets `0x0C` and the session never does it; a pick during the name pass waits for the `0419` to end (§6.2). |
| S4 | AV | Just Works in the window → bonded in `bt_hp_bonds.bin`. Another address or outside the window → refused at the confirm (patch D). Entropy gate closed → refused; failed pairing → key burned. |
| S5 | AV | AVDTP to STARTED: 48 kHz peer → 48; 44.1-only peer → 44.1 (SEID 1); peer max bitpool 27 → CFG shows ≤ 27; delay report reaches `lat_ms`; `0C28` never sent. |
| S6 | AV | Stream a tone: RTP/SBC headers valid, timestamps contiguous, frames per packet = fpp, packets ≤ `audio_mtu`. |
| S7 | AV | Saturation (BR model, 2 s stall): stack queue ≤ cap, `a2d_list` ≤ 1 (no BTA drops), AVRCP and SUSPEND still get through; after resume `down_sbc` is 0. |
| S8 | AV | Flush/seek with and without txq drop; local suspend, remote SUSPEND (auto-pause, rewind snapshot includes the frames the stack flushed), remote START (stack suspends it, no resume, no false pause); START rejected (→ `START_FAILED`, no hang); START then SUSPEND back-to-back (busy ack handled); counters rebased on STARTED. |
| S9 | AV | AVRCP keys incl. held FORWARD (one skip) and held FF (scrub stays alive), separately from volume: absolute volume, re-registration after CHANGED, fallback. No TG notification registered. |
| S10 | AV | Link loss: detach; `defer_detach` ghost; NOCP stops without detach → `LINK_STALLED`, snapshot rewind, `0406` through BTA; `0406` silent (H2) → escalation after `HP_STALL_DISC_MS`. |
| S11 | AV | Reconnect on mode entry; headset pages us; unbonded address rejected at the connection request (hook). |
| S12 | link | Phone mode with a headset bond: bond survives; the headset paging us is rejected. Phone → headphones (no `bt_hp_bonds.bin` yet) → phone: the phone bond is never copied into the headset file and the phone is still admitted (§6.4 store reset). |
| S13 | AV | Enable, stream 0.5 s, disable, with a frozen peer every 10th loop: no leaks, no hard path. ×20 in `test-bt-host`; ×200 in `make test-bt-host-soak`, which runs on the real clock (several minutes) and is required only at the H5 and H7 gates. |

Pointer width: Bluedroid stores pointers in `UINT32` in some timers (`tests/bt_host/README.md:157-161`); AVDTP does
it in `avdt_ccb_alloc` and the SCB alloc (`timer_entry.param = (UINT32) p_ccb`), so check AVDTP and BTA AV with ASan
under `-no-pie` first.

Harness notes (rev 3, review gate-3): `btc_msg_t` puts `arg` at byte offset 4 (`btc_task.h`), so a callback that
reads an 8-byte-aligned member straight from it can trip UBSan's alignment check on x86-64 (phone mode's SPP needed
patch 0024 for a related over-read); fix any A2DP/AVRC hit the same way, by copying the event's own struct. The
fake ROM records at most `FAKE_ROM_LOG_MAX` (512) ACL packets, so the AV peer (or the step) drains `fake_rom.acl`
per step for S6/S13. The host build of `test_headphones` passes its own `BT_CTRL_EXP1_PASSED_LIST` (`0401`, `0402`,
`0405`, `0408`) so the default list stays empty until each silicon flip.

---

## 9. UI flow (English only, `/* TODO(i18n) */` on every string)

Settings gains a `Bluetooth headphones` row (`BT=1` only).

| Screen | Content | Keys |
|---|---|---|
| Headphones | `Headphones: Off / On` · `Connected: <name>` or `Not connected` · `Pair new headphones` · `Forget <name>` | ENTER toggles or selects, BACK leaves |
| Searching | `Searching… <n> s` and a growing list (`Unknown device` until named) | ENTER pick, BACK cancel |
| Pairing | `Pairing with <name>…` then `Paired` / `Pairing failed` | BACK cancel (`0408`) |
| Toasts | `Headphones connected`, `Headphones disconnected`, `Headphones not connected` (play with no stream), `Headphones not ready` (START failed), `Reconnecting…`, `Pairing unavailable` (entropy gate), `Headphones paused` (remote suspend) | — |
| Player header | BT icon (connected / streaming / reconnecting). Volume overlay `BT 0`-`BT 10` (one decimal, `format_vol_label`'s scale) derived from `bt_vol` (§6.6). | — |

The wasm build does not define `OPENFW_BT_BLUEDROID`, so it cannot render these screens as they stand; H6 adds a
wasm-only stub of the `bt_hp` status API under a `WASM_BT_UI=1` define so the screens compile and render there
(rev 3, review gate-4), or the "wasm build" check for them is dropped in favour of silicon screenshots.

---

## 10. QEMU gate and bench trigger

### 10.1 `make test-qemu-bt-hp` (next to `test-qemu-bt`)

QEMU has no RW-BT controller. Image: `BT=1 RTOS_SELFTEST=1 HP_SELFTEST=1` in `/tmp/sl6801_openfw_bt_hp`.
- Boots, RTOS self-test passes, all §5.3 roots linked (post-link check).
- `HP_SELFTEST` runs `hp_sbc` over 375 frames of a fixed tone at 48 kHz, bitpool 35, and compares a CRC32 with the
  host golden (bit-exactness, critique #22). A second run changes bitpool every 10 frames.
- `hp_sink` ring + resampler 1 s of 16 → 48 kHz, CRC against the host golden.
- `esp_timer_get_time()` monotonic over 100 ms of `vTaskDelay`.
- `--expect-log "HPST sbc=OK rs=OK clk=OK"`. Also builds `BT=0` and `cmp`s the default image.

### 10.2 Bench keys (`bt_port/bt_bench.h`, `BENCH=1` only; strict parser, `bt_bench.h:15-17`)

| Key | Values (default) | Meaning |
|---|---|---|
| `mode` | `phone` \| `hp` (`phone`) | Session; `hp_*` keys need `mode=hp`. |
| `hp_stage` | `scan` \| `connect` \| `tone` \| `player` (`scan`) | Exp 4a / 4b / 4c / 4d |
| `hp_addr` | `XX:XX:XX:XX:XX:XX` | Target; required from `connect` unless `hp_pick=first` |
| `hp_pick` | `addr` \| `first` (`addr`) | `first` = first audio-class device |
| `hp_pair` | 0/1 (0) | Open the pairing window for `hp_addr` (`pair_secs`) |
| `hp_pscan` | 0/1 (0) | Leave page scan on during `0401`/`0405` (Exp 4a/4b variants) |
| `hp_rate` | `44100` \| `48000` \| `auto` | `auto` = §4.1 recipe. A fixed rate overwrites **both** SEIDs with that one rate (`btc_av` pre-registers every SEID with the default 44.1 kHz / bitpool 53 caps, so leaving SEID 1 alone would keep a 44.1 kHz fallback) |
| `hp_bitpool` | 18..53 (35) | Max bitpool advertised |
| `hp_speech_bp` | 0 \| 18..35 (24) | Per-frame bitpool for ≤ 16 kHz sources (0 = use max) |
| `hp_mono` | 0/1 (0) | Prefer mono SBC (`esp_a2d_source_set_pref_mcc`) |
| `hp_tick_ms` | 10..30 (20) | Pacer tick; lowered at STARTED if it breaks the §4.3 headroom rule |
| `hp_pkt_per_tick` | 1..2 (2) | `HP_MAX_PKT_PER_TICK`; 1 only with `hp_tick_ms` ≤ 15 (§4.3) |
| `hp_txq` | 2..8 (4) | `TXQ_PKT_CAP` |
| `hp_credit_ticks` | 1..4 (2) | Elapsed clamp and the tick term of `CREDIT_MAX` |
| `hp_tone_hz` | 100..8000 (1000) | `tone` stage, 16 kHz mono through the full `hp_sink` path |
| `hp_file` | 8.3 path | `player` stage |
| `hp_credits` | 4..9 (4) | Experimental `1005` credit count; checked against the image's build-time `HP_CREDITS` (§7) and refused on mismatch |

Status line every 30 s: `BENCH hp t=S st=STARTED rate=48000 bp=35 fpp=10 pkts=N frames=N pads=N txq_max=N
tx_lost=N alloc_fail=N nocp_max_ms=N stall=N heap_min=N blk_max=N btc_stack_min=N btu_stack_min=N cpu_sbc_pct=N`.
After every connect and every Role Change: `BENCH hp linkinfo role=M|S pkt_type=0x.... max_slots=N edr=0|1 mtu=N`.
Peer addresses are cut to their last 3 bytes.

**Long runs and the 8 KiB log (rev 3, review missing-mode 8).** `SD:/BTBENCH.LOG` keeps only the head of an 8 KiB
capture (`bt_bench_fw.c`); a 600 s or 3600 s run at one status line per 30 s overflows it and would lose the final
summary. For `secs` > 300 the status interval becomes 300 s, the bench job writes a fixed-size `BENCH hp summary`
line (all maxima and counters) into space reserved at the end of the capture buffer at `BENCH end`, and every
duration counter is in ms (`uint32_t` µs would wrap at 71.6 min).

---

## 11. Silicon experiments (Exp 4), smallest risk first

Each is one `BTBENCH.RUN` with no key press; results go to `docs/BT_HW_RESULTS.md`.

| # | Run | Proves / measures | Pass |
|---|---|---|---|
| 4.0 | `mode=hp hp_stage=scan secs=30`, no headset | Bring-up/teardown (the §5.5 sequence; in H3 run by the bench job under its own 2 s timer), SBC encode cost from XIP vs SRAM (self-test loop) incl. the per-tick maximum, alarm pool | `teardown_ms` < 2000, 0 faults, `cpu_sbc_pct` logged |
| 4a | `hp_stage=scan`, 3 headsets, `hp_pscan` 0 and 1 | `0401`/`0402` golden vectors, CoD, RNR count, deferred cancel CC, LM-state behaviour with page scan on | All found and named, no `0x22`, cancel CC seen |
| 4b | `hp_stage=connect hp_pair=1`, `hp_pscan` 0 and 1; role-switch variant (a) or (b) from §7 | `0405` as master, Just Works with our key, AVDTP to OPEN, 48 kHz vs 44.1 choice, `linkinfo` | OPEN reached, pairing completes (`ESP_BT_GAP_AUTH_CMPL_EVT` success; the key is not persisted in H3, since `bt_hp_bonds.bin` is H5 work: persistence is checked in Exp 4g), `max_slots=5` after any Role Change |
| 4c | `hp_stage=tone secs=600`, sweep `hp_bitpool` 35→45→53 × `hp_credits` 4→6, then `hp_speech_bp` 20/24/28 | NOCP latency vs §3.5 budget, EM accounting, pads, `tx_lost`/`alloc_fail`, heap, stack watermarks, EDR on air | 0 pads and 0 drops at bp 35; highest clean bitpool; `HP_STALL_MS` set |
| 4d | `hp_stage=player`, 16 kHz mono, then 44.1 kHz stereo | SD stalls vs ring depth, resampler CPU, seek latency | 0 pads at priority 1, flush heard ≤ 300 ms |
| 4e | Manual: AVRCP keys, then volume, separately | §6.6 table, held keys, absolute volume | Every key does what the table says |
| 4f | 3 brands × `secs=3600` | Soak | No fault, no WDT, pads < 1/min, heap stable |
| 4g | Power-cycle the headset ×10, then the recorder; walk out of range | Ghost link, watchdog, reconnect, bond persisted in `bt_hp_bonds.bin` | Paused within 1 s of the stall; reconnect ≤ 60 s, also after a recorder reboot without re-pairing |
| 4h | Record 60 s with the stream suspended, link up | RF noise | No audible artefact, else REC disconnects |

Open silicon questions: EDR on air; EM buffer freed per HCI packet or per segment; inquiry or paging with an active
stream (v1 forbids discovery while STARTED); `0C1A` with exactly one headset link; IRQ-26 rate under streaming.

---

## 12. RAM and CPU budget

### 12.1 RAM (headphones mode, on top of the BT=1 phone baseline)

| Item | Size | Source |
|---|---|---|
| Internal A2DP source static (1752 B) and resample statics (1536 B) | **−3.3 KB** (gone) | `ASRC:139-170`, `:1274-1277` compile out (`:42`) |
| SBC encoder `.bss` | ~645 B (kept) | llvm-size |
| `hp_sbc` `SBC_ENC_PARAMS` | ~1.7 KB static in `.bt_bss` (RTOS heap region, not main SRAM) | §4.2 |
| PCM holdover | **0** (rev 3: removed; rev 2's `hold_pcm` would have been 5.6 KB at fpp 11, 7.7 KB at fpp 15, unbudgeted) | §4.3 |
| Packet-record FIFO | ~60 B | §4.4 |
| PCM ring | 0 B extra | reuses `g_dac_buf` |
| Resampler state + table | ~1.3 KB heap (72 or 1024 coefficients, Q15) | §3.4 |
| Encoded packets, steady state | ~2-3 KB heap (1-2 in flight × ≤ 1.03 KB: `BT_HDR` 8 + offset 24 + data ≤ 992) | `btc_av.c:3852-3856` |
| Encoded packets, worst case | ~11.4 KB heap: `TXQ_PKT_CAP` 4 + `a2d_list` 1 + L2CAP 5 + 1 being built | §4.4 |
| Second SEP (`AVDT_NUM_SEPS`, `bt_target.h:2586`; BTA SEP slot) | measured in milestone H5 **[H]** | — |
| AVDTP/AVCT/AVRC/BTA AV control blocks | static, already linked | **[H]** |
| L2CAP/AVDTP signalling, SDP buffers | ~4-6 KB estimate | **[H]** |
| bt_hp session (event ring 16 × 32 B, list 8 × 32 B, state, counters) | ~1.1 KB | — |
| **Total new heap at peak** | **≈ 18 KB** worst case, ≈ 10 KB steady | vs. ~62 KB phone-mode free min (`docs/BT_HW_RESULTS.md`, session 4 "Memory" row), floor 8 KB |

Exp 4c and 4f record heap low-water and largest free block.

**Main SRAM (rev 3, R-11).** Main SRAM `.data + .bss` must end below `0x00850000` (`sl6801_firm.ld` ASSERT) and
`.bss_hi` below `0x00867000`. New headphones state therefore goes to `.bt_bss` or the heap, never to main `.bss`.
The review quoted 7.9 KB free below `0x00850000`; a `BT=1` ELF in `/tmp` (revision unknown) showed `_ebss` at
`0x0084BD8C`, 16.6 KB free. Both are snapshots: every milestone gate records `_ebss`/`_ebss_hi` headroom for
`BT=1` and `BT=1 BENCH=1`, and moving `sbc_*` code into SRAM (§12.2) is allowed only if that headroom stays
≥ 4 KB after the move.

### 12.2 CPU (CPU0, 96 MHz)

| Load | Estimate | Basis |
|---|---|---|
| SBC joint stereo 48 kHz | ~12-13 M instr/s, 15-19 % (bitpool changes allocation, not analysis) | QEMU instruction count, CPI 1.1-1.4 from SRAM |
| SBC from XIP | unknown, possibly 1.5-3× | **[S]** Exp 4.0; if slow, move `sbc_*` analysis text to the SRAM code section (`sl6801_firm.ld:56-83`) under `BT=1` |
| Resampler 16 → 48 kHz polyphase | ~1.2 M MAC/s, < 3 % | §3.4 |
| BT stack | unknown | **[S]** Exp 4c |
| **Target** | **< 40 % CPU0 while streaming** | |

BTC stack is 3072 B (`bt_port/sdkconfig.h:11`); the tick encodes on it, as the internal encoder did. Exp 4.0/4c log the
watermark; raise to 4096 B only if headroom < 512 B.

---

## 13. Milestones (each ends in a gate-able state)

Every gate also requires zero-warning ARM builds (`all`, `RTOS=1`, `BT=1`, `BT=1 BENCH=1`), `test-host-san`,
`test-qemu-rtos`, `test-qemu-bt`, `wasm` when shared files change, and a **byte-identical default image** (`cmp`).

| M | Content | Gate |
|---|---|---|
| **H0** | Ext-codec config (§5.1) incl. generator keep-list (needs `IDF_PATH` + `kconfiglib`); prefix defines; patch A; link roots (§5.3) incl. dropping `SBC_Encoder_Init` from `check_bt_link_roots.sh`'s internal list; `PORTING_NOTES.md` fix. Desk: static read of stock `0405`/`040F`/`0C1A` parameters (§7). | `test-bt-host` green (phone scenarios unchanged); `bluedroid_srcs.mk` still lists the encoder; `nm libbluedroid.a` shows `SBC_Encoder`/`SBC_Encoder_Init` defined and no `btc_a2dp_source_setup_codec` (the image cannot show the encoder yet: nothing calls it and the link uses `--gc-sections`, §5.3); `test_xsrc_stats` green (patch A counters, cap, drop; §8.1); `make BT=1` links (root check passes); BT=0 `cmp` |
| **H1** | `hp_sink.c` (ring, flush index, epochs, 32.32 linear resampler), `hp_tx.c` (packet-record FIFO, snapshot, BTC-context stop) + `hp_sbc.c`, `test_hp_sink.c`, `test_hp_tx.c` (virtual time, decoder round trip); QEMU `HP_SELFTEST` SBC/resampler CRC; `SBC_Encoder_Init` back in the root check | Host tests green incl. TSan; QEMU CRC matches host golden; `nm` of the `BT=1 HP_SELFTEST=1` ELF shows `SBC_Encoder` |
| **H2** | bt_ctrl: `0402` layout and the validator change (§7); `bt_ctrl_rnr_pending()` (§6.2); fake-ROM handlers and golden vectors for `0401`/`0402`/`0405`/`0408` (incl. `0x0C`-in-inquiry, SAVED, race, NOCP-stall variants); still gated off | `test-bt-ctrl` green with and without the gate list |
| **H3** | **Thin silicon spike.** `bt_hp_fw.c` bench subset: tone stage only, minimal session (inquiry/name → connect `hp_addr` → AVDTP → STARTED → tone), `linkinfo` logging, bench keys of §10.2. Teardown is the §5.5 sequence run directly by the bench job under its own 2 s timer (H4 later moves it into the session core). No bond persistence. **Pairing prerequisite (rev 3, review gate-2):** Exp 4b pairs, which needs the ROM P-192 key staged (`bt_rom_real_stage_p192`) and the ROM RNG pool fed (`bt_rom_rng_feed`), both done today only inside `bt_xrec_fw.c`'s phone-mode bring-up; H3 first moves those two helpers (and the `bt_store` reset of §6.4) into a small shared `bt_port/bt_session_plat.c` that both modes call, a mechanical move whose gate is `test_phone.c` unchanged. The rest of H4 stays where it is. Host: only the scenario steps the link-level fake can reach (S1a enable/disable without a peer, S2 discovery, S3 connect order). Run Exp 4.0, 4a, 4b, 4c (10 min tone, bitpool × credit sweep, page-scan on/off, role-switch variant). | S1a/S2/S3 green; Exp results in `docs/BT_HW_RESULTS.md`; feasibility decided (master role, LM states, packet type/slots, throughput). Gated rows that passed flip to FWD, one commit each |
| **H4** | Shared session core refactor: deadline, fault ladder, store, entropy out of `bt_xrec_bluedroid.c` into `bt_session_core.c`; mode dispatch in `bt_shutdown`; patch B hook (phone-mode rule only) | Phone scenarios (`test_phone.c`) unchanged and green; `test-bt-host-tsan` green; phone mode on silicon unchanged (user, normal session) |
| **H5** | Full `bt_hp_bluedroid.c`; Bumble-or-`fake_hp.c` peer; scenario steps S1-S13; patches B (hp rule), C, D, E; separate bond file and per-mode store reset; media-control serialisation; stall escalation | `test-bt-host` and `-tsan` green, all S steps; `test-bt-host-soak` (S13 ×200) 0 leaks |
| **H6** | `audio_out.h`/`audio_out.c`, `aout_write(p,n,ch)`, async start and demand start, player hooks (PA/jack gating, AVRCP merge past the screen-off gate, AVRCP scrub source), session stop on power-off/USB/phone entry, `test_player` BT=1 variant, polyphase resampler, UI screens (+ wasm stub), `bt_vol` in the hp store, REC suspend. The firmware Makefile has no `-MMD` dependency files, so objects that include `audio_out.h` list it explicitly | BT=0 `cmp`; `test_player` both variants; QEMU `test-qemu-bt-hp`; Exp 4d and 4e pass |
| **H7** | Exp 4f/4g/4h: 3 brands, 1 h soak, reconnect/watchdog, bond persistence, recording; tune `HP_LAT_MS`, `HP_STALL_MS`, bitpool, credits, tick | Plan Exp 4 accepted; `test-bt-host-soak` green; lessons in `docs/BT_HW_RESULTS.md` and here |

H1 and H2 can run in parallel after H0. H3 needs H0-H2 and nothing later: its teardown, pairing and host checks are
scoped above so that no part waits for H4 or H5. H4 does not depend on H3 and can overlap it. If H3 shows a
showstopper (no master role, 1-slot packets, throughput below bitpool 28), H5-H7 stop and the design is revisited.

---

## 14. Risks (ranked)

| # | Risk | Mitigation |
|---|---|---|
| 1 | `0405`/`0401` behave differently than mapped; master role breaks; LM states 1-2 swallow pages | Gated rows; H3 spike first; page scan off around paging; stock firmware static read |
| 2 | Role switch leaves 1-slot packets (`040F` REJ) | §7 decision before Exp 4b; `linkinfo` logging |
| 3 | ACL throughput below the SBC rate | Bitpool 35 cap, per-frame speech bitpool, mono SBC, credit sweep, full-MTU packets |
| 4 | Dead link undetected for 20-40 s | Credit-stall watchdog, snapshot rewind, `0406` via BTA, escalation for ghost links (§4.6) |
| 5 | Ext-codec source path less field-proven upstream **[S]** | Small surface (256 lines), scenario H coverage, Exp 4f soak |
| 6 | Teardown overruns 2 s → hard path | No graceful close at shutdown, sub-deadline, frozen-peer tests (§5.5) |
| 7 | SBC from XIP too slow | Exp 4.0; SRAM code section under BT=1 |
| 8 | Phone mode wipes headset bonds / headset steals phone-mode link | Separate bond file; connection allow-list hook |
| 9 | Heap peak under congestion (~13 KB packets) | `TXQ_PKT_CAP` 4, downstream bounded by BTA/L2CAP; heap low-water in Exp 4c/4f |
| 10 | Position wrong by headset latency | Delay report, 150 ms fallback, Exp 4d tuning |
| 11 | SEID 0 must be a superset of every config | Registration recipe §4.1; S5 asserts |
| 12 | Just Works pairing bypasses the entropy gate and key burn | Patch D required in v1 (§6.3); allow-list hook; post-hoc check as second line |

## 15. Open decisions (with recommendations)

1. **48 kHz preferred, 44.1 kHz fallback?** Recommend **yes**, via SEID 0 {44.1|48} + SEID 1 {44.1} and the default
   rate define; no stack patch. 16 → 48 kHz is exact, which matters for our 16 kHz recordings.
2. **Post-hoc bond removal or the Just Works confirm patch?** rev 2 recommended post-hoc plus the allow-list hook.
   **rev 3 changes the recommendation to patch D in v1 (H5)** (R-8): Bluedroid auto-confirms Just Works without an
   app event, so post-hoc alone also bypasses the entropy gate and the P-192 key burn, not just the window. Exp 4b
   (H3, bench, picked address) may still run before D lands, with `ssp_block()` checked before the connect.
   *Needs human confirmation (it reverses the rev 2 recommendation).*
3. **Idle timers.** Recommend `HP_IDLE_SUSPEND_S` 20 s and `HP_IDLE_DISCONNECT_S` 10 min, both bench-tunable;
   remote suspends do not restart the idle clock.
4. **REC: suspend or disconnect?** Recommend **suspend** (faster resume), with disconnect as the fallback if Exp 4h
   finds RF noise.
5. **Delay reporting.** **Resolved:** it is already enabled (`BTA_AV_FEAT_DELAY_RPT`, `btc_av.c:2775,2779`) and
   delivered as `ESP_A2D_REPORT_SNK_DELAY_VALUE_EVT` in the ext path. Use it; 150 ms fallback for sinks that do not send
   one.
6. **Frozen-peer A2DP teardown (patch E).** Recommend a BTA-level local close while disabling; the alternative is a
   forced reset of the `btc_av` statics at `esp_bluedroid_deinit`. Decide in H5 after S1 is red. *Human decision.*
7. **Stall escalation.** A stall whose `0406` gets no Disconnection Complete within 8 s restarts the controller
   through the fault ladder (§4.6). Recommend yes (credits are otherwise held until reboot); the cost is a few seconds
   of `Reconnecting…`. *Human confirmation.*

---

## 16. Critique disposition (rev 2)

(Historical. The patch numbers 0029-0032 below are rev 2's; rev 3 renamed them A-D and added E, §5.2.)

| # | Point | Outcome | Reason |
|---|---|---|---|
| 1 | Queue cap 10 starves both rates; `txq_len×128` undercounts | **Accepted (moot by redesign)** | Verified `ASRC:1229-1236,1517-1535`. Ext path: our pacer separates per-tick work (`HP_MAX_PKT_PER_TICK`) from the packet cap (`TXQ_PKT_CAP`), and `queued_frames` counts frames (§4.3-4.4). The `frm_nb_threshold` underflow at `ASRC:1230` is clamped to 21 by `:1231-1233`, so it is harmless; moot anyway. |
| 2 | Unbounded catch-up after a stall | **Accepted** | Verified `ASRC:1222-1227` (`UINT8 result`, `:1196`). Our pacer clamps elapsed and credit (§4.3); virtual-time test bounds any 200 ms window to nominal + `CREDIT_MAX` + one packet (§8.1). |
| 3 | Ghost link: playing into nothing, wrong resume | **Accepted** | Credit-stall watchdog, NOCP-acked rewind, `0406`, fake-ROM "NOCP stops" variant (§4.6, S10). Evidence: `docs/BT_HW_RESULTS.md` session 4 "Fault" row, session 5 "Stale link" row, session 6 open question. |
| 4 | No 48 → 44.1 fallback | **Accepted, fixed by config** | Verified the single `codec_cfg` path for the internal codec (`bta_av_co.c:2429-2434`). Ext path retries per SEID (`:1802-1811`); two SEPs + default-rate define (§4.1), no patch. |
| 5 | Role switch + rejected `040F` → 1-slot packets | **Accepted** | Verified `btm_acl.c:1680-1683`, `bt_ctrl_cmds.c:56`. Decision (a)/(b) before Exp 4b and `linkinfo` logging (§7, §11). |
| 6 | LM states 1/2; `0405` during inquiry is `0x0C` | **Accepted** | Verified `BT_ROM_HCI_MAP.md:78,82`. Page scan off around `0401`/`0405`, Exp 4a/4b on/off variants, fake ROM models `0x0C` (§6.2, §8.2). |
| 7 | Delay reporting is available | **Accepted** | Verified `btc_av.c:2775,2779`, `bta_av_co.c:1520-1524`, `XSRC:100-111`. Feeds `lat_ms` (§4.4); decision 5 resolved. |
| 8 | Flush-generation race | **Accepted** | Producer records `flush_wr`; consumer drops only up to it (§3.3). |
| 9 | EOF/`is_busy` never settle while padding | **Accepted** | `is_busy` from `last_real_sbc` vs `done_sbc`; pads tracked separately (§4.4). |
| 10 | Phone-mode headset reconnects; no connection filter | **Accepted** | Verified `bt_target.h:1300`, `btc_gap_bt.c:91`, `btm_sec.c:2738-2743`. Patch 0030 allow-list hook, both modes (§6.3). |
| 11 | Name pass pages every device | **Accepted** | Verified `btc_gap_bt.c:325-328`. Patch 0031 CoD hook; S2 asserts RNR count. |
| 12 | Remote SUSPEND/START unhandled | **Partly accepted** | Remote SUSPEND → auto-pause with rewind (§4.5). Remote START cannot mean "ready": the stack itself suspends a sink-initiated START (`btc_av.c:1455-1461`); AVRCP PLAY resumes instead. |
| 13 | Main silicon risks come last | **Accepted** | H3 thin spike right after host basics, scoped so it needs nothing from H4/H5; stock-firmware static read in H0; Bumble evaluated in H5 (§13). Sub-point "patches land in H0 but nothing tests them": patch 0029 gets `test_xsrc_stats.c` in the H0 gate (§8.1), since `test_hp_tx.c` only uses a fake `send()`. |
| 14 | Teardown can trip the 2 s hard path | **Accepted** | Verified `bt_shutdown` in `bt_xrec_bluedroid.c` (deadline check raises `BT_XREC_FAULT_DOWN_DEADLINE`). No graceful close at shutdown, sub-deadline, shared session core milestone H4, frozen-peer tests (§5.5). |
| 15 | AVRCP pitfalls | **Accepted** | Press-edge skip, watchdog re-arm on repeats, re-register volume, BTC-callback replies, keys outside the player, 0-127 vs 0-31 mapping (§6.6). |
| 16 | ACL packet rate wrong | **Accepted** | Full-MTU packets; 34-54 pkt/s and 75-117 ms NOCP budget (§3.5). The ext path removes the 1280 B per-packet buffer. |
| 17 | Per-frame bitpool | **Accepted** | Verified `sbc_packing.c:91`, `sbc_enc_bit_alloc_ste.c:60`. `hp_speech_bp` knob (§3.5). |
| 18 | Resampler spec vs SNR gate | **Accepted** | 3×24-tap exact case, 64×16 general, gates per stage (§3.4). |
| 19 | Mono pass-through vs scrub | **Accepted** | Verified `player.c:522`. `aout_write(p,n,ch)` with downmix; `test_player` BT=1 variant (§3.1, §8.1). |
| 20 | Settings persistence resets users | **Accepted** | Verified `persist.c:61-63,73-75`. `bt_vol` in the `bt_hp` store; settings layout untouched (§6.6). |
| 21 | `aout_start` blocks on the event ring | **Accepted** | Start is asynchronous; wait time counts as queued (§3.2). |
| 22 | Harness clock real; SBC bit-exactness assumed | **Partly accepted** | Pacing tested on virtual time in `test_hp_tx.c`; harness checks framing only; QEMU CRC moved to H1 (§8, §10.1). "×200 with streaming is long" on the real clock: S13 runs ×20 (0.5 s streams) in `test-bt-host`; the ×200 run stays, but in a separate `test-bt-host-soak` target required only at the H5 and H7 gates. |
| 23 | Smaller corrections | **Accepted** (`0C28` partly) | `TXQ_PKT_CAP` defined (§4.3); `0C1A` at `:330`; `cmp` gate only; roots list (§5.3). `0C28`: sent only if the link's flush timeout differs (`l2c_api.c:1196-1201`); S5 asserts it is never sent. |
| 24 | `BTC_AV_EXT_CODEC` alternative | **Accepted and adopted** | Verified end to end (§2.1, §4, §5). One finding the review missed: the source-list generator drops the encoder in ext mode (§5.1). |

Additional findings from the rev 2 verification: the source-list generator drops the SBC encoder when the switch is
on (§5.1); a sink-initiated START is auto-suspended by the stack (§4.5); BTA `a2d_list` drops are silent and packets
downstream of `audio_tx_q` are invisible to the queue (counted by patch 0029 and NOCP, §4.4); worst-case packet heap
is ~13 KB, not ~4 KB (§12.1); the `bt_esp_timer` shim is documented as used only by `osi/alarm.c`, so the tick uses
`osi_alarm` rather than raw `esp_timer` (§4.3).

## 17. Revision history

- rev 1 (`488da72`): internal codec with patches 0029 (clock), 0030 (queue cap), 0031 (caps/bitpool).
- rev 2 (`eb0b307`): external-codec architecture; app pacer and SBC wrapper in BTC context; config-only rate fallback;
  patches 0029 (ext queue stats/cap/drop), 0030 (connection allow-list), 0031 (inquiry name skip), 0032 optional;
  stall watchdog; thin silicon spike moved to H3; session-core refactor milestone; disposition of all 24 review points.
- rev 2.1: fixes from the rev 2 re-check. §4.4 counters named by unit (`*_sbc` SBC frames, `*_pcm` PCM frames) and
  `queued_frames` converts SBC frames with × 128; `ts` rewind in samples. Counters rebased on every stop and STARTED
  (stop/suspend paths drop `a2d_list` and flush L2CAP without counts or NOCP); `acked_sbc` saturates; `is_busy` uses
  `done_sbc` incl. `flushed_sbc`. `fpp` fixed per STARTED period at the max bitpool; bitpool changes per packet.
  Pacer: `CREDIT_MAX` term `HP_MAX_PKT_PER_TICK × fpp × 128`, cap 2 (was an unreachable 3), headroom rule, encode
  cost per tick from §12.2. H0 gate checks the encoder in `libbluedroid.a` (image check moved to H1) and adds
  `test_xsrc_stats`. H3 scoped to need nothing from H4/H5. Scenario steps renamed S1-S13. `bt_xrec_bluedroid.c` and
  `BT_HW_RESULTS.md` cited by function/section instead of line. Dispositions #13 (0029 test added) and #22 (partly
  accepted) corrected.
- rev 3: external review disposition (§18). Position model rebuilt (§4.4): packet-record FIFO of real post-flush
  frames, flush epochs, bounded latency term, stop-time rewind snapshot; the `acked_sbc × 128` stall seek and the
  `hold_pcm` holdover are gone (pre-check and allocate before pulling PCM). Demand start on write/unmute. Tick stopped
  in BTC context (no generation spin). Stall: `0406` through `BTA_DmCloseACL`, wait for Disconnection Complete,
  escalate ghost links. Teardown: AVRC before A2DP, patch E so `clean_up()` runs with a frozen peer, second-cycle
  check. AVRCP TG notifications dropped. Patch D (Just Works confirm) required. Patches renamed A-E (0029-0033 are
  taken). `a2d_list` drop counter removed (unreachable). Per-mode `bt_store` reset. Media-control serialisation,
  unasked-STARTED handling, START failure. PA/jack gating, AVRCP past the screen-off gate, AVRCP scrub source,
  session stop on power-off/USB/phone entry. H0 link-root script edit; H3 pairing helpers moved early.

## 18. External review disposition (rev 3)

Two external reviews were requested of rev 2.1. **Codex** returned no findings: its output was only a progress line
("Running. I'll wait for the monitor notification."), so nothing could be checked and no point has agreement from
both reviewers. **agy** returned 52 points (numbered here R = correctness §1, M = missing failure modes §2, G = gates
§3, W = "what I would do differently" §4). Each was checked against the code at the time of writing.

Totals: **42 accepted, 5 partly accepted, 5 rejected.**

| # | Summary | Outcome | Reason / where |
|---|---|---|---|
| R-1 | Frozen-peer teardown skips `btc_av` `clean_up()`, leaves statics stuck; AVRC deinit order inverted | **Accepted** | `bta_av_do_close` arms a 4 s close timer; 0026/0027 end BTU first, so `BTA_AV_DISABLE_EVT` never runs; `.bt_bss` is zeroed only in `startup.c`. `btc_avrc_*_deinit` warn when A2DP is already deinit. §5.5 reordered, patch E, S1/S13 second-cycle check. |
| R-2 | TG `PLAY_STATUS_CHANGE`/`TRACK_CHANGE` RN and Cat-1 queries unimplemented; no "absolute volume reply" exists; `esp_avrc_tg_send_rn_rsp` deprecated | **Accepted** | `cs_rn_allowed_evt` = volume only; `btc_avrc_tg_send_rn_rsp` "todo"; `btc_rc_upstreams_evt` rejects GET_PLAY_STATUS/GET_ELEMENT_ATTR; deprecated attribute in `esp_avrc_api.h`. TG RN dropped (§5.3, §5.4, §6.6). |
| R-3 | `acked_sbc × 128` stall seek mixes a stream counter with a file offset; `queued_frames` breaks pause/seek/scrub (lat term after flush, flushed-on-suspend frames lost) | **Accepted** | `player_heard_frames`/`player_pause_rewind` in `player.c`; SUSPEND path frees `audio_tx_q` before the event. New position model (§4.4). |
| R-4 | Direct `0406`/`0C1A` would violate the hciT single-producer contract; ghost links never send `0x05`; credits stay held | **Accepted** | `bt_ctrl.h` contexts; ROM map §11 H2 and the `0406` row. Mechanism differs from the review's: `BTA_DmCloseACL` is an existing BTA API (no new helper), and an HCI command is not blocked by zero ACL credits. Escalation after 8 s (§4.6). |
| R-5 | Unpause after idle/remote suspend or reconnect never restarts the stream | **Accepted** | `aout_start` only at file open in `openfw_play_file_ui`. Demand start (§3.2, §6.5). |
| R-6a | `SBC_Encoder_Init` clobbers `s16BitPool` | **Rejected** (already handled) | rev 2 §4.2 already set the bitpool after Init "(Init derives it from `u16BitRate`)". Wording made explicit. |
| R-6b | Q32 phase step overflows for 48 → 44.1 kHz | **Accepted** | 32.32 in `uint64_t` (§3.4), test added (§8.1). |
| R-7 | Cross-mode `bt_store` contamination; `s_store_flush` latched; bond file is BTS1/INI | **Partly accepted** | `bonds_load` returns without clearing → per-mode `bt_store_reset()` (§6.4), BTS1 parser for the deny list (§6.3). Rejected part: `bt_port_init` **does** replace `s_store_flush` when called again with a non-NULL callback (`bt_port_glue.c`). |
| R-8 | Just Works auto-confirm bypasses `ssp_block`/`ssp_burn` | **Accepted** | `btc_dm_sp_cfm_req_evt` calls `BTA_DmConfirm(..., true)` with no app event. Patch D required (§5.2, §6.3, §15.2). |
| R-9 | `a2d_list` never exceeds 1; `bta_av_co_audio_drop` unreachable | **Accepted** | Traced `bta_av_data_path`: pop, then `< 3` test, then prepend. Counter removed, S7 asserts ≤ 1, heap −2 KB. |
| R-10 | Waiting for a `bt_esp_timer_generation` step spins forever | **Accepted** | Generation advances only on set/cancel/free (`bt_alarm.c`). Stop in BTC context (§4.3). |
| R-11 | `hold_pcm` (5.6-7.7 KB) unbudgeted; main SRAM nearly full | **Accepted** (figure not reproduced) | Holdover removed (§4.3). Review's 7.9 KB free vs 16.6 KB in a `/tmp` BT=1 ELF of unknown revision; gates now record headroom (§12.1). |
| R-12 | `0402` gate fails `bt_ctrl_cmds_init` validation | **Rejected** (already planned) | rev 2 §7 and H2 already add the zero-length layout before gating; the validator detail is now spelled out. |
| R-13a | Patch numbers 0029-0032 already taken | **Accepted** | README rows 0029-0033, next free 0034. Letters A-E (§5.2). |
| R-13b | Cancel during the name pass reports STOPPED before `0x07` | **Accepted** | `bta_dm_search_cancel` posts SEARCH_CMPL right after `041A`. `bt_ctrl_rnr_pending()` wait (§6.2), S3. |
| R-14 | Speaker PA powered during BT playback; jack edges change volume/pause; power-off calls `audio_dac_stop` | **Accepted** | 11 `audio_pa_set` sites in `player.c`; `ui_poll_power_button` in `main.c`. §3.1. |
| R-15a | `040F` sent on every connection, not only on role change | **Accepted** | `btm_establish_continue`. §7 row corrected (phone mode already runs with it refused). |
| R-15b | Headset-initiated link leaves us slave with role switch masked | **Accepted** (as an Exp item) | Not a demonstrated defect; Exp 4g logs it (§6.5). |
| R-15c | `L2CA_SetFlushTimeout` races BTU and flushes the whole ACL | **Rejected** | The design never calls it; BTA's call is a no-op for timeout 0 when unchanged (§7 `0C28` row) and S5 asserts `0C28` is never sent. |
| R-15d | `hp_rate` fixed rate leaves SEID 1 at 44.1 kHz/bp 53 | **Accepted** | Both SEIDs overwritten (§10.2). |
| R-15e | `hp_credits` is a compile-time constant | **Accepted** | `BT_CTRL_H2C_ACL_CREDITS`; separate bench builds (§7, §10.2). |
| R-15f | Screen-off gate drops AVRCP presses | **Accepted** | `openfw_play_file_ui` flushes keys when `!show_player`. §6.6. |
| R-15g | AVRCP scrub ends at once (physical-key end test) | **Accepted** | Scrub-end condition in `openfw_play_file_ui`. `scrub_src` (§6.6). |
| R-15h | Player has only a play/pause toggle | **Accepted** | `include/openfw_player.h`. State-aware mapping (§6.6). |
| R-15i | "PLAY key on home" does not exist; default focus is Files | **Accepted** | Home ENTER with `home_focus` 0 plays the last file. §6.6 now calls that action directly. |
| R-15j | Volume label is 0-10, `vol_0_31` vs `bt_vol` 0-127 vs 32-entry gain table | **Accepted** | `format_vol_label` in `src/ui/ui_screens.c`. Mapping defined (§6.6, §9). |
| M-1 | `esp_a2d_audio_buff_alloc` may return NULL | **Accepted** | Allocate before pulling PCM (§4.3). |
| M-2 | No session stop on USB MSC / power-off / phone entry; no card-removal detection | **Accepted** | `ui_poll_power_button`, `run_usb_msc_mode`, `run_bt_xrec_mode` in `main.c`; no card-detect code. §6.7. |
| M-3 | Rejected START hangs the player; BUSY on overlapping media ctrl; STOP unhandled | **Accepted** | `btc_a2dp_control_media_ctrl`. Serialisation and `START_FAILED` (§4.5). |
| M-4 | Sink-initiated START → stack auto-suspend → false remote SUSPEND | **Accepted** | STARTED state handler in `btc_av.c`. Unasked STARTED handling (§4.5). |
| M-5 | ROM link-end flush reports never-aired packets as NOCP | **Accepted** | ROM map "Link-end finalisation" row. Snapshot adds back ≤ 4 packets (§4.4). |
| M-6 | `aout_start` −1 wipes `resume_sec` silently | **Partly accepted** | `resume_sec` is kept (the player does not touch it before returning; `main.c` copies it back), but `unplayed` is cleared and no toast is shown. Toast + skip the clear (§6.7). |
| M-7 | Stale tail after BACK; REC from Home bypasses the suspend | **Accepted** | `stop()` now flushes and drops (§3.2); REC suspend in the main-loop switch branch (§6.9). |
| M-8 | 8 KiB bench log loses long-run summaries; µs counters wrap | **Accepted** | `bt_bench_fw.c` capture. Reserved summary, slower status, ms counters (§10.2). |
| G-1 | H0 breaks `make BT=1` (`SBC_Encoder_Init` in the root script); generator needs `IDF_PATH` | **Accepted** | `tools/check_bt_link_roots.sh` internal list; `PORTING_NOTES.md`. §5.3, §13. |
| G-2 | H3 cannot pair without key staging/RNG feed from `bt_xrec_fw.c`; `bt_port_init` single-shot | **Partly accepted** | Helpers moved early into `bt_session_plat.c` (§13 H3). Rejected part: `bt_port_init` is safe to call again (returns 0, replaces the callback). |
| G-3a | S1/S13 fail as specified | **Accepted** | Same root cause as R-1. |
| G-3b | S7 "a2d_drop counted" always 0 | **Accepted** | R-9. |
| G-3c | S9 fails on TG RN | **Accepted** | R-2. |
| G-3d | `FAKE_ROM_LOG_MAX` 512 overflows in streaming steps | **Accepted** | `tests/fake_rom.h`. Drain per step (§8.2). |
| G-3e | UBSan alignment on `btc_msg_t.arg` (offset 4) | **Accepted** (watch item) | Layout confirmed in `btc_task.h`; no hit reproduced (phone scenario passes). Fix pattern noted (§8.2). |
| G-3f | AVDTP stores pointers in `UINT32` | **Rejected** (already in rev 2) | §8.2 "Pointer width" already required ASan under `-no-pie` first; sites now named. |
| G-3g | Host build must pass its own gate list | **Accepted** | §8.2. |
| G-4a | Firmware Makefile has no `-MMD` | **Accepted** | Explicit header deps in H6. |
| G-4b | `wasm` does not build `BT=1` UI | **Accepted** | Stub or drop the claim (§9). |
| G-4c | `run_qemu.py` has no `--expect-log` | **Rejected** | It has: `--expect-log` (append) in `tools/run_qemu.py`, with its own run budget. |
| W-1 | Remove the holdover buffer | **Accepted** | Adopted (§4.3). |
| W-2 | Fast local AV close while disabling; AVRC first | **Accepted** | Patch E, shape decided in H5 (§15.6). |
| W-3 | Track real frames, snapshot before rebase | **Accepted** | Adopted with a packet-record FIFO (§4.4). |
| W-4 | Move H4 ahead of H3; Just Works patch mandatory | **Partly accepted** | Patch D mandatory: yes. Reorder: no; only the pairing helpers and store reset move early, so the silicon spike stays early and H4 stays one reviewable refactor. |
| W-5 | Drop TG RN from v1 | **Accepted** | Adopted. |
| W-6 | Escalate a stall after ~1 s to a controller reset; stop the tick in BTC | **Partly accepted** | BTC stop adopted. Escalation after 8 s, not 1 s: `bt_ctrl` answers a silent `0406` only at its 7 s forwarding deadline, and a 1 s bound would restart the controller on normal slow detaches. |

**Points for a human decision** (also in §15): (1) patch D in v1 reverses rev 2's recommendation 2; (2) the shape of
patch E; (3) restarting the controller on a stall without Disconnection Complete; (4) the review's H4-before-H3
reorder, declined here in favour of moving only the pairing helpers.

## 19. Lead decisions on rev 3 open points (2026-11-25)

1. Patch D (pre-confirm hook for Just Works) is **mandatory in v1** — without it headsets bypass the pairing window, the entropy gate and the key-burn rule.
2. Patch E: close the AV stream locally inside BTA during disable (no peer round trip), with a forced `btc_av` state reset at deinit as the backstop; S1/S13 must pass a second full init cycle.
3. A controller restart after a credit stall with no disconnect event (8 s) is accepted.
4. Milestone order stays H3 (silicon spike) before H4 (session-core refactor); only the store reset and key-staging helpers move early.
5. The Codex review (`docs/reviews/A2DP_DESIGN_REVIEW_CODEX.md`) was obtained after rev 3 was written; its 12 findings overlap rev 3 on Just Works, teardown, AVRCP notifications and position tracking. The remaining ones (SBC frame geometry from the accepted config, enqueue-failure propagation and packet retention, acquire/release ring indices with consumer ack, pacing µs→frames remainder, START/SUSPEND failure transitions, RAM for FIR taps in flash, H3 throughput gate with speech-bitpool override disabled) are to be applied to the implementation in the follow-up pass and recorded in §18.

### 19.6 Live log: the §19.5 follow-up items at `integ/bt-all-local` (2026-11-25)

Audit of be8db84 plus the merge review of `integ/bt-all-local` (triage and fixes: `BT_A2DP_HP_SESSION.md`,
"Third review round"). How the §19.5 items landed, deviations included:

- **SBC geometry** from the negotiated CIE (applied again at CONNECTED, session deviation 4). **Audio MTU:** the
  connection event's `audio_mtu` already excludes the RTP header and the 1-byte SBC media header
  (`btc_av.c` `BTC_AV_AUDIO_MTU_RESERVE`); the audit's "one byte over the MTU" was a false positive, frames may fill
  it exactly (`test_hp_tx` checks an exact multiple).
- **Enqueue failure:** patch 0034 reports it and the buffer stays ours. Deviation: the refused packet is not held
  and resent; it is freed and its real frames count as unheard (a rewind replays them). The tick no longer pauses
  until a STARTED that an already started stream never gets: it goes on, and refusals lasting `stall_ms` raise the
  stall event (rewind snapshot, ACL close, reconnect).
- **Ring indices:** acquire/release; after a flush the consumer's copy is validated by a seqlock-style flush
  generation check instead of waiting for a consumer acknowledgement (TSan stress, `BT_A2DP_H0_H1.md`).
- **µs → frames** carried exactly (credit in µs × frames). **START failures:** one retry, then final, 2 s cooldown.
  **SUSPEND refused:** retried up to 3 times 1 s apart while no new audio or demand arrived; REC → disconnect
  fallback still open (Exp 4h).
- **Local SUSPEND** no longer stops the tick first (§4.5): the stack's flushing flag makes `can_send` fail, which
  also keeps a rejected SUSPEND's stream fed.
- **FIR taps** in flash (`.rodata` in XIP). **H3 throughput gate:** the bench line now prints the bitpool on air
  (`bp_eff`) and payload bytes per second (`Bps`); a throughput run at the max bitpool needs `hp_speech_bp=0`
  (`BT_HW_RESULTS.md` recipe).
- **Patch E backstop** (forced `btc_av` state reset at deinit, §19.2) not in yet: deferred, only a headset that
  freezes during a stop reaches it (patch 0039 already closes a stream stuck closing).
