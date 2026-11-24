# Bluetooth Classic via Espressif Bluedroid — Integration Plan

Status: **plan / not started**. Goal: two modes on the SL6801 (BR/EDR only, ROM `RW-BT` controller):
1. **Headphones**: A2DP Source (SBC) + AVRCP Target (+ AVRCP controller for headset buttons optional).
2. **Phone companion**: SPP (RFCOMM) carrying the existing `+XREC v5` protocol (`xrec_proto.c`).

Host stack: ESP-IDF `components/bt` (Bluedroid, Apache-2.0), vendored — not the whole IDF.

## 1. What we found (esp-idf master, checked 2026-11-24)

Bluedroid C size (lines): stack 139k, bta 71k, btc 47k, api 12k, external (incl. SBC enc/dec) 6k, hci 3k.
We compile only the profiles we need (GAP, SDP, L2CAP, RFCOMM, SPP, AVDT/A2DP, AVRC), so the linked size will be a fraction; **measure early** (Phase 1).

Porting surface is small and well-defined:
- `components/bt/common/osi/*` (thread, mutex, semaphore, alarm, future, allocator, queue) — FreeRTOS-based.
- `host/bluedroid/hci/hci_hal_h4.c` + `hci_layer.c` — transport to controller. We replace the HAL.
- `esp_bt.c` controller bring-up — not used; our `bt_rf.c`/`bt_hci.c` already do that.
- Config headers (`sdkconfig.h`, `bt_user_config.h`) — we hand-write a minimal one.

Hard constraint: Bluedroid runs **three tasks** (HCI host, BTU, BTC) and uses **blocking waits** (`future_await`, `osi_sem_take` in `controller.c`, `btu_hcif.c`, `esp_bt_main.c`). A single-threaded cooperative shim would deadlock on those. open_fw is currently a bare-metal super-loop.

## 2. Architecture decision (needs user sign-off)

**Recommended: vendor FreeRTOS-Kernel (MIT, Cortex-M4F port) and run only BT as tasks.**
- BT tasks (HCI/BTU/BTC) at higher priority; the existing `main` super-loop becomes the lowest-priority "app" task (or idle hook). ISR-driven audio DMA, USB, SD stay as they are.
- OSI layer then maps ~1:1 onto FreeRTOS, so Espressif code runs unmodified. Fewer patches to upstream = fewer bugs and easier rebases.
- Cost: ~10 KB flash, a few KB RAM, SysTick+PendSV+SVC claimed. Must check they are free (CPU0) and that WFI/sleep logic in the idle hook still works.
- Rejected: writing a cooperative OSI shim (deadlocks, diverges from upstream); BTstack (license, see earlier discussion); keeping the hand-rolled L2CAP/RFCOMM (bug source).

Open question: BT stack on CPU0 (with FreeRTOS) vs CPU1 (currently the LVGL worker). Default: CPU0, because the ROM controller tasks (`ke`) and IRQ 26 live there.

## 3. Memory budget (must be written down before coding)

SRAM: main `0x0081A380 +0x35C80` (~220 KB), `SRAM_HI` 44 KB (file list), `_ebss < 0x00850000`.
Targets to fill in from Phase 1 measurement: Bluedroid heap, 3 task stacks, SBC encoder state, A2DP PCM ring, RFCOMM buffers. Use a dedicated static heap arena for BT (`osi_malloc` → arena), with a high-water-mark counter exposed in the telemetry log, and `BT_BLUEDROID_MEM_DEBUG`-style leak checks in host tests.

Flash: XIP 2 MB image; track `.text` growth per phase in CI via `size -A`.

## 4. Phased work, each with a gate

Rule: nothing goes to hardware before its host gate is green. Existing gate: `make -C open_fw all test-host-san`.

**Phase 0 — Baseline & guard rails (no BT code)**
- Freeze current `bt_hci.c`/`bt_spp.c` behaviour with recorded HCI traces (capture from the Python tools / hardware) as golden regression data.
- Add a `bt_mode` build flag so the old path stays buildable until the new one passes parity.
- Gate: nothing changes in firmware size/behaviour.

**Phase 1 — Vendor + compile-only (size measurement)**
- `open_fw/third_party/bluedroid/` pinned to an esp-idf commit (record SHA; keep local patches as a `patches/` series, never edit in place).
- Minimal `sdkconfig.h`: Classic BT only, BLE off, A2DP source + AVRCP + SPP, no HFP/HID/PAN/GATT/SMP-LE.
- Compile with clang `thumbv7em` + `-Os`. Gate: zero-error link into a **test ELF** (not flashed); report `size -A`, fix budget numbers in §3. **Go/no-go point**: if flash+RAM do not fit, stop and reconsider (e.g. trim A2DP sink/SBC decoder, drop AVRCP).

**Phase 2 — OS + OSI port, on host first**
- Add FreeRTOS-Kernel. Implement `osi` shims only where ESP-IDF specifics leak (`esp_log`, `esp_err`, `esp_timer`, `esp_heap`, ringbuf) in a small `bt_port/` directory.
- Host build: FreeRTOS POSIX port (or a pthread OSI) so Bluedroid runs on x86-64 under ASan/UBSan/TSan. Gate: stack initialises and shuts down 1000× without leaks (arena returns to baseline).

**Phase 3 — HCI transport (the one custom piece)**
- New `hci_hal_openfw.c`: wraps `openfw_hci_send_acl()`/command send and the `ke` event/ACL RX callbacks from `bt_hci.c`, with H4 framing semantics and ACL flow control (credits).
- Remove the SSP responder / ACL logic duplicated in `bt_hci.c` (Bluedroid does SSP itself) — keep only controller bring-up and the ROM glue.
- Host test: a **mock controller** (scripted HCI event generator) plus the golden traces from Phase 0. Fuzz the HAL RX path (random/truncated HCI packets). Gate: stack reaches `ESP_BT_STATUS_ENABLED` and answers inquiry against the mock.
- Hardware experiment 1 (user): `Reset` → local name → inquiry scan visible from a phone; dump HCI over the existing vendor SCSI `0xE0` log.

**Phase 4 — SPP + `+XREC`**
- Port `xrec_proto.c` onto SPP callbacks (`ESP_SPP_*`); drop `bt_spp.c`. Keep SPP UUID `0x1101` (avoids Android HFP contention, per `XREC_PROTOCOL.md`).
- Gate: `tools/bt_sync.py` / `bt_companion.py` full file sync against a **Linux BlueZ peer + USB dongle in loopback tests** where possible, plus the existing xrec unit tests. Large-transfer soak (100 MB, dropouts, resume).
- Hardware experiment 2 (user): Android app pairing and sync.

**Phase 5 — A2DP Source**
- PCM feed from the player path → SBC encoder (Espressif's bundled `external/sbc/encoder`) in the data callback. Never block in the callback; pre-fill ring buffer from the player task.
- Host test: encode→decode round-trip (bundled decoder), underrun/overrun counters, timing under a simulated 44.1 kHz clock.
- Hardware experiment 3 (user): connect to a headphone, 1 h playback soak; log underruns, CPU usage, and whether UI stays smooth. Headphone mic-less; AVRCP play/pause/vol → player.

**Phase 6 — Modes, UI, power**
- Mutual exclusion (`headphone` vs `companion`) in Settings; reconnect to last paired device; pairing storage in persist (`persist.c`) with key persistence tests; power-off/USB-mode teardown must stop the BT tasks cleanly.

## 5. Bug-reduction measures (cross-cutting)

1. **Upstream stays pristine.** Vendored at a pinned SHA; every local change is a patch file with a comment on why. Re-apply cleanly on upgrades.
2. **Host-first everything**: ASan/UBSan (and TSan for the OSI/FreeRTOS-POSIX build) in `make test-host-san`.
3. **Mock controller + golden HCI traces** instead of hardware-only debugging; every hardware bug found becomes a recorded-trace regression test.
4. **HCI snoop**: dump HCI/ACL to a btsnoop file (via the existing telemetry channel) so Wireshark can show any failure.
5. **Resource accounting**: BT heap high-water mark, per-task stack high-water marks, flash size per phase — asserted in tests with budgets from §3.
6. **Watchdog interplay**: BT tasks must feed/yield correctly with the existing WDT policy (`wdt_timer.c`); test a blocked-task case.
7. **Small reversible steps**: each phase merges separately; old stack remains selectable until Phase 4 parity is shown.
8. **Interop matrix** (tracked in this doc): ≥3 Android phones, ≥3 headphone brands (one cheap, one with multipoint), Linux BlueZ, macOS. Record pass/fail per phase.

## 6. Risks

| Risk | Mitigation |
|---|---|
| Flash/RAM does not fit | Phase 1 go/no-go with real numbers |
| Unknown ROM-controller quirks (SCO/eSCO, sniff, role-switch, packet types) | Phase 3 mock + hardware dumps; limit features (disable sniff/role-switch) until proven |
| ACL throughput/latency for A2DP vs. CPU | Measure in Phase 5; SBC bitpool/bitrate tunable |
| iOS cannot use SPP without MFi | Companion is Android/desktop only; documented |
| FreeRTOS + existing bare-metal assumptions (SysTick, WFI, cpu1 mailboxes) | Audit in Phase 2; keep ISR-driven drivers untouched |
| Bluedroid single-controller assumptions (VHCI/credits, BLE paths) | Compile out BLE; review `hci_layer.c` command flow control |

## 7. What I need from hardware experiments

- Exp 1: ROM controller supports standard HCI command set needed by Bluedroid init (`Read_Local_Supported_Commands`, `Read_Buffer_Size`, `Write_Simple_Pairing_Mode`, etc.): run a scripted HCI command list through the current glue and log each Command Complete status.
- Exp 2–3 as in phases above.
