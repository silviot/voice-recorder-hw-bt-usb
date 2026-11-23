# `+XREC` v5 Bluetooth RFCOMM Protocol Specification (`HW_76` / Smartlink SL6801)

This document specifies the `+XREC v5` wire protocol over Classic Bluetooth BR/EDR RFCOMM Channel 1, designed for mobile (`cassini-android`) and desktop (`tools/bt_sync.py`) clients.

> **Implementation status:**
> - **Standalone `open_fw`** (`open_fw/src/xrec_proto.c`, `open_fw/src/bt_spp.c`, `open_fw/src/bt_hci.c`, `open_fw/src/bt_rf.c`): Full protocol state machine, SPP framing, and RF/modem drivers are implemented and host-tested, with end-to-end on-device Bluetooth connectivity bring-up as the next active development milestone. `open_fw` also extends the command set with `+XREC:DSP`, `+XREC:GAIN`, `+XREC:LOG`, `+XREC:DFU`, and staged `PUT`/`PUTEND` uploads (see `open_fw/src/xrec_proto.c`).
> - **Legacy stock-firmware patch** (`fw_mod/bt_filesync.c`, `v5+`): Runs on top of the stock `pen133_2.0.65` Bluetooth stack (requires the HFP `SLC_OPEN` handshake below).

---

## 1. Transport & Connection Setup

* **Bluetooth Type:** Classic Bluetooth BR/EDR only (SL6801 `RW-BT` silicon has no BLE hardware).
* **Device Name:** `HW_XX` (e.g., `HW_76`, derived from the last byte of the BT MAC).
* **RFCOMM Channel:** `1`
* **Recommended Android Socket API:**
  * With `--patch bt-spp-uuid` installed (recommended for dedicated Android recorders):
    ```kotlin
    val sppUuid = UUID.fromString("00001101-0000-1000-8000-00805F9B34FB")
    val socket = device.createRfcommSocketToServiceRecord(sppUuid)
    socket.connect()
    ```
    Advertising SPP (`0x1101`) prevents Android's `HeadsetService` from routing phone calls to `HW_76` or contending for RFCOMM Channel 1.
  * Without `bt-spp-uuid`: `HW_76` advertises HFP (`0000111E-0000-1000-8000-00805F9B34FB`); users should toggle **Phone calls** OFF in Android Bluetooth device settings.

### Mandatory HFP `SLC_OPEN` Handshake (On Every RFCOMM Connect)
Immediately after RFCOMM Channel 1 opens, the SL6801 HFP state machine initiates an AT handshake. The client **must** complete this before sending any `+XREC:` command.

**Critical framing rule:** Send each response line in a **separate `write()` + `flush()` with ~50 ms spacing**, because the SL6801 AT parser processes one line per RFCOMM frame during handshake.

| Recorder Sends (`\r`-terminated) | Client Replies (each wrapped in `\r\n...\r\n`, 50 ms apart) |
| :--- | :--- |
| `AT+BRSF=<n>` | `+BRSF: 1007` then `OK` |
| `AT+BAC=...` | `OK` |
| `AT+CIND=?` | `+CIND: ("service",(0,1)),("call",(0,1)),("callsetup",(0,3)),("callheld",(0,2)),("signal",(0,5)),("roam",(0,1)),("battchg",(0,5))` then `OK` |
| `AT+CIND?` | `+CIND: 1,0,0,0,5,0,5` then `OK` |
| `AT+CMER=...` | `OK` → wait ~150 ms → **`SLC_OPEN` established!** |
| Any other `AT...` | `OK` |

---

## 2. Casual-User Pairing & Authentication State Machine (`v5`)

All `+XREC:` commands are terminated with `\r` (CR). Text replies end with `\r\n` (CRLF).

### 2.1 Discovery & Status (`+XREC:PING`, unauthenticated)
* **Request:** `+XREC:PING\r`
* **Response:**
  ```text
  +XREC:PONG,pen133_2.0.65,v5,bound=<0|1>,auth=<0|1>,pair=<0|1>\r\n
  ```
  * `bound`: `1` if a 256-bit secret is persisted in `PSMP` (`"xrec_key"`), else `0`.
  * `auth`: `1` if this RFCOMM session is currently authenticated, else `0`.
  * `pair`: `1` if the 60-second physical-presence pairing window (opened via the recorder's Bluetooth menu) is active, else `0`.

### 2.2 First-Time Physical-Presence Binding (`+XREC:PAIR`)
1. User opens the **Bluetooth** menu on the recorder:
   * Recorder enables BT discoverability (`Inquiry Scan = ON`) for **60 seconds**.
   * Recorder renders a yellow **`PIN: XXXXXX`** (6 decimal digits) banner at the bottom of the LCD.
2. App generates a cryptographically random 32-byte key (`SecureRandom`, stored in Android Keystore) and sends:
   ```text
   +XREC:PAIR,<6-digit-PIN>,<64-lowercase-hex-key>\r
   ```
3. **Responses:**
   * `+XREC:OK,PAIR\r\n` — Key persisted in `PSMP` (`"xrec_key"`), pairing window closed, discoverability disabled, current session immediately authenticated.
   * `+XREC:ERR,PAIRCLOSED\r\n` — User has not opened the Bluetooth menu (or 60s expired).
   * `+XREC:ERR,PIN\r\n` — Wrong PIN (window auto-closes after 5 failed attempts).

### 2.3 Per-Connection Challenge-Response (`+XREC:CHAL` → `+XREC:AUTH`)
Required on every new connection before calling any privileged command:
1. Client sends `+XREC:CHAL\r` → Recorder replies `+XREC:CHAL,<32-hex-nonce>\r\n` (16-byte one-time random nonce).
2. Client computes `mac = HMAC-SHA256(key = stored_32B_key, msg = nonce_16B)` (lowercase 64-char hex) and sends:
   ```text
   +XREC:AUTH,<64-hex-hmac>\r
   ```
3. **Responses:**
   * `+XREC:OK,AUTH\r\n` — Session unlocked (bound to current `hf` handle + peer `BD_ADDR`, auto-expires after 10 minutes idle or on disconnect/reboot).
   * `+XREC:ERR,AUTH\r\n` — Wrong key (nonce is consumed; call `+XREC:CHAL` again to retry).
   * `+XREC:ERR,UNBOUND\r\n` — Recorder has no bound key yet; prompt user to pair.

---

## 3. Privileged Commands (Require `auth=1`, Else `+XREC:ERR,LOCKED\r\n`)

### 3.1 Telemetry & Recording State (`+XREC:STAT`)
* **Request:** `+XREC:STAT\r`
* **Response:**
  ```text
  +XREC:STAT,<bat_pct>,<voltage_mv>,<charge_status>,<vbus>,<media_status>,<rec_sec>,<free_mb>,<total_mb>\r\n
  ```
  * `charge_status`: `0` = discharging, `1` = charging, `2` = full.
  * `media_status`: `0x00` = idle, `0x10` = `BTREADY`, **`0x21` = `RECORDPLAY`**, **`0x22` = `RECORDPAUSE`**.

### 3.2 Hardware RTC Read & Sync (`+XREC:RTC`)
* **Read:** `+XREC:RTC\r` → `+XREC:RTC,YYYY-MM-DD HH:MM:SS,W\r\n` (`W`: `0`=Mon..`6`=Sun).
* **Set:** `+XREC:RTC,<YYYY>,<MM>,<DD>,<hh>,<mm>,<ss>,<wd>\r` → `+XREC:OK,RTC\r\n`.

### 3.3 Directory Listing (`+XREC:LS`)
* **Request:** `+XREC:LS\r`
* **Response:** Zero or more file lines followed by `+XREC:END\r\n`:
  ```text
  +XREC:F,<size_bytes_dec>,<filename>\r\n
  ...
  +XREC:END\r\n
  ```
  Lists files in `0:\record`.

### 3.4 Binary Burst Download (`+XREC:GETS` & `+XREC:GET`)
* **Request:**
  ```text
  +XREC:GETS,<offset_dec>,<total_len_dec>,<chunk_len_dec>,<filename>\r
  ```
  * With `bt-rfcomm-turbo` (`v5.1+`): use **`chunk_len = 640`**, **`total_len = 3840`** (6 × 640 B frames per burst → **~92–96 KB/s**).
  * Without `bt-rfcomm-turbo`: use `chunk_len = 480`, `total_len = 1920` (4 × 480 B frames → ~59 KB/s).
  * Arbitrary `offset_dec` is supported for **resumable downloads** after Bluetooth dropouts.
* **Binary Response Frame (repeated until `total_len` bytes or EOF):**
  | Byte Offset | Size | Type | Description |
  | :---: | :---: | :--- | :--- |
  | `0..3` | 4 | ASCII | Magic `"XREC"` (`0x58 0x52 0x45 0x43`) |
  | `4..7` | 4 | `uint32_le` | Byte offset of this chunk in the file |
  | `8..9` | 2 | `uint16_le` | Payload length `N` (`0..640`; `< chunk_len` indicates EOF) |
  | `10..11` | 2 | `uint16_le` | `CRC-16/CCITT-FALSE` over the `N` payload bytes |
  | `12..12+N-1` | `N` | raw bytes | Raw file bytes |
  * On error, recorder replies with text `+XREC:ERR,<reason>[,<code>]\r\n` instead of `"XREC"`.

### 3.5 Delete, Remote Record & Wireless OTA
* **Delete:** `+XREC:DEL,<filename>\r` → `+XREC:OK,DEL\r\n`
* **Start local WAV recording:** `+XREC:RECSTART[,<filename>]\r` → `+XREC:OK,RECSTART,<filename>\r\n`
* **Stop local WAV recording:** `+XREC:RECSTOP\r` → `+XREC:OK,RECSTOP\r\n`
* **OTA chunk write (`0:\update.up`):** `+XREC:PUT,<offset>,<len>,<crc16_dec>,OTA:<raw_bytes>` → `+XREC:OK,PUT,<offset>,<len>\r\n` (max `len = 600` B with `bt-rfcomm-turbo`).
* **Trigger OTA flash & reboot:** `+XREC:OTA\r` → `+XREC:OK,OTA\r\n` (reboots into Stage-1, flashes `0:\update.up`, auto-boots new `FIRM`).
* **Reboot:** `+XREC:REBOOT\r` → `+XREC:OK,REBOOT\r\n` (device may drop RFCOMM immediately).

---

## 4. Recommended `cassini-android` Polling & Sync Algorithm

1. **Background Poll Cycle (e.g., WorkManager / Foreground Service while paired device is nearby):**
   * Connect RFCOMM → complete HFP `SLC_OPEN` handshake → `+XREC:PING` → `+XREC:CHAL` / `+XREC:AUTH`.
   * *(Note: `v5.2+` firmware does **not** wake the recorder's LCD screen or reset `idleshut` on `PING`/`CHAL`/`AUTH`/`STAT`/`LS`, so background polling won't light up the screen in the user's pocket!)*
2. **Check `+XREC:STAT` First:**
   * If `media_status` is **`0x21` (`RECORDPLAY`)** or **`0x22` (`RECORDPAUSE`)**: **do NOT download yet** — the user is actively recording and the `.wav` file is still open/unflushed. Disconnect cleanly and check again on the next poll.
3. **Auto-Sync Hardware Clock:**
   * Read `+XREC:RTC`; if drift from phone wall clock > 5 seconds, send `+XREC:RTC,<now>`.
4. **Diff & Resumable Download:**
   * Call `+XREC:LS` and ignore any file with `size <= 44` bytes (empty WAV header).
   * Compare `(filename, size)` against Cassini's imported recordings table.
   * For each new recording, download into `<filename>.part` starting at `offset = partFile.length()` using `+XREC:GETS,<offset>,3840,640,<filename>`, verifying `offset` and `CRC-16/CCITT-FALSE` on every frame (up to 3 retries per burst).
   * Atomically rename `.part` → `.wav` once `offset == size`, enqueue into Cassini's transcription pipeline, and optionally call `+XREC:DEL,<filename>` if auto-delete is enabled.

---

## 5. Cryptographic & Checksum Test Vectors (For Unit Tests)

### CRC-16/CCITT-FALSE (`poly = 0x1021`, `init = 0xFFFF`, no reflect, `xorout = 0x0000`)
* Input ASCII `"123456789"` → **`0x29B1`**
* Input empty `""` → **`0xFFFF`**

### HMAC-SHA256 (`+XREC:AUTH`)
* Key (32 B hex): `000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f`
* Nonce (16 B hex): `a0a1a2a3a4a5a6a7a8a9aaabacadaeaf`
* Expected `+XREC:AUTH` hex: `b1489727b2602a0e047cb237580a4c795aaf9023c5bc6b0b865c7fa17e0a2367`
