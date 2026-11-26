# `open_fw` Independent Post-Development Review (`3942608..0f407ce`)

- **Reviewed range:** `3942608..0f407ce` (20 commits, 59 files changed)
- **Review branch:** `review/r2-0f407ce`
- **Binding rules:** `AGENTS.md` (`open_fw` Engineering Rules & Invariants)
- **Methodology:** Full source, build-system, linker-script, protocol, and host-tool audit across all 8 landed areas; strict failing-host-test-first workflow for every real bug; clean-from-scratch firmware builds (`default BT=1`, `BT=0`, `BT=1 BENCH=1 HP_GATES=1`) with 0 warnings and full host/ASan/UBSan/TSan/Bluedroid sweep verification in isolated `/home/silvio/dev/.build-scratch/agy/fw/*` directories.

---

## 1. Summary of Findings

| ID | Severity | Status | Fix Commit | Area | Summary |
|---|---|---|---|---|---|
| **F1** | **High** | **Fixed** | `4f2917c` | S2 Recorder Counters / `AGENTS.md` IRQ Rule | `bm_log_rec()` in `open_fw/src/recorder.c` masked `PRIMASK` (`OPENFW_IRQ_SAVE`) across ~450 XIP `openfw_log_putc()` calls instead of using FreeRTOS `BASEPRI` (`taskENTER_CRITICAL`), masking priority-`0x00` audio DMAC IRQs (61/60); host MMIO test also omitted `-DOPENFW_BT_BENCH_IMAGE=1`. |
| **F2** | **High** | **Fixed** | `b72b950` | S2 `+XREC:DBG` / `bench_meas` Sizing | Commit `054513e` added `bclk_smp`, `bdrift`, `sd_err`, `sd_mdto`, and `take_us` without updating `BENCH_MEAS_LINE_MAX` (`560`) or `XREC_REPLY_MAX` (`576`). Worst-case `+XREC:DBG,LIVE` reaches `585 B`, truncating `take_us` and silently dropping the entire reply in `xo_send()` when `tx_free` is `576..584 B`. |
| **F3** | **Medium–High** | **Fixed** | `ac31bf3` | XREC `PUT REC` Sidecar & Transfer State | (a) `+XREC:DEL,<stem>.wav` during an in-progress `PUT REC,<stem>.cwx` left `.xrec_rec.tmp` open; if a new recording reused `<stem>.wav` before `PUTEND`, `PUTEND` attached the deleted recording's `.cwx` transcript onto the new recording. (b) Early `ERR,ARG` rejections in `try_start_put_header()` failed to clear `put_done_valid`, so a subsequent `PUTEND` falsely returned `OK,PUTEND,<prev_total>`. (c) Aborted uploads / failed `PUTEND`/`OTA` commits left `xfer_valid = 1` on the BT screen. |
| **F4** | **High** | **Fixed** | `5d1dfce` | Headphone Auto-Connect (`main.c`, `bt_hp_bluedroid.c`) | (a) `bt_hp_connect_addr(bda, 0)` did not check `!bonded(bda)`, clearing `pair_again` and paging on air without a pairing window. (b) `hp_auto_connect_for_play()` leaked a newly started (`started_here`) BT session on REC slide-switch abort, `connect_addr` failure, `CONNECT_FAIL`/`PAIR_AGAIN`/`UNSUPPORTED`, and timeout—leaving BT paging during microphone recording or every 60 s in idle. (c) `connect_addr` ran before `u.msg_seq != seen_seq`, clobbering `rc_poll`'s `PAIR_AGAIN`. (d) `t0` was sampled before RF calibration/bring-up (`UP`/`CONFIG`), stealing bring-up time from `BT_HP_CONNECT_TO_MS` and leaving an in-flight page uncancelled on `!started_here` timeout. |
| **F5** | **Medium** | **Fixed** | `4a489e9` | S2 Host Tooling (`rf_audio_check.py`, `bt_sync.py`) | (a) In `tools/rf_audio_check.py`, whenever `>= 2` baselines had device counters (`C0a`, `C0b` — the standard M2 setup), `compare()` took `elif refs:` and completely skipped the `|ppm| <= 300` gross rate check, allowing a systematic clock/IRQ rate error shared by `C0` and test captures (e.g. `-837 ppm`) to falsely `PASS`. (b) `tools/bt_sync.py` `--bench-load --load-file` did a case-sensitive dictionary lookup against case-insensitive exFAT `0:\record`, and `--for-recording` used the CLI argument's case instead of the on-card stem. |
| **N1** | Low | Not Fixed (By Design) | — | XREC `PUT CAS` Filename Check | `is_safe_filename()` allows UTF-8 bytes `> 0x7F` at `PUT CAS` staging, while `exfat_replace_file()` enforces ASCII (`0x20..0x7E`) at `PUTEND` (`ERR,SDIO`). Pre-existing `CAS` behavior; `test_xrec_rec.c:560` intentionally uses `PUT CAS,<utf8>` to unit-test UTF-8 boundary truncation in `xfer_copy_name()`. New `PUT REC` strictly enforces ASCII in `is_rec_sidecar_name()`. |
| **N2** | Low | Not Fixed (By Design) | — | `bt_rom_bringup.c` Active-Link RF Hooks | Small RF callbacks (`rf_rssi_convert`, `cs_set_common`, `rf_txpwr_*`, ~200 B) moved verbatim from deleted `src/bt_hci.c` remain in XIP `.text` on CPU0 (`bt_ctrl` task), while all CPU1 hot loops and SBC/controller hot paths are in SRAM. No XIP deadlock hazard (CPU1 never fetches XIP during active UI/audio). |
| **N3** | Low | Not Fixed (Tracked S3/S5) | — | Manual Background HP Reconnect vs. REC Switch | Entering REC while a *manually started* (`!started_here`) HP session is disconnected with background reconnect scheduled calls `bt_hp_suspend()` (`main.c:3131`), which suspends an active stream (`BT_HP_LINK_STARTED`) rather than pausing `rc_poll` timers. Explicitly scoped to the coexistence policy work in `docs/BT_SYNC_SPEED_AND_RECORDING.md` (S3/S5); auto-started sessions from Play are now fully stopped on REC switch by **F4**. |

---

## 2. Detailed Findings — Fixed

### F1 — `bm_log_rec()` Masked `PRIMASK` Across ~450 XIP Log Calls Instead of `BASEPRI`
- **Severity:** High (`AGENTS.md` Concurrency, Interrupts & LVGL violation; recording-integrity hazard on `BENCH=1` images)
- **Introduced in:** `121e267` (`bt: S2 measurement image and tools for sync during recording`), `open_fw/src/recorder.c:488-500`
- **Evidence at `0f407ce`:**
  - `AGENTS.md`: *"Never use `OPENFW_IRQ_SAVE` / `__disable_irq()` around exFAT, recorder, or UI work. Use FreeRTOS `BASEPRI` (`taskENTER_CRITICAL`, threshold `0x80`) for shared state, and keep audio ADC/DAC DMAC IRQs (61/60) at priority `0x00` so they preempt BT (`0x80`) and are never masked."*
  - In `open_fw/src/recorder.c:495`, `bm_log_rec()` serialized the ~450-byte `BENCH rec ...` line into the RAM log ring under `uint32_t irq = OPENFW_IRQ_SAVE(); ... OPENFW_IRQ_RESTORE(irq);` (`CPSID i` / `PRIMASK=1`), calling XIP `openfw_log_putc()` in a per-byte loop while masking priority-`0x00` audio DMAC IRQs.
  - Additionally, `open_fw/Makefile` only built `tests/test_audio_bt_mmio.c` without `-DOPENFW_BT_BENCH_IMAGE=1`, so `bm_log_rec()` and the `recorder.c` / `audio_dma.c` `BENCH_MEAS` hooks were never compiled or exercised in `test-host` / `test-host-san`.
- **Failing-first test:** Added `open_fw/tests/audio_bt_mmio_irq_hooks.h`, `-DOPENFW_BT_BENCH_IMAGE=1` target `test_audio_bt_mmio_bench` in `open_fw/Makefile`, and IRQ-masking assertions in `open_fw/tests/test_audio_bt_mmio.c` verifying `OPENFW_IRQ_SAVE` is `0` during `bm_log_rec()` while `taskENTER_CRITICAL` (`BASEPRI`) is used. Confirmed red failure (`Assertion 'g_irq_save_in_log == 0u' failed`).
- **Fix (`4f2917c`):** Replaced `OPENFW_IRQ_SAVE`/`OPENFW_IRQ_RESTORE` in `bm_log_rec()` with `taskENTER_CRITICAL()`/`taskEXIT_CRITICAL()` under `OPENFW_RTOS` (retaining `OPENFW_IRQ_SAVE` only on non-RTOS host builds).

---

### F2 — `+XREC:DBG` Reply Buffer Overflow / Truncation and Silent `xo_send()` Drop
- **Severity:** High (Protocol bug / silent loss of S2 benchmark diagnostic replies)
- **Introduced in:** `054513e` (`bt: S2 review fixes (block clock, SD error counters, rf_audio_check false passes)`)
- **Evidence at `0f407ce`:**
  - `open_fw/include/openfw_bench_meas.h:22` defined `BENCH_MEAS_LINE_MAX 560u` and `open_fw/include/openfw_xrec.h:42` defined `XREC_REPLY_MAX (72u + 504u)` (`576u`), while `bt_xrec_bluedroid.c:918` (`xo_tx_free`) gates `+XREC:` commands on `xo_tx_free() >= XREC_REPLY_MAX` and `xo_send()` drops any reply larger than the remaining ring space all-or-nothing (`bt_xrec_bluedroid.c:907`).
  - Commit `054513e` appended `bclk_smp=`, `bdrift=`, `sd_err=`, `sd_mdto=` to `bench_meas_fmt_rec()` and `,take_us=` to `bench_meas_fmt_cpu()` without updating `BENCH_MEAS_LINE_MAX`, `XREC_REPLY_MAX`, `handle_dbg()`'s `char resp[576]`, or `test_bench_meas.c`'s `t_widths()` test.
  - With legal maximum-width fields (30-char filename, `el_ms = 134217600`, `ov = 4000000000`, `comp = 1000000000`, `wav_frames = 3219126272`, negative 11-char `drift`/`bdrift`/`acct`), `bench_meas_fmt_rec()` produces **`457 B`** and `bench_meas_fmt_cpu()` produces **`110 B`**:
    - `a + 1 + b = 457 + 1 + 110 = 568 B >= BENCH_MEAS_LINE_MAX (560 B)`;
    - `+XREC:DBG,LIVE,` (`15 B`) + `457 B` + `,` (`1 B`) + `110 B` + `\r\n` (`2 B`) = **`585 B > 576 B`**, truncating `take_us` inside `handle_dbg()` and silently dropping the entire `+XREC:DBG` response whenever `tx_free` is in `576..584 B`.
- **Failing-first test:** Updated `t_widths()` in `open_fw/tests/test_bench_meas.c` with true maximum-width values and added an all-or-nothing `tx_free = XREC_REPLY_MAX` `send()` assertion in `open_fw/tests/test_xrec_rec.c`. Confirmed both failed red (`a=457 b=110 (sum+1=568 vs 560, reply=585 vs 576)` and `Assertion 'strcmp(g_out, "") != 0' failed`).
- **Fix (`b72b950`):** Raised `BENCH_MEAS_LINE_MAX` to `576u`, raised `XREC_REPLY_MAX` to `(88u + 512u)` = `600u` (`15 B` margin above `585 B`), sized `handle_dbg()`'s buffer to `XREC_REPLY_MAX`, and added a compile-time `_Static_assert(XREC_REPLY_MAX >= 16u + BENCH_MEAS_LINE_MAX + 2u)`.

---

### F3 — `PUT REC` Sidecar Misattachment Across `DEL` + Reuse, Stale `put_done_valid`, and Stuck Upload UI
- **Severity:** Medium–High (Recording/sidecar data-integrity risk + protocol & UI state-machine bugs)
- **Introduced in:** `a842197` / `2acd5b9` (`PUT REC`, idempotent `PUTEND`) and `e2ff227` / `b3fa6c6` (BT transfer progress UI)
- **Evidence at `0f407ce`:**
  1. **Deleted-recording sidecar attached to next recording on stem reuse (`open_fw/src/xrec_proto.c:830-839, 1031-1060`):** When `+XREC:DEL,REC_0003.wav` arrived during an active `PUT REC,REC_0003.cwx` upload, `handle_del()` deleted `/record/REC_0003.wav` and any committed `/record/REC_0003.cwx`, but left `ctx->put_writer` open on `/record/.xrec_rec.tmp` (`ctx->put_stream_mode == XREC_PUT_REC`). If a new recording started before `PUTEND` (e.g. via `+XREC:RECSTART` or after reconnect/session reuse), `recorder_start()` reused the freed number `REC_0003.wav`; `handle_putend()` then saw `rec_wav_exists("REC_0003.cwx") == 1` and committed the **deleted** recording's `.cwx` transcript onto the **new** recording.
  2. **Early `ERR,ARG` `PUT` header did not clear `put_done_valid` (`open_fw/src/xrec_proto.c:1319-1335`):** `ctx->put_done_valid` was only cleared inside `put_refuse()` and `handle_put_complete()`, not on early `send_err(ctx, "ARG")` returns in `try_start_put_header()` (malformed fields or `chunk_len > XREC_MAX_CHUNK`). A subsequent `+XREC:PUTEND` falsely replied `+XREC:OK,PUTEND,<prev_total>` for the failed upload.
  3. **Aborted uploads left stale transfer progress on the BT screen (`open_fw/src/xrec_proto.c:895-910, 1051-1088`):** `abort_staged_put()` and error paths in `handle_putend()` / `handle_ota()` left `ctx->xfer_valid = 1` (`xfer_upload = 1`), keeping failed upload progress displayed (and indefinitely when `ops.now_ms == NULL`).
- **Failing-first test:** Added assertions for all three cases in `open_fw/tests/test_xrec_rec.c`; confirmed red failure before fixing.
- **Fix (`ac31bf3`):** Added `put_rec_deleted` in `xrec_ctx_t`; `handle_del()` immediately calls `abort_staged_put(ctx)` and sets `put_rec_deleted = 1u` when `<stem>.wav` matches an in-progress `PUT REC,<stem>.cwx`, so `PUTEND` returns `ERR,NOENT` even if `<stem>.wav` was recreated first. Cleared `put_done_valid` and `put_rec_deleted` at the top of `try_start_put_header()` and in `xrec_on_disconnect()`. Cleared `xfer_valid` on upload aborts and failed `PUTEND`/`OTA` commits.

---

### F4 — Headphone Auto-Connect Leaked BT Sessions into Recording/Idle and Clobbered `PAIR_AGAIN`
- **Severity:** High (Recording RF/CPU isolation, battery drain, and headphone pairing/reconnect state machine)
- **Introduced in:** `35b4397` (`open_fw: auto-connect the remembered headset when playing with output BT`) and `bt_hp_bluedroid.c`
- **Evidence at `0f407ce`:**
  1. **Unbonded `bt_hp_connect_addr(bda, 0)` cleared `pair_again` and paged on air (`open_fw/bt_port/bt_hp_bluedroid.c:2591-2633`):** Unlike `rc_poll()` (`line 1249`), `bt_hp_connect_addr(bda, 0)` (`pair == 0`, no pairing window) did not check `!bonded(bda)`. Calling it for a remembered headset whose bond was missing reset `g->pair_again = 0u` (`line 2620`) and queued an outgoing page (`HCI Create Connection`) that was guaranteed to fail SSP (`auth st=9`).
  2. **Leaked `started_here` BT session across REC switch & failures (`open_fw/src/main.c:2331-2404`):** When `hp_auto_connect_for_play()` started BT (`started_here == 1`), it called `bt_hp_stop()` on `KEY_BACK` (`line 2353`) and `!u.has_last` (`line 2376`), **but omitted `bt_hp_stop()`** on:
     - **REC slide switch asserted (`line 2331`):** sliding REC ON while `Starting...` or `Connecting...` returned `0` while leaving the newly started BT session finishing bring-up and paging/reconnecting on air throughout microphone recording (`main.c:3151` only calls `bt_hp_suspend()`, a no-op unless `BT_HP_LINK_STARTED`);
     - **`bt_hp_connect_addr` error (`line 2383`), `CONNECT_FAIL` / `PAIR_AGAIN` / `UNSUPPORTED` (`line 2393`), and timeout (`line 2401`):** pressing Play once with headphones powered off left BT active and paging every 60 s indefinitely.
  3. **`PAIR_AGAIN` ordering & toast (`open_fw/src/main.c:2381-2399`):** `main.c` called `bt_hp_connect_addr(u.last, 0)` *before* checking `u.msg_seq != seen_seq`, overwriting `rc_poll()`'s `BT_HP_MSG_PAIR_AGAIN` and showing generic `"Connect failed"` instead of `"Pair again"` when `connect_addr` failed.
  4. **Premature timeout & uncancelled page (`open_fw/src/main.c:2309, 2401`):** `t0 = millis()` was sampled *before* `hp_start_session()` (RF calibration + bring-up + P-192 key generation), subtracting bring-up time from `BT_HP_CONNECT_TO_MS` so `main.c` timed out *before* `bt_hp`'s own `BT_HP_CONNECT_TO_MS` and without calling `bt_hp_disconnect()` when `!started_here`.
- **Failing-first tests:**
  - Added `fn_connect_last_nopair` check in `S13a` of `open_fw/tests/bt_host/test_headphones.c` — confirmed red failure (`S13a: bt_hp_connect_addr(last, 0) accepted without a stored key` and `the keyless headset was paged again (1 -> 1 tries, 1 pages)`).
  - Extracted initial 1:1 mirror into `open_fw/bt_port/bt_hp_autoconn.h` and wrote `open_fw/tests/test_bt_hp_autoconn.c` (wired into `test-bt-port`) — confirmed 8 red failures across REC-switch leak, `CONNECT_FAIL`/`PAIR_AGAIN`/timeout leaks, `PAIR_AGAIN` clobber, premature timeout, and `!started_here` page cancel.
- **Fix (`5d1dfce`):**
  - `bt_hp_connect_addr()` now checks `if (!pair && !bonded(bda)) { pair_again("no stored key"); return -1; }` right after `hp_rows_refused("connect")` (preserving `BT_HP_MSG_UNSUPPORTED` when rows are gated off).
  - `bt_hp_autoconn.h` + `main.c` stop `started_here` sessions on every non-ready exit, check `u.msg_seq != seen_seq` before `connect_addr`, surface `PAIR_AGAIN`/`UNSUPPORTED` messages on refused `connect_addr`, anchor the connect deadline wrap-safely (`(int32_t)(now - deadline) >= 0`) to `BT_HP_ST_READY`, and cancel in-flight pages via `bt_hp_disconnect()` on `!started_here` timeout.

---

### F5 — `rf_audio_check.py` False Pass on Systematic Block-Clock Drift with `>= 2` Baselines & `bt_sync.py` Case-Sensitive `--load-file`
- **Severity:** Medium (S2 measurement gate false-pass risk + CLI failure on uppercase exFAT names)
- **Introduced in:** `054513e` / `121e267` (`tools/rf_audio_check.py:589-604`, `tools/bt_sync.py:800, 851`)
- **Evidence at `0f407ce`:**
  1. **`tools/rf_audio_check.py:589-604`:** When `>= 2` baseline captures (`C0a`, `C0b` — required by the S2 M2 protocol so repeatability is not `INCONCLUSIVE`) have device counters in `--bench-log`, `others` (`line 639`) is non-empty for every baseline and `refs` (`line 642`) is non-empty for every test capture. Consequently, `compare()` **always** entered `elif refs:` (`+-8` samples relative to `C0`) and **never** checked `abs(ppm) <= lim.drift_ppm` (`300 ppm`). A systematic clock/PLL/IRQ error shared across `C0a`, `C0b`, and `C1..C3` (e.g. `bdrift = -1000` over 75 s = `-837 ppm`) produced `PASS` for both `C0` and the test captures.
  2. **`tools/bt_sync.py:800, 851`:** `--bench-load --load-file` used `dict(client.list_files()).get(args.load_file, 0)` (case-sensitive) against exFAT's case-insensitive `0:\record` directory, and `--upload-cwx --for-recording` built the `.cwx` name from `Path(args.for_recording).stem` instead of the recording's on-card stem.
- **Failing-first tests:** Added a `-837 ppm` systematic drift test to `self_test()` in `tools/rf_audio_check.py` (confirmed red: `verdict=PASS not-pass=[]`) and `find_remote_file` case-insensitive assertions in `tools/test_bt_sync_xrec.py` (confirmed red).
- **Fix (`4a489e9`):** Added `(e) block-clock rate` (`|ppm| <= lim.drift_ppm`) alongside `(e) block-clock drift vs C0` when `refs` is non-empty in `tools/rf_audio_check.py`, and added `find_remote_file()` in `tools/bt_sync.py` for both `--bench-load --load-file` and `--upload-cwx --for-recording`.

---

## 3. Observations Not Modified (By Design / Out of Scope)

- **N1 — `PUT CAS` non-ASCII filename rejected at `PUTEND` rather than `PUT` (`open_fw/src/xrec_proto.c:106, 1378` vs. `open_fw/src/exfat.c:1920`):**
  `is_safe_filename()` allows bytes `> 0x7F` when staging `CAS` uploads to `/cassini/.xrec_cas.tmp`, whereas `exfat_replace_file()` enforces ASCII (`0x20..0x7E`) at `PUTEND`, returning `+XREC:ERR,SDIO` (and with **F3** fixed, cleanly deleting the temp file and clearing `xfer_valid`). Kept unchanged because `PUT CAS` is pre-existing and `open_fw/tests/test_xrec_rec.c:560` explicitly uses `PUT CAS,<utf8>` to unit-test multi-byte UTF-8 truncation in `xfer_copy_name()`; the newly added `PUT REC` target already enforces strict ASCII (`0x20..0x7E`) at header time via `is_rec_sidecar_name()`.
- **N2 — `bt_rom_bringup.c` small RF callbacks in XIP `.text` (`open_fw/bt_port/bt_rom_bringup.c:49-128`):**
  Plan step 9 (`4805867`) moved `bt_rom_bringup.c` verbatim from deleted `src/bt_hci.c`. The 5 tiny RF callbacks installed into `ip_funcs_p` (~200 B total) remain in XIP `.text` and run only on CPU0 (`bt_ctrl` context), matching `3942608`. CPU1's main loop, fault handlers, LCD/LVGL hot paths, and SBC/BT hot loops remain in SRAM, satisfying `AGENTS.md`'s CPU1/CPU0 XIP fetch deadlock rule without shrinking `RTOS_HEAP_LO`.
- **N3 — Manual background HP reconnect timer across REC switch (`open_fw/src/main.c:3131`):**
  If a user manually enabled Headphones mode (`!started_here`), disconnected or lost link (leaving periodic `rc_poll` backoff scheduled), and then slid REC ON, `main.c` calls `bt_hp_suspend()`, which suspends an active `BT_HP_LINK_STARTED` stream rather than pausing `rc_poll`. Leaving a manually started session's policy untouched here matches `docs/BT_SYNC_SPEED_AND_RECORDING.md` (which schedules the unified BT-during-recording policy for stages S3/S5), whereas auto-started sessions from Play (`started_here`) are now torn down cleanly on REC switch by **F4**.

---

## 4. `AGENTS.md` & Scope Verification Matrix

| Rule / Scope Item | Status | Evidence |
|---|---|---|
| **Header-last flashing (`0x00D000`)** | Verified Clean | `tools/flash_ranges.py:210-258` invalidates `0x00D000` (`4 KiB` sector erase) first, programs/verifies all other dirty sectors next, and writes/verifies `0x00D000` last (`flash_ranges.py --self-test` & `test_tools.py` green). |
| **Strict USB PID guard (`301a:2800` only)** | Verified Clean | `tools/flash_ranges.py:72-95` and `tools/dev_cycle.py` strictly bind BootROM commands to `301a:2800` and never issue BootROM vendor CDBs to `301a:2801` MSC mode. |
| **Keep CPU1 hot loops in SRAM** | Verified Clean | `sl6801_firm.ld`, `cpu1_ui.c`, `lcd_st7735s.c`, and `.bt_fast` keep CPU1 loops, fault handlers, LCD blit, and SBC/controller hot paths in SRAM; `BT link roots OK (47 entry points)`. |
| **No long `PRIMASK` locks (`BASEPRI=0x80`, DMAC IRQs `0x00`)** | **Fixed (F1)** & Verified | `bm_log_rec()` fixed in `4f2917c` to use `taskENTER_CRITICAL()` (`BASEPRI`); `bm_dma_stamp()` in `audio_dma.c` / `recorder.c` uses a lock-free monotonic stamp without masking IRQs; DMAC CH1/CH0 stay at priority `0x00`. |
| **Zero SD/file I/O on LVGL/UI path & static LVGL memory** | Verified Clean | `LV_USE_FS_* = 0`, `LV_MEM_POOL_EXPAND_SIZE = 0`; new BT transfer progress bar (`open_fw/src/ui/ui_screens.c`) renders strictly from CPU0's `ui_model_t` snapshot (`bt_xfer_*` fields) with static LVGL widgets and UTF-8-safe truncation. |
| **MSC/exFAT boundary, CBW zeroing & SHA-256 parity** | Verified Clean | `open_fw/src/usb_musb.c` zeroes `cbw->signature` immediately after handling each SCSI CBW; `open_fw/tests/test_core.c` `exfat_usb_msc_sha256_parity` verifies 10,000 B exFAT write vs. USB MSC `READ(10)` SHA-256 parity. |
| **Battery (`< 3450 mV` / `3550 mV`) & 2 GiB WAV guards** | Verified Clean | Preserved in `recorder.c`, `main.c`, and `bt_port/bt_lowbatt.h` (`test_bt_lowbatt: 58 checks, 0 failures`). |
| **Cleanroom MIT licensing** | Verified Clean | All new files (`bt_rom_bringup.{c,h}`, `bt_hp_autoconn.h`, `bench_meas.c`, `openfw_bench_meas.h`, `rf_audio_check.py`, tests) carry clean MIT headers and cleanroom implementations. |
| **Strict BT privacy (Phone MITM-only, window auto-close)** | Verified Clean | Phone XREC session (`bt_xrec_bluedroid.c`) remains SSP MITM-only, purges non-MITM bonds, and requires HMAC-SHA256 `AUTH` before `PUT REC` / `GETS` / `LS` / `DEL` (`test_bt_xrec: 694 checks, 0 failures`). |
| **Wrap-safe timers & auto-resume durability** | **Fixed (F4)** & Verified | `bench_meas.c` uses 64-bit `us_timer_now()`; `bt_hp_autoconn.h` (`5d1dfce`) uses wrap-safe `(int32_t)(now - deadline) >= 0`; `apmem` durability tests all pass (`test_core`). |
| **Step 9 (`BT=1` default, legacy BT deleted) & HP HCI rows (`0401/0402/0405/0408`)** | Verified Clean | `src/bt_hci.c`, `src/bt_spp.c`, and `include/openfw_bt_spp.h` removed cleanly; `GATE_FLAGS := -DBT_CTRL_EXP1_PASSED_LIST=BT_CTRL_HP_GATE_LIST` enabled in every `BT=1` firmware build while keeping host gated-off test (`hpgated`) covered; `+XREC:PONG` advertises `,rec=1`. |

---

## 5. Build & Test Gate Verification

All gates were executed using isolated scratch directories under `/home/silvio/dev/.build-scratch/agy/fw/` with `ANDROID_HOME=/home/silvio/Android/Sdk`:

| Gate | Command / Scratch Dir | Result |
|---|---|---|
| **Default Firmware (`BT=1`)** | `make -C open_fw BUILD_DIR=.../fw/bt1 PICOLIBC_WORK=.../fw/picolibc all` | **PASS — 0 warnings** (`843,832 B` FIRM: `3,920/4,096 B` `ram_boot` + `838,664 B` XIP, 47 BT link roots OK) |
| **`BT=0` Firmware** | `make -C open_fw BT=0 BUILD_DIR=.../fw/bt0 all` | **PASS — 0 warnings** (`419,500 B` FIRM: `3,924/4,096 B` `ram_boot` + `414,332 B` XIP) |
| **Bench + HP Gates Firmware** | `make -C open_fw BT=1 BENCH=1 HP_GATES=1 BUILD_DIR=.../fw/bench PICOLIBC_WORK=.../fw/picolibc all` | **PASS — 0 warnings** (`863,184 B` FIRM: `3,940/4,096 B` `ram_boot` + `858,016 B` XIP, 47 BT link roots OK) |
| **`test-host`** | `make -C open_fw BUILD_DIR=.../fw/host test-host` | **PASS** (`test_core`, `exfat-img` + `fsck.exfat`, `test_xrec_rec` + bench + `fsck.exfat`, `test_bt_sync_xrec.py`, `test_ui`, `test_audio_bt_mmio` + bench, `test_sd_hsprobe`, `test_rtos_coop`) |
| **`test-host-san`** | `make -C open_fw BUILD_DIR=.../fw/host_san test-host-san` | **PASS** (all host suites under ASan + UBSan) |
| **`test-bt-xrec`** | `make -C open_fw BT_XREC_TEST_DIR=.../fw/bt_xrec test-bt-xrec` | **PASS** (`694 checks, 0 failures`; release object verified free of bench override symbols) |
| **`test-bt-port`** | `make -C open_fw BT_PORT_TEST_DIR=.../fw/bt_port test-bt-port` | **PASS** (`entropy`, `p192`, `rom_rng` vs. mask ROM dump, `alarm`, `task_sup`, `lowbatt`, `hp_autoconn` 24/24, `store` 20k fuzz, `hci_probe`, `bt_bench`, `bench_meas` 68/68) |
| **`test-bt-ctrl`** | `make -C open_fw BT_CTRL_TEST_DIR=.../fw/bt_ctrl test-bt-ctrl` | **PASS** (default gate-off, `BT_CTRL_HP_GATE_LIST`, and `BT_CTRL_HCI_TRACE=1`) |
| **`test-bt-hp`** | `make -C open_fw BT_HP_TEST_DIR=.../fw/bt_hp test-bt-hp` | **PASS** (`hp_sink` ASan + TSan, `hp_tx` SBC round-trip, `xsrc_stats`, `audio_out` BT=0/BT=1, `player_bt`, golden check) |
| **`test-bt-host`** | `make -C open_fw BT_HOST_TEST_DIR=.../fw/bt_host test-bt-host` | **PASS** (`all 200`, `phone` gate-off & HP-gate, `hp` link smoke, `hps` 230/230 checks, `hpgated` 20/20 checks, and `init`/`enable`/`disable`/`deinit` OOM sweeps: 247/247 points, `0 WAIVED, 0 CRASH, 0 HANG, 0 CHECK`) |
| **Python Host Tool Suites** | `rf_audio_check.py --self-test`, `test_tools.py`, `flash_ranges.py --self-test`, `dev_cycle.py --self-test` | **PASS** (`rf_audio_check`: `46 checks, 0 failures`; all safety & protocol self-tests green) |
