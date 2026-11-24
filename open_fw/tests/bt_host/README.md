# Bluedroid host harness (plan M3, step 5, path (b) "through M1")

Real Bluedroid (vendored, patches 0001-0010) + `bt_port` (bt_alarm, esp_timer shim, bt_store, bt_entropy/DRBG) +
`bt_ctrl` + `tests/fake_rom.c`, on FreeRTOS-Kernel V11.1.0 (the firmware's kernel sources) with its POSIX port, built
for x86-64 under ASan/UBSan (and a TSan variant). Nothing here is linked into firmware.

```
make -C open_fw test-bt-host        # build (~1 min, -j nproc), scenarios, allocation-failure sweep
make -C open_fw test-bt-host-tsan   # ThreadSanitizer variant (scenarios, 50 cycles)
```

| File | Role |
|---|---|
| `Makefile` | host build (clang, `-no-pie`, `--gc-sections`, `--wrap` for the allocator and esp_timer accounting) |
| `FreeRTOSConfig.h`, `idf_freertos_compat.h` | host counterparts of `src/rtos/FreeRTOSConfig.h` and `bt_port/idf_freertos_compat.h` |
| `host_heap.c` | tracked allocator: every Bluedroid/bt_port/kernel allocation; live bytes/blocks; failure injection |
| `host_glue.c` | host `bt_port_glue.c`: CLOCK_MONOTONIC `us_timer_now`, bt_alarm task, store, **TEST ONLY** deterministic entropy |
| `host_ctrl.c` | bt_ctrl + bt_dlv tasks over the fake ROM, the `esp_bluedroid_hci_driver_operations_t` |
| `host_main.c` | `main()`, step watchdog (a non-FreeRTOS pthread; exit 124 + step name on a hang) |
| `test_bt_host.c` | scenarios |
| `run_sweep.py`, `sweep_waivers.txt` | allocation-failure sweep driver and the explicit waivers for upstream sites |
| `posix_port_shim.h`, `real_alloc.h`, `host_link_stubs.c`, `sys/lock.h`, `tsan.supp` | see "Lessons" |
| `portmacro.h` | NOT used here: type-only header for `test-bt-ctrl` (the POSIX port dir precedes it on this include path) |

## Scenarios (all must pass; `test_bt_host [all [N] | fixedq | noack | cycles N | sweep-count | sweep-one N]`)

0. Patch 0009 (`fixedq`): the vendored `fixed_queue.c` under the tracked allocator. Capacity 2, every allocation
   failing: three `fixed_queue_enqueue(q, x, 0)` all fail at `list_append` (3 injected node failures, not 2); then
   two timeout-0 enqueues succeed and a third does not (free-slot count back at exactly the capacity), FIFO order,
   heap back to baseline. Verified to FAIL (6 checks) with 0009 compiled out.
1. `esp_bluedroid_init()` + `esp_bluedroid_enable()` reach `ESP_BLUEDROID_STATUS_ENABLED` (controller.c start-up,
   BTM reset, BTA enable: 20 HCI commands through `bt_ctrl`, 5 forwarded to the fake ROM).
2. GAP: set device name, scan mode connectable + general discoverable, get name: `ESP_BT_GAP_GET_DEV_NAME_CMPL_EVT`
   returns the name; `0C13` with the name and `0C1A = 0x03` reach the ROM.
3. disable, `bt_ctrl` shutdown (deliveries quiesced before deinit, contract §2), deinit: heap bytes and blocks, task
   count and esp_timer slots back to the pre-init baseline; no fake-ROM contract error, unknown opcode, BT fault or
   HCI command timeout (patch 0007 hook).
5. Patch 0010: ROM withholds a forwarded command's reply (host command credits 0), a second command is queued behind
   it, 6 ACL packets arrive (window 4): Bluedroid's `0C35` still reaches `bt_ctrl`, all 6 credits come back, the two
   held ACL packets are delivered, and the queued command does not overtake the withheld reply. Verified to FAIL with
   0010 compiled out.
4. 200 x (init, enable, scan mode, disable, deinit) with the baseline checked after every cycle (~215 ms/cycle).

**Allocation-failure sweep** (`run_sweep.py`): enable makes 111 allocation attempts (all tasks). Each is failed in turn
in its own process; a point passes on ENABLED or a clean `ESP_FAIL`, plus the full teardown back to baseline. A point
whose N-th attempt never happened (`injected=0`) tested nothing: `sweep-one` CHECKs `injected == 1` and the runner
classifies it NOT_INJECTED, which fails the run and cannot be waived. Each waiver in `sweep_waivers.txt` carries the
exact number of points it covers: more matches fail the run (a new failing path ending at a waived function), fewer
print a warning (fixed or stale entry). The per-waiver counts were identical over 3 full runs.
Result (2026-11-24): 46 ENABLED, 4 CLEAN_FAIL, **61 WAIVED** (10 upstream sites in `sweep_waivers.txt`), 0 other.
The 17 RX-copy failures and 17 RX list-node failures all complete with one `bt_dlv` retry and no leak: that is patch
0008's coverage (with 0008 compiled out the same points assert / lose the Reset CC, patch 0007 then reports the
timeout; with only its `osi_free(pkt)` removed, 17 CHECK leaks). The sweep does **not** cover 0009: see the lesson
below and scenario 0.

## Lessons (non-obvious, each cost a debugging round)

- **A green sweep does not prove each patch is exercised: mutate each patch once.** Removing 0009 alone left the
  enable sweep byte-for-byte identical (111 points, same classes): 0008 frees the copy and returns -1 whether or not
  the slot comes back, and 1 lost slot of the RX queue's 254 has no visible effect during enable. A patch whose effect
  is a capacity/counter invariant needs a direct check that makes the invariant observable (scenario 0: capacity 2,
  timeout 0). Mutations checked 2026-11-24: 0009 removed -> scenario 0 fails; 0008's `osi_free` removed -> 17 sweep
  CHECK leaks; 0010 NOACK sent through `command_queue` -> scenario 5 fails.

- **Bluedroid stores pointers in `UINT32`** (`TIMER_LIST_ENT.param` in l2cap/rfcomm/avdt/sdp/btm, BTA JV handles).
  On x86-64 that truncates. All such pointers point at static control blocks or code, so the harness links
  **`-no-pie`** (image below 4 GiB; `test_bt_host.c` asserts `_end < 4 GiB`) and silences the cast warnings for
  vendored code only. A 32-bit host build would be the alternative (needs i386 libc).
- **Alignment**: BTC copies callback arguments right after the 4-byte `btc_msg_t`. Aligned for Cortex-M4 (4-byte
  pointers, LDRD/LDM need word alignment), misaligned for x86-64. `-fno-sanitize=alignment` for vendored code only.
- **`-fsanitize=function`** (clang >= 17, on by default in UBSan) flags Bluedroid's `osi_free_func` passed as typed free
  callbacks: disabled (well-defined on our ABIs).
- **POSIX port pre-emption vs. libc locks**: the port switches tasks from its SIGALRM handler. A task pre-empted inside
  `malloc` (glibc/ASan lock held) deadlocks the next task that allocates. The tracked allocator runs every allocation
  inside a FreeRTOS critical section (signals blocked); `bth_log()` does the same for stdio.
- **V11.1.0 POSIX port can hang on `vTaskDelete(other)`** (Bluedroid's `osi_thread_stop()` deletes its suspended
  tasks): the victim is cancelled inside `pthread_cond_wait()`, exits holding the event mutex, and the deleter's
  `event_signal()` blocks forever. Fixed upstream in V11.2.0 (robust mutex, FreeRTOS-Kernel `ad4e723`);
  `posix_port_shim.h` applies the same attribute at compile time so the vendored files stay pristine.
- **bt_port's empty `pthread.h`** (for `bta_jv_act.c`) shadows the system header for anything compiled with
  `-Ibt_port`: the kernel, the port and `host_main.c` are compiled without it.
- **Host link differs from the firmware link**: GNU ld at -O1 keeps `smp_proc_pairing_cmpl()`, which references
  BLE-only `smp_keys.c` functions; clang -Os + lld drop it (all-NULL `smp_sm_action[]` without BLE).
  `host_link_stubs.c` traps them (abort if ever reached).
- **The fake ROM's own allocations must not be injected** (it models the ROM's g_bt_heap with its own budget):
  `real_alloc.h` routes them to `__real_calloc/free`.
- **TSan**: the port's in-handler task switch deadlocks under TSan (signals are deferred). The TSan build is
  cooperative (`configUSE_PREEMPTION 0`): every switch happens at a blocking call, where TSan sees the port's
  mutex/condvar hand-over. Two port-internal reports are suppressed (`tsan.supp`); Bluedroid, bt_port and bt_ctrl
  are clean over the full scenario set.
- **Host-only differences to keep in mind**: `TickType_t` is 64-bit on the POSIX port (bt_osal's `uint32_t` timeouts
  truncate `portMAX_DELAY`; not on the Classic path); task stacks are pthread stacks sized 8x the IDF byte size
  (`BTH_STACK_SCALE`), so stack watermarks are meaningless here; the idle task busy-loops (one core at 100%).

## Not covered yet (path (a) and the rest of M3)

- Bumble (path (a)) for protocol scenarios (SSP, SDP, RFCOMM/SPP, AVDTP): not started. Checked 2026-11-24:
  `bumble==0.0.236` (pip, venv) emulates BR/EDR connections in its virtual controller (`Create_Connection`,
  `Accept_Connection_Request`, `Write_Scan_Enable`, local name/CoD) but has **no Inquiry / inquiry-scan** handling, so
  the planned "GAP inquiry-scan smoke test" needs either a page/connect smoke test instead or a real controller. It
  also needs an H4 transport for the harness in place of `bt_ctrl` (a FreeRTOS task polling a pty/socket: a foreign
  pthread must not call FreeRTOS APIs on the V11.1.0 POSIX port).
- Sweeps over init, disable/deinit and connected-state traffic; the 10 waived upstream sites need patches (0011+).
- Connected-link scenarios through M1 (ACL both directions with an L2CAP channel, credit exhaustion with real
  consumers, delayed/dropped CC, truncated events, recovery ladder).
