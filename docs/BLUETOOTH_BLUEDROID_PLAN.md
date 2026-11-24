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
  **Interim deviations** (to close in step 8 / follow-ups): (1) no local accept prompt yet (needs i18n strings): a
  numeric comparison is auto-confirmed only inside the user-opened pairing window, its value is shown through
  `bt_status_t.pair_pin` (the BT screen's existing "PIN" toast) and logged; outside the window it is rejected.
  (2) Just Works cannot be refused at the request (`btc_dm.c` auto-confirms it without an app event): the bond is
  removed at `AUTH_CMPL` (unauthenticated key type, or not confirmed in the window) and the link dropped; passkey
  display/entry likewise rejected. XREC bytes flow only on links bonded that way. (3) The teardown deadline is a
  FreeRTOS timer-daemon one-shot (priority 9; preempts the app even in an SD write), not a SysTick-ISR hook, to keep
  `startup.c` untouched. (4) `bt_recover` stack/TCB live in SRAM_HI (`.bss_hi`) instead of `.rtos_static`: with
  bt_ctrl's rings and stacks linked, RTOS_HEAP would fall below the 32 KiB heap floor. (5) No credited entropy source
  is registered in firmware yet, so `bt_entropy_get()` fails: the SSP window still opens, but XREC PAIR answers
  PAIRCLOSED and CHAL closes the link (fail closed) until Exp 2's source is wired. (6) Without eFuse the BD_ADDR is a
  fixed fallback (probe scheme), never timer-derived; NOR-UID derivation pending. (7) `main.c` still calls
  `bt_rf_init_calibrated()` before `bt_xrec_start()`, so RF is calibrated twice per BT-mode entry (bt_rom_real
  calibrates again at HCI Reset). (8) SSP pairing without credited entropy on a ROM-generated P-192 secret of ~33
predictable bits: bench-only waiver, see the review-round entry below.
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
  with its 30 s timeout (deviation (1)).

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
