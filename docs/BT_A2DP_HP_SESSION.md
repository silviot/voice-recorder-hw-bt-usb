# BT headphones mode: the session, UI, bench and scenario S (implementation record)

Branch `feat/bt-a2dp`, on top of the integrated audio core (H0/H1, `docs/BT_A2DP_H0_H1.md`) and controller work (H2,
merge `08bee01`). Implements the pragmatic subset of milestones H3-H6 of `docs/BT_A2DP_DESIGN.md` (rev 3, kept on
`feat/freertos-bluedroid`) needed for a first silicon test with real headphones. **This file is the live log for this
work: fold it into the design's live log section** (the design file lives in the other worktree). Paths are relative to
`open_fw/`. The silicon recipe is in `docs/BT_HW_RESULTS.md`, "Headphones (BT=1) on silicon: how to test".

## What is in place

| Part | Files | Notes |
|---|---|---|
| Session | `bt_port/bt_hp.h`, `bt_port/bt_hp_bluedroid.c` | Design §6: states OFF/UP/CONFIG/READY/FAULTED, link NONE/CONNECTING/OPEN/STARTED/STALLED. Config per §5.4 (GAP, AVRC CT+TG with the passthrough filter, A2DP source init, SEPs after PROF_STATE). Discovery with the CoD audio filter and names; pick → pairing window (120 s, that address only) → connect; Just Works through patch 0036; AUTH check; A2DP connect; transmit path wired in BTC context; media control one-at-a-time with one START retry; demand start; idle suspend 20 s; remote SUSPEND / unasked START; reconnect backoff 0/3/10/30/60 s then every 60 s; credit stall → 0406 → reconnect, escalation after 8 s; AVRCP keys and absolute volume (capabilities, register, re-register after CHANGED, SetAbsoluteVolume); teardown. |
| Shared platform | `bt_port/bt_xrec_bluedroid.h` (`bt_xrec_plat_set_mode`), `bt_xrec_fw.c`, `bt_store.c` (`bt_store_reset`) | Both modes use phone mode's platform seam: bring-up/teardown jobs, the 2 s hard-stop deadline, hard path, entropy + ROM key staging, controller notes, bond store. The mode picks `bt_bonds.bin` or `bt_hp_bonds.bin`, resets the RAM store on a mode change (§6.4), and adds the profile teardown (AVRC + A2DP deinit, ≤ 600 ms) to the down job. `bt_shutdown()` stops either mode; each mode refuses to start while the other runs. |
| Settings file | `bt_xrec_fw.c` (`bt_hp_plat_cfg_*`) | `SD:/bt_hp_cfg.bin` (64 B): output mode, `bt_vol`, last headset address + name. |
| Bluedroid patches | 0036-0039 (`third_party/bluedroid_patches/README.md`) | 0036 Just Works hook (design patch D); 0037/0038 event/message copy sizes (ASan over-reads found by scenario S); 0039 local AV close on deregister (design patch E). |
| Transmit path | `hp_tx.c` (`nocp_gap_max_ms`), `hp_tx_bd.c` (`hp_tx_bd_close_acl`) | Stats for the bench; the stall's 0406. |
| Output / player | `src/audio_out.c`, `src/player.c` | Detach while a file plays drops the headset route; player: PA off and jack edges ignored on the headset route, AVRCP keys past the screen-off gate, headset events pause with the snapshot rewind, `bt_vol` volume (gain unity with absolute volume), toasts. |
| UI | `include/openfw_ui.h`, `src/ui/ui_screens.c`, `src/main.c` | `UI_SCR_LIST`: a generic list on the Settings layout. Bluetooth tile → `Phone (sync)` / `Headphones`; headphones screen; Settings row `Audio output` (Speaker/Headphones/Both) before About. English, `/* TODO(i18n) */`. BT=0 image unchanged (FIRM CRC-16 0xC786). |
| Bench | `bt_port/bt_bench.c/.h`, `bt_bench_fw.c`, `src/main.c` | `mode=hp`: stages scan / connect / tone / player with per-second stats; `hp_tone_hz=0` alternates 440/880 Hz; player stage defaults to the first recording. |
| Host scenario S | `tests/bt_host/test_headphones.c` (`test_bt_host_hp hps`, in `make test-bt-host`) | Fake headset: L2CAP acceptor, SDP server (A2DP sink, AVRCP CT and TG records), AVDTP acceptor (SBC sink, delay report), media receiver (RTP/SBC checks, vendored SBC decoder, pacing), minimal AVCTP/AVRCP. |

## Host results (`test_bt_host_hp hps`, ASan/UBSan, 90 checks, 0 failures)

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
   Home resumes the last file). PLAY/PAUSE are state-aware.
10. **Idle:** suspend after 20 s without real audio; the 10 min idle disconnect is not implemented.
11. **Player:** writes stereo (mono pass-through not used yet); the volume overlay keeps its HP/SPK label.
12. **Headphones mode starts when the Headphones screen opens** (and reconnects then), not at boot.
13. **Known leak, frozen-peer teardown only:** AVDTP's stream and signalling blocks of the dead link keep their
    fixed_queues (16 heap blocks, ~1.7 KB on the host) until reboot; a clean cycle afterwards adds nothing. A
    responsive headset does not leak.
14. **Bench:** the tone goes through `aout` (Audio output forced to Headphones for the run), -9.5 dBFS; stats every
    second for 15 s, then every 15 s (the 8 KiB log capture); the player stage plays one file once.

## Not tested / open

- Nothing on silicon. ROM behaviour of `0401`/`0405` as master, LM states, role switch / packet types / slots, real
  NOCP latency, CPU of SBC from XIP, heap under streaming: the recipe's stages answer these.
- Not on the host: a 44.1 kHz-only sink (SEID 1 fallback), bitpool-limited peers, page-scan variants, the player and
  UI integration (no `test_player` BT=1 variant yet), wasm rendering of `UI_SCR_LIST`, TSan for scenario S, soak.
- Design items still open: patches B/C, `bt_ctrl_rnr_pending()`, AVRCP scrub, Home PLAY, idle disconnect, mono
  pass-through, "BT" volume label, LRU of 2 headset bonds, REC → disconnect fallback (Exp 4h).
- The controller branch's H2 findings are still only in `docs/BT_CTRL_OPCODE_TABLE.md` §7 and
  `docs/BT_ROM_HCI_MAP.md` §6: fold them into the design's live log with this file.
