# Bluedroid (ESP-IDF) port notes — Phase 1 (vendor + compile + size)

Upstream: `espressif/esp-idf` `ad20ded62a2d67a26e501fdc1905f43ba7ad6c81` (2026-11-15), Apache-2.0 (`LICENSE`).
Vendored: `host/bluedroid`, `common` (osi, osal, btc core, tinycrypt), `deps/esp_ringbuf`, `deps/ble_profiles_common/include/bt_prf_task.h`.
Local changes: see `../bluedroid_patches/README.md` (0001-0010). Port layer: `open_fw/bt_port/`. Build: `open_fw/bt/Makefile.bluedroid`.

## Reproduce
```
open_fw/tools/build_picolibc.sh <workdir>                 # picolibc + 6 compiler-rt builtins for Cortex-M4F; prints PICOLIBC=...
open_fw/tools/measure_bluedroid.sh <PICOLIBC> [builddir]  # compile 322+ files, link 4 test ELFs, size + unresolved + per-component
# regenerate config/sources (needs `pip install kconfiglib` and an esp-idf checkout at the SHA above):
IDF_PATH=<esp-idf> python3 open_fw/tools/gen_bluedroid_config.py && python3 open_fw/tools/gen_bluedroid_srcs.py
```
Build is clean with clang 21, `-Os -ffunction-sections`, zero errors; link test uses `ld.lld --gc-sections` against the real FreeRTOS-Kernel V11.1.0 (`third_party/freertos`, `src/rtos/FreeRTOSConfig.h`).

## Configuration (bt_port/sdkconfig.defaults, generated bt_port/sdkconfig.h)
Host-only (`BT_CONTROLLER_DISABLED`), Classic only: `BT_CLASSIC_ENABLED`, `BT_A2DP_ENABLE` (+AVRCP, source-only via patch 0001),
`BT_SPP_ENABLED`, `BT_OSAL_ENABLED` (needed: `bt_osal_freertos_funcs_init()` is unconditional; callouts on FreeRTOS timers, not esp_timer),
`BT_STACK_NO_LOG=y` (all trace strings out), `BT_ACL_CONNECTIONS=2`, `BT_SMP_MAX_BONDS=4`; off: BLE, GATT, HFP, HID, PAN, OPP, PBAP, BLUFI, Espressif coex/enc-key **vendor HCI commands** (`BT_BLUEDROID_ESP_COEX_VSC`, `BT_ENC_KEY_SIZE_CTRL_VSC` — the ROM controller does not implement them).
SSP is a runtime API (`esp_bt_gap_set_security_param` / `esp_bt_gap_ssp_confirm_reply`), no Kconfig.
Sources excluded from the CMake list: BLE-only API (`esp_gap_ble_api.c`, `esp_ble_ead.c`; need `esp_random.h`/BLE), blufi, ble_log, HCI-log insights, SBC decoder/PLC, `btc_a2dp_sink.c`.

## Key findings
1. **HCI needs no custom HAL.** Upstream has a pluggable driver: `esp_bluedroid_attach_hci_driver(const esp_bluedroid_hci_driver_operations_t*)` (api/include/api/esp_bluedroid_hci.h) with `send(data,len)`, `check_send_available()`, `register_host_callback(cb)`; `cb->notify_host_recv(data,len)` and `cb->notify_host_send_available()`. Packets are **H4-framed** (`data[0]` = 1 cmd / 2 ACL / 3 SCO / 4 event). `hci_layer.c` / `hci_hal_h4.c` stay pristine. `send()` is called from the HCI host task: it must only enqueue to the controller-owner task. Must be attached while Bluedroid is `UNINITIALIZED` (before `esp_bluedroid_init()`). Not exercised by the link test (it is `BT_CONTROLLER_INCLUDED != TRUE` code, reached at runtime only).
2. Bluedroid starts **3 tasks** (HCI host 2048 B, BTU 4352 B, BTC 3072 B; priorities `configMAX_PRIORITIES-3/-5/-6`, so `configMAX_PRIORITIES >= 7`; ours is 8). Stack sizes are in **bytes** at the IDF API; `bt_port/idf_freertos_compat.h:xTaskCreatePinnedToCore` converts to words. Upstream `esp_bt_main.c` also starts a 4th "profile" task only if `CONFIG_BT_PRF_TASK_ENABLED` (off).
3. `osi/allocator.c` calls plain `malloc/calloc/free`. `bt_port/bt_port_heap.c` overrides the libc malloc family (8-byte size header) onto FreeRTOS heap_5, which also keeps picolibc's own malloc/sbrk (`__heap_start/__heap_end`) out of the image. Never allocate from ISR.
4. Critical sections: IDF `portENTER_CRITICAL(&mux)` maps to `vPortEnterCritical()`; `*_ISR(&mux)` saves BASEPRI inside the lock object. `esp_ringbuf` (used by SPP) relies on internal kernel APIs (`vTaskPlaceOnEventList`, `xTaskRemoveFromEventList`) under such critical sections: **must be stress-tested on host (TSan/ASan) before trusting** — fallback is FreeRTOS stream buffers.
5. SPP can run with `ESP_SPP_MODE_CB` (no VFS). VFS symbols (`esp_vfs_*`) are only stubbed; they must never be reached.
6. Persistence: Bluedroid bond keys / bt_config go through `nvs_*` (stubs now) → implement over `persist.c`. `esp_random` must be a real RNG (SSP/ECDH nonces): use the SL6801 TRNG or a seeded DRBG; the link-test stub is NOT random.
7. `esp_timer_*` (used by `osi/alarm.c`, A2DP media timer) and `esp_log_*` are shims to implement: timers on FreeRTOS timers (1 ms tick; A2DP media tick ~ 20–30 ms is fine) + `us_timer_now()`.
8. Upstream does not compile A2DP **source-only** (`BTA_GetDelayValue` — patch 0003); upstream a2dp source also allocates 4 KB per encoded frame (patch 0002).

## libc / runtime surface (picolibc + compiler-rt builtins vs in-tree libc)
Link of the full image against picolibc (Cortex-M4F, `-Os`, `--gc-sections`): **unresolved = `__aeabi_ldivmod`, `__aeabi_uldivmod` only**, both already implemented in `src/startup.c` (verified `startup.c:540,558`). picolibc contributes ~10 KB `.text` total.
Needed beyond mem*/str*: `snprintf`, **`sscanf`/`vfscanf`** (osi/config.c, bdaddr.c, btc_config.c → pulls strtod/dtoa and soft-double `__aeabi_dadd/dmul/f2d/d2f/ul2d`, ~1.2 KB from compiler-rt `adddf3 muldf3 extendsfdf2 truncdfsf2 floatundidf fp_mode`), `puts` (bt_osal assert print), `__assert_func`/`abort`, `qsort`-free. Unavoidable `stdout/stderr` objects (picolibc idiom `FDEV_SETUP_STREAM`), `gettimeofday`, `_exit`.
Verdict: **use picolibc + a handful of compiler-rt builtins**, not an in-tree libc: a hand-written `sscanf`/`snprintf`/strtod would be exactly the kind of bug source we are trying to remove, the cost is ~10 KB, and it builds with the project's clang+lld in one script. Overlap hazards to resolve deliberately: (a) `startup.c` already defines `__aeabi_memcpy*/memset*/memclr*` and 64-bit div: keep them strong and never link a compiler-rt/libgcc member that also defines them (duplicate-symbol); build compiler-rt as individual objects (as `build_picolibc.sh` does) not the whole archive; (b) picolibc's `memcpy/memset` are separate from `__aeabi_*` (arg order of `__aeabi_memset` differs) — no clash; (c) `src/rtos/libc_min.c` weak symbols are replaced by picolibc automatically; (d) override `malloc` family as in `bt_port_heap.c`; (e) picolibc needs `-ffreestanding`-compatible headers first on the include path (`-I$(PICOLIBC)/include`) — the main Makefile uses `-fno-builtin`; Bluedroid TU's are compiled without it so `memcpy` etc. inline normally.

## Size (clang 21, -Os, BT_STACK_NO_LOG, source-only A2DP; full = SPP + A2DP + AVRC + core)
| Item | Flash (text+rodata) | Static RAM (.data+.bss) |
|---|---|---|
| **Bluedroid + OSI + osal + ringbuf + SBC enc** (+ 12.5 KB merged string/const pool) | ≈ 237 KB (+12.5) | ≈ 20.3 KB |
| FreeRTOS kernel (tasks/queue/list/timers/event groups/stream buffers/port/heap_5) | 8.9 KB | 0.75 KB |
| picolibc + compiler-rt | ≈ 11.3 KB | 0.14 KB |
| link test main + stubs | 0.7 KB | – |
| (alignment/unattributed, from `llvm-size` vs map) | ≈ 8 KB | – |
| **Total (full link)** | **278.6 KB** | **21.4 KB** |
Variants: core only 276.6 KB; +SPP 277.4; +A2DP 277.8. (gc cannot drop either profile: BTC dispatch tables reference both; per-mode numbers are therefore ≈ equal.)
Before patches/tuning (sink in, 15 bonds/4 ACL, logs off): 287 KB / 35.2 KB. With Bluedroid trace logging on (warning level, `BT_LOG_TRACE=1`): **394 KB** (+115 KB, all strings).
Current firmware is 385 KB; 2 MB XIP flash → ≈ 665 KB used with BT. **Flash is not a constraint.**
Largest contributors (flash KB): btm 28.5, btc/profile 27.6, l2cap 23.7, bta/av 19.6, bta/dm 15.7, avdt 15.1, rfcomm 14.1, merged strings 12.5, osi 11.8, sdp 11.5, btu 8.5, avrc 7.2, btc core 6.2, jv 6.0, sbc enc 5.7.
Largest static RAM (.bss bytes): btm_cb 2188, bta_dm 1352+, l2cb 1392, rfc_cb 1700 (RFCOMM+port), bta_jv 2564 (after SDP buffer trims; upstream 6764), sdp 840, avdt 768, smp 720, a2dp_source 1752 (incl. SBC encoder state 1684).

## RAM budget (must hold in the 84 KB `.rtos_heap` region at 0x86B000 + whatever static goes there)
> Superseded (Phase 1 estimate): see "Integration lessons" below and plan §4. Measured on silicon: 118 KB of heap free at boot with both RTOS regions.
Static RAM above (21.4 KB) lives in normal `.bss` unless the linker script moves the Bluedroid objects' `.bss/.data` into the freed region; main SRAM has only ~8.7 KB free, so **place BT static data in the new region** (`*bluedroid.a:*(.bss* .data*)`, `*freertos*`).
Dynamic estimate (heap arena; ESTIMATES from config constants — to be replaced by measured high-water marks in Phase 2 host runs and on hardware):
| Item | Bytes |
|---|---|
| Task stacks: HCI 2048 + BTU 4352 + BTC 3072 + timer 1536 + idle 512, TCBs 5×~100 | ≈ 12.0 KB |
| OSI/FreeRTOS objects (queues, mutexes, semaphores, event groups, ≤50 alarms × ~80 B, work items) | 2–5 KB |
| HCI command bufs (660 B × 2–3), ACL rx (≈1 KB in flight), L2CAP tx queue (controller credits × ≤1 KB) | 4–12 KB |
| bt_config / bond DB in RAM (4 bonds) | 2–4 KB |
| **SPP mode** extra: RFCOMM rx/tx queues (high watermark 10 × 990 B each way, credits ≤ 16 frames) | 4–20 KB |
| **A2DP mode** extra: encoded frames ≤ 5 × 1280 B (patch 0002) | ≈ 6.4 KB |
| A2DP app-side PCM staging ring (our code, 100 ms of 44.1 kHz stereo) | ≈ 17.6 KB |
| App task stack (the current main loop moves into a FreeRTOS task: needs its own stack) | not counted |
Totals (dynamic): SPP mode ≈ 24–40 KB typical, ≈ 55 KB worst case; A2DP mode ≈ 35–45 KB typical, ≈ 50 KB worst. With 21.4 KB static: **≈ 60–70 KB total per mode**, inside the 84 KB region with ≈ 15–25 KB margin; the coordinator's "80 KB first, 160 KB max" budget holds. Biggest knobs if tight: `PORT_RX/TX_BUF_HIGH_WM` (RFCOMM), `BT_ALARM_MAX_NUM`, `BT_BTU/BTC_TASK_STACK_SIZE` (measure stack high-water first), PCM ring size.

## Go / no-go
**GO.** Everything compiles with zero patches to logic, links to completion against real FreeRTOS + picolibc with only 64-bit division missing (already in `startup.c`), flash ≈ 279 KB (+115 KB for logs) vs 2 MB, RAM ≈ 60–70 KB per mode vs an 84 KB free region, and the controller boundary is an official, narrow API.
Risks to retire in Phase 2: (1) `esp_ringbuf` on upstream FreeRTOS internals, (2) heap fragmentation under A2DP (5 × 1280 B frames churn + BT_HDR); run a long host soak with allocation tracking, (3) real stack high-water marks (HCI 2 KB, BTU 4.3 KB are IDF numbers for Xtensa/RISC-V; Cortex-M4F with `-Os` should be similar or smaller, verify), (4) `NVS`/`esp_random`/`esp_timer` shims correctness (SSP security depends on the RNG), (5) FreeRTOS priority/BASEPRI interplay with IRQ 26 (RW-BT) and ROM `ke` callbacks (owner-task design), (6) SPP SDP buffers trimmed (`BTA_JV_SDP_DB_SIZE` 1500): fine for server role; re-check if the device ever connects out to a peer's SPP record.

## Integration lessons (2026-11-24, `BT=1` firmware build)
Status: Bluedroid links into the firmware (`make BT=1`, ~710 KB image), not yet started. Port layer: `open_fw/bt_port/`.

Build / link
- **lld archive patterns need a member glob**: `*libbluedroid.a:*(.bss .bss.*)`. GNU ld's bare `archive.a:` form matches
  nothing in lld, silently (the sections stayed empty and main `.bss` overflowed).
- Route Bluedroid/port/picolibc `.data` to a **loadable** `.bt_data` (LMA in XIP, copied at boot, `-j .bt_data` in the
  objcopy) and `.bss` to `.bt_bss` (zeroed); exclude both from the catch-all sections with `EXCLUDE_FILE`.
- Until the app calls Bluedroid, `--gc-sections` would drop the whole stack: `bt/bt_link_roots.c` (KEEP section) +
  `tools/check_bt_link_roots.sh` keep the size honest.
- `bt/Makefile.bluedroid` needs `-MMD -MP`: a change to `bt_port_prefix.h`/`sdkconfig.h` otherwise leaves stale objects.
- Firmware sources must not see picolibc headers; only Bluedroid and `bt_port` objects do (`bt_port` additionally gets
  `open_fw/include`). Use plain extern declarations when firmware code needs a port function.
- Upstream warning classes that are benign here are silenced for vendored code only; file-local exceptions carry a
  static assert (e.g. `btc_spp.c` `size_t*` vs `UBaseType_t*`).

Upstream behaviours that matter on a small target (found in the Codex/agy plan review; plan §5 has the fixes)
- Many waits are unbounded (`OSI_THREAD_MAX_TIMEOUT` posts, `future_await`, `osi_mutex_lock` forever):
  `esp_bluedroid_disable/deinit` can block forever → graceful recovery runs in a worker the app does not wait on
  without a deadline.
- `host_recv_pkt_cb()` ignores `fixed_queue_enqueue()`'s result and asserts on allocation failure → fail-soft patch.
- hciT checks command credits before dequeuing, so Host Number Of Completed Packets (`0C35`, NOACK) can be stuck behind
  a credit-blocked command → separate NOACK queue.
- `check_send_available()` has no packet type, and hciT may check and then not send → TX admission must be a pure
  query over one ring that fits any packet.
- Host Buffer Size announces `L2CAP_MTU_SIZE` (1691) × `L2CAP_HOST_FC_ACL_BUFS` (100) → patch 0004 + overrides (4 × 1021).
- Startup reply parsers `assert(stream != NULL)` on any non-zero status → mandatory startup opcodes must succeed.
- `command_timed_out` only logs; once it feeds a fault path it must ignore stale expiries (patch 0007).
- FreeRTOS software timers deliver an expiry by slot pointer and process due timers before queued stop commands, so a
  cancelled/re-armed alarm can fire "as the new one" → BT alarms use `bt_port/bt_alarm.c` (caller-synchronous
  generation), not FreeRTOS timers.
- Classic-only Bluedroid never calls `esp_random`; SSP keys/nonces come from the ROM controller → the entropy work
  must trace the ROM's RNG hooks, not only feed `esp_random`.

Process
- Two independent AI reviewers (Codex, agy) converged on v18 of the plan after 17 rounds; most late findings were real
  edge cases in upstream code paths, which is why every one became a patch or a test rather than a comment.

## Host harness lessons (2026-11-24, step 5 / M3, `make -C open_fw test-bt-host`, `open_fw/tests/bt_host/README.md`)
- Bluedroid runs unmodified on x86-64 under ASan/UBSan/TSan on the FreeRTOS POSIX port, with three host-only
  accommodations: link **`-no-pie`** (timer `param` and BTA JV fields are `UINT32` holding pointers to static control
  blocks and code), `-fno-sanitize=alignment` for vendored code (BTC places callback arguments at offset 4 after
  `btc_msg_t`: fine for 32-bit Cortex-M4, misaligned for 64-bit pointers), `-fno-sanitize=function` (`osi_free_func`
  passed as typed free callbacks).
- With BLE off, `smp_utils.c`/`smp_api.c` still reference BLE-only `smp_keys.c` functions; the firmware link drops the
  callers (all-NULL `smp_sm_action[]`), a less aggressive link does not.
- One enable (controller start-up + BTM reset + BTA enable) costs 111 allocation attempts and ~5 ms on the host; a full
  init/enable/disable/deinit cycle returns every byte (200-cycle loop).
- **Allocation failure on the enable path is mostly unhandled upstream**: of 111 single-failure points, 50 complete or
  fail cleanly (incl. all RX-path points thanks to 0008; 0009's slot restore is not observable there and has
  its own `fixed_queue` scenario), 61 hit one of 10 sites that assert
  (`make_command`, `future_new`, `bta_dm_enable`), dereference NULL (`btu_hcif_command_complete_evt`), drop an enable
  event (BTA sys/DM, `btc_dm_sec_cb_handler`) or lose a sent command from the pending list (`event_command_ready`).
  They are explicit waivers in `open_fw/tests/bt_host/sweep_waivers.txt`; on target an assert reboots and a lost
  event leaves enable blocked, so the app's BT enable path needs a deadline (recovery ladder) until patches 0011+.
  **Update (same day, patches 0011-0023): zero waivers.** The sweep now runs per lifecycle call (init 125 points,
  enable 93, disable 24, deinit 5; `run_sweep.py --mode`), and every point completes or fails cleanly, tears down to
  the baseline and leaves a following clean cycle working. Init had ~30 more unhandled sites (OSI primitives asserting,
  BTU/BTM/L2CAP init ignoring or asserting on failures, cleanup of never-initialised blocks, BTC config failure after
  the init future was already completed) and disable 5 (BTA timer start, DM disable request, the DISABLE_EVT hand-off).

## Allocation-failure lessons (patches 0011-0023, `third_party/bluedroid_patches/README.md`)
- **Three fixes, in this order of preference.** (1) Remove the allocation when it is avoidable: the command-pending
  list links commands through their own `pkt_linked_item_t` (0015), `btc_dm_sec_cb_handler` allocates the BLE message
  only for BLE events (0017), the bt_osal table is used in place (0023). (2) Do the step without the allocation:
  BTA SYS self-posted events run synchronously from a stack copy on the BTU task (0014), a lost enable/disable
  completion is posted to BTC by value with `osi_thread_post()` (allocation-free work item) (0018). (3) Fail the API
  call cleanly: start-up commands (0011/0012) -> `BTM_DEV_STATUS_DOWN` -> BTA DM `BTA_DM_ENABLE_EVT(BTA_FAILURE)`
  (0013); init failures -> `BT_STATUS_NOMEM` / `FUTURE_FAIL` (0020-0023). Bounded retries only where nobody can be
  told: the CC/CS hand-off to BTU (0016: then BT fault through the 0007 hook) and `bta_sys_start_timer()` (0019).
- **Failing an API call is only clean if a retry still works.** The sweep therefore runs, after every point, teardown
  to the baseline AND a full clean init/enable/disable/deinit cycle. That caught nothing extra once the patches were
  in, but it is what proves "nothing half-initialised blocks a later retry" (e.g. 0013's `bta_dm_enable_pending` is a
  static outside `bta_dm_cb`, which BTA_SYS_HW_ON re-initialises; 0022's `btu_core_started` gates the core free).
- **A successful init must be exercised.** Several upstream init functions ignore an allocation failure and return
  normally (`l2c_init`, `sdp_init`, `btm_acl_init`, `bta_sys_init`); the init sweep only found them after it was made
  to enable the stack after every successful swept init. Teardown alone passed.
- **Ordering matters for reporting**: `btc_init_bluetooth()` used to run `btc_config_init()` after
  `bte_main_boot_entry()`, by which time the BTU task may already have completed the init future, so a config failure
  could not fail `esp_bluedroid_init()` (`future_ready` may be called once). 0023 loads the config first.
- **Failing back from the BTU task to BTC is safe with `future_ready()`** (only a semaphore give) and with
  `osi_thread_post()` (work item copied by value into the FreeRTOS queue); `btc_transfer_context()` allocates.
- **Mutation-testing a patch series needs `git revert`, not `patch -R`**: adjacent hunks of later patches (0014/0019 in
  `bta_sys_main.c`, 0015/0021 in `hci_layer.c`) make a reverse patch fail; reverting one commit of the series in a
  scratch git repo keeps the others. And copy restored files with a fresh mtime (`rsync -a` keeps the old one, so make
  does not rebuild them and the next mutation silently contains the previous one).
- **Open (not an allocation crash, so not a sweep failure)**: an allocation failure while `config_new()` parses the
  stored config makes `btc_config_init()` fall back to an empty config (init "succeeds", sweep points 21-26). Nothing
  is written back at init (`config_save()` refuses an empty config), but the next bond save writes a config holding
  only the new bond, i.e. stored bonds are lost. Treat a parse allocation failure as an init failure (needs
  `config_new()` to tell "no/invalid config" from "no memory") before relying on persistent pairings under memory
  pressure.

## SPP / GAP behaviour the app relies on (phone mode, `bt_port/bt_xrec_bluedroid.c`)

- **CB-mode `esp_spp_write()` copies** into BTC's per-connection ring (`tx_buffer_size`) and returns `ESP_ERR_NO_MEM`
  when it does not fit (setting `upper_cong`); `ESP_SPP_CONG_EVT` (always `cong=false`) fires once the ring drains
  below half. `ESP_SPP_WRITE_EVT.len` reports chunks submitted to RFCOMM (any size, not per write). The session keeps
  its own TX queue, writes only while uncongested and keeps written-but-unreported bytes within `tx_buffer_size`.
- **No negotiated MTU in the API**: `rfc_srv_open.peer_mtu` stays in btc's private slot. `bt_xrec_spp_mtu.c` reads it
  in BTC context via `BTA_JvRfcommGetPortHdl()` + `PORT_GetQueueStatus()` (as btc_spp resolves the port itself).
- **Just Works never reaches the app**: `btc_dm_sp_cfm_req_evt()` auto-confirms `just_works` requests; only numeric
  comparison raises `ESP_BT_GAP_CFM_REQ_EVT`. A policy that forbids Just Works must act at `ESP_BT_GAP_AUTH_CMPL_EVT`
  (`lk_type` 0x04/0x07 = unauthenticated) by removing the bond (`esp_bt_gap_remove_bond_device()` also drops the ACL).
- **`AUTH_CMPL` success is raised only for new link keys** (`bta_dm_new_link_key_cback`), not for reconnections with a
  stored key. "Authorized" for a reconnect must therefore check the STORED key type (`LinkKeyType` 5 or 8 in the BTC
  config, `bt_port/bt_xrec_bond.c`), not bond existence: Bluedroid stores (and flushes) a Just Works key before the app
  sees `AUTH_CMPL`, and `esp_bt_gap_remove_bond_device()` on a connected peer only marks it `BTA_DM_UNPAIRING`; the
  store entry goes when BTA DM sees the ACL drop, which a BT stop (disable drops DM's ACL events, see 0026) or a power
  loss pre-empts. Purge the config entry yourself (`btc_storage_remove_bonded_device()` under btc_config's mutex is
  safe from the app task) and drop unauthenticated bonds at start.
- **`ESP_SPP_SEC_MITM` does not enforce MITM.** BTM asks for a key upgrade only when an authenticated key is possible
  (`btm_sec_is_upgrade_possible`: needs the peer's IO caps, unknown on a reconnect with a stored key, and a capable
  IO pair); otherwise it accepts the unauthenticated key and opens the service. esp_spp_api.h also advises against
  the flag. The app-level key-type check is the enforcement.
- Internal Bluedroid headers define `bt_status_t`, which clashes with open_fw's (`include/openfw.h`): keep files that
  need Bluedroid internals separate from files that include `openfw.h`.

## Phone-mode end-to-end lessons (2026-11-24, `test_bt_host phone`, patches 0024-0026)

The first run of the real XREC backend over Bluedroid against a scripted laptop (`open_fw/tests/bt_host/test_phone.c`)
found what the mocked `test-bt-xrec` could not:
- **A disable with a connected peer is bounded by BTA DM's timers, never by the link-down.** `bta_dm_disable()` calls
  `bta_sys_disable(BTA_SYS_HW_BLUETOOTH)`, which runs every module's disable, BTA DM's own included
  (`bta_dm_sm_disable()` -> `bta_sys_deregister(BTA_ID_DM)`). From then on BTA SYS drops DM's
  `BTA_DM_ACL_CHANGE_EVT` ("BTA got unregistered event"), so the "all links down -> 1 s -> done" path in
  `bta_dm_acl_change()` is dead code during a disable, and upstream always waits 5 s (profiles' graceful close; an SPP
  peer never makes it: RFCOMM keeps the multiplexer up for its own inactivity timer) + 1.5 s (after the forced
  detach). Patch 0026 shortens both (100 ms / 500 ms): measure any future change with a connected peer.
- **Keep the peer answering while you stop.** A host test that blocks the peer's thread during `bt_xrec_stop()`
  measures the stalled-peer case (RFCOMM DISC, L2CAP disconnect and LMP detach unanswered), not the normal one. A
  stalled peer still ends inside the 2 s deadline with 0026 (detach + 500 ms, then `bt_ctrl` stop kills the link), but
  on silicon the ROM's own detach timeout for an out-of-range phone is unmeasured: the deadline's hard path covers it.
- **btc_spp's `s_spp_resident` mutex and TX event group are allocated by the first `esp_spp_init()` and never freed**
  (upstream comment: a VFS `write()` may still wait on the group after deinit). Leak checks around SPP must take their
  baseline after one SPP init/deinit cycle; exactly 2 blocks (224 B on x86-64) stay.
- **Link keys are stored byte-reversed** in the BTC config (`LinkKey = <hex>` of the `LINK_KEY` array, which
  `btu_hcif` fills with `STREAM_TO_ARRAY16`): a check of the bond file must reverse the wire key; the reconnect is the
  real proof (Bluedroid's `040B` reply carries the peer's key).
- **`TIMER_LIST_ENT.param` holds heap pointers too**: L2CAP keeps its link control blocks in a heap list
  (`l2cb.p_lcb_pool`) and passes them through `TIMER_PARAM_TYPE` (UINT32 on target: fine). The x86-64 harness defines
  `TIMER_PARAM_TYPE` pointer-wide (`tests/bt_host/Makefile`); sites that cast to `(UINT32)` explicitly (RFCOMM,
  SDP, AVDT, HF AG) point at static control blocks in this configuration (`*_DYNAMIC_MEMORY` off).
- BTC callback arguments are 4-aligned (copied right after the 4-byte `btc_msg_t`): fine on Cortex-M4, an UBSan
  alignment report on x86-64; the app's two BTC callbacks carry `no_sanitize("alignment")` (`bt_xrec_bluedroid.c`).
- **A disable never waits for the link to go down, so deinit can meet a live link** (review round, patch 0027): with
  the peer out of range the LMP detach only completes at the supervision timeout (up to 20 s), long after 0026's
  600 ms. `btu_free_core()` then releases the still-connected L2CAP link control block on the BTC task after
  `btu_thread` was freed; every BTA/BTM callback it triggers must tolerate a missing BTU thread. The fake ROM's
  `fake_peer.defer_detach` models it; a fake that completes `0406` at once (as before) cannot catch it.
- **btc_spp's congestion only shows if the app writes more than `tx_buffer_size` ahead of `ESP_SPP_WRITE_EVT`.** An
  in-flight cap equal to the ring size keeps BTC's ring from ever refusing a write (`ESP_ERR_NO_MEM` / `CONG_EVT`
  never happen in a test), except that `items_waiting` also counts a partially accepted, still checked-out chunk
  (`write_held`), so on air partial acceptance can still reach it. `test_bt_host phone` P5b raises the cap with a
  test hook to run that path against the real ring.
