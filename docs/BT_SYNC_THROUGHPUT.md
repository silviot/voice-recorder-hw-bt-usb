# BT sync throughput: why phone mode tops out near 39 KB/s, and how to raise it

Status: **measured on silicon 2026-11-26 (section 6); the winning TX path is the default of every BT=1 image** since
`feat/sync-throughput-defaults` (coalesced 990 B writes, BTC ring 3960, TX ring 8192, BT screen paced to 500 ms during
a transfer): ~39 -> ~51-58 KB/s on the Pixel 8 (bench, section 6). The next lever is EDR (section 7). Analysis and bench instrumentation
from `exp/sync-throughput` (off `main` 0f407ce). Companion to `docs/BT_SYNC_SPEED_AND_RECORDING.md` (A1 = 64 KiB GETS
windows, F2/F3/F5).

## 1. The data (silicon 2026-11-25 23:29, bench image `main` 0f407ce, Pixel 8 within 30 cm)

| Item | Value |
|---|---|
| App | `REC_0012.wav`: 15,953,580 B in 402.4 s = **39 KB/s**, 0 retries, 244 GETS x 65536 B, chunk 640 |
| Device GETS bytes | 5,330,952 (t~180 s) -> 11,163,656 (t~330 s): **38.9 KB/s** steady |
| `tx` (= `esp_spp_write()` calls, not ACL packets: `bt_xrec_get_status`) | ~2300 / 30 s = **77/s**, so **~505 B per write** |
| CPU (150 s windows, permille) | idle 223-227, app 401, BTU 134-139, bt_ctrl 112-117, hciT 58-60, BTC 32-34, bt_dlv 21-22 |
| CPU at t=30 (almost no traffic) | idle 481, **app 417**, bt_ctrl 73, BTU 12 |
| Same phone, Bluedroid `feat/bt-step9`, 3840 B stop-and-wait windows | **53-67 KB/s** on 8-10 MB files |

The last row is the key fact: with 3912 B bursts and 40-60 % idle turnaround the same phone and the same Bluedroid TX
path moved **more** data than with continuous 64 KiB windows. Inside a burst the path ran at roughly 90-150 KB/s, so
neither the air link nor the phone is the hard limit; something that **refills only during idle gaps** throttles a
continuous stream. Per GETS window: 64 KiB / 39 KB/s = 1.65 s of streaming, the turnaround is < 5 %: A1 removed the
stop-and-wait cost as intended, and the remaining ceiling is in the streaming path itself.

## 2. The TX path, stage by stage

1. **xrec_proto GETS pump** (`src/xrec_proto.c:1640-1700`): per frame one `exfat_read_file()` of 640 B (no read-ahead,
   F3), CRC-16, a 12 B header, `ops.send()` of 652 B, all-or-nothing. App task (priority 1, the lowest).
2. **Session TX ring** (`bt_port/bt_xrec_bluedroid.h:29-32`): `BT_XREC_TX_RING` 2048, `BT_XREC_SPP_TX_BUF` 2048 (BTC
   ring and in-flight cap), `BT_XREC_WRITE_MAX` 990. `xo_send()` copies a frame and calls `tx_kick()` at once
   (`bt_xrec_bluedroid.c:757`), so writes are one frame each, **cut in two at the ring wrap**
   (`tx_kick`, `bt_xrec_bluedroid.c:646`): 2048 / 652 = 3.14 frames per wrap gives ~4 writes per 2048 B = **~512 B per
   write**, which is exactly the 505 B measured.
3. **BTC** (`btc_spp.c:1876` `spp_send_data_to_btc`, `:511` `spp_drain_tx_under_lock`): bytes go into a 2048 B byte
   ring; the drain takes **one contiguous piece up to the peer MTU** (`xRingbufferReceiveUpTo`, `:522`; cut again at
   *this* ring's wrap) and posts it to BTU; **only one piece is in BTA at a time** (`is_writing`). The next piece
   leaves when `BTA_JV_RFCOMM_WRITE_EVT` comes back to BTC (`:1461`).
4. **BTA JV / RFCOMM port** (`bta_jv_act.c:2426-2452`, `port_api.c:1518-1624`): `PORT_WriteDataCO()` turns each piece
   into **its own RFCOMM frame** (no coalescing), queues it, and reports WRITE at once (synchronous). The port stops at
   `PORT_TX_BUF_HIGH_WM` = 10 frames (`bt_target.h:2292`) -> `lower_cong` -> BTC stops draining -> BTC ring full ->
   `ESP_ERR_NO_MEM` -> session `cong`.
5. **RFCOMM credit flow control**: one peer credit per frame; `credit_tx == 0` sets `tx.peer_fc` and the port waits
   for the phone's credit frame (`port_rfc.c:1013-1035`, `:1131-1133`). Credits are per **frame**, whatever its size:
   505 B frames move half the data per credit that 990 B frames would.
6. **L2CAP -> HCI**: one RFCOMM frame (<= 990 + 10 B) = one ACL packet (controller ACL length 1021). Host ACL window
   **4 packets** (`bt_ctrl_cmds.h:30`), returned by NOCP through ROM -> bt_ctrl -> bt_dlv -> hciT -> BTU.
7. **ROM baseband**: in phone mode the device is the **slave**: it transmits only when the Pixel polls it; the master
   picks the packet types and max slots. The ROM advertises 3/5-slot and EDR 2 Mb/s ACL (`BT_CTRL_FEAT_P0_AND_MASK`,
   opcode table §8) but not role switch, and `080B` (Switch Role) and `040F` are REJ: the role and packet types
   cannot be changed from our side.

What 77 writes/s of ~505 B means: ~77-100 RFCOMM frames/s (BTC can merge pieces up to 990 B), i.e. ~77-100 ACL
packets/s. Air time per ~515 B ACL packet as slave: one 2-DH5 (679 B) or two DH5/DH3 (basic rate) plus the master's
POLL, ~3.75-6.25 ms -> 160-266 packets/s possible. **The air link is used at well under half its capacity.**
BT CPU per packet (BTU+hciT+BTC+bt_dlv+bt_ctrl above their idle values) is ~270 ms/s / ~80-100 pkt/s =
**~3 ms of CPU per ACL packet**: Bluedroid runs from XIP flash without a cache (AGENTS.md). With 22 % idle the CPU is
not saturated, but every packet crosses five tasks.

The app task's ~40 % is constant, traffic or not: the BT-mode loop (`src/main.c`, `run_bt_xrec_mode`) wakes every
1 ms (`OPENFW_TASK_WAIT` = `ulTaskNotifyTake(1 tick)`, `src/rtos/rtos_port.c:50-58`), and spins without waiting while
`tx` changes. It runs at priority 1, so it takes time from IDLE, not from the BT tasks; it costs power, and is
measured (not assumed harmless) by `BENCH tl` below.

### Ranked hypotheses

| # | Hypothesis | Signature in the new lines | Fix |
|---|---|---|---|
| H1 | **RFCOMM credit starvation**: the phone grants k credits and returns them only after its app reads; small frames waste them; bursts never ran out, a continuous stream does | `tq pc0` and `pfc` high, `pq` high, `crmax` = the phone's window; `tp cong/nomem` > 0, `tq bf/rf` high | full 990 B frames (`coalesce=1 spp_buf=3960`): ~2x per credit |
| H2 | Per-packet latency/CPU through 5 tasks (XIP Bluedroid) with one BTA write in flight | `tq pc0` low, `pq` low, `re`/`starve` low, `bf` high, `rc4` low, `lat` short | bigger frames (fewer packets); later: SRAM for the hot BTU/L2CAP/RFCOMM path, a multi-piece BTC drain |
| H3 | ACL credit starvation (4 x 1021) with slow NOCP (slave polling) | `tq rc4`/`hc0` high, `tp lat` ~ 4 / packet rate, `cr4` high | `ACL_CREDITS=8` image; bigger frames |
| H4 | Producer (app loop / SD reads) cannot keep the ring full | `tq starve` high (GETS running, ring empty), `tl gets` large | F3 read-ahead, larger TX ring, loop changes |
| H5 | Air-limited (master grants few slots / basic rate only / Wi-Fi coexistence) | `link slots` < 5 or `f3` without EDR, `lat` long with `rc4` high | 669 B frames (one 2-DH5), nothing on our side for the role |

## 3. Bench instrumentation (bench images only: `make BT=1 BENCH=1`)

Every status group (period `stat_secs`, default 30 s) now logs, after `BENCH status` and `BENCH BT ctrl stats`:

```
BENCH link h=0x00000080 role=S rs=0 slots=5 pkt=0x00000000 mode=0 feat=1 f0=0x000000FF f3=0x000000FE sent=.. done=.. mtu=990
BENCH tp ms=30001 Bps=38912 wr=2300 wB=505 wev=.. cong=.. nomem=.. busy=.. rx=.. acl=.. aB=.. nocp=.. lat=.. latmax=.. cr4=.. co=.. cof=.. wrap=..
BENCH tq n=15000 on=.. gets=.. re=.. rf=.. starve=.. bf=.. cg=.. pc0=.. pfc=.. pq=.. pqmax=.. crmin=.. crmax=.. hc0=.. rc4=..
BENCH tl it=.. xrec=.. gets=.. xn=.. rec=.. ui=.. wait=.. loop=..
```

`link` (once when the SPP link is up, and at the end): bt_ctrl's view (`bt_ctrl_link_info`): role, role changes, last
Max Slots Change, last packet-type change (expect 0: `040F` is REJ), the phone's page-0 features (`f0` bit 0/1 =
3/5-slot, `f3` bit 1/2 = EDR 2/3 Mb/s), ACL sent/completed, RFCOMM frame size. The packet types the master actually
uses on air are not visible to the host (would need the ROM's LC state, not mapped).

`tp` (counter deltas over the window):

| Field | Meaning |
|---|---|
| `Bps` `wr` `wB` | bytes/s handed to `esp_spp_write()`, writes, average write size |
| `wev` `cong` `nomem` `busy` | `ESP_SPP_WRITE_EVT`s, `ESP_SPP_CONG_EVT`s, writes refused by BTC (ring full), frames refused by the session ring |
| `rx` | SPP data events from the phone (GETS commands) |
| `acl` `aB` | ACL packets handed to the ROM and their average payload (L2CAP+RFCOMM+data): **the real packet size** |
| `nocp` | NOCP messages from the ROM |
| `lat` `latmax` | ACL handover -> NOCP latency, mean and max (us; `bt_ctrl` FIFO over all links: exact with one link) |
| `cr4` | permille of the window with all ACL credits in the ROM |
| `co` `cof` `wrap` | coalesce holds, 5 ms forced short writes, writes cut by the ring wrap (`coalesce=0`) |

`tq` (a 2 ms FreeRTOS timer in the timer task, so the samples are uniform in time; permille of the samples with the SPP
link open unless noted): `on` (link open, of all samples), `gets` (a GETS stream runs), `re` (session ring empty),
`rf` (ring has no room for a frame), `starve` (GETS running and ring empty = producer behind), `bf` (in-flight cap
reached = BTC ring full from our side), `cg` (congested), `pc0` (RFCOMM `credit_tx == 0`), `pfc` (port flow-stopped),
`pq` (RFCOMM port TX queue not empty), `pqmax` (bytes), `crmin`/`crmax` (peer credits seen), `hc0` (L2CAP thinks all
controller buffers are used), `rc4` (the ROM holds all ACL credits). The RFCOMM/L2CAP reads are lock-free plain loads
(`bt_port/bt_bench_probe.c`).

`tl` (app task BT-mode loop, wall-clock us per second, includes preemption): `it` iterations/s, `xrec`
(`bt_xrec_poll`), `gets` (inside it: `xrec_poll`, i.e. SD read + CRC + framing), `xn` (`xrec_poll` calls/s), `rec`
(`recorder_poll`), `ui` (the 150 ms block), `wait` (`OPENFW_TASK_WAIT`), `loop` (whole iterations).

Log budget: the capture keeps the first 8 KiB (`.bss_hi` has ~0.5 KiB left, it cannot grow). One group is ~700 B,
the BT vbat/mem/stack lines ~270 B per 30 s, the cpu/tasks/meas block ~650 B about every 150 s. **Use `secs=200
stat_secs=50`** (four groups) or `secs=240 stat_secs=60`.

### New `SD:/BTBENCH.RUN` keys (phone mode; refused with `mode=hp`)

Defaults changed on 2026-11-26 to the release TX path (section 6); `coalesce=0 spp_buf=2048 tx_ring=2048` is the old
path for A/B. Experiments E0-E5 below ran with the old defaults (E0 = no keys).

| Key | Range (default) | Effect |
|---|---|---|
| `ui` | 0/1 (1) | 0: the BT-mode loop skips telemetry/status/render (charge management stays) |
| `smp` | 0/1 (1) | the 2 ms sampler (0 = off, to A/B its own cost) |
| `coalesce` | 0/1 (1; was 0) | send only full `spp_wr` writes while a GETS keeps producing (staged copy: never cut at the wrap); shorter writes when the producer is idle/blocked, for text replies, or after 5 ms. 0: one write per frame, cut at the ring wrap |
| `spp_wr` | 64..990 (990) | largest `esp_spp_write()` = largest RFCOMM frame (also capped at the peer's RFCOMM MTU) |
| `spp_buf` | `spp_wr`..9900 (3960; was 2048) | BTC ring (`esp_spp_cfg_t.tx_buffer_size`) and in-flight cap; use a multiple of `spp_wr` with `coalesce=1` so BTC pieces stay whole |
| `tx_ring` | 2048/4096/8192 (8192; was 2048) | session TX ring (every image allocates 8192) |
| `stat_secs` | 10..300 (30) | status group period |

Build option (bench only): `make BT=1 BENCH=1 ACL_CREDITS=n` (4..9, default 4): the 1005 ACL buffer count bt_ctrl
reports and enforces.

## 4. Experiment plan (no user hands: USB flash + SD file + reboot + adb "Sync now")

Each run needs a file the phone has not synced yet (copy a 10-16 MB WAV onto SD under a new `REC_NNNN.WAV` name over
USB MSC first, or delete the app's copy). Phone: Bluetooth on, app open, then `Sync now` once the device is in BT
mode (~5 s after boot); wait for the run to end (`secs`). Pull `BTBENCH.LOG`; read the app's per-file line from logcat.

| # | `BTBENCH.RUN` | Decides |
|---|---|---|
| E0 | `secs=200 stat_secs=50` | Baseline with the new lines. H1 if `pc0`/`pfc` >= ~300 with `pq` high; H3 if `rc4`/`hc0` high and `lat` ~ 4/(acl/s); H4 if `starve` high; H2 if none of these and `bf` high; `link` gives slots/EDR (H5) |
| E1 | `secs=200 stat_secs=50 coalesce=1 spp_buf=3960 tx_ring=8192` | **The main fix candidate.** `aB` should go to ~1000 and `Bps` up ~1.5-2x if H1/H2/H3 |
| E2 | `secs=200 stat_secs=50 ui=0` | App-loop share (`tl ui`, `tasks app`); throughput change expected ~0 |
| E3 | `secs=200 stat_secs=50 coalesce=1 spp_wr=669 spp_buf=2676 tx_ring=8192` | One 2-DH5 per ACL packet (EDR fit) vs E1: better only if air-limited (H5) |
| E4 | `secs=200 stat_secs=50 spp_buf=9900 tx_ring=8192` | Deeper buffering alone (no coalesce): helps only if H4/bursty producer |
| E5 | E1 with the `ACL_CREDITS=8` image | Only if E0/E1 show `rc4`/`hc0` high (H3) |
| E6 | `secs=200 stat_secs=50 smp=0` | Sampler cost check (compare `tp Bps`, `tasks`) |

Phone-side alternative if `crmax` is small (H1): nothing to change on the phone's Bluetooth settings; Android's RFCOMM
credit window is fixed by its stack, so the device must send bigger frames.

## 5. Likely code-level fixes (expected gain)

1. **Full-MTU RFCOMM frames by default** (what `coalesce=1 spp_buf=3960` tests): staged 990 B writes in `tx_kick`,
   `BT_XREC_SPP_TX_BUF` = 4 x 990, TX ring 4096-8192 (~6 KB heap). Halves packets, credits, and per-packet CPU:
   **~1.5-2x (60-75 KB/s)** if H1/H2/H3. **Done (section 6): +39 % measured (then air-limited, H5).**
2. **F5 (`gmax`)**: GETS chunks up to `mtu - 12` = 978 B so the protocol frame itself is 990 B (no staging copy; 0.6 %
   less header/CRC overhead). Needs the app.
3. If H2 remains after 1: let BTC hand several pieces per BTA round trip (patch `spp_drain_tx_under_lock` /
   `PORT_WriteDataCO` to fill to the high watermark), and/or move the BTU/RFCOMM/L2CAP/hciT TX path into SRAM
   (`.bt_fast`): ~3 ms -> < 1 ms CPU per packet.
4. If H3: `ACL_CREDITS` 6-8 (EM TX pool is 9; needs a fault-injection pass and the A2DP clamp review).
5. Power, not speed: let the BT-mode loop block on the notification instead of a 1 ms poll, and skip the render
   while the screen is off (the app task's constant ~40 %). **Render pacing done (section 6); the 1 ms poll stays.**

## 6. Silicon results (2026-11-26 00:00-00:33) and the decision

Bench image from `exp/sync-throughput` c60b5c1 (old defaults), Pixel 8 syncing with 64 KiB GETS windows, 50 s status
windows, `secs=200 stat_secs=50`. The device is the slave. Logs: `.build-scratch/tput/results/<E*>/BTBENCH.LOG`
(outside the repo). Steady-state windows (the first window includes the connection and is lower); KB = 1000 B:

| Run | Knobs | `tp Bps` | `aB` (ACL avg) | `lat` (ACL->NOCP) | `cr4` / `rc4` | `pc0` | `tl ui` |
|---|---|---|---|---|---|---|---|
| E0 baseline | none (old defaults) | **38.4-39.4 KB/s** | 544 B | 38 ms | 562 / ~575 permille | ~455 | 160 ms/s |
| E2 | `ui=0` | **42-43 KB/s** (+9 %) | 544 B | 36 ms | ~565 | ~470 | 60 ms/s |
| E1 | `coalesce=1 spp_buf=3960 tx_ring=8192` | **51-58 KB/s** (mean 54, +39 %) | 855-860 B | 45-52 ms | 612-642 | 460-500 | 158 ms/s |
| E1 + E2 | E1 + `ui=0` | **55-57 KB/s** (mean 56, +44 %) | 856-883 B | 49 ms | 656 | ~505 | 61 ms/s |
| E5 | `ACL_CREDITS=8` image, E1 + `ui=0` | 56 KB/s (no gain) | 840-857 B | 46-48 ms | 637 | ~505 | 60 ms/s |
| E5 base | `ACL_CREDITS=8` image, old knobs + `ui=0` | 41-42 KB/s (no gain) | 544 B | 37 ms | 570 | ~475 | 60 ms/s |

Reading:

- **Bigger frames are the main win.** `aB` 544 -> ~860 B: one ACL packet per write (L2CAP + RFCOMM headers included):
  990 B writes that span the 652 B GETS frames, instead of one write per frame, cut in two at the ring wrap. `wrap=0` and `starve=0` with coalescing; holds (`co`) ~280 / 50 s, forced 5 ms writes (`cof`) ~80 / 50 s.
- **Not ACL-credit-limited:** 8 credits instead of 4 changed nothing in either configuration, although all 4 credits
  sit in the ROM ~57-65 % of the time. The NOCP latency grows with the packet size (38 -> 49 ms for 4 packets in
  flight, ~10-12 ms per packet): the air link (slave, polled by the Pixel) is the limit, **basic-rate-like**: a
  ~870 B ACL packet in DH5 needs three baseband packets (3 x 339 B), ~11 ms with the master's polls.
- RFCOMM peer credit 0 (`pc0`) ~46-50 % in all runs (`crmax` 10): the phone returns credits as it reads; with bigger
  frames each credit carries ~1.6x the data.
- The UI block (150 ms period) cost ~100 ms/s of app-task wall time and ~3-4 KB/s; it shares the task with the GETS
  producer (SD read + CRC + framing).
- Free heap minimum with E1 (8192 + 3960 + staging): 28.1-28.6 KB on the bench image (E0: 33.4 KB); `mallocfail 0`.

**Decision (2026-11-26): make E1 + a paced UI the default of every BT=1 image** (release and bench alike):

1. `bt_port/bt_xrec_bluedroid.c` `tx_kick`: coalesced writes of up to `min(990, peer RFCOMM MTU)` bytes, staged in a
   990 B buffer when they cross the ring wrap (never cut by it). A shorter write is held only while a GETS stream keeps
   producing (the producer still has room for a frame) or while the in-flight cap made it short (the next
   `ESP_SPP_WRITE_EVT` frees room), and at most `BT_XREC_COALESCE_MS` = 5 ms (app-task ms clock: 5-6 ms). **Text
   replies are never held**: `xo_send` marks everything up to a non-GETS frame (PONG, OK, STAT, LS lines, ERR, LOG)
   as urgent (`tx_urgent`), so their latency is unchanged. `BT_XREC_SPP_TX_BUF` = 4 x 990 = 3960, `BT_XREC_TX_RING` =
   8192 (+~7 KB heap per session vs. the old 2048/2048, +1.9 KB in BTC while connected; the bench measured 28 KB free
   at the minimum with these sizes, the release image has more because it carries no bench code).
2. `src/main.c` `run_bt_xrec_mode` + `include/openfw_bt_ui_pace.h`: the UI block (status, telemetry, toast, render)
   runs every 150 ms, but every **500 ms while a transfer is shown** (`bt_state 2`); any key press forces it at once;
   nothing is rendered while the screen is off or fading out. Charge management keeps its own 150 ms tick.
3. The bench keys stay; their defaults are the new values (section 3), `coalesce=0 spp_buf=2048 tx_ring=2048` gives
   the old path, `ui=0` still skips the UI block entirely.

Host tests: `test-bt-xrec` [26] (GETS from a real exFAT image: frames merge into 990 B writes across the wrap, the
5 ms bound on an in-flight shortfall, text replies written at once, a 660 B peer MTU, the `coalesce=0` path),
[6]/[6b] adapted to the new sizes; `test-host` `test_bt_ui_pace` (period, forced pass, millis wrap, render gate).
Expected on silicon: ~55 KB/s (E1 + `ui=0` is the upper bound; 500 ms pacing keeps ~70 % of the UI saving).
**Not yet measured on silicon with the default image** (bench image of this branch: rerun E0 with no keys).

## 7. Next lever: EDR

At ~860 B per ACL packet the link moves ~77 ACL packets/s in steady state, which fits basic rate (DH5, 339 B per
baseband packet) and not EDR (2-DH5 carries 679 B, 3-DH5 1021 B; the stock `bt-rfcomm-turbo` reached 92-96 KB/s on
this ROM, above the basic-rate ceiling of ~90 KB/s). Our side cannot pick the packet types today:

- the device is the **slave**; the master (the Pixel) chooses the packet types and polls; `080B` Switch Role is REJ
  and role switch is not advertised;
- `040F` Change Connection Packet Type is gated (REJ in `bt_ctrl`, `docs/BT_CTRL_OPCODE_TABLE.md`): the ROM fails any
  set that differs from its own computed set with `0x11` and is silent in some LC states (`docs/BT_ROM_HCI_MAP.md`
  §1.2 item 6), and Bluedroid has no handler for a failure;
- we advertise EDR 2 Mb/s ACL and 3/5-slot EDR (page-0 bits 25/39/40) and the Pixel's `f3` shows EDR; whether the
  `LMP_packet_type_table_req` (ptt=1) exchange happens and whether the ROM then sends 2-DHx is unknown: the
  packet types used on air are not visible to the host (`BENCH link pkt=0`).

Research needed before any change:

1. **Is EDR on air?** Pixel HCI snoop of a sync (developer option, `adb bugreport`): the Pixel host's `040F` /
   Connection Packet Type Changed and its per-packet ACL sizes; Pixel vendor/controller logs if they show LMP
   (`LMP_packet_type_table_req/res`). Cross-check: an ACL size sweep (`spp_wr=669` = one 2-DH5, experiment E3) gains
   only if EDR is used.
2. **ROM side:** reverse the ROM's LMP handler for `LMP_packet_type_table_req` (accept/reject, the `ptt` flag in the
   LC env) and the LC/EM TX descriptor field that holds the packet type, so a bench probe (like `bt_bench_probe.c`)
   can read the packet type actually sent; map the "computed allowed set" (`0x311CC`/`0x31214`) to know which `040F`
   set would pass.
3. **If the ROM supports EDR but nobody asks for it:** a safe `040F` path (exactly the ROM's set, a fault-injection
   pass for the silent states, a CS failure handler in Bluedroid's `btm_acl`), or a vendor command that sets `ptt`.
4. **If the ROM does not deliver EDR:** clear page-0 bits 25/39/40 (`BT_CTRL_OPCODE_TABLE.md` V-C) so masters stop
   planning for it; the remaining levers are then CPU per packet (SRAM for the hot Bluedroid TX path, section 5.3)
   and F5 (978 B GETS chunks, no merge needed).
