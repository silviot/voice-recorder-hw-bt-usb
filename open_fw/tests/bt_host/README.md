# Bluedroid host harness (plan M3, step 5, path (b) "through M1")

Real Bluedroid (vendored, patches 0001-0026) + `bt_port` (bt_alarm, esp_timer shim, bt_store, bt_entropy/DRBG) +
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
| `test_phone.c` | phone-mode end-to-end scenario (`test_bt_host phone IMG`) and the laptop's host stack: L2CAP signalling, SDP client, RFCOMM client with credit-based flow control, `+XREC` client (as `tools/bt_sync.py`) |
| `host_xrec.c`, `host_xrec.h` | host platform of the BT=1 XREC session (`bt_xrec_plat_*`, mirrors `bt_port/bt_xrec_fw.c`): app task, `bt_recover` worker (up/down jobs), teardown deadline timer, bonds and XREC key on the RAM exFAT card |
| `run_sweep.py`, `sweep_waivers.txt` | allocation-failure sweep driver (`--mode init/enable/disable/deinit`) and the explicit waivers for upstream sites (none since 0011-0023) |
| `posix_port_shim.h`, `real_alloc.h`, `host_link_stubs.c`, `sys/lock.h`, `tsan.supp` | see "Lessons" |
| `portmacro.h` | NOT used here: type-only header for `test-bt-ctrl` (the POSIX port dir precedes it on this include path) |

## Scenarios (all must pass; `test_bt_host [all [N] | fixedq | noack | cycles N | phone IMG | sweep-count | sweep-one N]`)

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
P. **Phone mode end to end** (`test_bt_host phone IMG`, its own process; IMG = a 4096-sector exFAT image, created by
   the make target): the real BT=1 XREC backend (`bt_port/bt_xrec_bluedroid.c`, `src/xrec_proto.c`, `src/exfat.c` on
   a RAM copy of IMG with `/record/REC0001.WAV` 40000 B and `/record/BIG0002.WAV` 300000 B) over this stack and the
   fake ROM's scripted peer (`fake_peer_*`), driven as a laptop. 527 checks:
   - warm-up start/stop (no peer): only btc_spp's 2 resident objects stay; the baseline is taken after it;
   - P1 `bt_xrec_start` -> READY (connectable only), pairing window -> discoverable (`0C1A 0x03` at the ROM), PIN;
   - P2 laptop A (DisplayYesNo) pages and authenticates: Bluedroid's set-up reads, link key request -> negative
     reply -> IO capabilities (ours: DisplayYesNo) -> numeric comparison inside the window held on the device prompt
     (digits and the remote name "laptop-A" checked), answered ENTER (`BTHX_SSP_ANSWER 1`; value logged) ->
     link key notification -> bond -> `bt_store` flush -> `/bt_bonds.bin` holds the key; A encrypts;
   - P3 SDP ServiceSearchAttribute 0x1101: "Cassini", RFCOMM channel 1; P4 RFCOMM mux, PN (CFC accepted, N1 990),
     DLC, MSC -> SPP open and authorized, MTU 990; no bytes from the device before the first command (bt_sync.py's
     native-SPP detection);
   - P5 PING (`bound=0,auth=0,pair=1`), PAIR with the PIN (window and discoverability close, `0C1A 0x02`,
     `/cassini.key` written), LS, GETS 40000 B in 63 frames, byte-exact, with a 300 ms stall after frame 12 (no
     RFCOMM credits, no baseband acks): the session's in-flight cap (2048 B) holds, nothing lost or duplicated;
   - P5b the same GETS with the session's in-flight window at 2 x `tx_buffer_size` (`bt_xrec_test_set_inflight_max`,
     host-only hook): BTC's own ring fills, `esp_spp_write()` returns `ESP_ERR_NO_MEM` (session congested) and the
     real `btc_spp` low-watermark `ESP_SPP_CONG_EVT(false)` resumes it; byte-exact. Without it the congestion path ran
     only against the mocked BTC of `test-bt-xrec` (review finding);
   - P6 link loss (reason 0x08): SPP closed, XREC authorization cleared;
   - P7 reconnect, window closed: stored key (`040B` carries the peer's key), no pairing; RFCOMM straight to channel 1;
     PING (`bound=1`), CHAL/AUTH (HMAC with the bound key), GETS;
   - P8 stop with a 300000 B GETS outstanding (laptop app not reading, its stack answering): graceful, ~0.64 s
     (patch 0026), link down, heap/tasks/esp_timer back to the baseline;
   - P9 restart: bond loaded from `/bt_bonds.bin`, XREC key from `/cassini.key` (`bt_xrec_fw.c` key-load fix):
     reconnect with the stored key, CHAL/AUTH, GETS;
   - P10 laptop B pairs with the window closed: numeric comparison rejected (`042D`), no bond;
   - P11 window open, laptop C (NoInputNoOutput): Bluedroid auto-confirms Just Works, the session removes the
     unauthenticated bond at `AUTH_CMPL` and the link drops; C never reaches XREC;
   - P12 A reconnects, then its LMP detach never completes (`fake_peer.defer_detach`: phone out of range); stop:
     graceful in ~0.64 s although `esp_bluedroid_deinit()` finds the L2CAP link still allocated (crashed in
     `osi_thread_post` before patch 0027); the fake ROM's `stop()` kills the peer's link (`fake_peer.stop_kills`);
   - P13 window open, C pairs Just Works with its detach deferred, BT stops right after the rejection: C's key is not
     in `/bt_bonds.bin` (the session purges it from the BTC config synchronously; BTA DM's own removal needs the link
     down, which never came);
   - P14 restart, C reconnects with that key: never authorized (no PONG, `conns_authorized` unchanged);
   - P15 a stale Just Works bond of C planted in `/bt_bonds.bin` (`plant_bond`), the card failing every write
     (`bthx_fail_bond_writes`) while the session removes it at start; once the card recovers the bounded retry
     rewrites the file without C (`bt_store` stays dirty until `bt_store_mark_saved()`); C never authorized;
   - P16 window open, laptop B pairs, the test answers BACK on the prompt (`BTHX_SSP_ANSWER 0`): negative reply, no
     bond, window still open, an ENTER right after it is swallowed (-3), one 1 s later finds no prompt (-1); the
     phone build is a bench build (`-DOPENFW_BT_BENCH_IMAGE=1`, for P17-P19);
   - P16b (silicon session 6) laptop E, name unknown: BTM's link set-up name request is held (`fake_peer.name_hold`)
     and every later one refused `0x0C` (ROM: RNR already running), so the prompt shows E's address; the held answer
     (`fake_peer_name_release`) puts "laptop-E" on the prompt in place, digits unchanged; BACK with `defer_detach`
     (the device's `0406` accepted, no Disconnection Complete): the screen stops saying connected at once
     (`ui.connected`, `bt_status_t.acl_connected`) while the link stays counted until its supervision timeout;
   - P17 bench autoconfirm (`BTHX_BENCH_AUTOCONFIRM`, as `SD:/BTBENCH.RUN autoconfirm=1`): laptop D's comparison is
     accepted without the user, MITM bond, SSP part closed; D runs CHAL/AUTH twice (distinct, non-zero nonces), GETS;
   - P18 restart with the session's entropy withheld (`bthx_withhold_entropy`: a BT-mode entry whose mic collection
     credited nothing; `host_xrec.c` models the firmware gate: key staged only with credit, "installed" when
     `esp_bluedroid_enable()` returned): nothing opens even with autoconfirm on, B rejected without a prompt, D's CHAL
     fails closed and the link closes; a later credit gives the XREC PIN but the SSP part stays refused (ROM key);
   - P19 bench-style start (`BTHX_START_PAIR` with the up job held by `bthx_hold_bringup`, bench XREC PIN 246810):
     the gate is deferred while UP and decided at READY (SSP part opens, `0C1A 0x03`); D: XREC PAIR with the bench PIN
     and a new key, CHAL/AUTH with it, LS, GETS; bench PIN off: DRBG PIN again;
   - P20 stop: baseline; `/bt_bonds.bin` holds A (and D) but never B or C; the scenario's heap peak (`bth_heap_peak_reset()` after the
     warm-up) under a host regression ceiling (x86-64 sizes; the target budget is read from the firmware's `BT mem`
     lines). Every stop also checks: no fake-ROM contract error, unknown opcode, ROM id without a packer,
     forwarded-command deadline, BT fault, HCI command timeout, XREC hard path.
   Debug aids: `BTH_PHONE_DUMP=1` (bond file text, down-job step times on a failed stop), `BTH_HEAP_TRACE=1` (call
   stacks of leaked blocks: `bth_heap_dump_since()`, symbolise with `addr2line -f -e test_bt_host`).

H. **Headphones-path smoke** (`test_bt_host_hp hp`, docs/BT_A2DP_DESIGN.md milestone H2): the same harness linked
   with `bt_ctrl_cmds.c` built with `-DBT_CTRL_EXP1_PASSED_LIST=BT_CTRL_HP_GATE_LIST` (0401 0402 0405 0408 forwarded;
   only that object differs, `bt_ctrl_cmds_hpgates.o`). The fake ROM's scripted peer is a discoverable headset (CoD
   0x240418, "HP-Fake") next to a phone and a speaker that does not answer names (`fake_rom_add_device`). 84 checks:
   - H1 enable + SPP (client side only), page scan on then off (`bt_ctrl_scan_enable()` 0x02 -> 0x00, design §6.2);
   - H2 `esp_bt_gap_start_discovery`: one `0401`, three results, one Inquiry Complete, BTA's name pass (three `0419`,
     all after Inquiry Complete, none SAVED), the headset's name and CoD and the phone's name reach the GAP callback,
     the speaker stays nameless (page timeout);
   - H3 discovery cancelled while the inquiry runs: `0402` reaches the ROM as `E001` with no payload, the deferred
     CC ends the discovery, no Inquiry Complete;
   - H4 `esp_spp_start_discovery(headset)`: `0405` (allow role switch 0, R1, Bluedroid's packet types), ACL up as
     master (`bt_ctrl_link_info`: outgoing, master), set-up reads `041F 041D 041B 041C` forwarded, `080D 0C37 040F`
     refused locally, the headset refuses the SDP channel (PSM not supported), Bluedroid drops the idle link (`0406`);
   - H5 teardown back to the heap / task / esp_timer baseline (taken after a warm-up cycle: btc_spp keeps resident
     objects after its first init); no unknown opcode, ROM id without packer, deadline, BT fault or HCI timeout.

**Allocation-failure sweeps** (`run_sweep.py --mode M`, `test_bt_host sweep-count|sweep-one N M`): one per lifecycle
call `esp_bluedroid_<M>()`, M = init, enable, disable, deinit. The stack is brought to the state before the call, the
call's allocation attempts (all tasks) are counted, and each is failed in turn in its own process. A point passes when
the call returned `ESP_OK` or an error with the API state unchanged (CLEAN_FAIL), teardown from there works (a failed
disable/deinit is retried: the API stays ENABLED/INITIALIZED, so a recovery has to call it again) back to the heap,
task and esp_timer baseline, and a **following clean cycle** (init, enable, disable, deinit) still succeeds and
returns to the baseline: a failure must leave nothing half-initialised that blocks a later retry. A successful swept
init is also enabled before teardown (several upstream inits ignore a failed allocation and "succeed"). A point whose
N-th attempt never happened (`injected=0`) tested nothing: `sweep-one` CHECKs `injected == 1` and the runner classifies
it NOT_INJECTED, which fails the run and cannot be waived. Waiver lines (`mode function[<-caller] CLASS COUNT reason`)
carry the exact number of points they cover: more matches fail the run, fewer print a warning. `make test-bt-host`
runs all four modes.

| Mode | Points | OK | CLEAN_FAIL | Waived / failing |
|---|---|---|---|---|
| init | 125 | 10 | 115 | 0 / 0 |
| enable | 93 | 55 | 38 | 0 / 0 |
| disable | 24 | 20 | 4 | 0 / 0 |
| deinit | 5 | 2 | 3 | 0 / 0 |

(2026-11-24, after patches 0011-0023; before them the enable sweep had 111 points with 61 waived at 10 upstream
sites, and init / disable, swept for the first time, had 56 crashes and 5 hangs.) Enable's 17 RX-copy and 17 RX
list-node points complete with one `bt_dlv` retry and no leak: that is patch 0008's coverage (with 0008 compiled out
they assert / lose the Reset CC; with only its `osi_free(pkt)` removed, 17 CHECK leaks). The 10 init points that still
"succeed" are `config_new()`/`config_save()` (the stored config is not loaded: see the open item in
`third_party/bluedroid/PORTING_NOTES.md`) and one optional hash map. The sweep does **not** cover 0009: see the lesson
below and scenario 0.

**Mutation check of 0011-0023** (2026-11-24): each patch reverted alone in a scratch copy (`git revert` of its commit
in a series repo; 0014 and 0015 by swapping their `OPENFW_BLUEDROID` blocks to the upstream `#else`, because adjacent
hunks of 0019/0021/0022 make a plain revert conflict), harness rebuilt, the patch's sweep mode(s) run: every one fails
(CRASH/HANG at the site the patch fixes). Patches that form one chain fail together when one link is removed (without
0012 the NULL from 0011's `make_command()` crashes in `transmit_command_futured()`; without 0013 a failed start-up
crashes in `reset_complete()`).

## Lessons (non-obvious, each cost a debugging round)

- **Phone scenario (2026-11-24)**: see `third_party/bluedroid/PORTING_NOTES.md` "Phone-mode end-to-end lessons"
  (BTA DM deregisters itself during disable -> patch 0026; btc_spp's resident objects; reversed link-key hex;
  heap pointers in L2CAP timer params). Harness-side: (1) never block the peer's thread while the device tears down:
  `bthx_post()` + `WAIT(bthx_done())` keeps the laptop answering RFCOMM DISC / L2CAP / detach, a blocking call measured
  the frozen-peer case instead (5 s); (2) `TIMER_PARAM_TYPE` is pointer-wide here (`-DTIMER_PARAM_TYPE=__UINTPTR_TYPE__`)
  because L2CAP's link control blocks are heap list nodes above 4 GiB on x86-64 (the first link timer crashed); (3) the
  numeric-comparison value is on screen only between the request and the bond, and the fake laptop's user confirms
  at once, so the check uses the session's log line, not a screen sample.

- **A green sweep does not prove each patch is exercised: mutate each patch once.** Removing 0009 alone left the
  enable sweep byte-for-byte identical (111 points, same classes): 0008 frees the copy and returns -1 whether or not
  the slot comes back, and 1 lost slot of the RX queue's 254 has no visible effect during enable. A patch whose effect
  is a capacity/counter invariant needs a direct check that makes the invariant observable (scenario 0: capacity 2,
  timeout 0). Mutations checked 2026-11-24: 0009 removed -> scenario 0 fails; 0008's `osi_free` removed -> 17 sweep
  CHECK leaks; 0010 NOACK sent through `command_queue` -> scenario 5 fails.

- **Bluedroid stores pointers in `UINT32`** (`TIMER_LIST_ENT.param` in l2cap/rfcomm/avdt/sdp/btm, BTA JV handles).
  On x86-64 that truncates. The explicit `(UINT32)` casts point at static control blocks or code, so the harness
  links **`-no-pie`** (image below 4 GiB; `test_bt_host.c` asserts `_end < 4 GiB`) and silences the cast warnings for
  vendored code only; L2CAP's link control blocks are heap list nodes stored through `TIMER_PARAM_TYPE`, which the
  harness makes pointer-wide (phone scenario, above). A 32-bit host build would be the alternative (needs i386 libc).
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
- Allocation-failure sweeps over connected-state traffic (ACL, SDP, RFCOMM, AVDTP) and persistent (not single)
  allocation failure (the bounded retries of 0016/0019 then end in a BT fault / upstream behaviour, untested here).
- Phone path: covered by scenario P (above) on `fake_peer_*` (`tests/fake_rom.h`). Not covered there: the device as
  the SSP initiator (Bluedroid's own `0411`; the fake peer supports it, the SPP server never initiates), a peer
  that never answers the detach (the fake ROM completes `0406` at once), allocation-failure sweeps during connected
  traffic, role switch / sniff (`0x12` / `0x14` events), the 30 s SSP prompt timeout (no local prompt yet).
- Connected-link scenarios through M1 beyond the phone path (delayed/dropped CC, truncated events, recovery ladder
  with a link up).
