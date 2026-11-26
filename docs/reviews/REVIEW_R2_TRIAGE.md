# Review R2 (agy-2) of `3942608..0f407ce`: triage

Reviewed tree: `main` at `0f407ce`. Reviewer branch `review/r2-0f407ce` (fix commits `4f2917c`, `b72b950`, `ac31bf3`,
`5d1dfce`, `4a489e9`; report `ff2c2e7`). Raw report: [REVIEW_R2.md](REVIEW_R2.md). Each finding was re-checked against
the code at `0f407ce`, and each test was mutation-checked against the code before the fix (*M*: the test fails
without the fix, passes with it).

Decision key: **accept** (cherry-picked with `-x`), **port** (re-implemented on the reviewer's commit, changes named),
**reject** (not a defect as stated). Applied on `triage/r2` (base `0f407ce`).

## Triage

| ID | Sev (review / ours) | Finding (short) | Decision | Reason | Test |
|---|---|---|---|---|---|
| F1 | high / medium | `bm_log_rec()` (BENCH images only) pushes the ~450 B `BENCH rec` line under `OPENFW_IRQ_SAVE` (PRIMASK), masking the priority-0 audio DMAC IRQs; the bench recorder hooks were never host-built | accept | Real AGENTS.md violation ("no long PRIMASK locks"); limited to `BENCH=1` images and one line per REC stop, so no field impact. Fix matches `bt_bench_fw.c` (`taskENTER_CRITICAL`, BASEPRI 0x80): BT tasks that log into the same ring are still excluded. New `test_audio_bt_mmio_bench` (`-DOPENFW_BT_BENCH_IMAGE=1`) covers the hooks | `test_audio_bt_mmio_bench`: `g_irq_save_in_log == 0` *M* |
| F2 | high / low | `+XREC:DBG,LIVE` worst case 585 B > `XREC_REPLY_MAX` 576 B (fields added in `054513e`): truncated `take_us`, all-or-nothing `xo_send` drop when 576..584 B free | accept | The sizing contract is real (the old width test used too-narrow values). The worst case needs values no recording reaches (15-char name, 10-digit `ov`/`comp`, 11-char negative drifts at the same time), hence low. Fix is minimal: `BENCH_MEAS_LINE_MAX` 576, `XREC_REPLY_MAX` 600 (TX ring 2048, static asserts hold), `handle_dbg` bounded by `XREC_REPLY_MAX` with a `_Static_assert` | `test_bench_meas` [4] (568 vs 560, 585 vs 576) *M*; `test_xrec_rec` DBG with `tx_free == XREC_REPLY_MAX` |
| F3a | medium-high / low-medium | `DEL,<stem>.wav` during `PUT REC,<stem>.cwx` leaves the staged temp open; a new recording reusing `<stem>.wav` before `PUTEND` gets the deleted recording's transcript | accept | Real (record numbers are reused after a delete; `RECSTART` exists). Needs a client that deletes during its own upload, then records. Fix: `handle_del` aborts the staged upload at once and latches `put_rec_deleted` so `PUTEND` answers `ERR,NOENT` (cleared by the next PUT header / disconnect) | `test_xrec_rec`: temp gone after DEL, `NOENT` after the name is recreated *M* |
| F3b | medium-high / low | a PUT header refused early with `ERR,ARG` leaves `put_done_valid`, so a following `PUTEND` repeats the previous `OK,PUTEND` | accept | Real; matches the documented "idempotent until the next PUT header". `XREC_PROTOCOL.md` now says "any, even one refused with `ERR,ARG`" | `test_xrec_rec` `PUT ... 9999 ...` then `PUTEND` -> `NOPUT` *M* |
| F3c | medium-high / low | aborted uploads / failed PUTEND / OTA leave `xfer_valid` (upload progress on the BT screen) | accept | Cosmetic: with `ops.now_ms` set (firmware) the view expires after `XREC_XFER_HOLD_MS` anyway. Clearing it at once is correct and costs nothing | `test_xrec_rec` SDIO mid-upload -> `!x.active` *M* |
| F4a | high / medium | `bt_hp_connect_addr(bda, 0)` with no stored key clears `pair_again` and pages a headset that can only fail SSP (`auth st=9`) | accept (in the F4 port) | Real; `rc_poll()` already refuses this case. Only callers with `pair=0`: auto-connect on Play and the bench (`bt_bench_fw.c`), both of which want a bonded headset | `test-bt-host` hps S13a (keyless connect refused, PAIR_AGAIN, no page) *M* |
| F4b | high / medium | an auto-started (`started_here`) HP session is left running after a failed connect (`connect_addr` error, CONNECT_FAIL / PAIR_AGAIN / UNSUPPORTED, timeout): background reconnects every 60 s the user did not start | port | Accepted for the failure exits (the 35b4397 message only named BACK; a Play press should not leave the radio paging). **Changed:** the REC switch does not stop the session. `bt_hp_stop()` blocks for the teardown (up to the 2 s shutdown deadline) and would delay the start of the recording; design §6.9 keeps HP mode across REC (`bt_hp_suspend`), and the BT-during-recording policy is S3/S5 (see N3). Open for a user decision: stop an auto-started session after REC instead | `test_bt_hp_autoconn` (26 checks) |
| F4c | high / low | `PAIR_AGAIN` from `rc_poll` overwritten by the generic "Connect failed" because `connect_addr` ran before the message check | accept (in the F4 port) | Real ordering bug, wrong toast text only | `test_bt_hp_autoconn` case 4 |
| F4d | high / low | timeout measured from before RF calibration / bring-up; no page cancel on a pre-existing session's timeout | port | Accepted: the connect deadline is anchored (wrap-safe) at READY + `BT_HP_CONNECT_TO_MS` + 1 s, so bt_hp's own CONNECT_FAIL comes first. **Changed:** the review's pre-READY bound was 12 s on the premise "UP + CONFIG = 9 s"; they are 12 s + 5 s (`bt_xrec_bluedroid.h`), so 12 s could stop a slow but healthy bring-up that bt_hp itself would let finish. Now `BT_XREC_UP_DEADLINE_MS + BT_XREC_CONFIG_DEADLINE_MS + 1000` (18 s): bt_hp's FAULTED comes first | `test_bt_hp_autoconn` cases 2b, 3c, 5 |
| F5a | medium / medium | `rf_audio_check.py`: with C0 counters (the standard 2-baseline M2 run) the gross `|ppm| <= 300` block-clock check is skipped, so a systematic rate error shared by C0 and the captures passes | accept | Real false-pass path in a measurement gate; one extra row | `rf_audio_check --self-test` (-837 ppm case: PASS before) *M* |
| F5b | medium / low | `bt_sync.py --bench-load --load-file` size lookup is case-sensitive; `--upload-cwx --for-recording` takes the stem from the CLI spelling | accept | Real, host tool only | `test_bt_sync_xrec.py` `find_remote_file` |
| N1-N3 | low | not fixed by the reviewer | agree | N1 pre-existing CAS behaviour; N2 CPU0-only XIP callbacks; N3 is the S3/S5 policy (now also covers F4b's REC case) | - |

No rejections. The F4 port is not tested on silicon (the original 35b4397 was); the Play-with-output-BT path should
be re-checked on the device: headset off -> "Connect failed" and BT off afterwards; slide REC during "Starting...".

## Commits on `triage/r2`

| Reviewer commit | Here | Decision |
|---|---|---|
| `4f2917c` (F1) | cherry-pick -x | accept |
| `b72b950` (F2) | cherry-pick -x | accept |
| `ac31bf3` (F3) | cherry-pick -x (+ `XREC_PROTOCOL.md` wording in the triage commit) | accept |
| `4a489e9` (F5) | cherry-pick -x | accept |
| `5d1dfce` (F4) | cherry-pick -n, modified (REC switch, pre-READY bound) | port |
| `ff2c2e7` (report) | copied to `docs/reviews/REVIEW_R2.md` | - |

## Merge notes (open branches based on `0f407ce`)

- `feat/bt-sync-rec-hardening` overlaps in `bt_hp_bluedroid.c`, `openfw_xrec.h`, `main.c`, `recorder.c`,
  `xrec_proto.c`, `test_audio_bt_mmio.c`, `test_xrec_rec.c`, `bt_sync.py`, `test_bt_sync_xrec.py`; textual conflicts
  in `tests/test_xrec_rec.c` and `tools/test_bt_sync_xrec.py`. Semantics: its `handle_del` answers `LATER` while
  recording before the F3a abort (compatible); its `abort_staged_put` split (`drop_staged_file`, `put_orphan`) must
  keep F3c's `xfer_valid` clear in `abort_staged_put`.
- `feat/bench-rec-trigger`, `exp/sync-throughput`, `feat/sync-throughput-defaults`: overlap only in `src/main.c`, no
  textual conflict (`git merge-tree`).
