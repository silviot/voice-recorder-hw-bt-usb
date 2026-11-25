# Bluetooth Classic for open_fw — Integration Plan (v18, AGREED by Codex and agy)

Branch `feat/freertos-bluedroid` (worktree `../voice-recorder-hw-bt-usb-bt`). Hardware: Smartlink SL6801 `HW_76`
(dual Cortex-M4F @ 96 MHz, 512 KB SRAM, 2 MB SPI NOR XIP, **BR/EDR-only** RivieraWaves `RW-BT` controller in mask ROM,
no BLE). This document is the single source of truth: it is reviewed by Codex and agy until both agree, then executed.
v3–v18 address rounds 1–16 (§11 traceability).

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
- **D3 HCI boundary = `esp_bluedroid_attach_hci_driver()`** (H4 framed); no custom HAL. `hci_layer.c`,
  `hci_hal_h4.c`, `fixed_queue.c`, `osi/alarm.c` and `btc_task.c` receive only the small documented patches listed in
  M1/M2 (D6).
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
 ISR       IRQ26 RW-BT top half        explicit NVIC prio: logical 4..7 i.e. IPR byte 0x80..0xE0 with 3 prio bits
                                       (numerically >= configMAX_SYSCALL_INTERRUPT_PRIORITY = 0x80), read back;
                                       ack, count per-bit occurrences, vTaskNotifyGiveFromISR(bt_ctrl) +
                                       portYIELD_FROM_ISR; code in .fast_ramtext (NOT .ram_boot: 172 B left)
 ISR low   SysTick                     g_systick_ms, keys, exp watchdog, xPortSysTickHandler
 9         FreeRTOS timer daemon       kernel/app timers only (BT alarms use bt_alarm, see M2)
 8         bt_ctrl (controller owner)  ALL RW-BT ROM/EM calls; cmd/ACL TX queues; RX packing into bounded rings
 7         hciT (upstream configMAX-3) Bluedroid HCI host
 7         bt_alarm                    BT deadline list → pending list → BTC drain post (never blocks otherwise)
 6         bt_dlv (delivery)           pops bt_ctrl RX rings → host callback (may block on host queues; bt_ctrl never does)
 5         BTU  (upstream configMAX-5)
 4         BTC  (upstream configMAX-6)
 2         bt_recover                  graceful disable/deinit on a BT fault; may block forever (app enforces deadline)
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
  `OPENFW_TASK_WAIT()`; only idle executes WFI. This lets idle run and reap deleted Bluedroid tasks.
- **SysTick start**: `vPortSetupTimerInterrupt()` calls `systick_init()` — i.e. inside `xPortStartScheduler()`
  after SHPR3 gives SysTick/PendSV the lowest priority and with interrupts masked (never earlier: a default-priority
  tick could enter the kernel before the first task). The later call in `main.c` is a no-op under `OPENFW_RTOS` when
  the configuration is unchanged. `configTIMER_TASK_PRIORITY = configMAX_PRIORITIES - 1` (9), static-asserted.

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
| BT stacks: hciT 2 + BTU 4.25 + BTC 3 + bt_ctrl 2 + bt_dlv 1.5 + bt_alarm 1 (+ bt_recover 1, static in `.rtos_static`) | ~14.75 KB | heap; host + silicon watermarks |
| bt_ctrl rings: ACL RX 4 × (1021+5) + event RX 8 × 260 + unified TX 3 × 1026 (any packet type) | ~9.2 KB | static in `.bt_bss`, capacity asserted in bytes |
| OSI objects, HCI cmd/ACL buffers, config cache | 8–20 KB | heap |
| SPP mode RFCOMM queues + XREC RX stream buffer | 4–20 KB | heap |
| A2DP mode encoded frames + PCM ring (100 ms @48 kHz) | ~26 KB | heap |
| **Worst case** | **~105–120 KB** | exceeds the 84 KB high region |

Placement: `.rtos_static` (kernel idle/timer objects), `.bt_data` (VMA) and `.bt_bss` are carved from the **start
of `RTOS_HEAP` (`0x0086B000`)**; `_srtos_heap0` starts after `_ebt_bss` and heap_5 takes the rest; `.bt_data`'s LMA is
contiguous after `.data` in XIP. (Main `SRAM` and `SRAM_HI` cannot hold them.)

Linker rules: Bluedroid is built into `libbluedroid.a` (plus `libbt_port.a`, `libc.a`); the new sections select input
explicitly (`libbluedroid.a:*(.bss .bss.* COMMON)` etc.) and appear **before** the catch-all `.data`/`.bss`; heap bounds
are disjoint from every section; `ASSERT`s on each region. The step-1 link roots the real entry points
(`esp_bluedroid_init/enable`, `esp_spp_*`, `esp_a2d_source_*`, `esp_avrc_*`) via a table in a dedicated section kept
with `KEEP(*(.bt_link_roots))`, and a post-link check (`nm`) fails the build if any required entry point is missing,
so `--gc-sections` cannot produce a deceptively small image.

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
- **ACL credit window (sized to physical storage)**: config overrides upstream `L2CAP_HOST_FC_ACL_BUFS` (default 100)
  to 4, and a documented patch at `controller.c`'s `make_host_buffer_size()` call passes a dedicated 1021 B constant
  instead of `L2CAP_MTU_SIZE` (1691, which keeps L2CAP reassembly unchanged): **4 packets × 1021 B**; a static assert ties the ring byte capacity to these values.
- **RX path (backpressure never reaches the ROM owner):** `bt_ctrl` copies each complete H4 packet into a bounded ring
  before completing the ROM `eif_write` callback (copy-before-ack). The RX dispatcher (rewrite of today's
  `bt_hci.c` path, which checks `tx_state` first and advances the ACL fragment pointer before writing) is ordered:
  (1) events are never held behind ACL that waits for a host credit; (2) only then ACL, and the ACL branch checks for a host credit **before** mutating any
  fragment state or marking the transport busy — with zero credits ACL is simply not selected (data stays in EM) and
  events keep flowing; (3) a credit return (`0C35` from the host) kicks the dispatcher. Events have their own ring with
  reserved slots for CC/CS/NOCP. Test: event delivery with zero credits and an empty ACL ring; credit return resumes ACL.
  **Host side of the credit return**: Bluedroid queues `0C35` like any command and hciT checks `command_credits > 0`
  before dequeuing (`hci_layer.c`), so with zero command credits a credit return could never leave the host. The HCI
  spec allows Host Number Of Completed Packets regardless of Num_HCI_Command_Packets; a documented `hci_layer.c` patch
  gives NOACK commands (`0C35`) their own queue, drained ahead of the credit check (and ahead of credit-blocked
  ordinary commands); `bt_ctrl` handles `0C35` locally without consuming a controller command credit. Test through
  hciT: command credits 0, ACL window exhausted, an ordinary command already queued → `0C35` still reaches `bt_ctrl`
  and inbound ACL resumes.
  Whether the ROM's `eif_write` stream lets events overtake a deferred ACL fragment is an Exp 1 stage-C question; if it
  does not, ACL-ring saturation is handled as a controlled link disconnect, not assumed impossible.
  `bt_dlv` delivers to the host callback and is the only context that may block in Bluedroid's queues; it checks the
  session id before each delivery and holds no lock across the callback.
  **No silent loss in host RX**: upstream `host_recv_pkt_cb()` (`hci_hal_h4.c`) copies the packet (asserting on
  allocation failure) and then ignores `fixed_queue_enqueue()`'s result, whose `list_append()` node allocation can fail
  after a slot was taken, leaking the copy and dropping the packet (a lost Command Complete stalls command dispatch).
  Documented patch: the copy allocation fails soft, an enqueue failure frees the copy and restores the slot
  (`fixed_queue.c`), and the callback returns failure to the driver. `bt_dlv` keeps ownership of the ring entry on
  failure and retries with a 1-tick backoff; after N consecutive failures it posts a **BT fault** to the app task (never tears down from
  `bt_dlv`, which must keep delivering disable-path events) — see "Recovery ladder" below (M1
  teardown), never a silent drop. Per-mode heap budgets reserve `window × (len + header)` for these copies.
  **General rule (steps 2–3)**: every allocation on the HCI/OSI/BTC paths that Bluedroid uses must either propagate
  failure or be covered by a documented patch; enforced by an **allocation-failure sweep** in the host harness that
  reruns each scenario failing the Nth allocation for every N up to the scenario's allocation count, and requires: no
  lost CC/CS/NOCP, no leak (heap returns to baseline), and either completion or a controlled restart.
- **TX admission (driver `send()` returns void)**: `check_send_available(void)` carries no packet type and hciT picks
  a command, ACL packet or pending fragment only after it returns true; it may also not send at all (`hci_layer.c`
  loop exit, `packet_fragmenter.c` bounce). So TX uses **one unified ring whose slots fit any H4 packet** (≤ 1026 B),
  and `check_send_available()` is a **pure, side-effect-free query** "unified ring has a free slot". Only hciT fills
  and only `bt_ctrl` frees, so a true result stays true until hciT's next `send()`, which copies and cannot fail
  (assert). Command credit / ACL credit gating happens inside `bt_ctrl` when draining the ring to the ROM, never in the
  check; the drain is **not strict FIFO**: an eligible command is taken ahead of a credit-blocked ACL entry (order
  preserved within each type), so ACL at the head cannot block `0C35`/Disconnect. `1005` never advertises more
  controller ACL buffers than `bt_ctrl` can hand to the ROM immediately, so the ring cannot fill with undrainable ACL. `notify_host_send_available()` (unbounded post + mutex) is never called from `bt_ctrl`: it notifies `bt_dlv`,
  which calls it. Tests: empty queue, zero command credits, zero ACL credits with a queued command (command must still
  go out), full host work queue, teardown with a pending send-available.
- **Reset**: real controller reset inside `bt_ctrl`, then synthesized CC; same path on re-enable.
- **Disable/teardown order**: Bluedroid disable + profile close completions run with transport alive → callbacks
  quiesced (session id bumped, late callbacks rejected) → admission closed → controller stopped, reset completion
  checked (status reported, not ignored) → IRQ 26 masked → rings freed → tasks deleted (idle reaps).
- **Recovery ladder (bounded, works under persistent allocation failure)**: the app task handles a BT fault (RX
  delivery failures, ring overflow, command timeout storm, `bt_ctrl` invariant violation): (1) graceful
  `esp_bluedroid_disable()` + `esp_bluedroid_deinit()` + M1 teardown run in a dedicated **`bt_recover` worker** (static
  TCB/stack in `.rtos_static`, created once at first BT enable and reused, so it exists without heap; priority 2).
  The app never calls into Bluedroid during recovery: it notifies the worker and then **keeps running its recorder
  service loop** (`recorder_poll()` at the normal cadence; the DMA ring holds only 3 × 128 ms of audio) while checking
  worker completion against **one absolute 2 s deadline** (it waits in `ulTaskNotifyTake(…, 1 tick)` but decides
  only on an explicit `bt_recover` done/status variable, since other tasks may notify the app too).
  **BT silencing does not depend on the app**: when recovery starts, the app arms a deadline that the SysTick ISR
  enforces (no heap, no task): at 2 s it masks IRQ 26 and sets a `bt_stop` flag. Guarantee, stated precisely: IRQ 26 is
  masked at the deadline; `bt_ctrl` checks `bt_stop` before every ROM call, so at most **one** ROM call that had already
  passed its check can still complete (ROM handlers are short and non-blocking; their worst-case duration is measured in
  Exp 1 and asserted ≤ 1 ms), and with IRQ 26 masked no bottom-half dispatch can follow; `bt_ctrl` then parks. This holds
  even if the app is inside a long synchronous SD write. When `bt_recover` reports success near the deadline, the app
  first disarms the deadline, then checks whether `bt_stop` already latched; if so it continues on the hard path (BT is
  never left masked after a "successful" graceful recovery). Recording finalisation and the reboot stay with the app,
  which reaches them when its current call returns.
  **Audio continuity bound**: recovery promises exactly what normal recording promises, no more. The DMA ring holds
  3 × 128 ms slots; a stall is lossless iff the DMA completions during it do not exceed the free slots at its start
  (so the lossless duration depends on the queue depth and DMA phase when the stall begins, as in normal recording).
  Beyond that the existing recorder overrun path applies (live counter + bounded silence substitution). Today the
  overrun count is not persisted in the WAV; persisting it is a separate recorder follow-up, outside this plan. Every
  upstream wait inside disable/deinit (`btc_transfer_context` posts with `OSI_THREAD_MAX_TIMEOUT`, `future_await`) may
  block the worker forever without affecting the app, so no timed variants are needed; (2) if the worker reports an
  error (e.g. `ESP_ERR_NO_MEM`) or the deadline passes, the app runs a **hard path that needs no heap and no BT task**: mask IRQ 26, stop the recorder
  through its existing finalize path (static buffers only) after a final `recorder_poll()`, so no audio is lost, write a fault record
  (`AON_BACKUP`, code `0xB1` + reason), and reboot via `system_reboot()`. After a BT-fault reboot BT starts **disabled**
  (fault record checked at boot; the UI shows "Bluetooth stopped after an error") so persistent faults cannot loop.
  **Allocation failure must reach the code on target**: today `vApplicationMallocFailedHook()` reboots immediately
  (`0xF1`), so `malloc()` never returns NULL and an OOM would lose an active recording. Under `BT=1` the hook only
  counts failures and returns, so `pvPortMalloc()` returns NULL and
  callers fail soft; kernel objects created at boot are checked by their callers (existing `openfw_rtos_fatal` on a
  failed boot-time create). The failed size is recorded by the `malloc` wrapper (`bt_port_heap.c`; the hook has no
  size argument). The WDT supervisor tolerates the 2 s graceful window.
  Tests: (a) host: allocation failure held active from the RX failure through recovery → hard path, recording valid,
  next boot BT off, and identifiable PCM fed throughout is continuous in the finalised file (no silence substitution);
  (b) host: worker stalled at each blocking point (BTC queue full with its consumer stalled before the
  disable post; disable completion withheld; deinit post blocked; deinit completion withheld) → app reaches the hard
  path, IRQ 26 is masked by the ISR at 2 s (also when an SD write crossing the deadline is injected), and the app
  then finalises the recording: with the initial queue depth and DMA phase pinned, an injected stall within the free-slot
  bound yields continuous audio; a longer one increments the live overrun counter (asserted before the reboot) and the
  file is valid afterwards; fake-ROM test with expiry between the `bt_stop` check and the
  ROM call → exactly that one call completes, then no further ROM calls; (c) target (QEMU +
  silicon): exhaust the real heap_5 allocator with a recording active → no `0xF1` reboot, recording finalised, BT
  fault record `0xB1`.

- **IRQ multiplicity**: per-source counters; bottom half loops until the status register reads clear.
- **Local identity**: stable per-device BD_ADDR: eFuse if present; else derived deterministically (SHA-256, fixed
  domain-separation label, stable across firmware upgrades) from the SPI-NOR unique ID (`0x4B`, read from RAM code
  with IRQs masked; Exp 1 verifies it is non-trivial and differs across units), with the locally-administered unicast
  bits set (as `bt_hci.c` does today) and reserved LAPs avoided; else generated once from the DRBG and persisted (then a
  card swap changes identity — documented). Never timer-derived. A production release would use an allocated public
  address range; this is a hobby/prototype firmware.

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
- **Store**: `nvs_*` surface used by `osi/config.c` served from an in-RAM KV cache that the app finishes loading
  **before `esp_bluedroid_init()`** (with SMP enabled, `btc_init_bluetooth()` → `btc_config_init()` already reads the
  store and silently falls back to an empty config). Test: cold boot with an existing bond reconnects without pairing; `nvs_commit()` posts an asynchronous flush to the app (atomic exFAT staged replace, CRC).
  No SD → RAM-only bonds for the session. Link keys never logged. Trust model documented: keys on MSC-visible media are
  readable/replaceable by anyone with physical access; CRC only detects corruption.
- **Alarms (`esp_timer` shim under `osi_alarm`)** — requirements: the timer context never blocks, an expiry is never
  lost, a cancelled or re-armed alarm never delivers a stale expiry. FreeRTOS software timers are **not** used for this:
  their start/stop commands go through the daemon's queue, so an old expiry can be processed before a queued stop/re-arm
  and can only be identified by a reusable slot pointer (round-7 finding). Instead a small **`bt_alarm` service**
  (`bt_port/bt_alarm.c`, host-tested) owns all BT deadlines:
  1. State: a deadline list (sorted, `us_timer_now()` based) and a pending list, both guarded by one short critical
     section. Each alarm carries `generation`, bumped by every set/cancel/free **synchronously in the caller** under that
     critical section, which also removes the alarm from both lists. There is no command queue, so nothing can lag behind
     a cancel or re-arm.
  2. A `bt_alarm` task (priority just below `bt_ctrl`, ~1 KB stack) sleeps in `ulTaskNotifyTake()` with a timeout to the
     earliest deadline (any positive remaining time rounded **up** to ≥ 1 tick, clamped below `portMAX_DELAY`, so it
     never busy-spins inside a tick); set/cancel call `xTaskNotifyGive()` (cannot fail) when the earliest deadline changes. On wake it
     moves every due entry, stamped with the `generation` it was armed with, from the deadline list to the pending list
     (one-shots leave the deadline list; periodics are re-inserted at `deadline + period`, coalescing if late), then posts
     a drain work item to BTC with **`osi_thread_post(btc_thread, drain_fn, NULL, 0, 0)`** (allocation-free; a failure
     can only mean the BTC queue is full). At most one drain item outstanding (flag cleared on failed post and at the
     start of `drain_fn`). The task never blocks on anything else.
  3. **Guaranteed delivery**: `btc_thread_handler()` calls `osi_alarm_drain_pending()` at its end, i.e. after every BTC
     message (documented patch in `btc_task.c`). A failed post means a full queue, so BTC runs again and drains.
  4. Drain: under the critical section unlink each pending entry and deliver only if its stamped generation equals the
     alarm's current `generation` (cancel/re-arm/free since expiry ⇒ drop); the callback runs in BTC context outside the
     critical section with the validated generation available via `osi_alarm_delivery_gen()`. `generation` survives
     `alarm_free()`'s `memset`.
  5. Callers whose meaning depends on "which arming" re-validate under their own lock (a higher-priority task can re-arm
     between drain validation and the callback taking that lock). All `osi_alarm` users are audited in step 2; the known
     case is `hci_layer.c` `command_timed_out`: the command-response timer generation is recorded when (re)armed under
     `commands_pending_response_lock`, and the callback ignores the expiry unless `osi_alarm_delivery_gen()` matches,
     before reading the queue head.
  Tests (host, FreeRTOS POSIX + deterministic fake clock): BTC queue full at expiry → delivered exactly once; heap
  allocation failing with no later traffic → delivered once; cancel or re-arm at every interleaving point (before due,
  after due but before the `bt_alarm` scan, after stamping but before drain, after drain validation but before the HCI
  lock) → the old expiry is never accepted and the new one fires exactly once; periodic cadence under a slow BTC;
  `bt_alarm` stack watermark under A2DP load.
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

### Threat model: keys on the SD card (STEP10 C6)
What is stored, in plaintext, in the card's root: `/bt_bonds.bin` (phone mode) and `/bt_hp_bonds.bin` (headphones
mode), `bt_store` images ("BTS1", SHA-256 prefix as an integrity check only) holding Bluedroid's bond config: peer
address, 128-bit BR/EDR link key, key type; and `/cassini.key` ("CSK5" + the 32-byte XREC HMAC key, the stock
firmware's format, kept so a card moves between stock and open firmware). No device-unique secret exists to encrypt
them with (the NOR UID follow-up of C18 is the same prerequisite); nothing else on the card is secret.

- **In scope, mitigated**: a radio-only attacker. Phone mode bonds need numeric comparison confirmed on the device
  inside a user-opened window; Just Works is refused in phone mode, and never replaces a stored MITM key in either mode;
  a failed authentication or "key missing" from a peer using a bonded address does not remove the bond (patch 0042);
  pairing is refused while the entropy gate is not met (M2.3, C3 latch). XREC commands need `CHAL`/`AUTH` with the
  key; `PAIR` needs the device's own window.
- **Out of scope (accepted)**: anyone who reads the card (seconds with the device in hand, or the card in a reader).
  With a bond file they can impersonate the bonded phone or headset to the device, and with the peer's address and link
  key decrypt recorded BR/EDR traffic of that pair or impersonate the device to the phone. With `cassini.key` they pass
  XREC `AUTH` and can list, download and delete recordings over BT, which the card in hand gives them anyway. Someone
  who can write the card can plant a bond or replace the XREC key (the same as pairing with physical access).
- **Recovery**: on a lost or copied card, remove the device from the phone's Bluetooth list, delete the three files
  (or Forget in headphones mode) and pair again; a new `PAIR` replaces `cassini.key`. Link keys are never logged
  (`bt_store.h`), and the bench log carries no key material.

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

1. **RTOS hygiene + build plumbing**: priorities to 10, `OPENFW_TASK_WAIT()` replacing task-context WFI, SysTick
   started in `vPortSetupTimerInterrupt()`, idle/timer static memory out of main `.bss`, `nvic_set_priority()`; picolibc + `libbluedroid.a`
   under the flag; `.bt_data`/`.bt_bss` sections with explicit input selection + copy/zero + objcopy + ASSERTs;
   KEEP-rooted reference table + post-link symbol check; ACL window overrides. Gate: flag build links with real entry points, QEMU boots, self-test passes (add a
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
   lifecycle gate (disable/re-enable, MSC during transfer, card removal and replacement + cold reconnect) and the WDT
   supervisor acceptance test before any user phone test; Exp 3.
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

## 9. Open questions for reviewers (round 3)

1. Does v18 (free-slot continuity bound; live overrun assertion) resolve round 16?
2. Any remaining blocker to starting steps 1–3?

## 10. Live log

- 2026-11-24: FreeRTOS port + QEMU self-test (10/10). Bluedroid vendored, Phase 1 GO. Codex architecture review and HCI
  matrix integrated. `main` (i18n, 18 languages) merged. Plan v2 reviewed: Codex and agy both CHANGES_REQUIRED.
  Plan v3 written. Round 2: agy APPROVE_WITH_CHANGES, Codex CHANGES_REQUIRED (4 items). Plan v4 written.
  Step-1 RTOS hygiene committed (`1d3e5db`: priorities, `OPENFW_TASK_WAIT()`, SysTick in port hook, `.rtos_static`,
  T_REAP; QEMU 11/11; default image byte-identical). Round 3: agy APPROVE_WITH_CHANGES, Codex CHANGES_REQUIRED
  (3 precise items). Plan v5 written. Round 4: agy APPROVE, Codex CHANGES_REQUIRED (1 item: lost alarm expiry).
  Plan v6 written. Round 5: agy APPROVE; Codex CHANGES_REQUIRED (retry wakeup not guaranteed; stale expiry after
  cancel). Plan v7 replaces the retry-timer approach. Round 6: agy APPROVE_WITH_CHANGES (no blockers); Codex 2 items
  (allocation-failure wakeup; validate-then-callback race). Plan v8 written. Step-1 build plumbing in progress.
  Round 7: agy APPROVE; Codex 1 item (FreeRTOS timer command lag lets an old expiry carry the new generation). Plan v9
  replaces FreeRTOS software timers for BT alarms with the `bt_alarm` service. Step 1 build plumbing committed
  (`fc8402e`: BT=1 links Bluedroid, `.bt_data`/`.bt_bss`, link roots, QEMU 12/12). Round 8: agy APPROVE; Codex 1 item
  (`0C35` blocked by hciT command credits). Plan v10. Round 9: agy APPROVE; Codex 1 item (H4 RX enqueue
  failure drops packet). Plan v11 adds the fail-soft RX patch and a general allocation-failure sweep. Round 10: agy
  APPROVE; Codex 1 item (recovery itself allocates). Plan v12 adds the bounded recovery ladder. Round 11: both
  reviewers found the same 2 items (malloc-failed hook reboots; disable waits forever). Plan v13. Round 12: both found
  unbounded posts before/after the timed wait (disable and deinit). Plan v14 moves graceful recovery into a worker
  whose blocking cannot affect the app. Code so far: step 1 complete; step 2 entropy/DRBG, `bt_alarm`, non-rebooting
  malloc hook committed with host/QEMU tests. Round 13: agy APPROVE; Codex 1 item (recovery wait starves the
  recorder). Plan v15. Round 14: agy APPROVE; Codex 1 item (app cannot enforce its deadline during a long SD
  write). Plan v16. Round 15: agy APPROVE; Codex 2 items (continuity beyond the DMA ring; check-then-ROM-call race).
  Plan v17 states both guarantees at their enforceable bounds. Round 16: agy APPROVE (no items); Codex 1.5 items
  (bound depends on queue depth; overrun not persisted). Plan v18.
  Round 17: **Codex APPROVE, agy APPROVE, no open items. Plan v18 agreed; execution continues with steps 2–3 and the
  step-4 hardware session.** Follow-up outside this plan: persist the recorder overrun count in recordings.
- 2026-11-24 (hardware session 1, `docs/BT_HW_RESULTS.md`): Exp 0 PASS (both regions intact after legacy BT, DMA,
  SD, USB, UI) → lower region enabled as heap_5 region 0; BT image free heap 34 KB → 118 KB. RTOS+BT self-test 14/14
  on silicon. Exp 2 first read: mic ADC low bits ≈ 0.95 bit/bit, SARADC and timer jitter unusable. Exp 0 probes (`exp/exp_sram_fill.c`, `exp/exp_sram_check.c`) written.
- 2026-11-24 (step 3): `bt_ctrl` controller shim against a fake ROM (`bt_port/bt_ctrl*.c`, `tests/fake_rom.c`,
  `tests/test_bt_ctrl.c`, 168,750 checks incl. Bluedroid's real reply parsers, fuzzing, a `bt_stop` sweep over every
  ROM call). Specs: `docs/BT_ROM_INTERFACE.md`, `docs/BT_HCI_DRIVER_CONTRACT.md`, `docs/BT_CTRL_OPCODE_TABLE.md`.
  Clarified M1 RX rule: credit gating happens before ACL enters its ring, so events never wait behind credit-blocked
  ACL, but `bt_dlv` delivers events and ACL in arrival order — strict event priority at delivery would let an event
  (e.g. Disconnection Complete) overtake ACL the ROM delivered earlier. Not yet wired into the firmware.
- 2026-11-24 (step 5, M3 path (b)): contract patches 0008 (fail-soft host RX), 0009 (fixed_queue slot restore),
  0010 (NOACK queue ahead of the command-credit check) applied. `make -C open_fw test-bt-host`: Bluedroid + bt_port +
  bt_ctrl + fake ROM on FreeRTOS V11.1.0's POSIX port under ASan/UBSan (`open_fw/tests/bt_host/`): enable reaches
  ENABLED, GAP name/scan mode reach the ROM, 200 enable/disable/deinit cycles return heap/tasks/alarms to baseline,
  0C35 leaves the host at zero command credits (fails without 0010). Allocation-failure sweep over enable (111
  attempts): RX paths (0008) all recover via `bt_dlv` retry; 10 upstream sites crash or hang and are explicit
  waivers with exact point counts (`tests/bt_host/sweep_waivers.txt`) pending patches 0011+. 0009 is checked directly
  (capacity-2 `fixed_queue`), since the sweep cannot see one lost RX slot. TSan variant clean (cooperative scheduling).
  Path (a) (Bumble) not started.
- 2026-11-24 (M1 "general rule"): patches 0011-0023 handle every allocation failure the sweep finds on the
  lifecycle paths: complete without the allocation, bounded retry where no caller can be told (CC/CS hand-off to BTU,
  BTA timer start), or fail `esp_bluedroid_init()`/`enable()` cleanly. The sweep runs per call (init 125 / enable 93 /
  disable 24 / deinit 5 points), each point followed by teardown to baseline and a full clean cycle: **0 waivers**.
  Each patch was mutation-checked (reverted alone -> its sweep fails). Contract for the app: after a failed enable,
  deinit (recovery ladder) before retrying. Open: an OOM while parsing the stored config loses bonds at the next save
  (`third_party/bluedroid/PORTING_NOTES.md`).
- 2026-11-24 (M4 XREC session, step 6 code; not yet on silicon): `bt_port/bt_xrec_bluedroid.c` implements the UI's
  `bt_xrec_*` API on Bluedroid SPP for every BT=1 image except the probe (`open_fw/Makefile` renames the legacy
  `bt_xrec_*` and IRQ handler in `bt_hci.c`; `bt_hci.c`'s ROM bring-up export now also builds under
  `OPENFW_BT_BLUEDROID`; default image byte-identical). `bt_port/bt_xrec_fw.c`: device callbacks (legacy F:1733-1966),
  bonds in `SD:/bt_bonds.bin` (staged replace) loaded before init, `bt_recover` worker running the up job (bt_ctrl
  open + attach + init + enable) and the down job (`esp_spp_deinit` -> disable -> bt_ctrl stop -> deinit),
  2 s teardown deadline, hard path = fault record `0xB1` (BT refused for the rest of the next boot). Legacy-compatible
  surface: name `Cassini_XX`, CoD 0x200404, SPP channel 1 "Cassini" with SDP 0x1101 (bt_sync.py connects to ch 1).
  `make test-bt-xrec`: 221 checks (data path, congestion, partial writes / in-flight window, write error, disconnect
  mid-transfer, policy, stop during transfer, deferred reboot behind congestion, faults, hard paths), mutation-checked.
  **Interim deviations** (to close in step 8 / follow-ups): (1) *[resolved: "pairing prompt" entry below]* no local
  accept prompt yet (needs i18n strings): a
  numeric comparison is auto-confirmed only inside the user-opened pairing window, its value is shown through
  `bt_status_t.pair_pin` (the BT screen's existing "PIN" toast) and logged; outside the window it is rejected.
  (2) Just Works cannot be refused at the request (`btc_dm.c` auto-confirms it without an app event): the bond is
  removed at `AUTH_CMPL` (unauthenticated key type, or not confirmed in the window) and the link dropped; passkey
  display/entry likewise rejected. XREC bytes flow only on links bonded that way. (3) The teardown deadline is a
  FreeRTOS timer-daemon one-shot (priority 9; preempts the app even in an SD write), not a SysTick-ISR hook, to keep
  `startup.c` untouched. (4) `bt_recover` stack/TCB live in SRAM_HI (`.bss_hi`) instead of `.rtos_static`: with
  bt_ctrl's rings and stacks linked, RTOS_HEAP would fall below the 32 KiB heap floor. (5) *[closed on the host, silicon
  check pending: "entropy + ROM key hardening" entry below]* No credited entropy source
  is registered in firmware yet, so `bt_entropy_get()` fails: the SSP window still opens, but XREC PAIR answers
  PAIRCLOSED and CHAL closes the link (fail closed) until Exp 2's source is wired. (6) Without eFuse the BD_ADDR is a
  fixed fallback (probe scheme), never timer-derived; NOR-UID derivation pending. (7) `main.c` still calls
  `bt_rf_init_calibrated()` before `bt_xrec_start()`, so RF is calibrated twice per BT-mode entry (bt_rom_real
  calibrates again at HCI Reset). (8) *[closed on the host, silicon check pending: "entropy + ROM key hardening" entry below; the ROM key
turned out to be a constant, worse than stated here]* SSP pairing without credited entropy on a ROM-generated P-192
secret of ~33 predictable bits: bench-only waiver, see the review-round entry below.
- 2026-11-24 (integration: bt_ctrl phone path + 0011-0023 + XREC backend, step 6 host gate): **phone mode end to end
  on the host**: `make test-bt-host` now also runs `test_bt_host phone` (`tests/bt_host/test_phone.c`, 228 checks):
  the real BT=1 XREC backend (`bt_xrec_bluedroid.c` + `xrec_proto.c` + `exfat.c` on a RAM exFAT card, platform
  `tests/bt_host/host_xrec.c` mirroring `bt_xrec_fw.c`) over Bluedroid + `bt_ctrl` + the fake ROM's scripted laptop,
  which this scenario extends with a laptop host stack (L2CAP signalling, SDP client, RFCOMM with credit-based flow
  control, the `+XREC` client side of `bt_sync.py`): pairing window -> page -> SSP numeric comparison -> link key
  through `bt_store` into `/bt_bonds.bin` -> SDP finds SPP "Cassini" on channel 1 -> RFCOMM -> PING/PAIR/LS -> 40000 B
  GETS with a mid-transfer stall (credits and baseband acks withheld: the session's 2 KiB in-flight cap holds) ->
  link loss -> reconnect with the stored key (window closed) -> CHAL/AUTH -> stop with a 300000 B GETS outstanding
  (636 ms, graceful) -> restart: bond from `/bt_bonds.bin`, XREC key from `/cassini.key` -> window-closed pairing
  rejected -> Just Works bond removed and link dropped -> stop; heap/task/esp_timer baselines restored after each stop.
  ASan/UBSan and TSan clean, 6/6 repeat runs. It found four real defects, all fixed:
  (1) **`bta_dm_disable()` with a connected phone took 6.5 s** (> the 2 s teardown deadline: every BT-mode exit with
  a phone connected would have taken the hard path and rebooted). `bta_sys_disable()` deregisters BTA DM itself, so DM
  never sees the link-down that would end its 5 s + 1.5 s waits; patch 0026 detaches after 100 ms and reports after
  500 ms more (disable ~0.63 s with a peer and data outstanding, 0.21 s without links).
  (2) Patch 0024: `btc_spp.c` copied `sizeof(tBTA_JV)` from smaller per-event structs (stack over-read on every
  `esp_spp_write()`). (3) Patch 0025: `bta_dm_act.c` copied 247 bytes from BTM's 65-byte device name (heap over-read on
  every bond). (4) `bt_xrec_fw.c` loaded the XREC bound key from `s_vol` before `stack_up_begin()` set it: the key was
  never loaded on target, so every reboot needed a new XREC PAIR; it now reads the context's volume.
  `tools/bt_sync.py` gains the native-SPP detection of M4 "Client compatibility" (`--transport auto|native|hfp`; auto:
  OpenFW sends nothing after RFCOMM open, the stock HFP unit sends `AT+BRSF` at once), unit-tested in
  `tools/test_tools.py`. Silicon recipe: `docs/BT_HW_RESULTS.md` "Phone mode (BT=1) on silicon". Still interim:
  deviations (1)-(7) above; on silicon XREC PAIR/CHAL fail closed until an entropy source is registered.
- 2026-11-24 (review round on the phone path: 11 findings, 3 blocking; all fixed or waived, each fix with a test
  that fails without it): (1) **BT stop crashed when a link's detach never completed** (phone out of range: patch
  0026 reports DISABLE after 600 ms either way, then `btu_free_core -> l2c_free -> l2cu_release_lcb ->
  btm_acl_removed -> bta_sys_sendmsg` posted to the freed BTU thread: assert / NULL dereference). Patch 0027:
  `btu_task_post()` fails once `btu_thread` is gone (every caller already frees on a failed post). Test: fake ROM
  `defer_detach`, phone step P12. (2) **Just Works keys could reach `SD:/bt_bonds.bin` and authorize XREC later**:
  `link_authorized()` trusted any bond. Now it needs a stored authenticated key type (`bt_xrec_bond.c`, under
  btc_config's mutex), a refused bond is purged from the BTC config synchronously (not only by BTA DM after link
  down), and unauthenticated bonds are dropped at every start before the device is connectable. The SPP server also
  sets `ESP_SPP_SEC_MITM`, but that alone does NOT refuse an unauthenticated key BTM cannot upgrade (NoIO peer, or
  unknown IO caps on a reconnect): the harness opened SPP for a stale Just Works bond with the flag alone. Steps
  P13-P15. (3) `bt_store_serialize()` cleared dirty before the write: a failed SD write was never retried; now
  `bt_store_mark_saved()` after success, bounded retry (1 s, 8 per session) and at stop (P15 with a failing card).
  (4) `bt_ctrl`: a command with the opcode of a deadline-answered one is held in the TX ring until the late reply was
  consumed, its LC link went, or 7 s more passed (its own deadline counted from when it was held, inside the host's 8 s
  timeout); before, the late reply was delivered as the new command's (`test_bt_ctrl fwd_late_same_opcode`).
  (5) CHAL without entropy armed a challenge over a zero nonce and an AUTH with HMAC(key, 0^16) in the same SPP write
  was accepted: RX now goes to xrec_proto line by line and the challenge is disarmed and the link closed at once.
  (6) The SSP part of the pairing window (discoverable, numeric comparison) closes at the first MITM bond: the device
  side does not verify the value (deviation (1)), and XREC PAIR, which used to close the window, fails without
  entropy. (7) The session block is wiped whole before it is freed (a PAIR line with the XREC key stayed in the RX
  ring). (8) The phone scenario's "congestion" never reached BTC's ring: step P5b raises the session window above
  `tx_buffer_size` (test hook) so `ESP_ERR_NO_MEM` and the `ESP_SPP_CONG_EVT(false)` resume run against the real
  `btc_spp`. (9) RAM telemetry on silicon: `BT mem` (free, min ever, largest block, malloc failures) and `BT stack
  free words` per BT task every 30 s and at stop; the host asserts a scenario heap peak against a host regression
  ceiling (x86-64 sizes are not the target's, so the plan §4 budget is checked from the silicon lines).
  (10) **ROM RNG trace done for the P-192 path** (`docs/BT_ROM_INTERFACE.md` §2.3): the ROM derives the SSP secret
  from SHA-256(BD_ADDR, one CLKN byte, four bytes of the `0x00803C24` hook) — about 33 predictable bits with the
  legacy timer hook, at most 40 even with DRBG bytes. **Waiver (bench only), deviation (8):** SSP pairing stays
  available without credited entropy, against M2.1 ("covered hooks get DRBG output") and M2.3 ("new pairing
  unavailable while not credited"), so that the silicon recipe can run; residual risk: a passive observer of the
  pairing can recover the link key. Release blockers: credited source; DRBG through the hook and into the unwritten
  bytes of the hashed block (or replace the secret at a hookable point; trace the other users of `0x0080340C` first);
  the SSP nonce source traced; the SSP window closed until `bt_entropy` reports credit; the local accept/reject prompt
  with its 30 s timeout (deviation (1); done, next entry).
- 2026-11-24 (pairing prompt, M4 "Pairing security policy"; **interim deviation (1) resolved**; host only, not yet on
  silicon): an SSP numeric comparison inside the user-opened window is no longer auto-confirmed. The session holds it
  (`bt_xrec_get_ui()` / `bt_xrec_ssp_answer()` in `bt_xrec_bluedroid.h`) and the BT screen shows the six digits in the
  device-name slot, the peer (remote name, else its address), "Pair? N s" and "ENTER Yes · BACK No"; ENTER accepts,
  BACK rejects and does not leave BT mode for that press, 30 s without an answer rejects. Also rejected: the window
  closing first (deadline, XREC PAIR, first MITM bond); a second request while one is pending (same peer: the stale
  prompt is withdrawn too, so the digits never change under the user's finger). Withdrawn without a reply: the peer's
  `AUTH_CMPL` (its user said no, LMP response timeout) or ACL down for that peer, BT stop. Outside the window:
  rejected without a prompt, as before. A key in the prompt's first 0.5 s is swallowed (`BT_XREC_SSP_ARM_MS`): an
  ENTER meant to re-open the window cannot accept a comparison that appeared at the same moment. The screen also
  shows "Pairing open N s" (window, no link), "Connected · <name or last 3 address bytes>" and "Bonded" (the shown
  peer just bonded with MITM, or its SPP link is authorized). The new strings are English literals marked
  `TODO(i18n)` (M4 i18n batch). **Bench override:** `bt_xrec_set_bench_autoconfirm(1)` (default off, survives
  `bt_xrec_start`, used only by the `SD:/BTBENCH.RUN` trigger) restores the old auto-confirm inside the window and logs
  `BENCH autoconfirm` on every use. Tests: `test-bt-xrec` [14]/[15] (accept, reject, timeout, window closing,
  peer disconnect / AUTH_CMPL mid-prompt, second request, stop, name handling, bench override; mutation-checked);
  `test-bt-host` phone step P2 now answers the prompt through the app task (`BTHX_SSP_ANSWER`) after checking the
  digits and the remote name the fake laptop reported. Lessons: (a) `ESP_BT_GAP_CFM_REQ_EVT` carries no name although
  BTA DM ran a Remote Name Request right before it (`bta_dm_sp_cback`); the session asks again with
  `esp_bt_gap_read_remote_name()` (once per peer, also for an authorized SPP link whose name is unknown), and
  `bta_dm_read_rmt_name()` drops the callback when BTM is busy, so "no answer" must mean "show the address", never
  wait. A successful `AUTH_CMPL` carries `device_name`, but a reconnect with a stored key raises no `AUTH_CMPL`.
  (b) A late `esp_bt_gap_ssp_confirm_reply()` is harmless: `BTM_ConfirmReqReply()` ignores a reply unless BTM still
  waits for that peer's numeric confirmation, so the prompt needs no cancel call when the pairing dies first.
  (c) The peer's LM gives up after the 30 s LMP response timeout, the same as the prompt's, so on silicon a slow
  answer may meet `AUTH_CMPL` failure first (prompt withdrawn, logged).
- 2026-11-24 (entropy + ROM key hardening, M2.1/M2.3/M2.5; **interim deviations (5) and (8) closed on the host**,
  silicon check: `docs/BT_HW_RESULTS.md` phone-mode recipe steps 4, 7 and 9): (1) **Correction of the ROM trace** (`docs/BT_ROM_INTERFACE.md`
  §2.3, re-derived from the ROM image by `test-bt-port`): `0x31D6C` builds the 128-bit SSP *nonce* (per-link Na/Nb,
  and the OOB R), not the P-192 secret; the local **P-192 key pair is a constant** that `rom_lm_sec_p192_init` copies
  from ROM `0xD7EC`/`0xD84C` into RAM `0x00803798`/`0x0080334C` and never regenerates (`d * G` checks; same on every
  SL6801). With it, a passive observer of any pairing computed the link key, whatever the nonces. (2) **Fix:** a fresh
  P-192 pair per BT-mode entry (`bt_port/bt_p192.c`, FIPS 186-4 B.4.2 scalar from the DRBG, constant-time ladder,
  on-curve self-check) is staged before the bring-up job and installed by `bt_rom_real` start() right after the LM
  bring-up, only if both slots still hold the ROM constant (`key_unexpected` otherwise: nothing written, pairing
  refused); stop() wipes the private slot. (3) **RNG hook per call site** (`bt_port/bt_rom_rng.c`): all 11 ROM call
  sites classified by return address; nonce / LMP-random / passkey sites take DRBG bytes from a 512-byte SPSC pool the
  app refills, and the first nonce call fills the whole 28-byte hash block (17 residue bytes + 4 hook bytes = 168 DRBG
  bits -> full 128-bit nonces); timing sites and unknown addresses keep the legacy `% 80` exactly (every caller masks
  or reduces the value: the `0..79` "contract" was never load-bearing for memory safety). (4) **Credited source**
  (`bt_port/bt_entropy_mic.c`): mic ADC 2 LSBs at gain 8 through the recorder's own capture path at each BT-mode
  entry, 2048 warm-up samples discarded, 4096 symbols through RCT/APT, **credit 0.5 bit/symbol** (a quarter of Exp 2's
  quick estimate; raise only from `ea_non_iid` on >= 1M symbols, recipe in BT_HW_RESULTS Exp 2; dumps: exp source
  5/6, `tools/bt_entropy_unpack.py`). Never touches a running recording (defers; defensive: BT mode is never entered
  while recording). (5) **Gate (M2.3):** `bt_xrec_plat_pairing_block()`: the SSP part of the window opens only with
  credited entropy, the key installed by this session's start() and no pool starvation; it is re-checked every poll
  while open (a pending prompt is rejected with it); without any entropy no window opens (screen "Pairing unavailable",
  `TODO(i18n)`). Tests: `test-bt-port` (P-192 vectors incl. the ROM and debug pairs + 400 random scalars cross-checked
  offline; hook/pool; ROM re-derivation: exactly 11 thunk calls, all tabled; mic cutoffs), `test-bt-xrec` [11] and
  [16]. Lessons: (a) read what the ROM does *after* the RNG: the first trace stopped at "SHA-256 -> 16 bytes -> 0x31A00"
  and called it the key; 0x31A00 is HMAC (ipad/opad 0x36/0x5C over the same block), i.e. f1, which identifies the
  value as a nonce, and the real key slot is a plain ROM copy. (b) An RNG hook routed by return address works because
  the ROM reaches it only through one `bl`'d thunk ending in `bx r3` (LR is the call site); `test_bt_rom_rng`
  re-checks that from the image so a different ROM revision fails the test instead of silently mis-routing.
  Residual: P-192 is the ROM's only curve (~96-bit level); the peer's nonce quality is the peer's; the key is per
  BT-mode entry, not per pairing; credit rests on Exp 2's quick estimate until the NIST run.
- 2026-11-24 (integration: entropy/ROM key + prompt + bench trigger; host and QEMU only, silicon recipe in
  `docs/BT_HW_RESULTS.md` "Phone mode (BT=1) on silicon: how to test", steps 1-10): (1) **Gate vs. bring-up order
  (bug found in integration):** `bt_xrec_open_pairing()` evaluated the M2.3 gate at once, but the session key is
  installed by `bt_rom_real` start() inside the asynchronous up job (HCI Reset in `esp_bluedroid_enable`), so a window
  opened in state UP/CONFIG was refused with "ROM P-192 key not replaced". The bench run opens its first window right
  after `bt_xrec_start()` (`pair=1`, t=0), and a user's ENTER during the ~1-3 s bring-up hit the same. Now the gate of
  a window opened before READY is decided once READY (`ssp_deferred`, debug `ssp_pending`; `BT SSP pairing window
  open (opened before READY)`); its XREC PIN is issued at once, and a deferred refusal with no PIN closes the window.
  "Pairing unavailable" is now driven by a refusal counter (`bt_xrec_debug_t.ssp_gate_refused`) in the BT screen's UI
  update, so it also appears for a deferred refusal, and holds the XREC PIN toast back for its 2.5 s. (2) **Bench XREC
  PIN** (`xrec_pin=NNNNNN` in `SD:/BTBENCH.RUN`, `bt_xrec_set_bench_xrec_pin()`): the operator cannot see the screen
  and the ring log is unreachable while BT mode runs (no USB), so an unattended `+XREC:PAIR` needs a PIN known in
  advance. Bench-only like `autoconfirm`, cleared at `bt_bench_end()`, logged per window, and still only issued when
  the DRBG draw succeeds (no entropy, no PIN). (3) **Host harness models the firmware gate:** `host_xrec.c` stages the
  key only with credited entropy at `stack_up_begin`, "installs" it when `esp_bluedroid_enable()` returned and wipes it
  at stop; `bthx_withhold_entropy()` models a BT-mode entry whose mic collection credited nothing;
  `bthx_hold_bringup()` holds the up job so the app task runs in state UP. New phone steps P16 (BACK on the prompt),
  P17 (bench autoconfirm accepts without the user; CHAL/AUTH twice, distinct nonces), P18 (no entropy: nothing opens
  even with autoconfirm on, B rejected without a prompt, CHAL fails closed; a later credit gives the XREC PIN but not
  the SSP part), P19 (bench-style start with the bench PIN: gate deferred to READY, XREC PAIR, CHAL/AUTH, LS, GETS);
  mutation-checked (no deferral: P19 and `test-bt-xrec` [16] fail; autoconfirm bypassing the gate: P18 fails).
  Lessons: (a) **the host hid the race:** the worker (priority 2) outranks the app task (priority 1) and the fake ROM
  answers synchronously, so on the host the whole bring-up finishes before the app task runs again and a window opened
  "right after start" already saw the key installed; on silicon the ROM bring-up yields. Any host test of "something
  the app does right after an async job is posted" needs an explicit hold of that job. (b) The ring log cannot be the
  channel for anything the operator needs *during* a BT run (USB is down); bench inputs go in the request file,
  results in `BTBENCH.LOG`. (c) `run_bt_xrec_mode()` in `src/main.c` is also the default image's (legacy) BT mode:
  every added line there needs the `OPENFW_BT_PROMPT` / `OPENFW_BT_BENCH` guard, or the default FIRM CRC moves (one
  unguarded `last_ui = 0u;` did, caught by the 0xC786 gate).
- 2026-11-24 (review round on the phone-mode / entropy / ROM-key work; host only, silicon recipe updated in
  `docs/BT_HW_RESULTS.md`): (1) **Invalid-curve oracle:** the ROM never checks the peer's P-192 key (no curve `b` in
  the image; `docs/BT_ROM_INTERFACE.md` §2.3 item 4), so an accepted pairing that fails tells the peer the result of
  a DHKey check with the session key. An accepted comparison whose pairing fails or whose link drops first now
  **spends the key** (`ssp_burned`): SSP refused until the next BT-mode entry (one query per key; the "~96-bit
  limit" claim now says passive only). (2) **Gate re-checked at the request and at the answer**, not only by the poll
  after the event drain: a CFM_REQ whose nonce met a dry pool between two polls was auto-confirmed by the bench path.
  (3) **Bench code only in bench images** (`make BT=1 BENCH=1` -> `OPENFW_BT_BENCH_IMAGE`): release `BT=1` images have
  no `BTBENCH.RUN` trigger, no autoconfirm, no fixed PIN (card / USB-disk access could arm them); `test-bt-xrec`
  checks the release session object for bench symbols. (4) **Mic source sanity gate** (`bt_entropy_mic_frame_check`):
  frames with a sample at >= 32000 or a peak-to-peak < 8 LSB are not credited (clipping passes RCT/APT at H = 0.5; the
  test shows it); peak logged; the radio-on retry collection is gone; the REC switch aborts the collection and skips
  keygen; the mic PGA / ADC analogue enables go off again afterwards (jack-detect bias kept). (5) **Exp 2 controls:**
  `exp_entropy_dump.c` bits 17/18 (PGA off / analogue path off) and 19 (literal gain, gain 0); restart rows across
  boots. (6) **UI:** prompt keys within 1 s after the prompt ended are swallowed (`bt_xrec_ssp_answer` -3); ENTER on a
  window with > 10 s left keeps its XREC PIN; stale prompt fields cleared at BT entry; gate-toast counter snapshot
  after start; BT status line `LONG_DOT`; toast label sized to its text (BT=1 only). Lessons: (a) **every toast in
  every image renders as a "..." pill** (`LV_LABEL_LONG_DOT` on a `LV_SIZE_CONTENT` label dots against the pre-layout
  0 px width) and the old render test only checked the pill's bounds: fixed under `OPENFW_BT_BLUEDROID` here (the
  default image must not move), **the same fix belongs on main** (done later: the guard is gone, every image sizes the toast label and
  `test_ui_render` checks toast text for BT=0 and BT=1); `make test-host` now also renders the BT=1 UI and
  asserts label texts. (b) The ADC analogue state is shared with the PB11 jack detect (`ANA_CTRL0 0x22`, keys.c):
  "power the codec down" must stop at what the capture added. (c) Health tests sized for H = 0.5 on 2-bit symbols
  are blind to anything but a stuck value; a source-specific plausibility check on the raw samples has to sit in
  front of the credit.
- 2026-11-24 (silicon bench `BT=1 BENCH=1`, CRC 0x3599, laptop as phone; diagnosis + fixes host-only): after a
  581,676-byte GETS the laptop closed RFCOMM and dropped the idle ACL (its `LMP_detach` was acknowledged), but the
  device's host never got a Disconnection Complete; its own idle `0406` left the link DISCONNECTING, so the laptop's
  re-pages were rejected `0x0F` (`l2c_link_hci_conn_req`); when the bench reopened the pairing window, `0C1A 03`
  came back `0x0C` and `bt_ctrl` stopped BT (`first code 6`, class-M rule). (1) **Fault site, proven offline:** the
  ROM's `0C1A` handler refuses page scan whenever more than one ACL link is up (`BT_ROM_HCI_MAP.md` §11; ROM
  `0x3EAC4`), here the dead link plus the rejected re-page. The fake ROM models the rule; the phone scenario's new
  step P20 replays the sequence and, without the fix, logs exactly the silicon line (`bt_ctrl fault, first code 6
  site 4 a=0x00000C1A b=0x0000000C`). **Fix:** `0C1A` answered `0x0C` is passed through, counted (`scan_refused`) and
  noted, not a BT fault (opcode table rule 7 exception); the session re-applies its scan mode when a link goes (a
  closing window is refused too, so the device could otherwise stay discoverable). With the fix P20 recovers: the
  ROM's supervision timeout ends the dead link, the scan mode is re-applied and accepted, the laptop reconnects.
  (2) **Unproven:** why the ROM kept (or silently dropped) the laptop's link. Ruled out on the host: ACL in flight at
  the detach (the ROM flushes it with a NOCP before E458, now modelled; `test_bt_ctrl detach_with_acl_in_flight`).
  Candidates and how the next run tells them apart: `BT_ROM_HCI_MAP.md` §11. (3) **Diagnostics:** every fault raise
  records a site + two values (`bt_ctrl.h BT_CTRL_SITE_*`), logged with the fault line; link notes (connection
  request / complete, disconnection complete with reason, the `0406` answer ROM or local, a refused scan enable) go to
  the session log one line each (`BT HCI ...`); `BT ROM links lc=.. ld=.. rep=..` (ROM LC/LD state per link) is logged
  on change and at a fault. (4) **"value 0" (bench autoconfirm):** upstream BTA reports a User Confirmation Request for
  a peer whose name is unknown after a name request, with a value it has not stored yet: the device showed the
  previous pairing's value or 0, so a user could not compare it. Patch 0028; P17 now fails the first name request and
  checks the logged value. Lessons: (a) a class-M write that also runs at run time (`0C1A` from every visibility
  change) needs its ROM-defined refusals listed, or a legitimate controller answer becomes a BT stop; (b) Bluedroid
  writes the scan mode twice per change (discoverability, connectability): expect two refusals; (c) the fake ROM's
  name request always succeeded, which hid an upstream BTA bug for the whole phone-mode work.
  Review round on this fix (host-only): (e) re-applying the scan mode only on a GAP link-down event left a lasting
  wrong state when the extra ROM link was one the host never knew (a rejected re-page ends with a failed Connection
  Complete): the session now retries every 3 s while refused, drains the refusal notes before the GAP events, and fails
  closed (scan off `0x00`) when a closing window's write is refused after a discoverable one (`test_bt_xrec` [21];
  P20 now ends the rejected link first, then refuses a window close: both fail without the fix). (f) The fault line
  printed `bt_ctrl_rtos`'s per-boot first code next to this session's site: now one `bt_ctrl_fault_format()` line
  read under one critical section, plus `bt_ctrl_stats_format()` (`unk short late unsol fwd_to late_reply late_drop
  scan_refused notes_lost`) at a fault, at stop and in every BENCH status, for the hypothesis that `bt_ctrl` dropped an
  E458. (g) `BT ROM links` change logging is bench-image only and keyed on LC/LD bytes. (h) §11's `0406` mapping
  fixed: a silent ROM shows as `st=0x22 local` after 7 s, not `0x02`.
- 2026-11-24 (silicon session 6 follow-up, host-only; release `BT=1` at `488da72`, laptop BlueZ, first pairing, no
  device log: the ring log was lost to a power cycle). (1) **Prompt showed the address, not "martianbfg".** Code path
  (proven on the host, not on silicon): on an incoming link whose name is unknown BTM sends its own `0419` at Connection
  Complete (`btm_sec_connected` -> `btm_sec_execute_procedure` -> `btm_sec_start_get_name`, origin SEC: its answer goes
  to BTM only, never to BTA/BTC). BlueZ authenticates at once; when the User Confirmation Request comes while that
  request still runs, BTA's own `0419` (`bta_dm_sp_cback`, patch 0028 path) and the session's (`request_name`) are
  refused by the ROM with CS `0x0C` (RNR already running, `0x3F348`, env `+0x217`, `BT_ROM_HCI_MAP.md` §2): the prompt
  comes nameless and nobody asks again, so the late success never reaches the screen. Unproven on silicon: whether
  BTM's request was late (this path) or failed outright (no name ever; the address is then correct), or the ROM SAVED
  it (`st=0x22 local` after 7 s). **Fix:** `bt_ctrl` notes every `0419` answer and every Remote Name Request Complete
  and keeps the newest successful name (`bt_ctrl_peer_name`, BD_ADDR order); the session takes it on the note and the
  prompt's peer line changes from the address to the name in place (digits unchanged). Inquiry/EIR names are
  irrelevant: the device never inquires. (2) **"verbunden" after BACK.** The screen's "connected" was `acl_links > 0`
  (GAP link events) or SPP open. After a rejected comparison BTM disconnects the link itself (`btm_simple_pair_complete`
  -> `0406` reason `0x05`; host log `BT HCI disconnect cmd`), and the laptop drops at the same time: a link-end with
  a late or missing Disconnection Complete (`BT_ROM_HCI_MAP.md` §11 H1/H2, open) keeps `acl_links` at 1. Proven on the
  host that such a link stays "connected" indefinitely; unproven that this is what happened on silicon (no log).
  **Fix:** a link whose `0406` the host sent (bt_ctrl DISC_CMD note, any answer) no longer counts as connected for the
  screen (`bt_xrec_get_ui().connected`, `bt_status_t.acl_connected`); `acl_links` (scan-mode rule) is unchanged until
  the link-down event. **Tests** (each fails without its fix, checked): phone P16b (fake ROM: `fake_peer.name_hold` /
  `fake_peer_name_release`, `0x0C` while one is held; BACK with `defer_detach`), `test_bt_xrec` [22],
  `test_bt_ctrl name_notes`; `test_ui_render` BT=1 checks the in-place name update and the waiting line after BACK.
  **New session-log lines:** `BT HCI name request st=.. rom|local`, `BT HCI name complete st=.. len=N`, `BT SSP prompt
  shows the peer name, ms after the prompt N`, `BT SSP prompt ended without the peer name (address shown)`. Next
  silicon run: first pairing with a laptop the device has not seen (`bluetoothctl remove` on both sides) and read the
  order of `connection complete` / `name request` / `name complete` / `waiting for the user`; after BACK expect
  `disconnect cmd` and then (or never) `disconnection complete`, while the screen already shows the window line.
- 2026-11-24 (review of the session 6 follow-up, host-only). (a) A `0406` note drained before its own GAP link-up
  event (both in one poll, notes first) is kept for that handle and applied when the link comes up; the handle's next
  Connection Complete note or its link-down clears it (`test_bt_xrec` [23]). (b) `bt_ctrl` stores the remote name
  before it queues the name note, so a task popping the note always finds it (`test_bt_ctrl name_before_note`). (c) a
  successful name complete with an empty name no longer applies the older stored name to the current peer. (d) the
  "ended without the peer name" line is not written when the name reached the screen through the poll's last note
  drain. (e) `bt_ctrl` link-note ring 8 -> 16 (a pairing adds up to four name notes). (f) `BENCH status` keeps
  `acl=` (now the screen's "connected", which hides a link the host is closing) and adds `links=` / `closing=` (raw
  ACL count, links with `0406` sent), so the H1/H2 check reads `links=1 closing=1` where it read `acl=1`. **Choice:**
  the closing mark is set whatever the `0406` answer; a refused `0406` (`0x0C`) or a local deadline answer (`0x22`) can
  then show a live link as not connected until it is reported down or SPP opens. **Not covered:** if the laptop drops
  first and BTM's `0406` is refused (`0x02`, marked) or never sent, and the Disconnection Complete is also lost, the
  screen still shows "connected". **Silicon check list:** after BACK read `disconnect cmd` (status, rom/local), the
  `BENCH status` `links=` / `closing=`, and `notes_lost` in the ctrl stats line (a lost note silently undoes (a)-(c)).

- 2026-11-24: **Review r1 disposition** (agy post-development review, branch `review/r1-28c8def`, 23 commits;
  triage against HEAD `4c83b3d`). Accepted = cherry-picked; ported = applied by hand to the moved code; review
  Bluedroid patches 0028-0032 renumbered 0029-0033 (dev already had 0028). Each applied fix keeps its test; every
  important one was checked to fail without its fix (exceptions noted). Default FIRM CRC unchanged.
  - port `e7fd4dd` -> `390ce27` bt_ctrl: purge held inbound ACL at the DISC/CONN_CMP delivery marker, drop ACL_RX on
    dead/out-of-range handles (a credit-blocked old-peer frame could reach a reused handle).
  - port `61c8d34` -> `a916e6a` bt_ctrl: consume unsolicited ROM CC/CS instead of forwarding a spurious credit/reply.
  - port `259cac3` -> `cbda816` bt_ctrl: 0x0C13 forwarded as 249 B with a NUL guard for ROM 0x3ED00's scan.
  - reject `a979fa7` SLOT_FILLING cancel: disconnect is already covered by the dead-handle drop; the Reset/reuse
    windows are not reachable in practice.
  - accept `bcd1858` -> `2febed5` bt_ctrl: no 0 ms timeout cascade after a `fwd_late_hold` expiry.
  - port `3386e7b` -> `230b493` bt_ctrl: ST_STOPPING completes as STOP_PARKED when parked (was stuck PENDING).
  - reject `7f5d637` NOCP clamp / NULL ROM params: scenario does not match the code (rom_nocp already clamps; packer
    n <= 7).
  - accept `3d1456d` -> `f847255` bt_ctrl_rtos: silence RW-BT and unwind a leaked PRIMASK before blocking (no host
    test possible: no-op portmacro).
  - reject `b670e8a` EM RX pool pointer validation: breaks continuation-fragment reads (`h->em + h->off`); rationale
    does not match the diff.
  - port `801bac8` -> `9b72da3` bt_alarm/esp_timer: (cb,arg) snapshotted inside the drain critical section (preempting
    `esp_timer_delete()` made the trampoline call NULL); 64-bit timeouts.
  - reject `693a8d9` bt_store nvs_get_str NUL / bounds: no caller of nvs_get_str; described header layout does not
    exist.
  - reject `c26b588` bt_entropy fractional stats: diagnostic counter only; seeding uses the exact x16 credit.
  - port `d06549a` -> `20d1925` Bluedroid **0029** osi alarm callback revalidated under `alarm_mutex`.
  - port `2d78e2a` -> `61deb19` Bluedroid **0030** stop `hci_host_thread` before HCI env teardown (negative test
    aborts in 3 of 4 runs; scheduling dependent).
  - port `e7effa4` -> `8724266` Bluedroid **0031** `bta_sys_free()` clears `bta_alarm_hash_map`.
  - port `d50eb0c` -> `e16e354` Bluedroid **0032** `config_parse()` NUL guard + stale `bt_cfg_key1+` erase (quota
    wording corrected to 4 KiB; the "resurrected sections" claim dropped).
  - port `f74ce88` -> `8ab815d` Bluedroid **0033** short L2CAP ACL bounds on unknown handles.
  - reject `3da75d6` bond case-variant shadowing: firmware always writes lowercase sections; only a hand-edited
    plaintext SD file can produce it.
  - reject `d33af3c` encryption-off close / cfm_ok expiry / BDA slots: encryption-off needs the authenticated peer
    itself and the close would kill sessions across a role switch; the cfm_ok/slot parts are wrong or churn at HEAD
    (NC confirm UI rewrite).
  - port `28591dc` -> `e9f0e6d` bt_xrec: RX bound to the active SPP handle, overflow latched/coalesced, chunk wiped
    (test loop uses 100 B frames so the EVQ fault path is exercised).
  - port `77034e0` -> `d7baca0` bt_xrec: SPP deinit also when the init was posted but not yet processed (test covers
    the flag lifecycle only).
  - accept `883f0ac` -> `2ae38ee` bt_xrec_fw: finalise an active recording before an XREC reboot; volatile key wipes
    (no host test).
  - accept `8b9dc77` -> `360c188` tools: split `+XREC:ERR` framing, atomic 0600 key file, probe-log token guards
    (self-test's always-true assert replaced by a real check).
  Silicon checks owed: disconnect during bulk inbound transfer then reconnect (R1-11); OTA during a recording.
- 2026-11-25: **Review r1 verifier lows** (three follow-ups on applied r1 fixes; each test fails without its fix).
  - `390ce27` (R1-11): the DISC / CONN_CMP inbound-ACL marker (`acl_min_seq`) never expired, so 2^31 RX sequence
    numbers after it `seq_before()` wrapped and every new ACL on that handle was purged as stale. A full
    `purge_stale_held()` pass now clears each marker it served (taken as one snapshot; cleared only if bt_dlv has not
    replaced it meanwhile): every later ACL_RX is numbered after it. `bt_ctrl_debug_t.acl_markers` shows live
    markers. Test `acl_marker_expiry` (test hook `bt_ctrl_test_advance_rx_seq`, `-DBT_CTRL_TEST_HOOKS=1` in
    test-bt-ctrl): 2^31 + 16 numbers after a reconnect ACL is still delivered; the R1-11 purge still works across
    the counter wrap.
  - `f847255` (R1-15): on a leaked ROM lock `bt_ctrl_task` unwound only `bt_rom_real` and then forced `more = 1`, so
    with any other ROM (the fake ROM) it re-ran at once forever, posting a fault per pass. The decision is now
    `bt_ctrl_leak_check()` (`bt_port/bt_ctrl_leak.h`): unwound -> re-run once to finish the stop; still held -> no
    immediate re-run (the task blocks; only reachable without PRIMASK) and one fault per leak. Test
    `lock_leak_recovery` drives it as the task loop does (1000 passes: 0 re-runs, 1 fault).
  - `d7baca0` (R1-18): the test covered only the `bt_xrec_spp_deinit_needed()` flag. Phone scenario step P0
    (test-bt-host) now holds BTC_TASK suspended after the up job so the SPP INIT stays queued, stops in CONFIG and
    checks the down job calls `esp_spp_deinit()`, SPP UNINIT arrives, and the next session reaches READY (without the
    fix: deinit skipped, next session FAULTED with CONFIG, SPP INIT -> `ESP_SPP_NEED_DEINIT`).
- 2026-11-25 (step 8, host and QEMU only; branch `feat/bt-step8` from `c15f6b2`; silicon recipe: `docs/BT_HW_RESULTS.md`
  "Exp 5"). Gap audit of step 8 against `c15f6b2`, then everything that needs no hardware:
  (1) **UI/i18n:** all BT=1 strings (phone prompt `Pair? N s` / `ENTER Yes · BACK No` / `Pairing open N s` / bonded,
  "Pairing unavailable", Bluetooth choice, headphones screen and messages, Settings "Audio output" row and values,
  player headset toasts, the new low-battery toast) moved to a separate table `include/openfw_i18n_bt.h` +
  `src/ui/ui_i18n_bt.c`, 43 strings x 18 languages, compiled only with `BT=1` (the BT=0 image keeps CRC `0xA3BB`:
  `openfw_i18n.h`'s enum and table untouched). Flash only (.rodata in XIP). `test_ui_render` (BT=1 build) checks
  every string: non-empty, every glyph in ui_font_sm/md, its UTF-8 bytes against the buffer it is copied into
  (`list_lbl` 26 -> 34, `list_foot` 44 -> 64 bytes: Cyrillic takes 2 bytes a letter) and its pixel width against the
  label that shows it (toast 136 px, footers 154, list rows by their value column, the Settings row with "- X +").
  The English texts that did not fit their label were shortened (the old headphones hint was cut by `list_foot`).
  Settings "Audio output" values are now Speaker / BT / Both. Left English: the headset-button probe (diagnostic,
  shared with BT=0).
  (2) **WDT supervisor** (M4 "Watchdog"): `bt_port/bt_task_sup.c`. The app task (lowest priority) feeds the WDT, so
  a feed already proves no task above it hogs the CPU; the supervisor adds liveness: while the stack is enabled the
  app posts a ping work item to hciT, BTU and BTC (osi queue 0, no wait) once a second. Idle threads answer at once
  (idle bonded phone, peer-stalled transfer: healthy); a ping unanswered 8 s = platform fault 4 (`BT task stalled:
  <task>`, graceful recovery or the hard path); unanswered 20 s = `wdt_feed()` stops writing the WDT (BT=1 only,
  weak hook in `wdt_timer.c`; the experiment deadline still extends: the separation the plan asks for). Armed at the
  first platform poll, disarmed when the teardown starts (the 2 s window and the bring-up keep their own deadlines).
  bt_ctrl keeps its hang / CPU-share guards; bt_dlv blocks only on the hciT/BTU queues. Tests: `test_bt_task_sup`
  (unit, `test-bt-port`) and the real Bluedroid threads in `test-bt-host` phone P7b (idle bonded phone), P8 (stalled
  transfer, 2 stall windows) and P22 (BTC blocked -> fault 4 -> FAULTED, no hard path -> restart). State in
  SRAM_HI; `.bt_bss` +8 B (heap_5 region 0: 33,776 B, floor 32 KiB).
  (3) **Power transitions:** audit of power-off, USB, card, REC switch and XREC reboot paths: all already went through
  `bt_shutdown()` (table in BT_HW_RESULTS Exp 5). New: **low battery** (`bt_port/bt_lowbatt.h`, the recorder's
  3450/3550 mV thresholds, 3 samples 0.5 s apart, VBUS cancels): phone mode leaves BT mode, headphones mode stops
  (home loop, Bluetooth screens, player), a start is refused; toast "Low battery: BT off"; `BT vbat mV last/min` logged
  every 30 s while BT runs (Exp 5 measures the sag under RF load). Unit test `test_bt_lowbatt`. Not changed: a USB
  cable plugged in phone mode keeps BT mode (as legacy; USB disk mode after BACK), no card-removal detection (§6.7 of
  the A2DP design). Needs hardware: Exp 5 E1-E8, the supervisor's real ping latencies (E7), VBAT under load (E6).
- 2026-11-25 (step 10, `integ/bt-all-local`): merged `fix/bt-verifier-lows` (41735ee), `fix/toast-text-all`
  (b389642) and `feat/bt-step8` (e44dcea). Conflicts: `test_ui_render.c` (the toast checks now run for every image; the
  BT=1 string checks sit in the BT=1 block before `check_bt_ui`) and this log (both entries kept). The step 8 entry's
  "BT=0 keeps 0xA3BB" held for its branch only: the toast fix (0x7EC9) and the review fixes below change the default
  image. Then the **joint review** of `c15f6b2` (Codex 18 + agy 11 findings), triaged in
  `docs/reviews/STEP10_JOINT_REVIEW.md` (raw: `STEP10_CODEX.md`, `STEP10_AGY.md`). Fixed: XREC DEL of the file being
  recorded (`+XREC:ERR,BUSY`), PAIR acknowledged only after `cassini.key` is written (`+XREC:ERR,SAVE`; also legacy
  `bt_hci.c`), bond store kept over an older file after a failed write and never written over an unreadable file
  (`s_bonds_hold`), Just Works hook refuses outside a headphones session (phone P11/P13 now expect the refusal),
  headphones numeric comparison bounded by the window deadline, teardown hard path before freeing the session and a
  stuck tick escalated, start refused after an unconfirmed A2DP deinit, `conn_hdl` invalidated on link down, Forget
  only while READY, discovery-cancel deadline, Play blocked on route NONE, About reboot stops BT, bench PIN 0000 cleared
  after the run. New tests: `test_core` (DEL busy, PAIR save failure), `test_bt_xrec` (PAIR save failure), phone P15b
  (unsaved bonds kept; unreadable file kept), headphones S10c (Forget off, Just Works with no session); the P15b, S10c,
  DEL and PAIR checks fail without their fixes. Gates (0 warnings): FIRM CRC def 0xECB8, bt 0xA199, hp 0x663A,
  hpb 0xD52F, bench 0x4FED. Open: A5 XREC TX flow control, C3, A3 key downgrade, A6/A9/C7/C11 OOM paths, C13; hardware:
  C5, headset IO capability (C2/A8), C18 address, A10, A11, Exp 5.
- 2026-11-25 (`fix/s10-xrec-flow` from `ab27527`): review STEP10 **A5** XREC TX flow control. `xrec_on_rx()` now returns the bytes consumed and, with the new optional `xrec_ops_t.tx_free`, holds back any byte that would produce a reply (line end, PUT header `:`, last PUT payload byte) while the TX ring has less than `XREC_REPLY_MAX` (576 B) free. `bt_xrec_bluedroid.c`: DATA records move into `rx_pend` (bytes stay in the RX ring until parsed) and are parsed before the next event; while a command is held the control events behind it still run, since the WRITE / CONG events among them are what drains TX; held bytes are dropped on close or a new handle; `rx_held` debug counter. Lesson: blocking the head of the event FIFO on TX room would deadlock (TX room comes back only through WRITE_EVT). Legacy `bt_hci.c` path unchanged (`tx_free` 0). Also the C1 residual: `exfat_delete_file()` no longer frees clusters when the shared-cluster scan fails with an I/O error (returns -1, clusters leaked). Tests: `test_core` `test_xrec_flow_control` and the delete-with-I/O-error check, `test_bt_xrec` [6b] (8 pipelined LOG replies against a congested BTC: 4 dropped without the op); all fail without their fix. `xrec_proto.c` and `exfat.c` are in the default image (as is the `bt_hci.c` ops table), so the BT=0 CRC changes: 0xECB8 -> 0x568B (baseline rebuilt in the same worktree path). Gates (0 warnings, all rc=0 incl. QEMU rtos / bt / hciprobe): FIRM CRC def 0x568B, bt 0xEF4C, hp 0x89B0, hpb 0xEBAB, bench 0x8934 (path-dependent, `voice-recorder-hw-bt-usb-xrecflow` worktree).
- 2026-11-25 (`test/aout-both-mode`, from `ab27527`): **output mode Both** read end to end (Settings row -> `bt_hp_cfg.bin`
  `out_mode` -> `aout_set_mode` -> `aout_start` route BOTH -> tap -> DAC; player volume split; PA). Mono files after
  the mono pass-through (`8983cb7`) are fine: the ring stays mono and `hp_sink_tap_peek` expands to L = R for the DAC.
  Bug found: when the app task is out longer than the DAC's 4 slots last (silicon 300-480 ms), the DAC runs dry while
  the headset keeps reading; the tap then trails the headset by the whole gap for good, the speaker stays late and the
  tap holds that backlog in the ring, so the headset's ring shrinks after each gap and starves (23 empty packets in
  3 s). Fix (`src/audio_out.c` `pump_dac`): with the DAC found dry and a backlog above what its queue takes at once
  (`AOUT_RESYNC_FRAMES` = 4 x 512), the tap drops it and restarts in step with the headset
  (`aout_stats_t.dac_resync_frames`). Tests: `test_player_bt` (test-bt-hp) now plays Both with mono and stereo files,
  headset connected (both outputs complete, sample exact, speaker never ahead of the headset, one volume = bt_vol) and
  not connected (speaker only, speaker volume), absolute volume (headset gain unity, speaker at bt_vol), and mono with
  the app task out 400 ms/s (0 empty packets; 23 without the fix). Headphones S1 checks that the mode is saved
  (byte 5 of `bt_hp_cfg.bin`, only on change). BT=1 only: default image unchanged (0xECB8); gates 0 warnings, bt
  0xECD8, hp 0xF183, hpb 0xA698, bench 0xC057 (CRCs depend on the worktree path).
- 2026-11-25 (STEP10 A6 + C13, branch `fix/s10-avdt-leak` from `ab27527`; host only): patch **0043**:
  `AVDT_Deregister()` / `AVCT_Deregister()` free the stream/channel/link blocks a frozen headset's link left allocated
  (they were lost at the next Register's memset: 31 blocks / 3.4 KB per run of S8b + S10, ~600 B more with AVCTP up)
  and 0039's local close runs the stream state machine directly instead of posting an `osi_malloc`'d `BT_HDR`. The
  session escalates job rc 5 (A2DP/AVRC deinit not confirmed) to FAULTED (`BT fault` on the Headphones screen,
  `last_fault` DOWN_FAIL; start still refused until reboot, no hard path). C13: the A2DP target is mirrored under the
  session lock, and `a2d_cb` leaves TX, ACL handle, pending flags and codec cache alone for another device's late
  CONNECTED / DISCONNECTED / AUDIO_CFG. Harness levers: `bth_heap_fail_task_size()` (BTU_TASK's 8-byte allocations
  fail during S10's frozen stop), `bth_btc_run()` (late events injected on BTC_TASK, S10a),
  `bthx_force_profile_down_fail()` (S10d). Each part reverted alone fails `test_bt_host_hp hps` (AVDT / AVCT: leak 31 /
  6 blocks; 0039: deinit timeout, FAULTED; session: S10a 5 checks, S10d 2 checks). BT=0 CRC unchanged 0xECB8.

- 2026-11-25 (`fix/s10-oom-paths` from `ab27527`): step-10 review A9 / C7 / C11, out-of-memory paths. Patch 0044
  (`btc_spp.c`, `btc_dm.c`): SPP DATA_IND and write-completion hand-offs fall back to a by-value BTC work item (a lost
  write completion left `is_writing` set: TX stalled for good); an RX frame whose queue node cannot be allocated breaks
  the stream (dropped with later frames, credits returned, reported as DATA_IND status `ESP_SPP_NO_RESOURCE`; the
  session closes the link); static FreeChannel user data in SPP deinit / stop server; a link key that cannot be stored
  ends the pairing with NOMEM instead of `BTC_ASSERTC`. Session: a refused `esp_spp_disconnect()` is retried every
  100 ms, 20 times, then `BT_XREC_FAULT_CLOSE_FAIL` (C11). BT images replace picolibc's `__assert_func`: an assert in a
  BT task is handed to the app task, which takes the BT hard path (recording finalised, fault 0xB1 reason 11 + line);
  backstop 1.5 s, then the old reboot. Host levers in `tests/bt_host/host_oom.c` (wrapped `btc_transfer_context`,
  `fixed_queue_enqueue` on BTU, `btc_storage_add_bonded_device`, one BTC allocation size); phone P7c / P20b / P21 fail
  without the fixes (stall, three different asserts, open link). The harness publishes its snapshot before a call's
  "done" (a stop's state could read stale: rare `state 3 after stop`). The assert routing has no host harness
  (picolibc and `bt_xrec_fw.c` are target only). FIRM CRC: def 0xECB8 (unchanged), bt 0x0022, hp 0xDFFC, hpb 0xBF95,
  bench 0x88C8 (worktree path). Residual: a lost CLOSE / SRV_OPEN hand-off in `btc_spp` (still logged only).
- 2026-11-25 (`fix/s10-sec-policy` from `ab27527`): STEP10 **C3**, **A3 second part**, **C6**. C3: `bt_rom_rng_starved()`
  (lock-free read of the hook's `starved` count, latched until the next session's `bt_rom_rng_reset()`) behind the new
  platform seam `bt_xrec_plat_rng_starved()`; the headphones Just Works hook (BTC), numeric comparison and bench PIN
  replies read it next to the polled `gate_ok`, so a nonce drawn from a dry pool after the last poll no longer lets
  that pairing through (phone mode re-runs the whole gate at the request and at the answer already). A3: Bluedroid
  patch **0042** (`btc_dm.c`): Just Works is refused before a key exists when the stored bond for that address has an
  authenticated key (read from the bond config, not BTM's record, which BTM clears on its Key Missing retry); an
  unauthenticated key reported anyway (legacy PIN) is not stored over it and the pairing is reported failed; stored
  bonds are no longer removed on `HCI_ERR_AUTH_FAILURE` / `HCI_ERR_KEY_MISSING`. Within the session BTA still drops
  BTM's RAM record after such a failure; the bond file reloads it at the next BT-mode entry. A headset bonded with
  numeric comparison that lost its key must be Forgotten before a Just Works re-pair. C6: threat model above (§5).
  Tests: `test_bt_rom_rng` (latch), headphones **S11** (`test_bt_host_hp hps`): C3 probe fails without the hook's latch
  read; the Just Works over a MITM bond fails without 0042's confirmation check; the final reconnect with the MITM key
  fails without the AUTH_FAILURE keep, and without both 0042 checks (the storage check alone keeps the key). Gates
  (0 warnings, all rc=0): FIRM CRC def 0xECB8 (unchanged), bt 0x4F01, hp 0x3D1D, hpb 0x343F, bench 0x6FC3.

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
| agy B4 (WFI starves idle; late SysTick) | `OPENFW_TASK_WAIT()`; SysTick before scheduler; reap test |
| agy B5 (.ram_boot 172 B, missing consumers) | IRQ handler in `.fast_ramtext`; table completed |
| NB items (WDT, sink ops, NVS cache, timer daemon, wasm/k_exp_api, NVIC helper, PRIMASK, byte-identity scope, trace scrubbing, Exp 2 ordering) | M4 WDT/sink/i18n; M2 store/timer; §3 rules; §6 gates; M3 traces; step 4 |
| **Round 2** | |
| Codex N1 / agy NB2 (SysTick before SHPR3) | SysTick started in `vPortSetupTimerInterrupt()` |
| agy NB1 (timer daemon priority) | `configMAX_PRIORITIES - 1`, static assert |
| Codex N2 (GC can drop reference table) | `KEEP(.bt_link_roots)` + post-link `nm` check |
| Codex N3 / agy N1 (TX slot leak, notify from bt_ctrl) | pure capacity query; send-available via `bt_dlv` |
| Codex N4 / agy NB4 (ACL window vs storage) | 4 × 1021 B window overrides; byte-capacity assert; credit before ROM accept; alloc-failure test |
| agy NB3 (target region) | `.bt_data`/`.bt_bss` at start of `RTOS_HEAP` |
| Codex NB (IPR units; card swap; WDT before phone test) | §3 IPR bytes; step 6 gate |
| agy A1/A3 (ROM C2H support; NOR UID safety, LAA bits) | Exp 1 stage C question; identity paragraph |
| **Round 3** | |
| Codex N1 / agy N1 (`check_send_available` has no packet type) | unified any-type TX ring; credit gating at drain |
| Codex N2 (events stall behind deferred ACL) | RX dispatcher order: events first, credit check before fragment mutation, kick on credit return |
| Codex N3 (bonds read at `esp_bluedroid_init`) | cache loaded before init; cold-boot bond test |
| agy NB (0C33 call site; header comment) | `controller.c` patch with 1021 B constant; comment fixed in `1d3e5db` |
| **Round 4** | |
| Codex N1 (bounded alarm post loses one-shot expiry) | `expiry_pending` + retry re-arm, exactly-once, cancel clears; test |
| agy NB (TX head-of-line; 1005 advertisement) | command-before-blocked-ACL drain; 1005 ≤ immediately drainable |
| **Round 5** | |
| Codex N1 (retry re-arm can fail) | no retry timers: BTC drains pending alarms after every message; full queue ⇒ BTC runs again |
| Codex N2 (stale expiry after cancel/re-arm) | per-alarm generation validated at dispatch; cancel/free unlink + bump |
| agy NB (timeout-0 BTC post, no `alarm_mutex` in daemon, periodic alarms) | non-blocking transfer variant; critical section; periodic coalescing |
| **Round 6** | |
| Codex N1 / agy NB1 (drain post allocates) | `osi_thread_post` work item, allocation-free; heap-failure test |
| Codex N2 (re-arm between validation and callback) | 4a: per-user re-validation under its own lock; `hci_layer.c` command generation; audit all alarm users |
| agy NB2 (hook placement, flag hygiene, memset) | end of `btc_thread_handler()`; flag cleared on failure/start; generation preserved |
| **Round 7** | |
| Codex N1 (timer command lag: old expiry stamped with new generation) | `bt_alarm` service: caller-synchronous list + generation under one critical section; no FreeRTOS timer queue; interleaving tests |
| **Round 8** | |
| Codex N1 (`0C35` cannot leave hciT at zero command credits) | `hci_layer.c` NOACK queue ahead of the credit check; local `0C35` in bt_ctrl; through-hciT test |
| agy NB (sub-tick busy spin) | wait rounded up to ≥ 1 tick |
| **Round 9** | |
| Codex N1 (enqueue node allocation failure drops packet) | fail-soft `host_recv_pkt_cb` + slot restore; `bt_dlv` retains/retries/restarts; allocation-failure sweep over all scenarios |
| **Round 10** | |
| Codex N1 (recovery needs memory) | recovery ladder: graceful with deadline → heap-free hard path (finalize recording, fault record, reboot, BT off next boot); persistent-failure test |
| agy NB (D3 wording; restart from app, not bt_dlv) | D3 lists the documented patches; BT fault posted to the app task |
| **Round 11** | |
| Codex+agy N1 (malloc-failed hook reboots, malloc never NULL on target) | hook counts and returns under BT=1; real-allocator exhaustion test with recording active |
| Codex+agy N2 (`esp_bluedroid_disable` waits forever) | timed disable variant over `future_await_timeout`; withheld-completion test |
| **Round 12** | |
| Codex+agy N1 (disable/deinit block on posts before any timed wait) | `bt_recover` worker; app waits with one absolute deadline and never calls Bluedroid in recovery; stall tests at each blocking point; timed-variant patches dropped |
| agy NB (hook has no size) | failed size recorded in the `malloc` wrapper |
| **Round 13** | |
| Codex N1 (recovery wait drops audio) | app keeps calling `recorder_poll()` while waiting; continuity test with identifiable PCM |
| agy NB (bt_recover memory; wording) | static in `.rtos_static`, created once; hook wording fixed |
| **Round 14** | |
| Codex N1 (deadline unenforceable during long SD write) | SysTick-ISR deadline masks IRQ 26 + `bt_stop` flag checked by bt_ctrl; app finalises when its call returns; SD-write-crossing-deadline test |
| agy NB (any notification wakes the app) | explicit `bt_recover` done/status variable |
| **Round 15** | |
| Codex N1 (continuity beyond DMA ring) | bound equals normal recording (3 × 128 ms); longer stalls use the existing overrun path, tested both sides |
| Codex N2 (bt_stop check vs ROM entry) | guarantee: IRQ masked at deadline, at most one in-flight bounded ROM call (≤ 1 ms, measured Exp 1); fake-ROM race test |
| agy NB (success near deadline) | disarm first, then check latched `bt_stop` → hard path |
| **Round 16** | |
| Codex (lossless bound depends on queue state; overrun not persisted) | free-slot bound with pinned initial state in test; live counter asserted before reboot; persistence = recorder follow-up |
