# Bluetooth Classic for open_fw — Integration Plan (v2, for joint review)

Branch `feat/freertos-bluedroid` (worktree `../voice-recorder-hw-bt-usb-bt`). Hardware: Smartlink SL6801 `HW_76`
(dual Cortex-M4F @ 96 MHz, 512 KB SRAM, 2 MB SPI NOR XIP, **BR/EDR-only** RivieraWaves `RW-BT` controller in mask ROM,
no BLE). This document is the single source of truth: it is reviewed by Codex and agy until both agree, then executed.

## 0. Goals, non-goals

Goals (two mutually exclusive runtime modes, selectable in Settings):
1. **Headphones** — A2DP Source (SBC) + AVRCP Target (headset play/pause/skip/volume → player).
2. **Phone companion** — SPP/RFCOMM carrying the existing `+XREC v5` protocol (`docs/XREC_PROTOCOL.md`,
   `open_fw/src/xrec_proto.c`) to the Android app and desktop tools (`tools/bt_sync.py`, `tools/bt_companion.py`).

Quality goal: replace the hand-written host (`bt_spp.c` L2CAP/SDP/RFCOMM, SSP responder in `bt_hci.c`) with a mature
stack and keep every custom line small, table-driven and host-tested.

Non-goals: BLE (no hardware), iOS companion (SPP needs MFi), HFP/HSP/SCO, A2DP Sink, multipoint, LE Audio.

## 1. Current state (done, on the branch)

| Item | Evidence |
|---|---|
| FreeRTOS-Kernel V11.1.0 vendored unmodified (`third_party/freertos`), optional `make RTOS=1` | builds zero-warning; default `RTOS=0` image byte-identical to baseline |
| SysTick stays owned by `startup.c` (1 ms, `g_systick_ms`, key scan, experiment watchdog); it calls `openfw_rtos_tick()` → `xPortSysTickHandler()`; SVC/PendSV vectors wired under `OPENFW_RTOS` | `src/startup.c`, `src/rtos/rtos_port.c` |
| App super-loop runs as a FreeRTOS task (16 KB PSP stack) | QEMU: SP inside heap region, UI/SD boot identical |
| heap_5 over `.rtos_heap` = `0x0086B000..0x00880000` (84 KB) | `sl6801_firm.ld`; region **not yet validated on silicon** |
| On-target RTOS self-test (delay, queue, PI mutex, timer, notify, event group, heap leak, preemption, FromISR from SysTick, stack watermark): **10/10 pass** under the SL6801 QEMU model | `make -C open_fw test-qemu-rtos` |
| Bluedroid (ESP-IDF `ad20ded6`, Apache-2.0) vendored pristine + 3 small patches (A2DP source-only build, frame buffer size, getdelay guard) | `third_party/bluedroid`, `third_party/bluedroid_patches/` |
| Classic-only config (A2DP src + AVRCP + SPP, 2 ACL, 4 bonds, logs off, ESP vendor HCI cmds off) links against real FreeRTOS + picolibc: **278.6 KB flash, 21.4 KB static RAM**; only unresolved symbols are 64-bit div helpers already in `startup.c` | `PORTING_NOTES.md`, `tools/measure_bluedroid.sh` |
| HCI compatibility matrix: every opcode/event Bluedroid needs vs current glue | `docs/BT_HCI_MATRIX.md` (static analysis by Codex) |
| Existing gates green after merging `main` (i18n, 17 languages) | `make all`, `make all RTOS=1`, `test-host-san`, `test-qemu-rtos` |

Not started: controller shim, platform shims (entropy, store, timer, log), host end-to-end harness, app integration,
hardware experiments. Nothing has run on silicon yet.

## 2. Key decisions (with rejected alternatives)

- **D1 Host stack = Bluedroid** (permissive, mature Classic profiles). Rejected: BTstack (non-commercial licence clause),
  Zephyr host (kernel dependency), NimBLE (BLE only), keeping the hand-rolled host (bug source).
- **D2 FreeRTOS confined to CPU0**; only BT tasks + the app task run under it. Audio DMA, USB, SDIO stay ISR/poll driven
  and untouched. CPU1 (LVGL worker, own vectors, WFE mailbox) stays bare-metal. Rejected: cooperative OSI shim
  (Bluedroid blocks on futures/semaphores → deadlock; would fork upstream).
- **D3 HCI boundary = Bluedroid's official driver hook** `esp_bluedroid_attach_hci_driver()` (`send`,
  `check_send_available`, `register_host_callback`; H4-framed). `hci_layer.c` stays pristine; we do not write an
  `hci_hal_*`.
- **D4 libc = picolibc** (BSD) + 6 compiler-rt builtins (~11 KB), built with the project clang/lld
  (`tools/build_picolibc.sh`, pinned commit). Bluedroid needs `snprintf`/`sscanf` (+soft-double). `startup.c`'s strong
  `__aeabi_mem*`/64-bit division stay authoritative. Rejected: in-tree libc (formatter bugs).
- **D5 Legacy path stays default** until the Bluedroid path reaches parity: build flag `OPENFW_BT_BLUEDROID=1`
  (implies `RTOS=1`). Legacy `bt_spp.c`/SSP responder are compiled out under the flag, deleted only after parity.
- **D6 Upstream stays pristine**: every Bluedroid change is a patch file with rationale (`bluedroid_patches/README.md`).

## 3. Runtime architecture (CPU0)

```
 priority  task / context            owns / does
 ISR prio0 DMA audio, USB, SDIO      unchanged; never call FreeRTOS APIs
 ISR >=0x80 IRQ26 RW-BT top half     ack RWBT_INTSTAT, OR status into pending, vTaskNotifyGiveFromISR(ctrl)
 ISR low   SysTick                   g_systick_ms, keys, exp watchdog, xPortSysTickHandler
 6         FreeRTOS timer daemon     osi alarms / esp_timer shim callbacks (short, non-blocking)
 5         bt_ctrl  (controller owner) ALL RW-BT ROM calls: ke_event_schedule, rwbt_*_isr bottom half, ke_msg alloc/send/free,
                                     EM alloc/copy, command/ACL TX queues, event/ACL RX packing → host callback
 5..3      Bluedroid HCI host / BTU / BTC (upstream priorities configMAX-3/-5/-6)
 2         storage worker (optional)  exFAT/recorder ops requested by BT callbacks (or done by app task via queue)
 1         app (legacy super-loop)   UI orchestration, recorder, player, USB poll, power; WFI at loop end
 0         idle                      DSB; WFI
```
Rules: (a) only `bt_ctrl` touches ROM/EM; others enqueue. (b) No BT callback touches exFAT, recorder, DAC or UI state
directly; it posts to the app/storage owner. (c) No allocation in ISRs. (d) IRQ 26 gets an explicit NVIC priority
numerically ≥ `configMAX_SYSCALL_INTERRUPT_PRIORITY` (implemented priority bits are discovered at runtime by the port;
assert at boot). (e) `bt_irq_lock` (PRIMASK) regions in the ROM glue are measured for worst-case duration.

## 4. Memory and flash budget

Flash: current image 423 KB (after i18n) + FreeRTOS 9 KB + Bluedroid ~279 KB (+picolibc) ≈ **720 KB of 2 MB**. OK.

SRAM (RTOS build): main `.bss` ends `0x84E990` (limit `0x850000`, ~5.7 KB left); `SRAM_HI` ~19 KB left.

| Consumer | Bytes | Placement |
|---|---|---|
| Bluedroid + OSI static `.data/.bss` | 21.4 KB | new NOLOAD section `.bt_bss` carved from the high region, zeroed in `startup.c` before scheduler |
| App task stack | 16 KB | heap (measure high-water; target 8–12 KB) |
| BT task stacks HCI 2 KB + BTU 4.25 KB + BTC 3 KB + bt_ctrl 2 KB | ~11.5 KB | heap; measure high-water on host + silicon |
| OSI objects, HCI cmd/ACL buffers, bond DB | 8–20 KB | heap |
| SPP mode: RFCOMM queues | 4–20 KB | heap (tune `PORT_RX/TX_BUF_HIGH_WM`) |
| A2DP mode: encoded frames 5×1280 + PCM ring (100 ms @ 48 kHz stereo 16-bit = 19.2 KB) | ~26 KB | heap / static in mode |
| **Total worst case** | **~95–110 KB** | **exceeds the 84 KB region** |

Therefore the plan needs at least one of (ordered by preference):
1. **Reclaim the old MSP stack** `0x867000..0x86AC08` (15.4 KB): with the app on PSP, MSP serves only exceptions
   (measure; keep 3 KB). +12 KB.
2. **Second heap_5 region** `0x805C00..0x81A37F` (~82 KB) — documented as stock-FIRM `.ramtext/.data`, untouched by
   open_fw; ROM/BootROM reserve only `0x800000..0x804BFF`. Gated on Experiment 0.
3. Shrink app stack after measurement; trim RFCOMM watermarks; PCM ring 60 ms.
4. Non-dev builds: reuse `EXP_ARENA`/`EXP_OUT` (48 KB) when experiments are compiled out.

Budget is enforced: linker `ASSERT`s per section, boot-time heap/stack high-water telemetry, host tests assert arena
returns to baseline.

## 5. Modules

### M1 — Controller shim `bt_ctrl` (the only fully custom component)
- Files: `src/bt_ctrl.c` (task, queues, lifecycle), `src/bt_ctrl_cmds.c` (opcode table), `src/bt_ctrl_evt.c` (event
  packers), `include/openfw_bt_ctrl.h`. ROM access through a function table `bt_rom_if_t` (ke_msg alloc/send/
  send_basic/free, EM tx alloc + copy, conhdl→task, ke event set/schedule, IRQ lock) so the same code runs against a
  **fake ROM** in host tests.
- Command table: every opcode in `docs/BT_HCI_MATRIX.md` gets exactly one of `FORWARD(ke_id, min_len, reply_packer)`,
  `SYNTH(fn)` (e.g. Read Local Name from a shim-held 248-byte name, coherent feature pages, supported-commands bitmap
  masked to what the shim implements), or `REJECT` with a **correctly sized** Command Complete / Command Status
  carrying `0x01 Unknown HCI Command` (Bluedroid parsers check minimum lengths before status). No opcode is ever silently
  consumed.
- Priorities from the matrix: (1) H4 ownership + exactly-one initial reply per command + 1 command credit;
  (2) `1005` Read Buffer Size (forward `E05C` + packer, or synthesize verified limits: ACL 1021 B × N, N validated);
  (3) controller→host flow control `0C31/0C33/0C35` implemented together in the shim (enforce host ACL length+count,
  credit return on `0C35`, no reply to `0C35`); (4) identity `1001/1009`; (5) `1002` 64-byte bitmap; (6) `1004` pages,
  `0C56`, `0C01`; (7) BTA setup writes + `0C14`; (8) discovery `0C05/0401/0402` + events `01/02/22/2F`;
  (9) link/security procedures + SSP events; (10) `13` Number Of Completed Packets, policy (sniff/role) and flush.
- Feature masking: until probed, advertise **no** sniff, role switch, Secure Connections, EDR packet types beyond what
  the ROM exposes; Bluedroid then never sends the unverified procedures.
- RX: copy each complete H4 packet (≤ 259 B events, ACL ≤ host ACL length) into owned buffers **before** completing the
  ROM `eif_write` callback; reserved capacity for Command Complete/Status and NOCP so they are never dropped.
- Reset: a real controller reset (`rwbt` soft reset + re-init via existing bring-up) inside `bt_ctrl`, then synthesized
  Command Complete; same path on re-enable. Teardown: close admission → stop producers → drain/free queued packets →
  stop controller → bump a session id so late callbacks are rejected.
- Legacy code in `bt_hci.c` (SSP responder, SPP filter, xrec wiring) guarded by `OPENFW_BT_BLUEDROID`.

### M2 — Platform shims (`bt_port/`)
- **Entropy/DRBG** (security-critical; current `bt_rand32` is a timer-seeded xorshift): entropy pool from measured
  noise sources (candidates: audio ADC LSBs with mics biased, SARADC/KADC noise, RF modem RSSI/I-Q samples, HOSC-vs-PLL
  timer jitter, CPU0/CPU1 drift), SP 800-90B-style health tests (repetition count, adaptive proportion), SHA-256
  conditioning, HMAC-DRBG (SP 800-90A) with reseed policy, seed file persisted atomically on SD, boot **entropy-ready
  gate** that blocks SSP key generation and XREC key creation until a conservative min-entropy credit is reached.
  Feeds `esp_random`, the ROM `bt_rand80` hook and XREC challenge generation. Quality validated by Experiment 2
  (raw-sample dumps → offline min-entropy estimate).
- **Store** (`nvs_*` surface actually used by `btc_config`/`bt_config`): small key-value file on SD with atomic
  replace (existing exFAT staged-replace pattern), CRC, 4 bonds, separate from the XREC pairing key (PSMP),
  migration of legacy link keys (4-slot cache/SD file) into Bluedroid's bond DB once. Link keys never logged.
- `esp_timer` → FreeRTOS timers + `us_timer_now()` (A2DP media tick 20–30 ms; measure jitter); `esp_log` →
  `openfw_log` with level filter and key redaction; heap accounting hooks for telemetry.
- `esp_ringbuf` (used by SPP) relies on kernel internals: stress-test on host; fallback to stream buffers via a patch.

### M3 — Host end-to-end harness
- Build Bluedroid + bt_port host shims + FreeRTOS POSIX port for x86-64 under ASan/UBSan (TSan where feasible).
- Peer: **Google Bumble** (Apache-2.0, pinned version) as virtual controller + remote host (SDP, RFCOMM, AVDTP/A2DP sink,
  SSP), connected over H4 (unix socket/pty) to the attached HCI driver. Fallback: scripted in-C mock controller.
- Scenarios: enable/disable ×1000 leak loop; SSP pairing both roles; SPP echo + `+XREC:PING` + large transfer with
  RFCOMM credits; A2DP open/start/suspend/close with SBC decode check; disconnect/reconnect storms; ACL flow beyond one
  full controller window; fault injection (dropped/delayed Command Complete, truncated events). `make test-bt-host`.
- The controller shim itself is tested separately against the fake ROM (`make test-bt-ctrl`, golden vectors for every
  matrix row, fuzzing both directions).

### M4 — Application integration
- Settings: `Bluetooth: Off / Headphones / Phone`; pairing screen (60 s window, numeric comparison display where
  applicable), status icon. New UI strings go through the i18n system (`openfw_i18n.h`, `ui_i18n.c`) for all 17
  languages; translations produced in one batch and reviewed.
- Phone mode: `xrec_proto` bound to `ESP_SPP_*` callbacks (callback mode, no VFS); reassembly across arbitrary read
  boundaries; congestion-driven writes; storage operations (list/download/delete/OTA PUT) executed by the
  storage owner, never in BTC context. SPP UUID 0x1101, RFCOMM channel 1 (`XREC_PROTOCOL.md`); the HFP `SLC_OPEN`
  handshake is legacy-only.
- Headphones mode: output-sink boundary in `player.c`: player writes PCM into a ring at the file rate; A2DP data
  callback pulls at the negotiated rate. Recordings are mostly 16 kHz: prefer **48 kHz** (integer ×3 upsampling) when the
  sink supports it, else 44.1 kHz with a polyphase resampler; SBC bitpool 35/53 (middle/high quality), joint stereo.
  Underrun → silence + counter. AVRCP Target: play/pause/next/prev → player; absolute volume if supported.
- Watchdog: supervisor in the app task feeds the hardware WDT only when all registered tasks (app, bt_ctrl, BTU, BTC)
  have advanced heartbeats; experiment keep-alive separated from hardware feeding.
- Power/USB/charging transitions: BT stop sequence (M1 teardown) runs before USB MSC mode, power-off, or deep standby.
- Diff to shared files (`main.c`, `Makefile`, `startup.c`) kept to small hook calls because `main` is under active
  i18n development.

### M5 — Hardware experiments (user runs; tools provided)
- **Exp 0 (SRAM)**: pattern-fill `0x86B000..0x880000` and (opt-in) `0x805C00..0x81A37F`; run BT bring-up + UI +
  recording; verify intact; report.
- **Exp 1 (HCI probe image, `OPENFW_HCI_PROBE=1`)**: through `bt_ctrl`, issue every matrix opcode one at a time
  (including stock KE-id candidates for "missing" rows) with timeouts and safe recovery; log reply bytes/timing/events
  to UART0 + log ring + vendor SCSI `0xE0`; `tools/bt_probe_report.py` turns the log into a table and a JSON that
  refines the shim's opcode table. Also NVIC priority-bit discovery and IRQ-26 latency/coalescing counters.
- **Exp 2 (entropy)**: raw noise-source dumps for offline min-entropy estimation.
- **Exp 3 (phone)**: Android pairing + XREC sync soak. **Exp 4 (headphones)**: 3 headphone brands, 1 h playback soak,
  AVRCP buttons, CPU load and UI smoothness. **Exp 5**: power cycling/USB-mode transitions with BT active.

## 6. Ordered steps and gates

Every step: zero-warning ARM builds (`all`, `all RTOS=1`, and the BT flag build once it exists), `test-host-san`,
`test-qemu-rtos`, plus the step's own tests; default (legacy) image byte-identical until step 9.

1. **libc + build plumbing**: picolibc build integrated into the main Makefile under `OPENFW_BT_BLUEDROID=1`; Bluedroid
   objects into a static library; `.bt_bss` section + zeroing; MSP stack reclaim. Gate: flag build links, QEMU boots,
   self-test passes, budgets asserted.
2. **Platform shims (M2)** with host tests (`test-bt-port`): DRBG known-answer vectors (NIST CAVP), health-test
   triggers, entropy gate, store power-fail injection on a real exFAT image, timer and log redaction tests.
3. **Controller shim (M1)** against the fake ROM (`test-bt-ctrl`): golden vectors for every matrix row, credit/ordering/
   fragmentation/backpressure/restart tests, fuzzing.
4. **Probe image (M5 Exp 0/1)** → **user runs on hardware** → shim table refined from the JSON. *Bluedroid is not run on
   silicon before this.*
5. **Host harness (M3)** with Bumble: enable/disable, SSP, SPP/XREC, A2DP scenarios green under sanitizers.
6. **Bluedroid on silicon, phone mode**: stack enable, discoverable, pairing, `+XREC` over SPP; Exp 3.
7. **A2DP source + AVRCP**: player sink boundary, resampler, Exp 4.
8. **UI/settings/i18n, WDT supervisor, power transitions**; Exp 5.
9. **Parity + switch default**: flag on by default; legacy host code deleted in a separate commit.
10. **Joint final review** (Codex + agy) of the whole branch.

## 7. Verification strategy

- Host: unit (fake ROM), protocol (Bumble), sanitizer builds, fuzzing of both HCI directions and of XREC framing.
- Emulator: QEMU SL6801 model for boot/RTOS/regression (cannot emulate RW-BT).
- Silicon: probe image first, then mode-by-mode experiments; every silicon bug becomes a recorded HCI-trace regression
  test (btsnoop export with key redaction).
- Resource telemetry: heap/stack high-water, IRQ-26 latency, A2DP underruns, exposed through the existing log/SCSI
  channel.

## 8. Risks

| # | Risk | Mitigation |
|---|---|---|
| R1 | ROM controller lacks/behaves differently for opcodes Bluedroid needs (init deadlock) | matrix + REJECT-with-correct-size policy + feature masking + Exp 1 before bring-up |
| R2 | SRAM budget exceeds 84 KB | §4 options; Exp 0; telemetry |
| R3 | Weak entropy → breakable pairing | M2 entropy design + gate + Exp 2; fail closed |
| R4 | IRQ-26 bit coalescing / lost baseband service under load (SD writes, UI, SBC) | owner task drains all pending sources; counters; Exp 1/4 |
| R5 | FreeRTOS interaction with existing bare-metal code (WFI loops, CPU1 sync busy-waits, PRIMASK sections) | confined RTOS (D2), QEMU self-test, audit of blocking helpers, WDT supervisor |
| R6 | A2DP CPU budget (SBC + resampler + UI on CPU1 + SD) | measure; bitpool/rate tuning; 48 kHz integer path |
| R7 | Storage reentrancy from BT callbacks | storage-owner queue rule |
| R8 | `esp_ringbuf` on vanilla FreeRTOS | host stress; stream-buffer fallback |
| R9 | Merge conflicts with ongoing i18n work on `main` | hook-only edits to shared files; merge `main` frequently |
| R10 | Interop (phones/headphones) | interop matrix in Exp 3/4 |

## 9. Open questions for reviewers

1. Is Bumble a sound choice for the protocol-level host harness (BR/EDR controller emulation, SSP, AVDTP), or should
   M3 use a custom mock controller plus a real BlueZ peer?
2. Is REJECT-with-correctly-sized-reply plus feature masking sufficient to keep Bluedroid from deadlocking on unknown
   ROM capabilities, or must some opcodes be fully synthesized?
3. Entropy: are the candidate sources plausible on this SoC, and is the gate design (block pairing until credited) the
   right trade-off for UX?
4. Memory: which of the §4 options should be primary? Is putting the app task on a 16 KB PSP stack plus shrinking MSP
   safe given the fault handler and the CPU1 interplay?
5. Ordering: is it right to probe the controller (step 4) before the Bumble harness (step 5), or should the harness come
   first to validate the shim end-to-end before using hardware time?
6. Anything missing that would make this unsafe to execute?

## 10. Live log

- 2026-11-24: FreeRTOS port + QEMU self-test (10/10). Bluedroid vendored, Phase 1 GO (278.6 KB / 21.4 KB static).
  Codex architecture review and HCI matrix integrated. `main` (i18n) merged. Plan v2 written for Codex+agy review.
