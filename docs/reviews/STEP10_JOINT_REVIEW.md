# Step 10 joint review (Codex + agy) of `c15f6b2`: triage

Reviewed tree: `integ/bt-all-local` at `c15f6b2`. Both reviewers read the whole BT tree (bt_port, the Bluedroid
patches, main/player/audio_out, exFAT, XREC protocol). Each finding was re-checked against the code before triage.
Raw outputs: [STEP10_CODEX.md](STEP10_CODEX.md) (Codex, 18 findings) and [STEP10_AGY.md](STEP10_AGY.md) (agy, 11
findings after verification; agy's own numbering differs). Here Codex findings are **C1-C18** and agy findings
**A1-A11**, in the order of the verified lists.

Fixed on `integ/bt-all-local` after merging `fix/bt-verifier-lows`, `fix/toast-text-all` and `feat/bt-step8`.

Decision key: **accept** (fixed here), **defer** (open, reason given), **reject** (not a defect as stated).
"Test" names the check that fails without the fix (mutation-checked where marked *M*).

## Triage

| ID | Sev (ours) | Finding (short) | Decision | Fix / reason | Test |
|---|---|---|---|---|---|
| A1 | high | `+XREC:DEL` can delete the WAV being recorded (writer keeps filling freed clusters) | accept | new optional `xrec_ops_t.file_busy`; `recorder_is_recording_file()` (case-insensitive) wired in `bt_xrec_fw.c` and legacy `bt_hci.c`; DEL answers `+XREC:ERR,BUSY` | `test_core` DEL of the busy file -> ERR,BUSY, file intact *M* |
| C1 | high (Codex) | temp-file cleanup after a failed / interrupted `exfat_replace_file` frees clusters the destination already uses | reject | already guarded since `cd46ead`: `exfat_delete_file()` frees a file's clusters only if no other entry in the directory references the same first cluster, which is exactly the half-replaced state. Residual fixed (`fix/s10-xrec-flow`): a reference scan that fails with an I/O error now returns -1 without freeing (clusters leaked, entry already gone) | `test_core` delete with directory reads failing after the entry-set write: bitmap bit kept *M* |
| A2 | high | a bond file that exists but cannot be read (no memory, SD error) leaves an empty store, which the start's `config_save` then writes over the file: every bond erased | accept | `bonds_load()` latches `s_bonds_hold` (file kept, `bonds_write()` skips) until a later load reads it; firmware and harness copies | phone P15b part 2 *M* |
| C10 | medium | a restart in the same mode reloads the older card file over RAM bonds whose write failed; a mode switch drops the other mode's unsaved bonds | accept | same mode + dirty store (and no hold) -> keep RAM and flush; mode switch -> try to flush the old mode's bonds into their own file first (logged if dropped) | phone P15b part 1 *M* |
| C9 | medium | `+XREC:PAIR` installs the new key and replies OK even when `cassini.key` could not be written | accept | `on_bound_key` now returns int and runs **before** the key is installed; failure -> `+XREC:ERR,SAVE`, old binding kept, pairing window stays open (Bluedroid, legacy `bt_hci.c`, harness) | `test_core` *M*; `test_bt_xrec` pair_and_auth |
| C4 / A3 | medium | patch-0036 Just Works hook allowed when no headphones session runs: in phone mode a NoIO spoofer could replace the phone's MITM key (then purged at AUTH_CMPL: DoS) | accept (hook part) | `openfw_just_works_allowed()` defaults to 0 unless a headphones session is open. Phone P11/P13 updated: Just Works is now refused at the confirmation (no key made). A3's second part (never replace an authenticated key with an unauthenticated one; keep bonds on AUTH_FAILURE / KEY_MISSING) **deferred**: a Bluedroid patch plus a fake-peer scenario; the cheap Just Works path is closed | headphones S10c *M*; phone P11/P13 |
| C2 / A8 | medium / low | headphones numeric comparison auto-accepted without showing the value; no window-deadline check | accept (deadline) / defer (UI) | `EV_CFM_REQ` now also requires `!time_reached(now, win_deadline)` (as the Just Works hook). Showing and confirming the 6 digits, or advertising NoInputNoOutput, **deferred to silicon**: changes which association model real headsets pick | - (needs a CFM after the deadline; no harness lever) |
| A4 | medium | `shutdown_now()` freed the session (ring, sink) before the hard path on fault exits, and ignored a tick that never confirmed its stop | accept | fault exits call `bt_xrec_plat_hard_path()` first (it reboots on target; the host harness frees after it); a tick that did not stop **and** a profile teardown that failed is escalated to `BT_XREC_FAULT_DOWN_FAIL` instead of freeing under it | - (trigger needs a stuck BTC tick; existing teardown tests unchanged) |
| C14 | medium | A2DP deinit timeout (job rc 5) logged "restart the recorder" but nothing stopped another start | accept | `s_restart_required` latch: `bt_hp_start()` refused until reboot (logged) | - (rc 5 has no harness lever) |
| C12 | medium | `conn_hdl` never invalidated: a later connect's timeout / cancel used the old link's handle | accept | invalidated in `on_link_down()` and `connect_now()` | - (needs a handle-reusing peer) |
| A5 | medium | XREC replies dropped when the 2 KiB TX ring is full (pipelined PAIR / CHAL / DEL after LS / GETS) | accept (`fix/s10-xrec-flow`) | RX backpressure: new optional `xrec_ops_t.tx_free`; `xrec_on_rx()` returns the bytes consumed and stops before any byte that would produce a reply (line end, PUT header `:`, last PUT payload byte) while less than `XREC_REPLY_MAX` (576 B, the LOG frame) is free. Bluedroid port: DATA moves to `rx_pend` (bytes stay in the RX ring), parsed before the next event; while held, the WRITE / CONG events behind it still run (no deadlock); held bytes are dropped on close / handle change; `rx_held` counter. Legacy `bt_hci.c` keeps `tx_free = 0` (old behaviour). A peer that pipelines more than the 2 KiB RX ring now hits the existing RX-overflow close | `test_core` `test_xrec_flow_control` (PING / PUT hold points) *M*; `test_bt_xrec` [6b] 8 pipelined LOGs vs congested BTC: `tx_busy` 4 and 4 replies lost without the op, all 8 in order with it *M* |
| C3 | medium (ours) | `gate_ok` is refreshed per app poll; RNG starvation between polls could let one pairing through on legacy randomness | defer | the gate already latches `rng.starved` per session and is re-read on every poll while a window is open; the residual window is one poll. Fix (lock-free starvation counter read in the hook and re-checked at `on_auth`) touches `bt_rom_rng` / ISR context | - |
| C5 | medium (ours) | `bt_ctrl_task` loops with zero wait while `more` is set; only the per-run hang guard is armed in production | defer, **hardware** | choosing a yield bound or arming the cumulative CPU guard in normal sessions needs real `ctrl_more` streaks during A2DP streaming (risk of false faults / throughput loss); nothing shows the ROM loops | - |
| A6 | low | AVDT/AVCT deregister leaks CCB/LCB queues per frozen-peer teardown; patch 0039 has no fallback on `osi_malloc` failure | defer | known leak #13 (BT_A2DP_HP_SESSION.md), bounded by S10; the rc 5 wedge is now latched (C14). Freeing CCB/LCB needs patch work | - |
| A7 | low | About long-press (USB bootloader reboot) skipped `bt_shutdown()` | accept | `bt_shutdown()` after `recorder_stop()` (BT=1 builds) | - (UI loop) |
| C8 | low | `bt_hp_forget()` cleared the remembered headset even when the bond was not removed (BT off, removal error) | accept | refused unless READY and the removal was posted; the Forget row is shown only in READY | headphones S10c |
| C15 | low | Play after the session detached under a headphones-only file unpaused into route NONE (silent "playing") | accept | unpause blocked while `aout_route() == AOUT_ROUTE_NONE` (same "not connected" toast) | - (player BT path has no host harness) |
| C16 | low | a discovery cancel whose "stopped" event never arrives leaves "Searching" forever | accept | `scan_cancel_until` (5 s, `BT_HP_SCAN_CANCEL_TO_MS`) then the search state is cleared; a pick still waits for bt_ctrl's inquiry flag | - (no lever to lose the event) |
| C17 | low | bench image: PIN 0000 stayed enabled after the bench run | accept | `bt_bench_hp_finish()` clears the tuning; the PIN path also checks `s_tun_set` | - (bench only) |
| C11 | low | failed `esp_spp_disconnect` leaves the SPP session "closing" | defer | only on a BTC queue allocation failure; the peer's drop / supervision timeout ends it. With the OOM fail-soft work (A9) | - |
| C13 | low | `a2d_cb` applies TX changes for any peer's late CONNECTED / DISCONNECTED in BTC context | defer | orphan gating narrows it; a BTC-side address filter needs the target under the lock and a late-event fake-peer scenario | - |
| C7 | low | `btc_transfer_context` failure loses an SPP write completion (`is_writing` stuck) | defer | out-of-memory only, upstream code; with A9 | - |
| A9 | low | live `assert()`s in Bluedroid OOM paths reset without finalising a recording | defer | fail-soft patch series, or route `__assert_func` to the BT hard path | - |
| C6 | low | `cassini.key` and link keys are plaintext on the removable card | defer (design) | format kept compatible with the stock firmware; encrypting needs a device-unique secret (same follow-up as C18). Threat model to document | - |
| C18 | low | all units without an eFuse BD_ADDR share `02:CA:55:00:01:00` | defer, **hardware** | known follow-up: derive from the NOR unique ID (read path to validate on silicon) | - |
| A10 | low | RF calibration before `bt_xrec_start()` repeats `bt_rom_real`'s and precedes mic entropy collection; start result ignored | defer, **hardware** | RF/mic coupling must be measured on silicon before reordering | - |
| A11 | low | BTC stack 3072 B unmeasured under streaming + pairing | defer, **hardware** | read the per-task high-water marks (`BT mem` log) during Exp streaming / pairing; raise to 4096 if headroom < 512 B | - |
| W1 | medium (ours) | output mode **Both** (follow-up check, not a reviewer finding): after a DAC underrun the speaker stayed late for good and the tap's backlog held the ring, so the headset starved after a few app-task gaps (the mono pass-through itself is correct in Both) | accept | `pump_dac`: DAC dry + backlog > 4 x 512 frames -> tap drops it and restarts aligned (`dac_resync_frames`); branch `test/aout-both-mode` | `test_player_bt` Both mono, app task out 400 ms/s: 0 empty packets (23 without) *M*; Both mono/stereo, connected / not, volume split; headphones S1 out_mode saved |

## Effect on the images

The default (BT=0) FIRM CRC changes again, 0x7EC9 (after the toast fix) -> 0xECB8, on purpose:
`xrec_proto.c` (DEL guard, PAIR saved before install), `bt_hci.c` (legacy XREC: key-save status and the busy-file
hook) and `recorder.c` (`recorder_is_recording_file`) are part of the default image. The legacy phone path gets the
same A1 / C9 behaviour as Bluedroid. BT=1 images: bt 0xA199, hp 0x663A, hpb 0xD52F, bench 0x4FED (all builds 0
warnings; CRCs depend on the source path, these are from the `voice-recorder-hw-bt-usb-integ2` worktree).

## Open items after step 10

Code (no hardware needed): C3 (starvation latch in the Just Works / CFM decisions), A3
second part (key-downgrade and AUTH_FAILURE bond removal patch), A6 (AVDT/AVCT deregister leak, 0039 fallback), A9 /
C7 / C11 (OOM fail-soft), C13 (BTC-side peer filter), C6 (threat model note). (A5 and the C1 residual: fixed on `fix/s10-xrec-flow`.)

Hardware only: C5 (`ctrl_more` streaks), C2 / A8 UI part (headset association models with a changed IO capability),
C18 (NOR-UID address), A10 (RF/entropy ordering), A11 (BTC stack high-water mark), plus step 8's Exp 5 (E1-E8).
