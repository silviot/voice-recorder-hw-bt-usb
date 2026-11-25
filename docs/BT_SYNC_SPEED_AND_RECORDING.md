# BT sync: speed, and syncing while recording

Status: **design approved (decisions in section 5).** S2 tooling implemented on `feat/bt-sync-measure`: F0 counters
(bench images), `+XREC:DBG`, `tools/bt_sync.py --bench-load`, `tools/rf_audio_check.py`; the M2/M3 run recipe is in
`docs/BT_HW_RESULTS.md` ("Sync during recording: M2/M3 measurement recipe"). No silicon data yet.
Based on three read-only research passes (throughput, firmware concurrency, app), firmware `feat/bt-step9` @ b3fa6c6, app `feat/recorder-sync`.

User requirement: *sync is extremely slow; when a phone is paired, sync should happen during recording, but must never, ever affect recording quality.*

The design takes the second half literally. Background sync while recording is **built but off by default**. It is switched on only after a silicon measurement gate (section 3) shows no measurable difference from a radio-never-on baseline. If the measurement shows an effect, a fallback ladder (section 2.6) gives up sync speed first and never recording quality.

---

## 1. Summary

### 1.1 Why sync is slow

| Fact | Number | Source |
|---|---|---|
| Observed Pixel rate | 8–10 MB in ~150 s = **53–67 KB/s** | user runs |
| App window per GETS | **3840 B** (6 frames x 640 B), strict stop-and-wait | `XrecProtocol.kt:22,25`, `XrecClient.kt:194-215` |
| Round trips for 10 MB | **~2,600** | |
| Time per round trip | ~57 ms = ~25–49 ms air + **~10–30 ms fixed turnaround** | model, section 2 of throughput report |
| Share of the 150 s that is idle turnaround | **~40–60 %** (estimate) | |
| Link ceiling, basic rate (DH5) | 90 KB/s | |
| Link ceiling, EDR 2 Mb/s (2-DH5) | ~160–180 KB/s (no 3 Mb/s in the ROM) | LMP features page 0 |

**Root cause:** the client's stop-and-wait. The 3840 B window was inherited from stock firmware ("safe within K=7 credits"). open_fw does not need it: it accepts any GETS `total` and back-pressures through its TX ring. Each turnaround also contains a per-GETS `exfat_find_path` directory scan (`xrec_proto.c:788`).

Not limiting: CRC (~1–2 % CPU), frame overhead (1.8 %), the app's I/O, RFCOMM credits.

Unknown that sets the final ceiling: **is the link actually sending EDR (2-DHx) packets?** The stock `bt-rfcomm-turbo` reached 92–96 KB/s on the same ROM, above the basic-rate ceiling, which hints yes. This is measured in M1 (section 3).

A second, smaller cost on every sync start: `STAT` scans the whole exFAT allocation bitmap (~240 sector reads, 100–250 ms on a 32–64 GB card).

### 1.2 Expected speed after each change (10 MB file)

| Step | Change | If basic rate only | If EDR on air |
|---|---|---|---|
| today | 3840 B stop-and-wait | 53–67 KB/s, ~150 s | same |
| A1 | App: one 64 KiB window per GETS | **~85 KB/s, ~2 min** | **~140–160 KB/s, ~65–70 s** |
| F2 | FW: deeper TX buffering (2 → 8 KiB ring) | +0–10 % | +0–10 % |
| F3 | FW: cache file entry across GETS, aligned read-ahead | +few % (mostly frees app-task time) | same |
| F5 | FW+app: negotiated frames up to `mtu-12` (978 B) | +3–8 % | +3–8 % |
| — | Ceiling | **90 KB/s** | **~160–180 KB/s** |

A1 alone gives most of the gain, needs **no firmware change** and works with every open_fw build. While the recorder records, sync is deliberately paced (section 2.4). Recording produces 32 KB/s mono and 64 KB/s stereo, so a paced sync still keeps up. Today's ~60 KB/s would barely keep up with stereo.

---

## 2. Background phone sync (sync while recording)

### 2.1 Architecture

Today the radio is up only on the modal BT screen. Leaving it stops the radio, and also stops any `+XREC:RECSTART` recording. **Background phone sync** moves a restricted phone session out of the modal screen, following the precedent of headphones mode, which already runs from the Home loop (`main.c:3120-3152`).

```
Home / REC screens (app task, prio 1)
  loop: recorder_poll()            <- always first
        bg_phone_poll()            <- XREC session, budgeted, recorder-gated
        recorder_poll()            <- again, after any XREC work
Radio: brought up on Home only when
  setting "Sync in background" = ON  AND  a bonded phone exists
  AND  HP mode is off  AND  battery >= BT threshold  AND  not recording
Never started, stopped, reset or recovered while recording.
```

Session rules:
- **Bonded-only.** Connectable (page scan), never discoverable, unbonded peers rejected. Pairing stays on the modal BT screen. This also avoids the entropy collection, which needs the ADC.
- **No idle link.** Sniff is unavailable (`0803` is REJ), so a connected link polls continuously. The phone connects, moves the finished files, and disconnects.
- **Who starts a sync:**
  - (a) The phone polls while the recorder records: every 2 min, fixed, for up to ~2 h.
  - (b) After REC stops, the device pages the bonded phone once, the same way it already auto-connects the headset (35b4397). Android CDM presence then wakes the app even if its process is dead. **The device never pages while recording.**
- **One controller owner.** Background phone sync and HP mode are exclusive. Starting HP mode (output=BT, auto-connect on PLAY) tears the background session down. That teardown is refused or deferred while recording.
- USB MSC, power-off and low battery stop the session as today. **While recording, the stop is deferred until REC ends**, unless M4 shows the teardown is audio-safe.

### 2.2 Firmware changes

All BT code stays behind the BT build flag. **The default (non-BT) image is byte-identical.**

| ID | Change | Why |
|---|---|---|
| F0 | Measurement counters in the BT bench image: `audio_get_overruns()`, sample accounting (DMA frames x 2048 vs `us_timer`), max app-loop iteration, per-task CPU line in phone mode. Exported in a debug reply. | gate data |
| F1 | **Recorder-first scheduling:** `recorder_poll()` before and after XREC work. GETS pump runs only when the audio ring is empty (`g_wr_idx == g_rd_idx`), with a **time budget of 5 ms per pass** while recording. | bounds CPU and SD contention |
| F1b | While recording, a token-bucket rate cap on GETS (initial **48 KB/s**, tuned from M3). | bounds RF duty and CPU |
| F1c | **Mono ring 4 x 128 ms → 8 x 128 ms**, same 32 KiB of RAM. Tolerance for app-task absence goes from 384 ms to **896 ms**. Stereo stays at 384 ms. | margin |
| F1d | BT-mode loop sleeps one tick while recording, instead of spinning. | power |
| F4 | **STAT free-space cache:** computed at mount, adjusted on allocate/free. Removes the 100–250 ms bitmap scan. | largest XREC stall |
| F6 | While recording: `DEL`, `PUT` (sidecar) and bond-store flush answer `+XREC:ERR,LATER,<ms>` or are deferred. `RTC` set is ignored. | second exFAT writer, SD busy |
| F7 | **Active-file protection:** GETS of `recorder_get_filename()` answers `+XREC:ERR,BUSY`. STAT gains numeric fields `rec_idx` (NNNN of the active file, 0 = none) and `pace_ms`. | correctness |
| F8 | **Fault while recording:** no hard-path reboot and no bt_ctrl recovery ladder. Silence RW-BT, mask IRQ 26, keep recording, reboot or recover after REC stops. | a BT fault must not end a recording |
| F9 | Background session: bring-up on Home only, the start/stop rules in 2.1, Home/REC BT icon, Settings toggle "Sync in background" (English, `TODO(i18n)`), **default OFF**. | the feature |
| F10 | Device pages the bonded phone once after REC stops (if background sync is on). | wakes the app |
| F2 | `BT_XREC_TX_RING` 2048 → 8192, `SPP_TX_BUF` 2048 → 4096–8192 (~8–12 KB heap; free-heap minimum was 44–62 KB). Cap at 3–4 frames per `xrec_poll`. | keeps air busy across recorder writes |
| F3 | Cache the exFAT entry across GETS of the same name (invalidated on DEL/PUT; the active file is never cached). Sector-aligned 4–8 KiB CMD18 read-ahead. | ~12 → 1–2 SD commands per 4 KiB |
| F5 | PONG flag `gmax=<n>`; GETS `chunk` allowed up to `mtu-12`. | +3–8 % |

### 2.3 Protocol changes (all backward compatible)

| Item | New firmware + old app | Old firmware + new app |
|---|---|---|
| Large GETS `total` | already accepted | already accepted (any open_fw); stock firmware (`pen133` in PONG) keeps 3840 |
| STAT `rec_idx`, `pace_ms` (fields 9, 10, numeric) | the old parser ignores extra numeric fields | field 9 missing → app keeps today's "skip everything while recording" |
| `ERR,BUSY` on GETS of the active file | already treated as "file changed, skip" | n/a |
| `ERR,LATER,<ms>` | old app sees OTHER_DEVICE_ERROR → file FAILED for this sync, no harm | never sent |
| PONG `gmax=<n>` | old app asks for 640 | no flag → app keeps 640 |

`docs/XREC_PROTOCOL.md` gets updated: the "recommended 3840" text, the new STAT fields, LATER, and gmax. The stale "K=7 credits" comment in `tools/bt_sync.py:37` is fixed too.

### 2.4 App changes (`cassini-android-recorder`)

| ID | Change |
|---|---|
| A1 | `XrecClient` gets a `getsWindow` parameter. **64 KiB when not recording, 16 KiB while recording**, 3840 for stock firmware. |
| A2 | Error recovery: after a failure, re-issue GETS at the last good offset immediately (this cancels the old stream) and skip stale frames, with a stale-byte budget of ~256 KiB. This is valid only because downloaded files are immutable, which F7 guarantees. Until A2 lands, keep window ≤ rate x (5 s − 0.3 s) so `drain()` still works (64 KiB qualifies). |
| A3 | Metrics hook: GETS → first frame, frame gaps, bytes/s per window. One log line per file. |
| A4 | Parse STAT `rec_idx` / `pace_ms`; parse the first 8 fields as numbers and keep extras raw. `SyncPlanner` skips **only** the active file (new `SkipReason.ACTIVE`) when the firmware names it; otherwise it keeps today's rule. Send LS before STAT. No per-file STAT when the firmware names the active file. No RTC set while recording. |
| A5 | `ERR,LATER,<ms>` → new `XrecFailure.LATER`: wait min(ms, 30 s), retry from the same offset without counting a failure, cap ~2 min per file, then "deferred" (not FAILED). `pace_ms` adds a wait between windows. |
| A6 | Triggers: fixed 2-min poll while the recorder records (no idle doubling). CDM presence (`CompanionDeviceService`, `startObservingDevicePresence`) so the device's page after REC (F10) starts a sync. Disconnect as soon as sync is done. No uploads while recording unless the firmware signals it (pace field present). |
| A7 | Optional: 64 KiB `BufferedOutputStream` on the `.part` file, flushed before `fd.sync()` and before PartialDownload is stored. |

Tests (core, app-logic, app) as listed in the app report, section 4: one GETS per window, small MTU, stale frames after a retry, STAT 8/10/non-numeric fields, LATER with a fake clock, active-file skip, LS-before-STAT, no clock set while recording, backoff while recording. Update `RecorderSyncTest.kt:205`, which asserts `GETS,5000,3840,640`.

### 2.5 Recording-quality guarantees

| Risk | Mechanism | How it is eliminated or bounded | Verified by |
|---|---|---|---|
| **Radio start/stop/reset during REC.** RF calibration + HCI Reset busy-wait 0.2–0.5 s, more than the 384 ms ring. | timing | **Eliminated by rule:** the radio comes up only on Home when not recording. No start, stop, reset or recovery while recording. Lowbatt/USB/HP transitions are deferred to REC end. | code review + E-series regression |
| **BT fault during REC** (fault 6 was seen after a large GETS) | reboot ends the recording | **Eliminated:** F8, keep recording and drop BT. | fault injection on bench |
| **CPU:** BT tasks (prio 4–8) preempt the recorder (prio 1) | starvation | Bounded: rate cap F1b + no idle link. Measured idle% and recorder-loop gap must stay ≤ 25 % of ring tolerance (≤ 96 ms stereo, ≤ 224 ms mono). | M3 |
| **SD contention:** GETS reads, STAT, DEL, PUT in the same task | delay of `recorder_poll` | No interleaving is possible (single task, no mutex needed). Bounded by: GETS only when the ring is empty, 5 ms budget per pass, STAT cached, writers deferred (F1, F4, F6). Worst XREC pass ≤ 10 ms, against a 128 ms slot. | M3 (max loop gap) |
| **SD busy timeout** (up to 500 ms, exists without BT) | timing | Not BT-specific; F1c raises mono tolerance to 896 ms. F6 means BT adds no extra writes during REC. | M3 counters |
| **Active file** | wrong / duplicate notes | **Eliminated:** firmware refuses GETS (BUSY) and names the file (`rec_idx`); the app skips it; LS before STAT. | host tests + M5 |
| **RF/digital coupling into the mic** (1600 Hz slot tick, 800/400/267 Hz packet rhythms, 40 Hz poll, page scan 0.78 Hz, VBAT sag, the phone's own TX) | electrical | **Unmeasured — the main risk.** Bounded by: no idle link, no discoverability, bursty paced transfers, rate cap. Release gated on M2. Fallback ladder 2.6. | M2 |
| **Silent sample loss** from long ROM PRIMASK sections (masks the audio DMA ISR) | timing | Unmeasured. Detected by the tone phase-continuity test and sample accounting. | M2(e), M3 |
| **Power** | battery | Measured; affects defaults, not audio. | M6 |

### 2.6 Fallback ladder (chosen by measurement, applied per firmware build)

| Level | During REC | Used if |
|---|---|---|
| L0 | Full background sync, paced (cap from M3) | everything passes |
| L1 | Same, lower cap / smaller window (e.g. 24 KB/s mono only, no sync during stereo) | M2 passes only at lower duty |
| L2 | Radio stays up (page scan only), **no ACL during REC**: the phone does not connect while recording. Sync starts the instant REC stops; no calibration is needed because the radio is already up. | slot tick and page scan are clean, any link or transfer is not |
| L3 | **Radio off during REC.** Turning it off at REC start is allowed only if M4 shows the teardown is audio-safe; otherwise REC start waits for teardown first (open question Q3). Sync after REC as today, but faster (A1). | the controller being up at all (slot tick) has any effect |

If even L3 cannot be made safe, background sync is not shipped and only the speed improvements (A1, F2–F5) land.

---

## 3. Silicon measurement protocol (must pass before default-on)

Roles:
- **The agent** builds and flashes the images over USB, drives the laptop (`tools/bt_sync.py`, `bluetoothctl`), pulls WAVs and counters over USB MSC / XREC, runs the analysis, and writes results into `docs/BT_HW_RESULTS.md`.
- **The user** does the physical steps: box, speaker, REC switch, the Pixel, and listening. Before each step the agent says what happens, how long it takes, and what may and may not be touched.

Analysis script (new): `tools/rf_audio_check.py` (numpy/scipy). It compares every capture against the C0 baseline with the same gain/profile/channels.

### M1 — Throughput and EDR (no recording; ~30 min; user: Pixel developer settings)
1. User enables the Pixel's HCI snoop log. Agent runs a sync with a debug app build at window 3840, then 64 KiB, on the same 8 MB file.
2. Agent pulls the snoop (`adb bugreport`) and splits each burst into device turnaround, data span and phone turnaround with `tshark`. It also reads the remote features and the RFCOMM MTU from the device log.
3. Laptop sweep: `bt_sync.py --burst-size 3840 / 65536 / 1048576`.
- **Result:** EDR yes/no (3912 B in ≤ 30 ms, or sustained > 95 KB/s = EDR), plus the real turnaround.
- **Pass for A1 release:** ≥ 80 KB/s on the Pixel at 64 KiB, byte-identical files, 0 retries over 5 files.

### M2 — Audio "no effect" (the gate; ~3 h incl. repeats; user: box, speaker, REC switch)
Setup: device in a foam box, fixed position. Laptop peer at 1 m; Pixel at 5 cm and at 1 m for C3p. Gain 20 (user default) and 31 (max); mono and stereo. DSP BYPASS for the metrics, then Voice Clarity for the listening check. Signals: S = silence; T = 1 kHz tone at about −20 dBFS from an external speaker.

Conditions, 60 s each, randomized order, x3 repeats:

| ID | Condition |
|---|---|
| C0 | fresh boot, radio never on (baseline) |
| C0b | after one BT session, back on Home, radio off |
| C1 | radio up, page scan only, no link (background session idle) |
| C2 | bonded link up, no traffic (worst case; the design avoids it) |
| C3 | continuous GETS of old files from the laptop, at the F1b cap and at full speed |
| C3p | Pixel syncing during REC (needs A1/A4) |
| C5 | headphones link up, stream suspended (today's shipped behaviour, for reference) |
| Cm | C0 and C2 with the mic covered (separates acoustic from electrical pickup) |

Metrics vs C0. **All must pass at both gains and both channel modes:**

| Metric | Pass |
|---|---|
| (a) noise floor, 20 Hz–8 kHz unweighted and A-weighted | within C0 ± 0.5 dB |
| (b) Welch PSD, 1 Hz bins; spur prominence at k x 1600, 800, 400, 266.7, 40 Hz | no bin > C0's same bin + 3 dB (C0 repeat spread as reference) |
| (c) envelope modulation (10 ms RMS → FFT; fold at 1.28 s): 0.78, 2.3, 7.5 Hz | indistinguishable from C0 |
| (d) SNR and THD+N with T | within 0.5 dB |
| (e) continuity: overruns, zero runs ≥ 2048 samples, tone phase steps > 0.05 rad, length = 16000·T ± 1 frame | **all exactly 0 / exact** |
| (f) blind ABX, C0 vs C3 at gain 31 (user, headphones) | no better than chance (≤ 12/20) |

A single failure in (e) fails the condition, whatever the other metrics show. Results select the ladder level in 2.6.

### M3 — CPU and timing margin (~1.5 h, mostly unattended; user: start/stop REC on request)
F0 image, 1 h REC + continuous paced GETS (`+XREC:RECSTART`), then a STAT/DEL/LS storm during REC, at each candidate cap (24 / 48 / 64 / 96 KB/s).
- **Pass:** overruns 0; sample accounting exact; max recorder-loop gap ≤ 25 % of ring tolerance; idle ≥ 20 %; no `SDW_MDTO`. The highest passing cap, halved, becomes the F1b default.

### M4 — Transitions (~30 min)
Measure the audio effect of: background-session teardown, HP-mode takeover refusal, USB plug, lowbatt threshold — all while recording, with the T tone.
- **Pass:** (e) all zero. Otherwise the transition stays deferred to REC end (the default design).

### M5 — End to end (~2.5 h; user: Pixel nearby, normal use)
Background image, setting ON: 2 h mono recording with the Pixel syncing finished files by poll and by CDM wake. Then repeat M2 C1/C3p on this image, and E-series regressions E4–E6.
- **Pass:** M2 criteria; all finished files synced byte-identical; active file never downloaded; BT fault injection keeps recording.

### M6 — Power (~1 h)
Home idle current (or the `BT vbat` trend): radio off vs page scan vs connected. Recorded, not a gate. It feeds Q1.

**Default-on rule:** "Sync in background" may default ON only after M2 (at the chosen ladder level), M3, M4 and M5 all pass on the release image. Any later firmware change touching BT, recorder or SD reruns M2(e) + M3 as a regression gate.

---

## 4. Staged implementation plan

Branch: a feature branch off `feat/bt-step9`, committed and pushed at milestones (never main). Each stage is followed by the usual Codex/agy reviews.

| Stage | Content | Size (rough) | Gate |
|---|---|---|---|
| **S1 — fast sync (app only)** | A1 (64 KiB, 3840 for stock), A3 metrics, tests | app ~150 LOC + tests | M1; host tests; 5 Pixel syncs byte-identical. **Ships independently: works with every open_fw build.** |
| **S2 — measurement image + baseline** | F0 counters, `tools/rf_audio_check.py`, run M2 C0/C0b/C1/C2/C3/C5 with today's BT-mode `RECSTART` | fw ~150 LOC (bench only), py ~300 LOC | M2 data → choose ladder level. **Decision point with the user.** If L3 or "no-go", skip to S6. |
| **S3 — firmware hardening for concurrent REC** | F1, F1b, F1c, F1d, F4, F6, F7, F8 + host tests; XREC_PROTOCOL.md | fw ~600–800 LOC | G1: host tests; M3; STAT/DEL/LS storm during REC with 0 overruns. Also improves today's in-BT-mode RECSTART. |
| **S4 — app: sync while recording** | A2, A4, A5, A6 (poll trigger), A7, tests | app ~600 LOC + tests | G2: Pixel sync during a 2 h recording in BT mode; M2 C3p passes; overruns 0. |
| **S5 — background session** | F9, F10, A6 (CDM presence), UI icon + setting (default OFF) | fw ~500–800 LOC, app ~250 LOC | G3: M4 + M5 + M6. Default ON only after this. |
| **S6 — throughput polish** | F2, F3, F5 (+ app `gmax`), optional page-scan interval via 0C1C | fw ~300 LOC, app ~50 LOC | M1 rerun, M3 rerun at the new rates |

Backward compatibility:
- New app + old open_fw: 64 KiB window works; no STAT fields → today's all-skip-while-recording rule; no LATER/gmax.
- New app + stock firmware: detected by `pen133` in PONG → keeps 3840/640.
- Old app + new firmware: unchanged behaviour (it sends 3840/640 GETS; extra STAT fields are ignored; BUSY/LATER degrade to skip/failed for that sync).
- Bonds, pairing flow and the modal BT screen are unchanged. The default non-BT image is byte-identical.

---

## 5. Decisions (user, 2026-11-25)

1. **Radio up on Home: option (b).** The background session is brought up after a recording ends and stays up,
   including through the next recording, until N minutes pass with nothing left to sync (N to be set from M6).
   It is not up at boot or on Home without a preceding recording.
2. **Acceptance bar: as proposed** (within the radio-never-on baseline's spread: ±0.5 dB noise floor, no spur > +3 dB,
   zero continuity errors, ABX at chance; gain 20 and 31, mono and stereo; Pixel at 5 cm is information).
3. (Only relevant if M2 selects L3; not asked yet.)
4. **Headphones win:** output=BT (auto-connect on PLAY) takes the controller from the background phone session;
   never during a recording.
5. **Mono only at first:** transfers during stereo recordings wait until REC stops; stereo may be enabled later if
   M3 shows margin.

The original questions follow for reference.

## 5b. Original open questions

1. **When is the radio up on Home?** (a) Always, when background sync is ON and a phone is bonded (simplest; costs idle power, quantified in M6); (b) brought up on Home only after a recording ends, and kept up (including through the next recording) until N minutes pass with nothing left to sync; (c) only after REC stops, until the phone has synced (this is then "sync between recordings", not during). Recommendation: (a), revisited after M6.
2. **Acceptance strictness for M2.** The proposed thresholds are "within measurement spread of the radio-never-on baseline" (± 0.5 dB, no spur > +3 dB, zero continuity errors). Is that your meaning of "never affect", or do you want a stricter bar (e.g. also tested at gain 31 in stereo with the Pixel at 5 cm as the pass condition, not just as information)?
3. **If only L3 is safe (radio off during REC):** may REC start be delayed by the radio teardown (~2 s) when the background session is up, or must REC always start instantly (then BT stays up for that recording but transfers nothing, i.e. L2, which requires C1 to pass)?
4. **Headphones vs background phone sync:** when output=BT wants the controller (auto-connect on PLAY), should it silently take over from the background phone session (proposed), or should background sync keep priority while a phone is in range?
5. **Stereo:** during stereo recording the ring tolerance stays at 384 ms. Allow background transfers during stereo REC at all, or restrict sync-during-recording to mono?
