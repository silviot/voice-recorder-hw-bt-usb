# Bluetooth Classic for open_fw — Integration Plan (v3, for joint review)

Branch `feat/freertos-bluedroid` (worktree `../voice-recorder-hw-bt-usb-bt`). Hardware: Smartlink SL6801 `HW_76`
(dual Cortex-M4F @ 96 MHz, 512 KB SRAM, 2 MB SPI NOR XIP, **BR/EDR-only** RivieraWaves `RW-BT` controller in mask ROM,
no BLE). This document is the single source of truth: it is reviewed by Codex and agy until both agree, then executed.
v3 addresses every round-1 blocking item (see §11 for the traceability table).

## 0. Goals, non-goals

Goals (two mutually exclusive runtime modes, selectable in Settings):
1. **Headphones** — A2DP Source (SBC) + AVRCP Target (headset play/pause/skip/volume → player).
2. **Phone companion** — SPP/RFCOMM carrying the existing `+XREC v5` protocol (`docs/XREC_PROTOCOL.md`,
   `open_fw/src/xrec_proto.c`) to the Android app and desktop tools (`tools/bt_sync.py`, `tools/bt_companion.py`).

Quality goal: replace the hand-written host (`bt_spp.c` L2CAP/SDP/RFCOMM, SSP responder in `bt_hci.c`) with a mature
stack and keep every custom line small, table-driven and host-tested.

Non-goals: BLE (no hardware), iOS companion (SPP needs MFi), HFP/HSP/SCO, A2DP Sink, multipoint, LE Audio.

## 1. Current state (on the branch; historical measurements, not acceptance results for the integrated image)

| Item | Evidence |
|---|---|
| FreeRTOS-Kernel V11.1.0 vendored unmodified (`third_party/freertos`), optional `make RTOS=1` | builds zero-warning; default `RTOS=0` image byte-identical to baseline at the same source revision |
| SysTick owned by `startup.c` (1 ms, `g_systick_ms`, key scan, experiment watchdog); it calls `openfw_rtos_tick()` → `xPortSysTickHandler()`; SVC/PendSV wired under `OPENFW_RTOS` | `src/startup.c`, `src/rtos/rtos_port.c` |
| App super-loop runs as a FreeRTOS task (16 KB PSP stack) | QEMU: SP inside heap region, UI/SD boot identical |
| heap_5 over `.rtos_heap` = `0x0086B000..0x00880000` (84 KB) | `sl6801_firm.ld`; **not validated on silicon** |
| On-target RTOS self-test: **10/10 pass** under the SL6801 QEMU model | `make -C open_fw test-qemu-rtos` |
| Bluedroid (ESP-IDF `ad20ded6`, Apache-2.0) vendored pristine + 3 patches | `third_party/bluedroid`, `third_party/bluedroid_patches/` |
| Standalone Classic-only link (Bluedroid + FreeRTOS + picolibc + compiler-rt + stubs): **278.6 KB flash, 21.4 KB static RAM** (whole standalone link, not an increment) | `PORTING_NOTES.md`, `tools/measure_bluedroid.sh` |
| HCI matrix (static analysis; procedures explicitly incomplete) | `docs/BT_HCI_MATRIX.md` |
| Gates green after merging `main` (i18n, **18 languages**) | `make all`, `make all RTOS=1`, `test-host-san`, `test-qemu-rtos` |

Nothing has run on silicon yet. Legacy BT host code has never shipped to users.

## 2. Key decisions

- **D1 Host stack = Bluedroid.** Rejected: BTstack (licence), Zephyr host, NimBLE (BLE only), hand-rolled host.
- **D2 FreeRTOS confined to CPU0**; CPU1 (LVGL worker) stays bare-metal. DMA audio/USB/SDIO stay ISR/poll driven.
- **D3 HCI boundary = `esp_bluedroid_attach_hci_driver()`** (H4 framed); `hci_layer.c`/`hci_hal_h4.c` stay pristine.
- **D4 libc = picolibc** (pinned) + 6 compiler-rt builtins; `startup.c`'s `__aeabi_mem*`/64-bit division authoritative.
- **D5 Legacy path stays default** until parity: flag `OPENFW_BT_BLUEDROID=1` (implies `RTOS=1`).
- **D6 Upstream pristine**: every change is a documented patch file.
- **D7 One owner for filesystem and XREC session = the app task.** No separate storage worker. BT callbacks only copy
  bytes / post messages.
- **D8 No legacy bond migration.** Legacy slots lack HCI key type and never shipped; every device pairs fresh.

## 3. Runtime architecture (CPU0)

`configMAX_PRIORITIES` raised **8 → 10** so upstream offsets land where we want them:

```
 priority  task / context              owns / does
 ISR       DMA audio, USB, SDIO        unchanged; never call FreeRTOS APIs (priority above MAX_SYSCALL)
 ISR       IRQ26 RW-BT top half        explicit NVIC prio (numerically >= configMAX_SYSCALL_INTERRUPT_PRIORITY);
                                       ack, count per-bit occurrences, vTaskNotifyGiveFromISR(bt_ctrl) +
                                       portYIELD_FROM_ISR; code in .fast_ramtext (NOT .ram_boot: 172 B left)
 ISR low   SysTick                     g_systick_ms, keys, exp watchdog, xPortSysTickHandler
 9         FreeRTOS timer daemon       osi alarm / esp_timer callbacks: bounded, never block (see M2)
 8         bt_ctrl (controller owner)  ALL RW-BT ROM/EM calls; cmd/ACL TX queues; RX packing into bounded rings
 7         hciT (upstream configMAX-3) Bluedroid HCI host
 6         bt_dlv (delivery)           pops bt_ctrl RX rings → host callback (may block on host queues; bt_ctrl never does)
 5         BTU  (upstream configMAX-5)
 4         BTC  (upstream configMAX-6)
 1         app (legacy super-loop)     UI orchestration, recorder, player, USB poll, power, ALL filesystem,
                                       XREC session, bond-store flush
 0         idle                        reaps deleted tasks; DSB; WFI
```
`openfw_rtos.h` priorities updated to match; static assertion that upstream offsets resolve to 7/5/4.

Rules:
- (a) Only `bt_ctrl` touches ROM/EM; others enqueue. (b) No BT callback touches exFAT, recorder, DAC or UI state; it
  posts to the app. (c) No allocation in ISRs. (d) IRQ-26 priority set with a new `nvic_set_priority()` helper
  (`sl6801_regs.h`), verified by reading back IPR; `configASSERT` in `vPortValidateInterruptPriority` stays on.
  (e) No FreeRTOS API call while `bt_irq_lock()` (PRIMASK) is held: `assert(g_irq_nest == 0)` at every
  FreeRTOS call site inside ROM callbacks (`eif_write`, notify paths); PRIMASK regions measured.
- **App-task sleeping**: under `OPENFW_RTOS`, every `OPENFW_WFI()` executed in task context (`wfi_sleep_ms`, USB/BT
  loops in `main.c`, `player.c`, `cpu1_ui_sync`) becomes `vTaskDelay`/`ulTaskNotifyTake` via one helper
  `openfw_sleep_ticks()`; only idle executes WFI. This lets idle run and reap deleted Bluedroid tasks.
- **SysTick before scheduler**: `openfw_rtos_start()` calls `systick_init()` before `vTaskStartScheduler()`;
  the later call in `main.c` becomes idempotent (no reprogramming) under `OPENFW_RTOS`.

## 4. Memory and flash budget

Flash: legacy image 426 KB + standalone BT link ≤ 279 KB (includes FreeRTOS/picolibc) ≈ **≤ 705 KB of 2 MB**.

SRAM (RTOS build): main `.bss` ends ≈`0x84EA18` (~5.5 KB left — nothing new goes there); `.ram_boot` 172 B left;
`SRAM_HI` ~19 KB left.

| Consumer | Bytes | Placement |
|---|---|---|
| Bluedroid/OSI/picolibc `.bss`/`COMMON` | ≤ 21 KB (measure) | `.bt_bss (NOLOAD)`, zeroed in `startup.c` |
| Bluedroid/OSI/picolibc `.data` | small (measure) | **loadable** `.bt_data > RAM AT > XIP`, copied in `startup.c`, added to `objcopy -j` in `Makefile` |
| `g_bt_heap` (ROM-required, already in main `.bss`) | 8 KB | unchanged (counted) |
| FreeRTOS idle + timer static stacks/TCBs | ~2.2 KB | currently main `.bss` → move to RTOS region |
| App task stack | 16 KB | heap (measure; trim only after silicon watermark) |
| BT stacks: hciT 2 + BTU 4.25 + BTC 3 + bt_ctrl 2 + bt_dlv 1.5 | ~12.75 KB | heap; host + silicon watermarks |
| bt_ctrl RX/TX rings (H4 copies, reserved CC/CS/NOCP slots) | ~6 KB | static in `.bt_bss` |
| OSI objects, HCI cmd/ACL buffers, config cache | 8–20 KB | heap |
| SPP mode RFCOMM queues + XREC RX stream buffer | 4–20 KB | heap |
| A2DP mode encoded frames + PCM ring (100 ms @48 kHz) | ~26 KB | heap |
| **Worst case** | **~105–120 KB** | exceeds the 84 KB high region |

Linker rules: Bluedroid is built into `libbluedroid.a` (plus `libbt_port.a`, `libc.a`); the new sections select input
explicitly (`libbluedroid.a:*(.bss .bss.* COMMON)` etc.) and appear **before** the catch-all `.data`/`.bss`; heap bounds
are disjoint from every section; `ASSERT`s on each region. The step-1 link calls the real entry points
(`esp_bluedroid_init/enable`, `esp_spp_*`, `esp_a2d_source_*`, `esp_avrc_*`) from a reference table so
`--gc-sections` cannot produce a deceptively small image.

Expansion (only after Exp 0 validates on silicon):
1. **Primary: lower region `0x805C00..0x81A37F` (~81.9 KB)** as a heap_5 region (heap_5 regions in ascending address
   order: lower first). Unused by open_fw (stock FIRM code/data), ROM/BootROM use `0x800000..0x804BFF`.
2. Non-dev builds: reuse `EXP_ARENA`/`EXP_OUT` (48 KB) when experiments are compiled out.
3. **Deferred**: MSP reclaim (`0x867000..0x86AC08`). Only after nested-interrupt/fault/pre-scheduler MSP usage is
   measured with margin on silicon.

Acceptance budget per mode: minimum free heap under traffic ≥ 8 KB and largest free block ≥ largest single allocation
(asserted in host tests and logged on silicon).

## 5. Modules

### M1 — Controller shim `bt_ctrl`
- Files: `src/bt_ctrl.c` (task, rings, lifecycle), `src/bt_ctrl_cmds.c` (opcode table), `src/bt_ctrl_evt.c` (event
  packers), `include/openfw_bt_ctrl.h`. ROM access through `bt_rom_if_t` so the same code runs against a **fake ROM**.
- **Opcode policy (two classes):**
  - *Mandatory-success* (startup/enable; parsers `assert(stream != NULL)` on nonzero status, or BTA enable stalls):
    `0C03, 1005, 0C31, 0C33, 0C35, 1001, 1009, 1002, 1004 (pages 0–1), 0C56, 0C01, 0C14` and the BTA setup writes.
    Each is `FORWARD` (verified ROM reply packer) or `SYNTH` with status 0 and coherent content; never `REJECT`.
    `0C35` produces **no** reply. A unit test proves every mandatory opcode yields success-path parsing in Bluedroid.
  - *Optional*: `REJECT` with correctly sized Command Complete / Command Status, status `0x01`.
  - Feature masking (no sniff, role switch, SC, unverified EDR types) reduces but does not prove absence of optional
    commands, hence the REJECT class. No opcode is silently consumed.
- **RX path (backpressure never reaches the ROM owner):** `bt_ctrl` copies each complete H4 packet into a bounded ring
  before completing the ROM `eif_write` callback (copy-before-ack). Separate event ring with reserved slots for
  CC/CS/NOCP; ACL ring sized from the host buffer count we program via `0C33`, enforced by controller→host flow control
  in the shim (no ACL sent to host without a host credit). `bt_dlv` delivers to the host callback and is the only
  context that may block in Bluedroid's queues. Ring overflow is impossible by construction for ACL; for non-reserved
  events it increments a counter and triggers a controlled stack restart (not silent drop).
- **TX admission (driver `send()` returns void):** `check_send_available()` returns true only after reserving a TX
  slot for the single caller (hciT); `send()` consumes the reservation and cannot fail (assert). Slot release →
  `notify_host_send_available`. Command credit = 1; ACL credits from NOCP.
- **Reset**: real controller reset inside `bt_ctrl`, then synthesized CC; same path on re-enable.
- **Disable/teardown order**: Bluedroid disable + profile close completions run with transport alive → callbacks
  quiesced (session id bumped, late callbacks rejected) → admission closed → controller stopped, reset completion
  checked (status reported, not ignored) → IRQ 26 masked → rings freed → tasks deleted (idle reaps).
- **IRQ multiplicity**: per-source counters; bottom half loops until the status register reads clear.
- **Local identity**: stable per-device BD_ADDR: eFuse if present; else derived (SHA-256) from the SPI-NOR unique ID
  (verified readable in Exp 1); else generated once from the DRBG and persisted; never timer-derived.

### M2 — Platform shims (`bt_port/`)
- **Entropy/DRBG — honest scope.** Classic-only Bluedroid never calls `esp_random`; P-192 SSP keys/nonces are
  generated inside the ROM. Therefore:
  1. **ROM RNG trace (before any pairing):** disassemble the ROM SSP path (`rom_lm_sec_p192_init` and callers) to
     identify every RNG hook it uses; `bt_rand80` keeps its `0..79` contract. Covered hooks get DRBG output; if the
     ROM uses an internal non-hookable RNG, document the residual risk explicitly.
  2. DRBG (HMAC-DRBG SP 800-90A, SHA-256 conditioning, SP 800-90B repetition-count + adaptive-proportion health tests)
     feeds: XREC `CHAL` nonces, XREC PINs, confirmed ROM hooks, `esp_random` (defensive).
  3. **Fallible API**: `bt_entropy_get(buf,len)` returns an error before credit; XREC challenge/PIN generation fails
     closed (no fixed fallback). New pairing and XREC key creation unavailable while not credited; UI shows
     "Preparing Bluetooth" / failure; recording unaffected.
  4. Seed file on SD is mixed in as additional input but earns **no** entropy credit (removable/replayable).
  5. Candidate sources (ADC LSBs, SARADC/KADC noise, RSSI/I-Q, clock jitter) get credit only after Exp 2 measures them
     across cold starts and quiet inputs.
- **Store**: `nvs_*` surface used by `osi/config.c` served from an in-RAM KV cache loaded by the app **before**
  `esp_bluedroid_enable()`; `nvs_commit()` posts an asynchronous flush to the app (atomic exFAT staged replace, CRC).
  No SD → RAM-only bonds for the session. Link keys never logged. Trust model documented: keys on MSC-visible media are
  readable/replaceable by anyone with physical access; CRC only detects corruption.
- `esp_timer`/`osi_alarm` → FreeRTOS timers + `us_timer_now()`. Patch (documented) so timer callbacks that post to
  threads use a bounded timeout and count failures instead of `OSI_THREAD_MAX_TIMEOUT`; timer daemon stack watermark
  measured under A2DP load.
- `esp_log` → `openfw_log` with redaction; `esp_ringbuf` stress-tested on host, stream-buffer fallback by patch.

### M3 — Host harness
- Bluedroid + bt_port + FreeRTOS POSIX port on x86-64 under ASan/UBSan (TSan where feasible).
- Two paths: (a) **Bumble** (pinned) as controller + remote host over H4 for protocol scenarios (SSP decisions, SDP,
  RFCOMM, AVDTP/SBC); smoke-tested first. (b) **Through M1**: Bluedroid ↔ `bt_ctrl` ↔ fake ROM (fed by synthetic or
  sanitized traces), since Bumble ignores Host Buffer Size and bypasses M1. Deterministic C tests cover credit
  exhaustion, saturation, delayed/dropped CC, truncated events.
- Scenarios: enable/disable ×1000 leak loop with traffic outstanding; SSP both roles; SPP/XREC transfers with
  congestion during control replies and final OTA ack; A2DP open/start/suspend/close; reconnect storms.
- Traces in the repo are synthetic or scrubbed of link keys **and ACL payloads** (XREC bind frames carry keys).

### M4 — Application integration
- **XREC session (app task owns everything)**: BTC SPP callback copies bytes into a bounded stream buffer and notifies
  the app; the app parses, executes filesystem work, and writes with `esp_spp_write` honoring congestion; the app owns
  queued bytes until `ESP_SPP_WRITE_EVT`; send results are checked (no discarded returns). Disconnect cancels pending
  work and clears authorization. Deferred reboot/OTA, MSC entry, card removal and power-off all go through one
  coordinated `bt_shutdown()` first.
- **Pairing security policy**: phone mode requires an encrypted, MITM-authenticated link (numeric comparison, IO cap
  DisplayYesNo) before XREC `PAIR`; local accept/reject on the device with a 30 s timeout; legacy PIN pairing rejected.
  Headphones mode: Just Works accepted only inside a user-opened pairing window.
- **Client compatibility**: `bt_sync.py`/`bt_companion.py` gain native-SPP detection (SDP for 0x1101; skip HFP
  `SLC_OPEN`) while keeping the stock path. Step-6 gate: Android app and both desktop tools bind, reconnect, transfer.
- **Headphones**: full output-sink abstraction in `player.c` (start, busy, partial write, flush, mute, stop) with DAC and
  A2DP implementations; ring flush on seek/pause/EOF/scrub; mono→stereo; negotiated rate. Verify the vendored
  `btc_a2dp_source` rate conversion before adding any resampler. AVRCP volume tested separately from transport keys.
- **Watchdog**: one function writes the hardware WDT; experiment deadline extension separated. Supervisor feeds only if
  each task with outstanding work made progress within its deadline (idle BTU/BTC with no pending work are healthy).
  Tests: idle bonded phone, intentionally stalled transfer. Player and nested BT loops audited.
- UI strings through i18n for all 18 languages (one batch). Shared-file diffs (`main.c`, `Makefile`, `startup.c`) stay
  hook-sized; `k_exp_api`, `test-host` and `wasm` targets keep compiling under both flag values.

### M5 — Hardware experiments (user runs; tools provided)
- **Exp 0 (SRAM, separate probe image, scheduler off)**: candidate ranges excluded from all allocation in this image;
  pattern-fill `0x86B000..0x880000` and `0x805C00..0x81A37F`; exercise legacy BT bring-up, UI, recording, USB; verify
  patterns; report. Ranges enabled for heap only after a pass.
- **Exp 1 (HCI probe, staged, state-aware)**: stage A reset + known read-only commands; stage B verified writes;
  stage C procedures with a controlled peer and a valid handle. Each row: expected initial reply, terminal event or
  explicit exemption, deadline, recovery action (soft reset → reboot). Candidate KE ids only when ROM disassembly has
  verified destination, payload layout and length — no blind sweep. Also: NVIC priority bits, IRQ-26 latency and
  per-bit multiplicity under load, SPI-NOR unique ID read, sustained-ACL NOCP accounting (stage C).
- **Exp 2 (entropy)**: raw dumps of each candidate source over cold starts and quiet inputs → offline min-entropy.
- **Exp 3 (phone)**, **Exp 4 (headphones: 3 brands, 1 h soak, AVRCP, CPU, UI)**, **Exp 5 (power/USB/card transitions
  with BT active, recovery image check)**.

## 6. Ordered steps and gates

Every step: zero-warning ARM builds (`all`, `all RTOS=1`, flag build once it exists), `test-host-san`,
`test-qemu-rtos`, `wasm` when shared files change; legacy image byte-identical **at the same source revision and
toolchain** until step 9.

1. **RTOS hygiene + build plumbing**: priorities to 10, `openfw_sleep_ticks()` replacing task-context WFI, SysTick
   before scheduler, idle/timer static memory out of main `.bss`, `nvic_set_priority()`; picolibc + `libbluedroid.a`
   under the flag; `.bt_data`/`.bt_bss` sections with explicit input selection + copy/zero + objcopy + ASSERTs;
   reference-table link. Gate: flag build links with real entry points, QEMU boots, self-test passes (add a
   task-delete/reap test), budgets asserted.
2. **Platform shims (M2)** with host tests: DRBG CAVP vectors, health-test triggers, fallible entropy gate, RAM KV store
   + flush with power-fail injection on an exFAT image, timer/log tests.
3. **Controller shim (M1)** against fake ROM: golden vectors per matrix row, mandatory-success proof, credits,
   TX reservation, RX saturation, restart/teardown ordering, fuzzing.
4. **Hardware session 1 (user)**: Exp 0 image, then Exp 1 stage A/B probe, Exp 2 dumps; ROM SSP RNG trace done
   offline in parallel. Shim table + entropy credits refined. *Bluedroid does not run on silicon before this.*
   (Step 5 harness work proceeds in parallel; it does not wait for hardware.)
5. **Host harness (M3)**: both paths green under sanitizers.
6. **Bluedroid on silicon, phone mode**: Exp 1 stage C, pairing policy, XREC with Android app + both desktop tools;
   lifecycle gate (disable/re-enable, MSC during transfer, card removal) before any user test; Exp 3.
7. **A2DP source + AVRCP**: sink abstraction; Exp 4.
8. **UI/settings/i18n, WDT supervisor, power transitions**; Exp 5.
9. **Parity + switch default**; legacy host code deleted in a separate commit.
10. **Joint final review** (Codex + agy).

## 7. Verification strategy

Host unit (fake ROM), protocol (Bumble), through-M1 path, sanitizers, fuzzing of HCI both directions and XREC framing;
QEMU for boot/RTOS regressions; silicon probes before features; every silicon bug becomes a sanitized trace regression;
telemetry: heap/stack watermarks, largest free block, IRQ-26 latency and multiplicity, A2DP underruns, ring overflows.

## 8. Risks

| # | Risk | Mitigation |
|---|---|---|
| R1 | ROM lacks/behaves differently for needed opcodes | two-class opcode policy; staged Exp 1 |
| R2 | SRAM budget | §4 sections; Exp 0 → lower region; per-mode budgets |
| R3 | Weak entropy / ROM-internal RNG | ROM trace; DRBG; fallible gate; documented residual risk |
| R4 | IRQ-26 coalescing / lost service | owner task, loop-until-clear, multiplicity counters |
| R5 | RTOS vs bare-metal code | task-context sleep helper, PRIMASK rule, self-test incl. reap |
| R6 | A2DP CPU budget | measure; bitpool/rate tuning |
| R7 | Storage reentrancy / XREC lifecycle | D7 single owner; coordinated shutdown |
| R8 | `esp_ringbuf` on vanilla FreeRTOS | host stress; stream-buffer fallback |
| R9 | `main` i18n merges | hook-only edits; frequent merges |
| R10 | Interop | Exp 3/4 matrix; real desktop/Android clients in gate |
| R11 | Host backpressure stalls controller | bt_dlv split; flow-controlled ACL; reserved event slots |
| R12 | Bonds/identity on removable media | stable identity; documented trust model; fresh pairing |

## 9. Open questions for reviewers (round 2)

1. Is the `bt_dlv` split (bt_ctrl never blocks; delivery task may) plus host flow control sufficient for R11?
2. Is the reserve-on-check TX admission sound given hciT is the only caller of `check_send_available`/`send`?
3. Is deriving BD_ADDR from the SPI-NOR unique ID acceptable when no eFuse address exists?
4. Any remaining blocker to starting step 1?

## 10. Live log

- 2026-11-24: FreeRTOS port + QEMU self-test (10/10). Bluedroid vendored, Phase 1 GO. Codex architecture review and HCI
  matrix integrated. `main` (i18n, 18 languages) merged. Plan v2 reviewed: Codex and agy both CHANGES_REQUIRED.
  Plan v3 written.

## 11. Round-1 traceability

| Round-1 item | v3 change |
|---|---|
| Codex B1 / agy B5 (Exp 0 overwrites live heap; MSP margin) | Exp 0 separate scheduler-off image; MSP reclaim deferred; lower region primary |
| Codex B2 / agy B1 (`.data` in NOLOAD, GC) | `.bt_data` loadable + copy + objcopy; explicit input selection; reference-table link; ASSERTs |
| Codex B3 (host backpressure, void send, teardown) | bt_dlv; ACL host flow control; reserved slots; TX reservation; teardown order |
| Codex B4 (blind opcode sweep) | staged state-aware Exp 1; KE ids only with verified layout; NOCP in stage C |
| Codex B5 / agy B6 (entropy scope, consent) | ROM RNG trace; honest DRBG scope; fallible API; no seed credit; pairing policy |
| Codex B6 (bond migration, identity) | D8 no migration; stable BD_ADDR; trust model |
| Codex B7 (FS/XREC ownership) | D7; coordinated shutdown; checked sends |
| Codex B8 (client compatibility) | native-SPP detection in tools; real-client gate |
| agy B2 (REJECT on startup reads) | two-class opcode policy |
| agy B3 / Codex NB3 (priority math) | `configMAX_PRIORITIES` 10; table fixed; static assert |
| agy B4 (WFI starves idle; late SysTick) | `openfw_sleep_ticks()`; SysTick before scheduler; reap test |
| agy B5 (.ram_boot 172 B, missing consumers) | IRQ handler in `.fast_ramtext`; table completed |
| NB items (WDT, sink ops, NVS cache, timer daemon, wasm/k_exp_api, NVIC helper, PRIMASK, byte-identity scope, trace scrubbing, Exp 2 ordering) | M4 WDT/sink/i18n; M2 store/timer; §3 rules; §6 gates; M3 traces; step 4 |
