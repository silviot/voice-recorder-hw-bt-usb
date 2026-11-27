# `+XREC` v5 Bluetooth protocol (open_fw `Cassini_XX`, Smartlink SL6801)

This document specifies the `+XREC v5` wire protocol over Classic Bluetooth BR/EDR SPP (RFCOMM channel 1), used by
the mobile app (`cassini-android`) and the desktop client (`tools/bt_sync.py`).

**Primary implementation: open_fw, BT=1 (Bluedroid) phone mode.** Protocol state machine `open_fw/src/xrec_proto.c`
(`include/openfw_xrec.h`), SPP session `open_fw/bt_port/bt_xrec_bluedroid.c`, device glue `open_fw/bt_port/bt_xrec_fw.c`.
Host tests: `make -C open_fw test-host` (`tests/test_core.c`, `tests/test_xrec_rec.c`), `test-bt-xrec`, `test-bt-host`.
Everything below describes open_fw unless a paragraph says otherwise. The legacy stock-firmware patch (`fw_mod`)
differs in several places: see [section 8](#8-legacy-stock-firmware-patch-fw_mod-differences).
The earlier clean-room host (`src/bt_hci.c` + `src/bt_spp.c`) was deleted in plan step 9
(`docs/BLUETOOTH_BLUEDROID_PLAN.md`).

---

## 1. Transport

* Classic BR/EDR only (the SL6801 RW-BT silicon has no BLE).
* **Device name `Cassini_XX`**: `XX` = 2 uppercase hex digits of `bd_addr[0]`. Class of Device `0x200404` (Android may
  show a headset icon). IO capability DisplayYesNo.
* **SPP server**: RFCOMM server channel 1, SDP UUID `0x1101` (`00001101-0000-1000-8000-00805F9B34FB`), service name
  `"Cassini"`, security AUTHENTICATE | ENCRYPT | MITM. On Android use the secure
  `createRfcommSocketToServiceRecord(SPP_UUID)`, not the insecure variant.
* **An SPP open requires a MITM-authenticated bond** (SSP numeric comparison). Without one the device closes the link
  at once. On Android call `createBond()` during the device's pairing window first (section 2).
* One SPP session at a time (a second open is dropped). At most 4 bonds are kept on the device.
* **No HFP / AT handshake**: after RFCOMM opens the device sends nothing; the client starts with `+XREC:` lines.
* Device RX ring 2048 B: **never pipeline uploads**. Keep one PUT chunk (<= 652 B on the wire) in flight
  (stop-and-wait). An RX overflow closes the link.
* No idle disconnect and **no authentication expiry**. Authentication is cleared on disconnect, which also aborts a
  staged (not yet committed) PUT upload.

## 2. Device states, pairing window, binding

* The radio is on only while the user is in BT phone mode on the recorder (Home -> BT tile -> ENTER; in
  headphones-capable builds a choice screen follows: "Phone"). The recorder is not connectable on Home, while playing,
  on USB, or while recording from the REC switch. Clients must tolerate connect failures quietly and retry.
* Inside BT mode the device is connectable after bring-up (up to ~12 s). **ENTER opens a 60 s pairing window**: the
  device becomes discoverable and shows the toast `PIN xxxxxx` (the 6-digit XREC PIN, from the DRBG). ENTER again
  with more than 10 s left extends the window and keeps the PIN.
* SSP numeric comparison is auto-accepted by the device inside the window (release default); the phone still shows
  its own pairing dialog. After the first MITM bond the device stops being discoverable. If an accepted SSP pairing
  fails, no more SSP until the user re-enters BT mode.
* Recording while in BT mode only via `+XREC:RECSTART`; leaving BT mode stops it.
* First-time binding (within 60 s of ENTER; ENTER again extends):
  1. User presses ENTER on the recorder's BT screen (window opens, PIN shown).
  2. Client finds `Cassini_XX` and bonds (`createBond()`); the user confirms on the phone.
  3. Client opens the SPP socket, sends `+XREC:PING`, expects `pair=1`.
  4. User types the PIN shown on the recorder; client generates a random 32-byte key and sends
     `+XREC:PAIR,<pin>,<64 lowercase hex>` -> `+XREC:OK,PAIR` (session authenticated). Store the key securely per
     device address.
  5. Later sessions: device in BT mode, client connects, `CHAL` / `AUTH`.
* The device stores the key in **`SD:/cassini.key`**. A new `PAIR` replaces it: other clients' keys become invalid
  (`ERR,AUTH`).

## 3. Framing

* Commands: ASCII lines starting with `+XREC:` and ending in `\r` (`\n` is ignored). Lines not starting with `+XREC:`
  are ignored. Max line 191 characters, else `+XREC:ERR,LINE`.
* Replies end in `\r\n`. Errors: `+XREC:ERR,<REASON>`. Unknown command: `+XREC:ERR,CMD`.
* Binary data appears only in GET/GETS frames (device -> client) and PUT payloads (client -> device).

## 4. Unauthenticated commands

| Command | Reply |
| :--- | :--- |
| `+XREC:PING` | `+XREC:PONG,openfw_v5,v5,bound=<0\|1>,auth=<0\|1>,pair=<0\|1>,rec=1,sync=1,live=1,lst=1` (`pair` = XREC PIN window open; capability flags: `rec=1` = `PUT ... REC,<stem>.cwx`; `sync=1` = `+XREC:SYNC,<pending>[,<eta_s>]`; `live=1` = live streaming `GETS` of active recording up to flushed disk bytes; `lst=1` = `+XREC:LS,T` timestamps. Parse `key=value` fields and ignore unknown keys; firmware without a flag lacks that feature) |
| `+XREC:PAIR,<6 digits>,<64 hex>` | `+XREC:OK,PAIR`, or `ERR,PAIRCLOSED`, `ERR,PIN` (5 failures close the window), `ERR,BADKEY`, `ERR,SAVE` (key not written to the card; old binding kept) |
| `+XREC:CHAL` | `+XREC:CHAL,<32 lowercase hex nonce>` (16 bytes). If the entropy pool is not ready: no reply and the link is closed |
| `+XREC:AUTH,<64 hex>` | HMAC-SHA256(key, nonce bytes): `+XREC:OK,AUTH`, or `ERR,UNBOUND`, `ERR,NOCHAL` (one AUTH per CHAL), `ERR,AUTH` |

## 5. Authenticated commands (else `+XREC:ERR,LOCKED`)

### 5.1 STAT, RTC
* `+XREC:STAT` -> `+XREC:STAT,<bat_pct>,<mV>,<chg 0/1/2>,<vbus 0/1>,<media>,<rec_sec>,<free_mb>,<total_mb>,<rec_idx>,<pace_ms>,<rec_bytes>`.
  `chg`: 0 discharging, 1 charging, 2 full. `media`: **33 (0x21) while recording**, else 16 (0x10).
  `free_mb` comes from a cache kept by the file system (one allocation-bitmap scan per mount): STAT is cheap.
  * Fields 9, 10, and 11 (sync while recording, section 5.5) are appended and numeric. Parse STAT by position and
    read any available prefix of fields (older open_fw sends 8 fields without live sync, or 10 fields without
    `rec_bytes`). Without field 9 (`rec_idx`), skip every download while `media == 33`.
  * `rec_idx`: the `NNNN` of the `REC_NNNN.wav` being recorded, 0 = not recording.
  * `pace_ms`: 0 = no pacing (not recording). Mono recording: the time one 16 KiB GETS window takes at the device's
    rate cap (342 ms at 48000 B/s), i.e. the shortest interval between the **starts** of two windows. The device
    paces the frames itself: send the next GETS as soon as a window is complete, **do not add a pause after it**
    (the device's burst allowance is only 2 KiB, so a pause after each window is lost and halves the rate). A client
    that paces itself starts windows at most every `pace_ms`. Stereo recording: 456 ms (or `30000` when `live=1` is not advertised / live transfers are blocked).
  * `rec_bytes`: checkpointed WAV bytes flushed to disk for the active recording (`0` when idle or before first checkpoint). Enables live streaming `GETS` of an in-progress recording (`live=1`) up to the flushed tail.
* `+XREC:RTC` -> `+XREC:RTC,YYYY-MM-DD HH:MM:SS,<wd 0=Mon..6=Sun>`.
* `+XREC:RTC,y,m,d,h,mi,s,wd` -> `+XREC:OK,RTC` | `ERR,ARGS` | `ERR,RTCRNG` | `ERR,RTC`. While recording:
  `+XREC:OK,RTC,LATER`: **the clock is not changed** (set it again after the recording; an `OK` and not an `ERR`
  because older apps treat any other reply to a clock set as a failed sync).

### 5.2 LS
`+XREC:LS` -> zero or more `+XREC:F,<size dec>,<name>` lines, then `+XREC:END`.
`+XREC:LS,T` -> zero or more `+XREC:FT,<size dec>,<ISO-8601 local or ->,<name>` lines, then `+XREC:END` (`lst=1` capability; emits `-` when the timestamp is unset `0`, legacy `2000-01-01`, or cold-boot fallback `2026-01-01`; other arguments return `ERR,ARGS`).
* Only `/record`, non-recursive, directory order. Dot-files (staging temps such as `.xrec_rec.tmp`, `.autoplay`) and
  subfolders are skipped. Every other file is listed, including `.cwx` transcript sidecars.
* Names may contain commas: split an `F,` line with limit 2 (size, then name) and an `FT,` line with limit 3 (size, timestamp, then name).
* While a recording is active and checkpointed beyond the 44-byte WAV header, the active file's reported size is its flushed byte length.

### 5.3 GETS / GET (download)
`+XREC:GETS,<off>,<total>,<chunk 1..640>,<name>` -> binary frames until `total` bytes were sent or EOF:

| Bytes | Type | Meaning |
| :---: | :--- | :--- |
| 0..3 | ASCII | `"XREC"` |
| 4..7 | u32 LE | file offset of this frame |
| 8..9 | u16 LE | payload length N |
| 10..11 | u16 LE | CRC-16/CCITT-FALSE of the payload |
| 12.. | N bytes | file data |

* The device may lower the chunk to `RFCOMM MTU - 12`: **a short first frame is NOT EOF**. Track EOF with the size
  from `LS`, and ask for at most `size - off` bytes. Every frame of a burst has the same size except the last one.
* A burst that starts at or after EOF returns one frame with N = 0. A burst that reaches EOF part-way (the file is
  shorter than requested, e.g. it was replaced after `LS`) ends with one short frame and **no** N = 0 frame: treat a
  frame shorter than the burst's first frame as its end, and a silence after a short first frame as EOF (then ask
  again from the new offset; that burst gets the N = 0 frame).
* Errors come as text instead of a frame: `+XREC:ERR,<NOENT|NOSD|NAME|ARGS|SDIO|BUSY|LATER,<ms>>`. `BUSY`: the
  file being recorded (never sent; skip it). `LATER,<ms>`: a stereo recording runs (also sent in place of further
  frames when one starts during a stream); ask again after `<ms>`. See 5.5.
* Do not send another command while a GETS streams (LS, PUT and GETS cancel it).
* `total` may be any size: open_fw streams it with back-pressure (there is no credit window to respect). Use large
  windows (64 KiB when idle, 16 KiB while recording, docs/BT_SYNC_SPEED_AND_RECORDING.md A1): each GETS costs a
  round trip and a directory lookup. Stock firmware (`pen133` in `PONG`) needs `total = 3840`. `chunk = 640` (the
  device lowers it to `MTU - 12`). Verify offset and CRC of every frame; on any error retry from the last good offset
  (downloads are resumable).
* `+XREC:GET,<off>,<len <= 640>,<name>` -> one frame.

### 5.4 DEL, RECSTART / RECSTOP, other
* `+XREC:DEL,<name>` -> `+XREC:OK,DEL` | `ERR,NOSD` | `ERR,NAME` (unsafe path or dot-file such as `.autoplay`) |
  `ERR,BUSY` (the file being recorded, or the staging temp `.xrec_rec.tmp` of an active REC upload) | `ERR,LATER,<ms>`
  (another file while recording, 5.5) | `ERR,DEL` (missing). Cancels any active `GETS` stream cleanly before unlinking,
  only operates in `/record`, and triggers a UI file-list reload (`on_fs_change`). Deleting a recording
  `<stem>.wav` also deletes its transcript sidecar `<stem>.cwx` (as the on-device delete does): the recorder reuses
  the number of a deleted newest recording, and the new recording must not inherit the old transcript.
* `+XREC:RECSTART` -> `+XREC:OK,RECSTART,<name>`; `+XREC:RECSTOP` -> `+XREC:OK,RECSTOP` | `ERR,NOTREC`.
* `+XREC:SYNC,<pending 0..65535>[,<eta_sec 0..65535>]` -> `+XREC:OK,SYNC,<pending>` | `ERR,ARGS`. Cooperative background
  sync status (`sync=1` in `PONG`): sent by the phone at the end of a sync pass. `pending` counts recordings still
  awaiting download, on-phone transcription/diarization, or `.cwx` upload. When `pending == 0` and the client closes
  SPP, a background sync session on the recorder clears its `sync_pending` flag and shuts down Bluetooth within ~1.5 s
  (and powers off automatically if the idle auto-off timer has elapsed); when `pending > 0`, the recorder stays
  connectable in low-power page scan so the phone can reconnect as soon as transcription finishes and push the `.cwx`.
* `REBOOT`, `DFU`, `OTA`, `LOG`, `RUN`, `DSP` / `GAIN` / `CH` / `VOR`: maintenance and development commands, not
  needed by a sync client (see `xrec_proto.c`). Bench images only (`make BT=1 BENCH=1`): `DBG` (measurement counters)
  and `DBG,PACE[,<bytes/s>[,<budget us>]]` (read / set the GETS rate cap of 5.5 and its counters:
  `+XREC:DBG,PACE,rate=..,budget_us=..,ring_waits=..,budget_stops=..,cap_stops=..,later=..,busy=..,rec_b=..`).

### 5.5 While the recorder records (open_fw with STAT field 9)
The recording always wins (docs/BT_SYNC_SPEED_AND_RECORDING.md): the device never lets a transfer delay it, so a
client sees slower replies, and some commands wait for the end of the recording.
* **Recorder first.** A command is taken, and a GETS chunk, an LS directory chunk or a GET / GETS file lookup read
  from the card, only when the recorder has no audio waiting; one pass stops reading once it used 5 ms (it always
  makes progress: at least one read per pass) and the next pass continues where it stopped, so a large `/record`
  costs more passes, not a longer one. Replies are later, never lost.
* **Rate cap (mono).** GETS payload is limited to about 48000 B/s (+ a 2 KiB burst); `pace_ms` says how long a 16 KiB
  window takes. Downloads of finished recordings continue at that pace.
* **The active file** (`rec_idx`): `GETS` / `GET` answer `ERR,BUSY` before the first 4 KiB checkpoint (`rec_bytes <= 44`),
  then stream read-only up to `rec_bytes` (`live=1`); `DEL` always answers `ERR,BUSY`.
* **No second card writer.** `DEL` of another file, `PUT` to `REC` / `CAS` / `OTA` (the payload is discarded
  unparsed), `PUTEND`, `OTA` and `PAIR` answer `+XREC:ERR,LATER,<ms>` (`<ms>` = 30000): nothing changed, ask again
  after that long, at the latest when `STAT` shows `media != 33`. An upload staged before the recording started is
  kept **for the rest of this connection** and continues with the same chunk after the recording. If the link goes
  during the recording, the staged upload is dropped (temp closed and deleted) once the recording stopped, or when
  Bluetooth is switched off, never during the recording; a new connection never sees it (`PUTEND` -> `NOPUT`, the
  next chunk -> `OFF`: start again at offset 0). `PUT ... EXP` (RAM) is unaffected. A clock set answers
  `OK,RTC,LATER` (not applied). The bond file of a new Bluetooth pairing is written when the recording stops.
  `GAIN` / `DSP` / `VOR` / `CH` apply at once as before; the settings file is rewritten after the recording.
* **Pairing needs the recorder idle.** `PAIR` answers `LATER` (no PIN attempt used) because the key is saved to the
  card; the pairing window closes after 60 s, so in practice a phone is bound only while the recorder does not
  record. Clients should say so ("stop the recording to pair") instead of reporting a generic failure.
* **Stereo recordings (`live=1`):** `GETS` / `GET` continue best-effort at a ~36,000 B/s rate cap (`pace_ms = 456`) with a
  2.5 ms pass budget while yielding to recorder audio blocks. (Legacy images without `live=1` returned `ERR,LATER,<ms>`.) `LS` and `STAT` still answer.
* Older clients: extra STAT fields are ignored by position-based parsers and `BUSY` skips the file. `LATER` is an
  unknown reason, and nothing on the card is lost or changed, but it is not always confined to one file: the Cassini
  app before its S4 change (cassini-android-recorder `c1104d47`, "Sync finished recordings while the recorder
  records") treats a `LATER` to a transcript `PUT REC` as a device error that **ends the whole sync as failed**, on
  every sync while a recording runs and a transcript is pending. **Release gate:** this firmware ships only together
  with an app that knows `LATER` (S4 or later). `tools/bt_sync.py` skips `BUSY` / `LATER` files and reports a clock
  set that answered `OK,RTC,LATER`.

## 6. PUT uploads

```
+XREC:PUT,<offset>,<len 1..640>,<crc16 decimal>,<target>:<exactly len raw bytes, no CR>
```
Reply per chunk: `+XREC:OK,PUT,<offset>,<len>`. Errors: `ERR,LOCKED`, `ERR,CRC`, `ERR,OFF`, `ERR,NAME`, `ERR,ARG`,
`ERR,SDIO`, `ERR,NOSD`, `ERR,UNSUP`, `ERR,NOENT` (REC: no `/record/<stem>.wav`), `ERR,LATER,<ms>` (recording: resend
the same chunk after `<ms>`, 5.5).

Client retry policy: resend the same chunk only after a timeout or `ERR,CRC`. `SDIO`, `NOSD`, `LOCKED`, `NOENT` and
`OFF` are final (after `SDIO` the device has already dropped the staged upload). A late `OK,PUT` for an earlier
offset (the first reply to a chunk that was resent after a timeout) must be dropped, not taken as the reply to the
next chunk or to `PUTEND`.

| Target | Destination | Commit |
| :--- | :--- | :--- |
| `REC,<stem>.cwx` | `/record/<stem>.cwx`: the transcript sidecar of the existing recording `/record/<stem>.wav` (else `ERR,NOENT` at offset 0, and at `PUTEND` if the recording was deleted meanwhile: a `DEL` of `<stem>.wav` drops the staged sidecar at once, and `PUTEND` answers `ERR,NOENT` even if a new recording reused the name first). Staged as `/record/.xrec_rec.tmp` | `+XREC:PUTEND` |
| `CAS,<name>` | `/cassini/<name>` (`/cassini` is created if missing). Staged as `/cassini/.xrec_cas.tmp` | `+XREC:PUTEND` |
| `OTA` | `/update.up`. Staged as `/.xrec_ota.tmp` | `+XREC:OTA` (needs >= 64 B; reboots into the SD updater) |

**Transcripts must use `REC`.** The player and the Files list look for `<stem>.cwx` in the same folder as the
recording; a file in `/cassini` is never shown next to a recording.

Staging rules (all targets):
* Offset 0 starts a new upload and aborts any staged one, of any target (its temp is deleted).
* Later chunks strictly in order: `offset` must equal the bytes written so far, with the same target and name, else
  `ERR,OFF`.
* Resending the last chunk with identical offset / len / crc / target returns OK without writing it twice (safe
  retry after a lost reply).
* `+XREC:PUTEND` -> `+XREC:OK,PUTEND,<total>` commits a staged REC or CAS upload by an atomic replace (an existing
  file of that name is replaced; the name never disappears on power loss). `ERR,NOPUT` when no REC/CAS upload is
  staged (PUTEND never commits an OTA image, and `+XREC:OTA` never commits a REC/CAS upload: `ERR,NOOTA`).
* `PUTEND` is idempotent: until the next PUT header (any, even one refused with `ERR,ARG`), a repeated `PUTEND` (its reply was lost) answers the same
  `OK,PUTEND,<total>`. Firmware before this change answers `ERR,NOPUT` instead; a client that gets `NOPUT` on a
  repeated `PUTEND` confirms the commit with `LS` (name and size).
* A disconnect before the commit discards the staged upload (temp deleted).
* After a power loss or reset during an upload the temp stays on the card (a REC temp is visible as
  `/record/.xrec_rec.tmp` over USB). REC and CAS temps are checkpointed after every chunk, so the leftover is a normal
  file that owns its clusters; the next upload of that target deletes it at offset 0 (or delete it over USB). An OTA
  temp is not checkpointed: its clusters stay marked used until `fsck`.
* If the header is rejected after its `<len>` was parsed (`NAME`, an unknown target, a bad crc field: `ERR,NAME` /
  `ERR,ARG`), the error is sent at once and exactly `<len>` payload bytes are discarded unparsed. An unparsable or
  out-of-range offset or length gives `ERR,ARG` and the payload goes to the line parser. **Firmware before this
  change** fed the payload of every refused header to the line parser: `\r`-terminated pieces were parsed as lines,
  pieces of 192+ bytes gave extra `ERR,LINE` replies. So after any refused header send `\r+XREC:PING\r` and drain
  replies up to the `+XREC:PONG`.
* A device that predates the REC target answers `ERR,ARG` to a `REC,` header: report "recorder firmware too old for
  transcript upload" and do not retry.

Example (transcript for `REC_0007.wav`, 1500 bytes, chunks of 600):
```
> +XREC:PUT,0,600,<crc>,REC,REC_0007.cwx:<600 bytes>      < +XREC:OK,PUT,0,600
> +XREC:PUT,600,600,<crc>,REC,REC_0007.cwx:<600 bytes>    < +XREC:OK,PUT,600,600
> +XREC:PUT,1200,300,<crc>,REC,REC_0007.cwx:<300 bytes>   < +XREC:OK,PUT,1200,300
> +XREC:PUTEND                                            < +XREC:OK,PUTEND,1500
```
`tools/bt_sync.py --upload-cwx REC_0007.cwx --for-recording REC_0007.wav` does exactly this.

### 6.1 File name rules
* All names (`GET`/`GETS`/`DEL`/`PUT`): not empty, not `.` / `..`, shorter than 63 bytes, no control characters and
  none of `/ \ : * ? " < > |`, must not end in `.` or space. Names are case-insensitive on the card (exFAT).
* `CAS` names: additionally at most 30 characters, ASCII.
* `REC` names: additionally at most 30 characters, printable ASCII, must not start with `.`, and **must end in `.cwx`**
  (any case). Anything else (`REC_0001.wav`, `../x.cwx`, hidden or longer names) is `ERR,NAME`, so a recording can
  never be overwritten. A recording is a `.wav`, so a REC upload never touches the file being recorded.

### 6.2 After the session
The recorder re-scans the file list when the user leaves BT mode, so a new or replaced `<stem>.cwx` shows the
transcript mark in the Files list right away; the player opens the sidecar from the card when playback starts.

## 7. Recordings and the CWX1 transcript sidecar

### 7.1 Recordings
`/record/REC_NNNN.wav`, RIFF WAV PCM 16-bit, 16 kHz, mono or stereo (files written by tools may be IMA-ADPCM).
Ignore files of 44 bytes or less. A file being recorded grows: call `LS`, then `STAT`. With STAT field 9 (`rec_idx`)
skip only `REC_<rec_idx>.wav` (the device refuses it anyway, `ERR,BUSY`) and download the others at the paced rate
(5.5); without it skip every download while `media == 33`. Key sync state on (device address, name, size), optionally plus a hash of the first 4 KiB, because
numbering restarts after the card is wiped.

### 7.2 CWX1 format (summary)
Reader: `open_fw/src/cwx.c`, `open_fw/include/openfw_cwx.h`. Reference encoder: `tools/cassini_to_device.py`
(`build_cwx_bytes`). Sample: `web_emu/samples/cassini-final-moments.cwx`. All little-endian.

* **Header (64 B)**: 0 `"CWX1"`; 4 u16 version = 1; 6 u16 speaker_count (<= 16); 8 u32 duration_ms; 12 u32
  word_count; 16 u32 speakers_off (>= 64); 20 u32 seek_off; 24 u32 seek_count; 28 u32 words_off; 32 char[32] title
  (NUL-padded, <= 31 bytes). Section order: speakers, seek, words.
* **Speakers**: speaker_count x 24 B: u16 RGB565 colour, u8 flags (0), u8 reserved (0), char[20] label (<= 19 B,
  NUL-padded).
* **Seek table**: seek_count = duration_ms / 1000 + 1 entries of 8 B: u32 byte offset of a word relative to
  words_off, u32 word index; entry k points at the first word whose end_ms >= k * 1000.
* **Words**: u32 start_ms, u32 end_ms, u8 speaker_idx, u8 flags (0x01 TURN_START, 0x02 LOW_CONF), u8 text_len, then
  text_len bytes of UTF-8 (the reader keeps up to 47 bytes). Words in turn order (not sorted by start). The encoder
  maps typographic quotes, dashes and ellipsis to ASCII and strips control characters; the device font covers
  Latin-1, Latin Extended-A, Greek and Cyrillic. A recording with no detected speech is represented by a valid `CWX1`
  sidecar with `word_count = 0` (`speaker_count = 0`, seek entries `{0, 0}`): its presence marks the recording as
  already transcribed (`[T]` badge, no repeat sync) and the on-device player displays `"No speech"`.

## 8. Legacy stock-firmware patch (`fw_mod`) differences

`fw_mod/bt_filesync.c` (v5+) runs on top of the stock `pen133_2.0.65` Bluetooth stack. It is not maintained for new
features (no `REC` / `CAS` targets, no `PUTEND`). Differences from open_fw:

* Device name `HW_XX` (last byte of the BT MAC); without the `bt-spp-uuid` patch it advertises HFP
  (`0000111E-...`) instead of SPP, and Android's HeadsetService may contend for channel 1 (turn "Phone calls" off).
* **Mandatory HFP `SLC_OPEN` handshake** on every RFCOMM connect before any `+XREC:` command. The recorder sends AT
  commands; answer each line in a separate write about 50 ms apart:

  | Recorder sends | Client replies |
  | :--- | :--- |
  | `AT+BRSF=<n>` | `+BRSF: 1007`, `OK` |
  | `AT+BAC=...` | `OK` |
  | `AT+CIND=?` | `+CIND: ("service",(0,1)),("call",(0,1)),("callsetup",(0,3)),("callheld",(0,2)),("signal",(0,5)),("roam",(0,1)),("battchg",(0,5))`, `OK` |
  | `AT+CIND?` | `+CIND: 1,0,0,0,5,0,5`, `OK` |
  | `AT+CMER=...` | `OK`, wait ~150 ms: SLC open |
  | other `AT...` | `OK` |

  `tools/bt_sync.py --transport auto` detects this (open_fw stays silent after connect).
* PONG firmware field `pen133_2.0.65` instead of `openfw_v5`; the key is stored in PSMP (`"xrec_key"`), not on the
  card.
* Authentication expires after 10 minutes idle (open_fw: never, only on disconnect).
* STAT `media`: `0x00` idle, `0x10` BTREADY, `0x21` RECORDPLAY, `0x22` RECORDPAUSE.
* Only the `OTA` PUT target (max 600 B per chunk with `bt-rfcomm-turbo`). Without `bt-rfcomm-turbo` use GETS chunk
  480 and total 1920.

## 9. Test vectors

### CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflection, xorout 0x0000)
* ASCII `"123456789"` -> **`0x29B1`**
* empty input -> **`0xFFFF`**

### HMAC-SHA256 (`+XREC:AUTH`)
* Key (32 B hex): `000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f`
* Nonce (16 B hex): `a0a1a2a3a4a5a6a7a8a9aaabacadaeaf`
* Expected `+XREC:AUTH` hex: `b1489727b2602a0e047cb237580a4c795aaf9023c5bc6b0b865c7fa17e0a2367`
