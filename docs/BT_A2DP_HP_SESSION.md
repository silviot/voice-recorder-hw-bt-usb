# BT headphones mode: the session, UI, bench and scenario S (implementation record)

Branch `feat/bt-a2dp`, on top of the integrated audio core (H0/H1, `docs/BT_A2DP_H0_H1.md`) and controller work (H2,
merge `08bee01`). Implements the pragmatic subset of milestones H3-H6 of `docs/BT_A2DP_DESIGN.md` (rev 3, kept on
`feat/freertos-bluedroid`) needed for a first silicon test with real headphones. **This file is the live log for this
work: fold it into the design's live log section** (the design file lives in the other worktree). Paths are relative to
`open_fw/`. The silicon recipe is in `docs/BT_HW_RESULTS.md`, "Headphones (BT=1) on silicon: how to test".

## What is in place

| Part | Files | Notes |
|---|---|---|
| Session | `bt_port/bt_hp.h`, `bt_port/bt_hp_bluedroid.c` | Design §6: states OFF/UP/CONFIG/READY/FAULTED, link NONE/CONNECTING/OPEN/STARTED/STALLED. Config per §5.4 (GAP, AVRC CT+TG with the passthrough filter, A2DP source init, SEPs after PROF_STATE). Discovery with the CoD audio filter and names; pick → pairing window (120 s, that address only) → connect; Just Works through patch 0036; AUTH check; A2DP connect; transmit path wired in BTC context; media control one-at-a-time with one START retry; demand start; idle suspend 20 s; remote SUSPEND / unasked START; reconnect backoff 0/3/10/30/60 s then every 60 s; credit stall → 0406 → reconnect, escalation after 8 s (stop + restart of headphones mode, which reconnects); AVRCP keys and absolute volume (capabilities, register, re-register after CHANGED, SetAbsoluteVolume); teardown. |
| Shared platform | `bt_port/bt_xrec_bluedroid.h` (`bt_xrec_plat_set_mode`), `bt_xrec_fw.c`, `bt_store.c` (`bt_store_reset`) | Both modes use phone mode's platform seam: bring-up/teardown jobs, the 2 s hard-stop deadline, hard path, entropy + ROM key staging, controller notes, bond store. The mode picks `bt_bonds.bin` or `bt_hp_bonds.bin`, resets the RAM store on a mode change (§6.4), and adds the profile teardown (AVRC + A2DP deinit, ≤ 600 ms) to the down job. `bt_shutdown()` stops either mode; each mode refuses to start while the other runs. |
| Settings file | `bt_xrec_fw.c` (`bt_hp_plat_cfg_*`) | `SD:/bt_hp_cfg.bin` (64 B): output mode, `bt_vol`, last headset address + name. |
| Bluedroid patches | 0036-0039 (`third_party/bluedroid_patches/README.md`) | 0036 Just Works hook (design patch D); 0037/0038 event/message copy sizes (ASan over-reads found by scenario S); 0039 local AV close on deregister (design patch E). |
| Transmit path | `hp_tx.c` (`nocp_gap_max_ms`), `hp_tx_bd.c` (`hp_tx_bd_close_acl`) | Stats for the bench; the stall's 0406. |
| Output / player | `src/audio_out.c`, `src/player.c` | Detach while a file plays drops the headset route; player: PA off and jack edges ignored on the headset route, AVRCP keys past the screen-off gate, headset events pause with the snapshot rewind, `bt_vol` volume (gain unity with absolute volume), toasts. |
| UI | `include/openfw_ui.h`, `src/ui/ui_screens.c`, `src/main.c` | `UI_SCR_LIST`: a generic list on the Settings layout. Bluetooth tile → `Phone (sync)` / `Headphones`; headphones screen; Settings row `Audio output` (Speaker/Headphones/Both) before About. English, `/* TODO(i18n) */`. BT=0 image unchanged (FIRM CRC-16 0xC786). |
| Bench | `bt_port/bt_bench.c/.h`, `bt_bench_fw.c`, `src/main.c` | `mode=hp`: stages scan / connect / tone / player with per-second stats; `hp_tone_hz=0` alternates 440/880 Hz; player stage defaults to the first recording. |
| Host scenario S | `tests/bt_host/test_headphones.c` (`test_bt_host_hp hps`, in `make test-bt-host`) | Fake headset: L2CAP acceptor, SDP server (A2DP sink, AVRCP CT and TG records), AVDTP acceptor (SBC sink, delay report), media receiver (RTP/SBC checks, vendored SBC decoder, pacing), minimal AVCTP/AVRCP. |

## Host results (`test_bt_host_hp hps`, ASan/UBSan, 120 checks, 0 failures; scenario G 20 checks)

After the second review round (table below) S also runs S5b, S5c, S6b and S8b; the two frozen-peer teardowns of the
run (S8b's escalation, S10) leave 31 blocks / 3392 B together (known leak 13, bound 32 blocks / 8 KiB).


- S1 start → READY → stop: 213 ms teardown, heap/tasks/timers back to the baseline.
- S2 discovery: 3 devices, 2 audio-class, the headset named.
- S3 pick + pairing: Just Works accepted once (window + gate), one pairing, A2DP SDP, AVDTP discover / caps / set
  config / open; 48 kHz chosen from SEID 0 (peer 44.1|48, max bitpool 53 → configured ≤ 35); delay report 150.0 ms
  reaches the pacer; `bt_hp_bonds.bin` written; last headset stored.
- S4 tone (16 kHz mono → 48 kHz): 375.5 SBC frames/s (want 375.0), fpp 10, packets ≤ MTU, RTP sequence and
  timestamps contiguous, 0 decode errors, 1 kHz share of the decoded signal 1.000, 0 pads, `tx_lost` 0.
- S5 remote SUSPEND → `REMOTE_SUSPEND` player event; demand START resumes.
- S6 AVRCP: PLAY/PAUSE/FORWARD → player keys; absolute volume: GetCapabilities, registration, INTERIM, CHANGED (90)
  → `bt_vol` 90 and re-registration; vol31 10 → SetAbsoluteVolume 41.
- S7 link drop → `LINK_LOST`, reconnect at once with the stored key (no new pairing).
- S8 NOCP stops while streaming → stall at ~800 ms, `STALLED` event, 0406, reconnect.
- S9 headset lost its key, window closed → Just Works refused by the hook, no link.
- S10 stop while STARTED with a frozen headset (nothing answers, LMP detach never completes): 641 ms, no hard path;
  a second start/READY/stop works (before patch 0039: A2DP deinit timed out and the second start FAULTED).

## Deviations from the design (and why)

1. **No `bt_session_core.c` split (H4).** The headphones session reuses phone mode's platform seam
   (`bt_xrec_plat_*`) and repeats only the ~40-line teardown loop; `bt_xrec_plat_set_mode()` adds the per-mode bond
   file, store reset and profile teardown. Phone mode's code path is unchanged apart from selecting its mode at start
   and refusing to start while headphones mode runs (`test-bt-xrec`, phone scenario P1-P20 green).
2. **Patches B and C are not in.** Headphones mode never enables page or inquiry scan (always non-connectable,
   non-discoverable), so it has no incoming connection to filter and nothing for the CoD name skip to save beyond
   BTA's name pass (each found device is named once after the inquiry). Phone mode does not yet deny bonded
   headsets (design §6.3): a headset paging the recorder in phone mode is refused by phone mode's own pairing policy
   (it cannot bond: Just Works bonds are removed there). `hp_pscan` is parsed and ignored (logged).
3. **0406 on a credit stall goes through `btsnd_hcic_disconnect()`** (Bluedroid's HCI layer: hciT stays the single
   producer for `bt_ctrl`). `BTA_DmCloseACL` is declared by this Bluedroid but not implemented (link error in the
   host build).
4. **`AUDIO_CFG` arrives before `CONNECTED`** (no audio MTU yet), so `hp_tx_set_cfg()` refused it; the session keeps
   the CIE and applies it again at `CONNECTED` (`cfg_ok=1` in the log). Found by S3.
5. **No `bt_ctrl_rnr_pending()`.** A pick waits for `DISC_STATE stopped` and for `bt_ctrl_inquiry_active()` to clear,
   at most 1 s more, then connects.
6. **Settings in their own file** (`bt_hp_cfg.bin`) rather than keys in `bt_hp_bonds.bin`: that file is Bluedroid's
   BTS1 store image, only loaded while the stack runs; the settings screen needs the output mode with BT off.
7. **PCM ring from the heap (16 KiB) first**, the DAC slot buffer only if that fails: with the ring in the DAC's buffer,
   output mode Both cannot run the DAC.
8. **Numeric comparison** (a headset with a display): accepted automatically for the picked device inside its window;
   no prompt.
9. **AVRCP subset:** FAST_FORWARD/REWIND (scrub) are not mapped; FORWARD/BACKWARD map to the player's seek actions (next
   file in Track skip mode, ±seek otherwise, as the arrow taps); keys outside the player are ignored (design: PLAY on
   Home resumes the last file) and dropped when the next file starts (`bt_hp_player_begin`). PLAY and PAUSE presses
   are both toggles and STOP pauses: we register no PLAYBACK_STATUS notification and a paused player keeps the
   stream STARTED until the idle suspend, so one-button headsets guess which of PLAY/PAUSE to send.
10. **Idle:** suspend after 20 s without real audio; the 10 min idle disconnect is not implemented.
11. **Player:** writes stereo (mono pass-through not used yet); the volume overlay keeps its HP/SPK label.
12. **Headphones mode starts when the Headphones screen opens** (and reconnects then), not at boot.
13. **Known leak, frozen-peer teardown only:** AVDTP's stream and signalling blocks of the dead link keep their
    fixed_queues (16 heap blocks, ~1.7 KB on the host) until reboot; a clean cycle afterwards adds nothing. A
    responsive headset does not leak.
14. **Bench:** the tone goes through `aout` (Audio output forced to Headphones for the run), -9.5 dBFS; stats every
    second for 15 s, then every 15 s (the 8 KiB log capture); the player stage plays one file once.

## Review fixes (second review round)

Each host-testable one has a check that failed before the fix (scenario S unless noted; `make test-bt-host`).

| Finding | Fix | Test |
|---|---|---|
| Release `BT=1` images (rows `0401/0402/0405/0408` refused) showed a 0-device search and an endless `CONNECT_FAIL` | `bt_hp_scan` / `bt_hp_connect_addr` fail at once with `BT_HP_MSG_UNSUPPORTED` ("Not in this build"), no reconnect schedule, `ready ... hp_rows=0/1`. Headphones tests need `BT=1 BENCH=1 HP_GATES=1` | scenario G (`test_bt_host hpgated`, rows refused) |
| The headphones stop switched to phone mode before its final bond flush: `bt_hp_bonds.bin` content was written to `bt_bonds.bin` (Bluedroid's disable always dirties the store) | flush first, then switch; `bonds_write` writes the file of the mode whose bonds the RAM store holds (`s_store_mode`), firmware and host platform | S1: no `bt_bonds.bin` after a headphones stop |
| `config_finish` evaluated the M2.3 gate (entropy mutex) inside `bt_xrec_plat_lock()` (a critical section on target) | gate computed outside the lock | S1: the host lock counts gate calls made inside it |
| Our SUSPEND (REC switch, idle) counted and reported as a remote suspend (the ext-codec source acks SUSPEND before it reports the audio state, so `mc_out` was already clear) | `suspend_pending` set under the lock in `mc_issue`, consumed in `a2d_cb` (`audio SUSPEND ... ours=1`) | S5b |
| START failed for good was sticky until the link dropped (play pressed later: silence, frozen position); demand START bypassed the 500 ms retry delay | retry only from `ready_poll` at `start_retry_at`; a failed send or an unacknowledged START counts as an attempt; after `BT_HP_START_COOLDOWN_MS` (2 s) new demand (play press) or a still-waiting unmuted player starts again; STARTED without an armed tick is a final failure (SUSPEND + `START_FAILED`) | S5c (headset rejects START twice) |
| Keys / player events queued while no file played (menu presses, LINK_LOST, suspends) acted on the next file | `bt_hp_player_begin()` drops them; the player calls it when a file starts on the headset route | S6b |
| `BT_HP_KEY_TOGGLE` never produced; PLAY dropped while playing, PAUSE while paused | PLAY/PAUSE → TOGGLE, STOP → PAUSE; the key decision is logged | S6 |
| VOL passthrough changed `bt_vol` by 4 and pushed a player key (double); the player's volume went stale after headset-side changes | passthrough is only a player key; the player follows `bt_hp_vol31()` every loop on the headset route | S6 (the player part has no host test: no BT `test_player` variant) |
| Stall escalation turned headphones mode off (design §4.6/Q7: restart and reconnect) | stop + `bt_hp_start()` again in the same poll (reconnect from backoff step 0), at most once per 5 min (`BT_HP_AUTO_RESTART_MIN_MS`); an explicit stop cancels it | S8b (0406 never completes) |
| Connect timeout / disconnect could not cancel a first connect (no handle yet) | cancel by address (`esp_a2d_source_disconnect`, deprecated upstream, the only form without a handle); until the abandoned connect reports (or 30 s) no new connect starts; a late `CONNECTED` for another target (or after a user disconnect) is closed; `connect_now` never pages while a link exists | not host-tested (needs a page/pairing that stalls > 30 s, which the fake ROM cannot model) |
| AVRCP CT commands used the A2DP handle (invalid before A2DP `CONNECTED` on headsets that open AVCTP first) | the CT connection's own handle (`ct_hdl`) | not host-tested (the fake headset opens AVCTP after AVDTP) |
| Role Change before Connection Complete was lost (linkinfo `role=M` while slave) | kept for the paged/accepted address and applied at Connection Complete | `test-bt-ctrl` `hp_link_view_early_role` |
| `pkt_type` in linkinfo is always 0 (040F REJ) | linkinfo also logs `req_pt` / `allow_rs` (our 0405) and the peer's `edr` / `slots35` (event 0x0B) | `hp_link_view_early_role` |
| Recipe deleted the bond between stages; `hp_pick=first` could pick a phone/PC | recipe split (bond kept), `hp_pick=last` (stored headset); `bt_bench_hp_pick`: AV major class only, headset types first | `test-bt-port` (`test_bt_bench`) |

## Third review round (audit of be8db84 + merge review of `integ/bt-all-local`)

| Finding | Decision / fix | Test |
|---|---|---|
| Merge #1: output mode Both, the speed dial silent (8x+) or at its end: nothing calls `pump_dac()`, the speaker misses the tail (click instead of the fade), plays ~150 ms of stale audio when the dial turns audible, and `scrub_stop` drops the rest | `aout_queued_frames()` pumps the DAC in Both and reports the later of both outputs (headset queue vs tap + DAC queue); the silent branch, `scrub_done` and `scrub_heard` all ask it every loop pass (1 ms wait). The suggested drop of the tap when the dial turns audible is not needed (and would desync): the tap now drains as the headset does | `test_audio_out` "BOTH drain without writes" (before: 12943 of 21135 frames reached the DAC, queued 0 early) |
| Merge #2: a link event and NEXT/PREV (or PLAY/TOGGLE) in one loop pass start the dial / resume on the dead or suspended link | an event that pauses the player (link lost / stalled / remote suspend / START failed, or headphones mode stopped) drops a play/pause or seek action of the same pass (`PLAYER_DROP_RESUME_KEY`) | none (no BT=1 `test_player`) |
| Merge #3 = audit bug 2: Both + absolute-volume headset: the DAC (speaker / jack) at level 31 | `aout_set_volume_split(dac, bt, dig)`; the player gives the DAC its volume and the headset unity only while absolute volume is on (`PLAYER_SET_VOL`). A jack edge on the headset route keeps `bt_vol` instead of loading the jack/speaker volume for one loop | `test_audio_out` split volume |
| Audit bug 1: packets one byte over the MTU | **rejected**: `ESP_A2D_CONNECTION_STATE_EVT`'s `audio_mtu` is the peer MTU less the RTP header **and** the 1-byte SBC header (`btc_av.c` `BTC_AV_AUDIO_MTU_RESERVE`; scenario S logs `mtu 829` for peer MTU 842), the bound `btc_a2d_src_audio_mtu_check` applies | `test_hp_tx`: an MTU that is an exact multiple takes that many frames and no packet exceeds it |
| Audit 2: a refused send paused the tick until a STARTED that a started stream never gets (frozen player, no toast) | no pause: the packet is freed (its real frames stay unheard), the next tick goes on; refusals lasting `stall_ms` raise the stall event (rewind snapshot, the session's stall path) | `test_hp_tx` refusal cases (before: no event after a lasting refusal) |
| Audit 3: ring seqlock instead of a consumer acknowledgement | recorded (design §19.6) | TSan stress (`BT_A2DP_H0_H1.md`) |
| Audit 5a: a refused SUSPEND (REC switch, idle) left the stream STARTED, sending silence | retried up to `BT_HP_SUSPEND_RETRIES` (3) times `BT_HP_SUSPEND_RETRY_MS` (1 s) apart while STARTED and no new audio / demand since the refusal; log `SUSPEND refused retry=.. n=..` | none (the fake headset cannot refuse SUSPEND) |
| Audit 11: tone stage logs `bp=35` while 24 is on air (speech bitpool, 16 kHz source) | per-second `BENCH hp` line adds `bp_eff` (bitpool on air) and `Bps` (SBC payload bytes/s); the `stream` log adds `speech_bp`; recipe: `hp_speech_bp=0` for a throughput run at the max | build only |
| Audit 7: stale "patch E is not in yet" comment | comment updated | - |
| Audit "smaller": tick not stopped before a local SUSPEND | recorded (design §19.6) | - |
| Audit items 1, 4, 6, 8, 9, 10 | implemented, no change | - |

**Deferred** (one line each):
- Held-packet resend after a refused send (audit 2): the stall path now bounds it; a refusal needs a heap
  node-allocation failure or a handle mismatch, watch `lost` in the bench line.
- REC → disconnect after the SUSPEND retries (audit 5a): needs a REC-aware session flag and a reconnect policy for
  the end of the recording; headsets rarely refuse SUSPEND (design Exp 4h).
- Unasked STARTED after the 5 s START no-ack timeout (audit 5b): needs the stack to send STARTED > 5 s late; fix
  when seen on silicon (SUSPEND it after ~500 ms).
- Just Works hook allows when the session is gone (audit 6): harmless while headphones mode is never connectable;
  tightening it touches phone mode's pairing path.
- Patch E backstop (`btc_av` state reset at deinit, new patch 0040) and refusing `bt_hp_start` after `job_rc=5`
  (audit 7): only a headset frozen during a stop reaches it, and 0039 covers the stream stuck closing.
- `mc_issue` clearing both pending flags on a send error: harmless (one command outstanding at a time).

## Integration with the local features (`integ/bt-all-local`: BT + `test/main-combo`)

`test/main-combo` (backlight fade, `.autoplay` folders + position memory, speed dial, screen transitions, idle-key fix,
Settings > Headset button test) merged on top of the BT branch. The BT=0 image stays byte-identical to
`test/main-combo`'s (FIRM CRC-16 0xA3BB): every BT-side hook is a macro that folds to the old call for BT=0.

- **Settings rows** (`include/openfw_ui.h`; row indices are UI-only, nothing persisted is indexed by them):
  0..9 fixed (mic gain .. USB disk), then BT=1 release/bench: 10 Audio output, 11 About, 12 Headset button test
  (`UI_SET_COUNT` 13); BT=0 and BT=1 `HCI_PROBE=1`: 10 About, 11 Headset button test (12). `UI_SET_ROWS` /
  `UI_SET_ROWS_STR` are gone (`UI_SET_COUNT`, `UI_SET_ROW_ABOUT`, `UI_SET_HSPROBE`, optional `UI_SET_ROW_AOUT`).
- **Player**: every speed-dial path writes, flushes, mutes and counts through `aout_*` and `PA_SET`, so Headphones
  and Both work. The silent clock (|speed| >= 8) starts once `aout_queued_frames()` is 0; on the headset route that
  includes the latency term, which drains within the delay estimate after the last real frame, so the clock starts
  ~150-250 ms later than on the DAC. Nothing is written while silent: `hp_tx` sends silence, and after
  `BT_HP_IDLE_SUSPEND_MS` (20 s) of it the session suspends; the next audible grain or 1x audio demands START again.
  The speed dial is a lasting state now (not a hold), so link lost / stalled / remote suspend / START failed and
  "headphones mode stopped" end it at the heard position before pausing, and starting the dial from pause on the
  headset route is refused like PLAY when no stream is ready ("Headphones not connected"). Such an event also drops
  a play/pause or seek key of the same loop pass. In Both, the DAC keeps being fed through `aout_queued_frames()`
  while the dial is silent or ending (third review round).
- **AVRCP keys**: FORWARD / BACKWARD = one fresh RIGHT / LEFT press (`keys.repeat` cleared): with the "Speed" skip
  setting one speed-dial step (1x -> 2x, ...; at another speed toward it), with "Track" (or FORWARD at the end) the
  next / previous file. PLAY/PAUSE toggle = OK (at another speed: back to 1x). STOP (`BT_HP_KEY_PAUSE`) during the
  dial leaves it at the heard position and pauses.
- **Headset button test on BT=1**: the 16 KiB log does not fit `.bss_hi` next to the BT bench / link-view buffers,
  and the heap floor stays. BT=1 images keep the log in the experiment output buffer (`.exp_out`, 0x00858000, idle
  outside a USB experiment run; a probe overwrites an undumped experiment output); BT=0 keeps its `.bss_hi` buffer.
  The probe loop now waits with `OPENFW_TASK_WAIT()` and keeps polling a background headphones session.
- **SysTick**: `lcd_backlight_fade_tick()` runs before `openfw_rtos_tick()`; it only rewrites the PWM3 duty and the
  task side arms a ramp with `s_fade_n` cleared, so the RTOS tick ownership does not change it.
- **Transitions**: `UI_SCR_LIST` maps to the Settings page (depth 1): Home -> Bluetooth pushes forward, Phone mode
  (BT page) forward, back to Home backward; Bluetooth choice -> Headphones pushes forward through `ui_model_nav`.

## First silicon runs (2026-11-25, laptop BlueZ as the sink; `docs/BT_HW_RESULTS.md` session 7)

- Worked at once: inquiry (13 results) + name pass, `0405` page as master (link in 150 ms, `role=M max_slots=5
  edr=3`), AVDTP 48 kHz SBC bp35 (on air `bp_eff=24` for the 16 kHz tone), ~375 frames/s, ~49 pkts/s, ~23 kB/s,
  lost/drop/pads/allocf 0, AVRCP CT/TG with absolute volume, teardown 217 ms idle / 647-658 ms with the link up.
- Pairing as initiator fell back to legacy PIN: the ROM keeps one "remote page 1" slot per link, and Bluedroid's
  page-2 `041C` overwrote the host-SSP bit its initiator decision tests (`docs/BT_ROM_HCI_MAP.md` §12). **Patch 0040**:
  read ext features up to page 1 → SSP Just Works. **Patch 0041**: our initiator IO capability reply asks for general
  bonding (BlueZ dropped the no-bonding key at disconnect). Host: the fake ROM models the slot and the decision
  (`fake_peer_cfg_t.rom_auth_decision`); `test_bt_host_hp hps` S3 pages a `max_page` 2 headset and checks SSP, one
  page-1 read, and `auth_req` with the general-bonding bit; `test_bt_host phone` P2 checks the responder reply is
  unchanged (`auth_req` 0x00, no PIN request). Without 0040, S3 gets `E464` and the connect fails; without 0041,
  `auth_req` is 0x01.
- Bench additions: HCI trace notes in `HP_GATES` bench images (`BT_CTRL_HCI_TRACE`, 64 notes; `test-bt-ctrl` builds a
  third variant with it), `hp_pin=1` (answer a legacy PIN with 0000, diagnostic), the player stage's time limit (the
  player blocked it: a 120 min recording outlived `secs`; the player's UI tick now drives the bench and injects BACK
  through `keys_inject()`, called only in bench images; the default image is unchanged).
- Open: no laptop recording of the stream yet; PipeWire showed the bluez card in `audio-gateway` with no A2DP source
  and the transport idle during the player stage (unexplained). Not yet with real headphones.

## Not tested / open

- On silicon so far only with a laptop as the sink (above). Real headphones: role switch / packet types / slots,
  NOCP latency, CPU of SBC from XIP, heap under long streaming: the recipe's stages answer these.
- Not on the host: the speed dial on the headset route (silent clock, link loss mid-dial), a 44.1 kHz-only sink (SEID 1 fallback), bitpool-limited peers, page-scan variants, the player and
  UI integration (no `test_player` BT=1 variant yet), wasm rendering of `UI_SCR_LIST`, TSan for scenario S, soak.
- Design items still open: patches B/C, `bt_ctrl_rnr_pending()`, AVRCP scrub, Home PLAY, idle disconnect, mono
  pass-through, "BT" volume label, LRU of 2 headset bonds, REC → disconnect fallback (Exp 4h; a refused SUSPEND is
  now retried 3 times), the third round's deferred list above.
- The controller branch's H2 findings are still only in `docs/BT_CTRL_OPCODE_TABLE.md` §7 and
  `docs/BT_ROM_HCI_MAP.md` §6: fold them into the design's live log with this file.
