# RW-BT mask-ROM controller interface (how open_fw drives it today)

Status: reference for plan v18 M1 (`bt_ctrl`) and for the host-test **fake ROM**. Describes the *existing* legacy
path in `open_fw/src/bt_hci.c` (branch `feat/freertos-bluedroid`), plus facts recovered by static disassembly of
`dumps/bootrom_384K.bin` (llvm-objdump, Thumb-2) while writing this document. Nothing here has been newly measured
on silicon; items that need hardware are collected in §11.

Citation key: `F:` = `open_fw/src/bt_hci.c`, `R:` = `open_fw/src/bt_rf.c`, `H:` = `open_fw/include/sl6801_rom.h`,
`G:` = `open_fw/include/sl6801_regs.h`, `M:` = `open_fw/src/main.c`, `S:` = `open_fw/src/startup.c`,
`P:` = `docs/BLUETOOTH_BLUEDROID_PLAN.md`, `FA:` = `docs/FIRMWARE_ANALYSIS.md`, `ROM 0xNNNNN` = mask-ROM address
(disassembled). Line numbers refer to the tree at the time of writing.

---

## 1. Big picture

```
 IRQ 26 (handler mode)                    thread mode, single caller (legacy: app super-loop)
 ┌──────────────────────┐   g_bt_irq_pending   ┌───────────────────────────────────────────────────────────────┐
 │ RWBT_IRQHandler      │ ───────────────────▶ │ bt_xrec_poll(): bt_irq_bh_poll() → bt_ke_event_schedule()      │
 │ read INTSTAT, ack,   │                      │   ev0 ke_task_schedule  → pops ONE ke_msg, runs task handler  │
 │ OR into pending      │                      │   ev1 ke_timer_schedule → expired ke_timers → ke_msgs         │
 └──────────────────────┘                      │   ev5 openfw_hci_tx_evt_cb (open_fw's H4 TX completion)       │
                                               │ TASK_HCI (0x3C) default handler (open_fw) ← ROM LM/LC/LD msgs │
                                               │ openfw_hci_send_cmd/acl → ke_msg_alloc/send → ROM tasks       │
                                               └───────────────────────────────────────────────────────────────┘
```

* The ROM contains the RivieraWaves kernel (`ke_*`), LM (`TASK_LM` 0x38), LC (`TASK_LC` 0x39, one instance per link),
  LD (`TASK_LD` 0x3B) and the EM buffer pools. It contains **no usable HCI transport and no `TASK_HCI`**: open_fw
  registers its own clean-room `TASK_HCI` (F:865–874, F:1105) and its own H4 "transport" (`hci_tl_env` layout at
  `0x00803C28`, F:71–115, F:545–670, F:1692–1715).
* Consequence for M1 (**plan wording to correct**): P:148–149 says `bt_ctrl` "copies each complete H4 packet into a
  bounded ring before completing the ROM `eif_write` callback". `eif_write`, its completion callback
  (`openfw_hci_tx_done_cb`, F:545–549) and the TX queues are **open_fw code**, not ROM. The only ROM-visible
  "acknowledgement" is the TASK_HCI handler's return code plus the later `ke_msg_free`/`em_buf_rx_free` calls (§4).
  `bt_ctrl` can therefore drop the `ext_if`/`tx_state` machinery entirely and pack/copy directly in its TASK_HCI
  handler. The copy-before-ack rule still holds, re-stated as "copy, then return CONSUMED / free EM".
* All ROM BT code is integer-only (FA:438 "VFP None") and calls the outside world **only through RAM hook pointers**
  (§2.2). Nothing in ROM calls FreeRTOS.

## 2. Controller bring-up sequence

Entry: `run_bt_xrec_mode()` → `bt_rf_init_calibrated(0x53, 32000000)` (M:1275) → `bt_xrec_start()` (M:1276,
F:2015–2072) → `rwbt_controller_init(g_bd_addr)` (F:2067, F:1353–1379) → synthetic `HCI_Reset` (F:2070).

### 2.1 Order (must be preserved)

| # | Step | Where | Notes |
|---|---|---|---|
| 0 | RF calibration + modem init | R:632–646, M:1275 | Uses EM `0x40030000`/`0x40033000` as loopback/capture scratch (R:353, R:425). **Must run before step 2**, which wipes EM. |
| 1 | BD_ADDR + name | F:1985–2013 | eFuse w0/w1 if valid (w3 bit 31) else timer-derived; LAA bits forced (F:2001–2003). Plan M1 replaces this (P:240–245). |
| 2 | IRQ 26 off, clear pending, reset ISR/lock counters | F:1355–1359 | |
| 3 | Zero all 32 KiB EM | F:1361 | volatile 32-bit stores (F:125–141) |
| 4 | `RWBT_CNTL |= bit29` soft reset, poll clear (≤200 000 iterations, no error path) | F:1363–1365 | |
| 5 | `bt_heap_init()` – whole 8 KiB heap becomes one free block | F:1367, F:311–316 | invalidates every earlier ke_msg/ke_timer pointer |
| 6 | Kernel hooks (§2.2), zero kernel queues/task table, `ke_event_init`, event callbacks 0/1/5 | F:1048–1079 | |
| 7 | HCI layer: `0x00803BBC[0..7]` hooks, zero+init `hci_tl_env`, event mask, lists, `ke_task_create(TASK_HCI)` | F:1081–1107 | TASK_HCI is created **before** LD/LC/LM so ROM messages to it never hit an unknown task (ROM faults on that, §3.4). |
| 8 | LD layer: hook tables `0x00802BB0`, `0x00802904`, `0x008030CC`, `rwip_rf` vtable `0x00803BDC`, SRAM zeroing, 37 stock constants ("Blocker #1/#2"), `ke_task_create(TASK_LD, 0x00800608)`, `ld_bd_addr_set`, EM init, `em_buf_init`, `ld_sched_init`, 10× `ld_util_cs_init`, TX power tables, LMP features, `ld_sco_audio_init`, `ld_afh_init`, baseband regs, **RWBTEN** (`RWBT_CNTL |= 0x100`) | F:1109–1287 | INTCNTL written `0xFFFF0003` (F:1283) |
| 9 | LC layer: `0x008021BC` hooks, `ke_task_create(TASK_LC, 0x008005F8)`, 7 LC instance states + 7×0x208 B env zero, `lc_pca_init`, `lc_sco_init` | F:1289–1311 | |
| 10 | LM layer: env zero, hooks, `rwbt_env_flags_clear`, `lm_ssp_init`, sync/acl/con-info/P-192 init, scan params, GIAC, `ke_task_create(TASK_LM, 0x00800660)` | F:1313–1351 | |
| 11 | `RWBT_TIMING`, `RWBT_REG(0x2C)=0x1FF`, clear + enable NVIC IRQ 26 | F:1374–1378 | NVIC priority **never set** → default 0 (highest). Plan §3 requires logical 4..7 (P:57–59). |
| 12 | `g_hci_stage = WAIT_RESET`; synthetic Reset CC (§3.3), then the legacy init chain Name → CoD → SSP → Scan Enable driven by CC events | F:2068–2070, F:1539–1566 | Bluedroid replaces step 12 (P:189, matrix "Prioritized shim work" #1). |

Stop: `bt_xrec_stop()` (F:2151–2176): NVIC off, `INTCNTL=0`, `INTACK=0xFFFFFFFF`, clear RWBTEN, soft reset + poll,
clear pending. It does **not** free heap objects; the next start re-zeroes everything (steps 3–10). A "real
controller reset" (P:189) is therefore `stop()` + `start()`, and `bt_ctrl` must drop every ROM-owned pointer it
holds (ke_msgs, EM buffers) before the restart.

### 2.2 RAM hook map installed by open_fw (ROM calls *through* these)

| RAM slot | Installed value | ROM consumer (disassembly) |
|---|---|---|
| `0x00803C1C` / `0x00803C20` | `bt_irq_lock` / `bt_irq_unlock` (F:1050–1051) | ROM `0x3DE80` / `0x3DE8C` (`bt_int_disable/restore`, H:116–117) are 3-instruction thunks through these slots; every ROM critical section uses them. |
| `0x00803C24` | `bt_rand80` (returns `0..79`, F:306–309) | thunk ROM `0x3DE98` |
| `0x008041CC` / `0x008041D0` | `bt_malloc(size,type)` / `bt_free(ptr)` (F:1053–1054) | `ke_malloc` ROM `0x3DEA4`, `ke_free` ROM `0x3DEB0` (thunks) |
| `0x008041D4..E0` | ROM `ke_timer_hw_set/set/clear/active` (F:1056–1059) | — |
| `0x008041A8/AC/B0` | ROM `ke_msg_alloc/send/forward` bodies `0x30A01/0x30A3D/0x30A7D` (F:1061–1063) | public entries `0x309EC/0x30A30/0x30A70` are thunks through these |
| `0x008040D0/D4`, `0x00804124` | ROM EM pool alloc/free `0x304BD/0x304E9`, EM RX alloc `0x305D9` (F:1065–1067) | `0x304B0`, `0x304DE`, `0x305CC` thunks |
| `0x00804200` / `0x00804204` | `bt_ke_event_schedule` / `bt_ke_event_set` (F:1073–1074) | ROM `ke_event_schedule` `0x30DB4` and `ke_event_set` `0x30DC0` are thunks through `0x008041E4+0x1C/+0x20` |
| `0x008041E8[0..5]` | ev0 = ROM `ke_task_schedule` `0x307D5`, ev1 = ROM `ke_timer_schedule` `0x30B8D`, ev5 = `openfw_hci_tx_evt_cb` (F:1076–1078) | written by `ke_event_callback_set` ROM `0x30DCC` (rejects ev > 5) |
| `0x00803BBC[0..7]` | `hook_ret0` ×7, `[7]` = `hook_3bbc_7_set_evt_mask` → writes `mask | mandatory` to `0x00803D04` (F:481–488, F:1083–1091) | semantics of `[0..6]` unknown (§11) |
| `0x00802BB0`, `0x00802904`, `0x008030CC`, `0x00803BDC`, `0x008021BC` | LD/LC/SCO/RF hooks (F:1111–1166, F:1291–1301) | `hook_21bc_acl_con_cmp` (F:452–479) completes ACL setup from LC context; `hook_2904_00` sets per-link TX power (F:442–450) |

Kernel SRAM layout confirmed by disassembly: task table `0x0080415C` (count byte + 8-byte entries `{type@+4,
desc@+8}`, ROM `0x3089C`), message queue `0x008041B4`, saved queue `0x008041BC`, timer queue `0x008041C4`
(zeroed together at F:1069), event field `0x008041E4` + 6 callbacks (`ke_event_init` ROM `0x30DA4` memsets 0x1C B).
The ROM also uses `0x00803C28` as a **base pointer for LM fields far beyond `hci_tl_env`** (`+0x45A`, `+0x497`,
ROM `0x41778`, `0x420F4`, `0x3F414`); open_fw's `hci_tl_env` struct only covers `0x803C28..0x803D0B`.
`bt_ctrl` must not reuse anything in `0x00800000..0x00804BFF` (FA:609).

## 3. Host → controller: HCI commands

### 3.1 Message layout (`struct ke_msg`, F:50–60; confirmed by ROM `ke_msg_alloc` body `0x30A00`)

```
+0x00 hdr.next (co_list)   +0x04 hci_type   +0x05 hci_hdr_offset (int8)   +0x06 hci_len (u16)
+0x08 id (u16)             +0x0A dest_id    +0x0C src_id                  +0x0E param_len   +0x10 param[]
```

* `rom_ke_msg_alloc(id, dst, src, len)` (H:88) calls `ke_malloc(len + 0x10, type = 1)` → **g_bt_heap**, fills the
  header, **zero-fills `param`** (ROM memset `0xBAA`) and returns the **`param` pointer**. The explicit
  `bt_memzero` at F:936 is redundant.
* **It never returns NULL.** The body does not test the `ke_malloc` result: on heap exhaustion it stores to
  `0x0000000C…` and `memset`s at `0x10`, then returns `(void*)0x10`. The `if (!buf)` checks at F:933 and F:973 are
  dead code. Heap exhaustion is therefore a fault, not an error return (§8).
* `rom_ke_msg_send(param)` (ROM `0x30A3C`): under the lock, `co_list_push_back(0x008041B4, param - 0x10)`, unlock,
  then `ke_event_set(0)`. Nothing is executed synchronously; delivery happens in a later `ke_task_schedule`.
* `rom_ke_msg_send_basic(id, dst, src)` = alloc(len 0) + send (ROM `0x30A60`).
* `rom_ke_msg_free(msg)` (ROM `0x30A9E`) is a direct `ke_free(msg)` → `bt_free(msg)`: it takes the **header**
  pointer (`param - 0x10`), unlike `send`, which takes `param` (F:944 vs F:952). The fake ROM must reject a param
  pointer here; `bt_rom_if_t` normalises both to `param` (§10).

### 3.2 Opcode → KE id, destination task, payload (F:876–956)

* Table `g_openfw_cmd_tbl` (F:882–904): `{opcode, msg_id, min_len}`. 21 entries. `dest_id = msg_id >> 10`
  (F:922): `0xE0xx` → `0x38` TASK_LM, `0xE4xx` → `0x39` TASK_LC.
* Payload: `max(param_len, min_len)` bytes, zero-filled, then the **HCI wire parameters copied verbatim**
  (F:930–939). This assumes the ROM struct equals the packed wire layout. That is wrong for any ROM struct with
  natural alignment padding (matrix row `0405`; the reply direction visibly has padding, §5.2). Each new opcode needs a
  verified layout (P:337–338).
* `min_len == 0 && param_len == 0` → `ke_msg_send_basic` (F:923–929): `1001`, `1005`, `1009`.
* LC-bound commands (`0405` Create Connection goes to LM; `0406` Disconnect `E400` goes to LC): the first two
  payload bytes are taken as a connection handle and `dest_id` is overwritten with `rom_hci_conhdl_to_task(h)`
  (F:940–948). ROM `0x3076C`: returns `(link_id << 8) | 0x39` when `h ∈ 0x80..0x86` (ROM `0x3075C`) and the
  per-link state byte at `0x00804128 + 7·link_id` is 1; otherwise **`0xFF`** (not `0xFFFF`), which matches the
  comparison at F:943. On failure the message is freed (F:944).
* `src_id = TASK_HCI` always. ROM LM handlers send their reply to the command's **`src_id`** (e.g. ROM `0x3F430`:
  `hci_cmd_cmp_evt_send(0xE09F, dst = src, src = 0x38, status)`), so all replies arrive at TASK_HCI.
* Command credits: `hci_tl_env.nb_h2c_cmd_pkts` starts at 7 (F:1096). Decremented per send (F:913, F:925,
  F:949–951), incremented (capped at 7) only by the CC/CS packers (F:679–681, F:694–696). The CC/CS sent to the
  host always says `Num_HCI_Command_Packets = 1` (F:676, F:691). `0C35` is exempt from the credit check, but its
  table lookup fails, so it is never sent (F:909, F:955; matrix row `0C35`).
* Unknown opcode → returns 0 silently (F:955). Bluedroid would time out (P:142–144 forbids this).

### 3.3 HCI Reset is synthetic

`0C03` does not reset anything: `rom_hci_cmd_cmp_evt_send(0xE07A, TASK_HCI, TASK_LM, 0)` (F:912–915).
ROM `0x32E40` = `ke_msg_alloc(id, dst, src, 1)`, `param[0] = arg4`, `ke_msg_send`. **The 4th argument is the
status byte; `param_len` is fixed to 1.** H:105 names it `len`; the header is misleading (status 0 is what F:914
intends). ROM `0x32E54` is an identical second entry point (likely the Command Status variant).

### 3.4 Delivery inside the ROM kernel (ROM `ke_task_schedule` `0x307D4`, event 0)

1. Under the lock, pop **one** message from `0x008041B4`; unlock.
2. Look up `dest_id & 0xFF` in the task table. **Not found → `udf #0xFF` (HardFault)**. `idx = dest_id >> 8`;
   `idx >= desc->idx_max` → message freed silently.
3. If `desc->state_handler` is set, search its table for `id`, then the default table (`0xFFFF` matches any id).
   No handler → message freed.
4. Call `handler(id, param, dest_id, src_id)` **with the lock released**. Return `0` = CONSUMED → `ke_msg_free`;
   `2` = SAVED → pushed on `0x008041BC` (re-queued by `ke_state_set`); anything else (open_fw uses `1`,
   F:22–23) = NO_FREE: the handler now owns the message.
5. If the queue is empty, `ke_event_clear(0)`. Otherwise ev0 stays set and the next `ke_event_schedule` pass
   handles the next message.

## 4. Controller → host: events and Command Complete

### 4.1 Path

1. ROM task sends a `ke_msg` to TASK_HCI (§3.4). open_fw's descriptor has no state handler and a single default
   entry (F:865–874), so every id lands in `openfw_hci_default_handler` (F:720–863).
2. The handler **repacks in place** inside the ke_msg (F:741–850): it writes the HCI event header into the bytes
   *before* `param` and records `hci_type = 4`, `hci_hdr_offset` (−5 for CC/CS, −2 for other events) and `hci_len`
   in the message header (F:855–857):
   * CC (`pack_cmd_cmp`, F:672–683): `msg[0x0B]=0x0E, [0x0C]=plen+3, [0x0D]=1, [0x0E..0F]=opcode`, followed by the
     ROM's `param[0..plen-1]` (status first). Overwrites `dest_id` high byte, `src_id` and `param_len`.
   * CS (`pack_cmd_status`, F:685–698): moves `param[0]` (status) to `0x0D` and writes the opcode over `param[0]`.
   * Other events (`pack_std_evt`, F:700–705): code + length at `msg[0x0E..0x0F]`, after per-event
     de-padding (F:763–850, e.g. `E474` moves the u32 numeric value from `param+8` to `param+6`).
   * The H4 type byte is later written at `buf[-1]` (F:553), i.e. `msg+0x0A` or `msg+0x0D`.
3. It pushes the message on `evt_queue` (`0x00803C98`) under the lock, calls `openfw_hci_tx_kick()` and returns
   **NO_FREE** (F:858–862). Unknown ids are **CONSUMED** and lost (F:851–852).
4. `openfw_hci_tx_next()` (F:557–631): if `tx_state == 1` (idle), takes the queue head (events before ACL), marks
   busy and calls `ext_if->write(buf-1, len+1, openfw_hci_tx_done_cb)` → `eif_write` (F:1692–1715), which parses
   **synchronously** (legacy host: `handle_hci_event`, F:1568–1690, may itself send commands, i.e. re-enter
   `ke_msg_alloc/send` from inside the TASK_HCI handler) and then calls `cb(0)`.
5. `openfw_hci_tx_done_cb` only does `ke_event_set(5)` (F:545–549). In the next scheduler pass,
   `openfw_hci_tx_evt_cb` (F:638–670) clears ev5, sets `tx_state = 1`, pops the sent message and calls
   **`rom_ke_msg_free`** (and, for ACL, `rom_em_buf_rx_free`, §6), then sends the next queued packet.

### 4.2 Ownership rules the ROM imposes

* A message delivered to TASK_HCI belongs to the handler only if it returns non-zero; it must eventually be released
  with `ke_msg_free(header)`. Holding it consumes g_bt_heap (8 KiB total, §8).
* There is **no ROM-side "callback" to complete**. The ROM never waits for the host. It allocates the next message
  and keeps going. Backpressure toward the ROM exists only as (a) g_bt_heap exhaustion (fatal, §3.1) and (b) EM RX
  buffers held by undelivered ACL (§6).
* Handlers run in the context that called `ke_event_schedule`, with the lock released. They may call
  `ke_msg_alloc/send` (legacy does it, F:1590–1683), but must not call `ke_event_schedule` recursively (the open_fw
  scheduler is not re-entrant: F:393–416 reads and dispatches without a guard).

### 4.3 KE ids packed today

CC (`pack_cmd_cmp`): `E079`→0C01, `E07A`→0C03, `E083`→0C13, `E08A`→0C1A, `E094`→0C24, `E0B1`→0C56 (status only);
`E0C0`→1009 (status + BD_ADDR); `E06A..E06D`→040B..040E, `E06F..E071`→042B..042D (status + BD_ADDR) (F:742–755).
CS: `E0C9/E43B`→0409, `E0CA/E43C`→040A, `E43A`→0406 (F:757–761). Events: `E0D2/E456`→03, `E457`→04, `E458`→05,
`E459`→06, `E45A`→08, `E45D`→0B, `E45E`→0C, `E462`→13, `E464`→16, `E465`→17, `E466`→18, `E468`→1B, `E472`→31,
`E473`→32, `E474`→33, `E477`→36, `E478`→38 (F:763–850). ACL RX: `E47D` (F:27, F:728–736). Everything else
(including the `1005`/`1001` replies, matrix row 1) is consumed silently.

## 5. Event-buffer details worth carrying into bt_ctrl

1. ROM reply structs are **naturally aligned C structs**, not wire format: e.g. LM handler ROM `0x41770` builds
   `E095` with `status` at `param[0]` and a u16 at `param[2]` (one pad byte). Every packer must be per-id and
   verified (P:337–338).
2. `E462` (NOCP) is clamped against `param_len` (F:817–833), the only packer that validates length. `bt_ctrl` should
   validate `param_len` for every id (fake ROM: fuzz `param_len`).
3. Events of up to 255 parameter bytes need a 258-byte H4 buffer (matrix §event, last paragraph); in-place repacking
   only works because the header room precedes `param`. `bt_ctrl` copies into its own ring instead.

## 6. ACL data

### 6.1 TX (host → air), `openfw_hci_send_acl` F:958–992

1. Reject `len > 0x3FD` (1021) (F:961). If `pb_bc_flag <= 3`, require `rom_hci_conhdl_type(h) == 1` (ACL) (F:964).
2. `dest = rom_hci_conhdl_to_task(h)` (LC instance, §3.2); `0xFF` → fail (F:967–970).
3. `ke_msg_alloc(0xE426, dest, TASK_HCI, 12)` → `struct hci_acl_data_pkt {u16 conhdl; u8 pb_bc; u8 pad; u16 length;
   u16 pad; u8 *buf_ptr}` (F:62–69, F:971–975).
4. `em = rom_em_buf_tx_alloc()` (F:976). ROM `0x305B8`: pops the **TX pool at `0x0080296C`** (9 buffers × 0x40C B at
   EM `0x40033FA0`, set up by `em_buf_init` ROM `0x3052A`) and returns `block + 12`. **Returns NULL when empty**
   (this one does). Usable payload = 0x40C − 12 = 1024 B.
5. Copy the payload with `bt_memcpy_to_em` (volatile; word stores when co-aligned, otherwise bytes) (F:185–207, F:982).
6. Fill the descriptor (masked handle, PB flag separate from the handle), `ke_msg_send` (F:984–990).
7. Ownership: after `send`, the ke_msg and the EM TX buffer belong to the ROM. The ROM frees the EM buffer after
   baseband acknowledgement and reports progress with `E462` (HCI `13`). The exact timing and count semantics are not
   verified (§11).
8. Legacy flow control = "EM alloc failed → stop and retry next poll" (F:2126–2133). There is no NOCP accounting.

### 6.2 RX (air → host)

1. LC sends `E47D` (`LC_HCI_ACL_DATA_RX`) to TASK_HCI; the param is the same `hci_acl_data_pkt`, with `buf_ptr`
   pointing into an **EM RX buffer** (pool `0x00802970`, 8 × 0x40C B at EM `0x40031F40`, alloc via hook
   `0x00804124` → ROM `0x305D8`, payload at `block + 12`).
2. The handler sets `hci_type = 2`, `hci_hdr_offset = −4`, queues it on `acl_queue` (`0x00803CA0`) and returns NO_FREE
   (F:728–736).
3. TX to the host (F:583–627): builds the 4-byte HCI ACL header **inside EM at `buf_ptr−4..−1`** and the H4 type at
   `buf_ptr−5`, fragmenting by `host_acl_data_pkt_len` (default 0xFFFF = never). `buf_ptr−4..−1` is the 4-byte
   headroom the ROM reserves (`+8..+11` of the pool block). `buf_ptr−5` is byte 7 of the ROM's 8-byte pool
   header (`+0` = owning pool, `+4/+5` = `0xAA,0xBB` marks, ROM `0x30508`). This works only because the ROM does not
   appear to read that byte, and it is not needed by `bt_ctrl`, which copies out.
4. A NULL `buf_ptr` is dropped and its message freed (F:596–603).
5. Release (F:651–668): when the last fragment completes, **`rom_em_buf_rx_free(buf_ptr)`** (ROM `0x305EC`:
   `p−4` → pool free, which routes by the back-pointer, so it works for any pool) **then `rom_ke_msg_free(msg)`**.
   If a handler returns CONSUMED for `E47D`, the kernel frees only the ke_msg: **the EM buffer leaks unless the
   handler frees it first.**
6. Holding `E47D` messages = holding EM RX buffers. With 8 RX buffers, at most 8 ACL packets can be deferred; what the
   baseband does when the pool is empty (flow-stop/NAK vs drop) is unknown (P:162–163, §11).

## 7. IRQ 26

* Vector: `S:46` (weak alias), `S:74` slot 42 = IRQ 26; `IRQ_BT = 26` (G:164). Registers: `RWBT_INTCNTL`
  `0x4002000C`, `RWBT_INTSTAT` `0x40020010`, `RWBT_INTACK` `0x40020018` (G:702–707).
* Top half `RWBT_IRQHandler` (F:241–251, section `.ramtext`, not `.fast_ramtext` as P:60 asks): `stat = INTSTAT;
  if 0 return; INTACK = stat; g_bt_irq_pending |= stat; g_bt_irq_count++`. **Ack happens before service**; nothing
  ROM runs in handler mode.
* Bottom half `bt_irq_bh_poll` (F:274–291), thread mode, called first in every `bt_xrec_poll` (F:2104): atomic
  fetch-and-clear under raw PRIMASK, then
  * bit 0 → `rom_rwbt_cscnt_isr()` (ROM `0x2F3A4`, clock/slot-counter interrupt),
  * bit 1 → `rom_rwbt_rx_isr()` (ROM `0x2F388`: samples CLKN, calls LD RX at ROM `0x36544`),
  * bit 10 → `rom_ke_event_set(1)` (kernel timer). Confirmed: `ke_timer_hw_set` (ROM `0x30B58`) writes the target to
    `0x400200E0`, acks `0x400` in INTACK and sets **INTCNTL bit 10**; NULL clears it.
  * **All other bits are acked and silently ignored.** INTCNTL is initialised to `0xFFFF0003` (F:1283); the meaning of
    bits 16–31 is unknown.
* Then `bt_ke_event_schedule()` (F:2105) runs to quiescence (≤ 64 dispatches, highest event bit first, F:393–416;
  ev0 handles one message per dispatch, §3.4) and again after ACL TX (F:2134–2136).
* Coalescing: several interrupts of the same bit between two polls collapse into one ROM call. Legacy tolerates this
  at idle/discoverable (silicon, BT_HW_RESULTS Exp 0: 32 021 IRQs in 20 s ≈ **1.6 kHz ≈ one per 625 µs slot**, HCI
  ready). That rate matters for M1: a task notification per IRQ is 1600 context switches/s at idle.
* The ROM ISR bodies and `ke_time` (ROM `0x30B08`) **busy-wait on `0x4002001C`** (write 1, poll until 0: CLKN sample)
  with no timeout; time is `(REG(0x40020020) >> 4) & 0xFFFF`. They require RWBTEN and the BT clock. A fake must
  model this as instantaneous.
* Plan ambiguity: P:239 "bottom half loops until the status register reads clear". With ack-in-top-half the hardware
  register already reads clear; the loop must be over the software pending word (and per-bit counters belong in the
  top half). Re-reading INTSTAT in the bottom half would only see interrupts not yet taken.

## 8. Locks, heap and execution context

### 8.1 `bt_irq_lock` / `bt_irq_unlock` (F:253–272)

Nested PRIMASK: saves PRIMASK on the outermost entry (`g_irq_nest`, global), `cpsid i`. Masks **every** interrupt
(audio DMA, USB, SysTick, PendSV). Regions:

| Region | Lines | Bound |
|---|---|---|
| ROM critical sections (msg queue push/pop, event clear, task create, EM pool alloc/free, timers) via `0x3DE80/8C` | ROM | short, unmeasured |
| `bt_malloc` first-fit walk **with forward coalescing, whole walk under lock** | F:329–369 | up to ~512 blocks over 8 KiB; worst case unmeasured |
| `bt_free` | F:384–387 | O(1) |
| event clear / set | F:408–410, F:421–423 | O(1) |
| H4 TX dequeue / complete, queue pushes | F:567–626, F:645–658, F:731–733, F:858–860 | O(1) |
| BH fetch-and-clear (raw `mrs/cpsid/msr`, not the nest counter) | F:277–280 | O(1) |

Rule for M1 (P:80–81): no FreeRTOS call while `g_irq_nest > 0`. Every hook the ROM calls (malloc/free/rand/
event_set/lock) may be entered with the lock already held by the ROM (ROM `ke_msg_send` releases before
`ke_event_set`, but other callers are unaudited), so hooks must stay lock-safe and RTOS-free.

### 8.2 `g_bt_heap` (F:233–239, F:311–388)

* `static uint32_t g_bt_heap[2048]` (8 KiB, main `.bss`, P:101). Blocks: 8-byte header `{size_flags (bit0 = used),
  tag = type}`, 8-byte aligned, minimum 16 B, split if the remainder is ≥ 16 B. Free = clear bit 0 (no validation,
  no double-free detection, no back-coalescing). The `type` argument from ROM (ke_msg = 1) is stored and unused.
* Consumers: every `ke_msg` (commands, events, ACL descriptors), every `ke_timer` (`ke_timer_schedule` frees expired
  timers via `ke_free`, ROM `0x30BCC`), and whatever else the ROM `ke_malloc`s. EM buffers are **not** here.
* Failure: `bt_malloc` returns 0, and the ROM does not check it (§3.1). **Rule: g_bt_heap must never run out.** `bt_ctrl`
  must bound outstanding messages (prompt copy+free, a bounded number of in-flight commands and ACL descriptors) and the
  real `bt_rom_if` malloc hook should record the high-water mark and turn an exhaustion into an immediate BT fault
  (`bt_stop` + IRQ mask, P:201–205) instead of returning 0.

### 8.3 Context requirements

* **Single caller.** The ROM kernel's globals are protected only for queue operations; task handlers, LD, the ISR
  bodies and the scheduler are not re-entrant. Every ROM entry (BH, scheduler, alloc/send/free, EM alloc/free,
  conhdl lookups, init/stop) must come from one context: legacy = app loop (M:1287–1289); M1 = `bt_ctrl` (P:65,
  P:77).
* The "ISR" ROM bodies run fine in thread mode (legacy, silicon).
* Message handlers may send messages (§4.2). `ke_task_schedule` calls them with the lock released.
* Stack: ROM frame sizes are unknown; `rwbt_cscnt_isr` alone pushes 9 registers + 0x2C B. The 2 KB `bt_ctrl` stack
  (P:104) needs a silicon watermark.
* No FPU use in ROM BT code, so the `bt_ctrl` task needs no FP context.
* Ordering: RF calibration before EM wipe (§2.1). TASK_HCI is created before any ROM task can send to it.
  Restarting invalidates all heap/EM pointers.

## 9. What bt_ctrl needs from the ROM (requirements summary)

1. start(bd_addr) / stop() with the full §2 sequence, IRQ left masked until `bt_ctrl` enables it.
2. A TASK_HCI upcall `(id, param, param_len, src)` that runs inside the scheduler, with CONSUMED/KEEP semantics.
3. Command submission: alloc (zeroed), set payload, send; `conhdl → LC task` mapping before alloc.
4. ACL submission: EM TX alloc (fallible), copy into EM, descriptor message.
5. ACL RX release: EM RX free + msg free.
6. IRQ: top-half notify hook, fetch-and-clear of pending bits, bottom-half service, enable/disable.
7. A scheduler step that reports whether work remains (event bits).
8. Heap observability (free, high-water mark, failure latch) and lock depth (for the PRIMASK assert).
9. RF calibration as a platform call, so the fake can skip it.

## 10. `bt_rom_if_t` — proposed minimal table

```c
/* include/openfw_bt_rom_if.h (proposed) */
typedef enum { BT_ROM_MSG_CONSUMED = 0, BT_ROM_MSG_KEEP = 1 } bt_rom_msg_ret_t;

typedef struct {
    void *ctx;
    /* TASK_HCI default handler. Runs only inside run(), in bt_ctrl context, lock NOT held.
     * CONSUMED: the ROM frees the ke_msg after return; `param` is dead (for E47D the callee must
     *           already have called em_rx_free(buf_ptr)).
     * KEEP:     bt_ctrl owns the message and must later call msg_free(param) (and em_rx_free for E47D).
     * May call msg_alloc/msg_send/em_* (not run()). Must not block or call FreeRTOS. */
    bt_rom_msg_ret_t (*on_hci_msg)(void *ctx, uint16_t id, uint8_t *param,
                                   uint16_t param_len, uint16_t src_id);
    /* Called from the IRQ-26 top half after the status was latched (handler mode,
     * PRIMASK clear): real impl -> vTaskNotifyGiveFromISR(bt_ctrl). */
    void (*on_irq)(void *ctx);
    /* Fatal ROM-environment fault (g_bt_heap exhausted, unknown-task send, bad free).
     * May be called with the lock held: set flags only. */
    void (*on_fault)(void *ctx, uint32_t code);
} bt_rom_upcalls_t;

typedef struct {
    uint32_t heap_free, heap_min_free, heap_largest, alloc_fail;
    uint32_t irq_count[32];          /* per-status-bit counts, from the top half */
    uint32_t irq_unhandled;          /* OR of bits acked but not serviced */
} bt_rom_stats_t;

typedef struct bt_rom_if {
    /* ---- lifecycle (bt_ctrl context only) ---- */
    uint32_t (*rf_calibrate)(void);                                   /* bt_rf_init_calibrated(0x53, 32 MHz) */
    int      (*start)(const uint8_t bd_addr[6], const bt_rom_upcalls_t *up); /* §2 steps 2-10, IRQ stays masked */
    void     (*stop)(void);       /* bt_xrec_stop() HW part; every param/EM pointer becomes invalid */

    /* ---- IRQ 26 ---- */
    void     (*irq_enable)(int on);           /* NVIC enable/disable (priority set by bt_ctrl init) */
    uint32_t (*irq_take)(void);               /* atomic fetch-and-clear of latched status bits */
    uint32_t (*irq_service)(uint32_t bits);   /* bit0 cscnt_isr, bit1 rx_isr, bit10 ke_event_set(1);
                                                 returns bits it did not handle */
    /* ---- kernel ---- */
    uint32_t (*events)(void);                 /* ke_event field (0x008041E4 & 0x3F); 0 = idle */
    uint32_t (*run)(uint32_t max_steps);      /* <= max_steps ke_event dispatches; returns events() */

    /* ---- messages (all pointers are PARAM pointers) ---- */
    uint8_t *(*msg_alloc)(uint16_t id, uint16_t dst, uint16_t src, uint16_t len); /* zeroed; NULL only if the
                                                 implementation's own budget check refuses (never ROM NULL) */
    void     (*msg_send)(uint8_t *param);     /* ownership -> ROM */
    void     (*msg_free)(uint8_t *param);     /* for KEEP'd messages; impl passes param-0x10 to ROM */
    uint16_t (*conhdl_to_task)(uint16_t conhdl);  /* (link<<8)|0x39 or 0xFF */
    uint8_t  (*conhdl_type)(uint16_t conhdl);     /* 1 = ACL (0x80..0x86) */

    /* ---- exchange memory ---- */
    uint8_t *(*em_tx_alloc)(void);            /* NULL when the 9-buffer pool is empty */
    void     (*em_write)(uint8_t *em, const uint8_t *src, uint16_t len); /* volatile copy */
    void     (*em_read)(uint8_t *dst, const uint8_t *em, uint16_t len);
    void     (*em_rx_free)(uint8_t *em);      /* the buf_ptr of an E47D (or an unused TX buffer) */

    /* ---- diagnostics ---- */
    uint32_t (*lock_depth)(void);             /* g_irq_nest, for assert(lock_depth()==0) before RTOS calls */
    void     (*stats)(bt_rom_stats_t *out);
} bt_rom_if_t;

extern const bt_rom_if_t bt_rom_real;   /* src/bt_rom_real.c: today's bt_hci.c init/hooks/ISR, verbatim */
/* tests: const bt_rom_if_t bt_rom_fake;  tests/bt_fake_rom.c */
```

Notes on the table:

* No `ext_if`/`hci_tl_env` and no `ke_event_set(5)`: `bt_ctrl` copies in `on_hci_msg` and returns CONSUMED, or keeps
  ACL when the RX ring/host credits are exhausted (P:149–153) and frees later. The real implementation still has
  to zero and own `0x00803C28..` because the ROM uses that base (§2.2).
* `msg_send_basic` and `cmd_cmp_send` are omitted: `msg_alloc(len 0)` + `msg_send` and a local synthetic reply
  cover them. Synthesised replies are built by `bt_ctrl` directly into its event ring; they never go through the ROM.
* `msg_alloc` in the real impl checks a byte budget against g_bt_heap free space *before* calling the ROM (because the
  ROM cannot fail), and `on_fault` covers ROM-internal allocations that hit the wall.
* The ISR stays in the real implementation (`.fast_ramtext`). Only `on_irq` crosses the boundary.
* **Fake ROM contract** (minimum model): a FIFO of messages; `run()` delivers one message per ev0 dispatch to
  `on_hci_msg` or to fake LM/LC handlers keyed by id; command ids from §3.2/§4.3 answered with configurable CC/CS
  payloads (aligned ROM layout, including pad bytes) sent to `src_id`; TX pool 9, RX pool 8 (EM as host memory);
  ACL TX consumed after a configurable delay with `E462`; injectable `E47D`; heap budget with exhaustion →
  `on_fault`; `msg_free` on a non-allocated or param−0x10-misaligned pointer → test failure; message to an
  unregistered task → test failure (mirrors the ROM HardFault); `irq_take/service` driven by the test clock; checks that
  every entry point is called from one thread with `lock_depth()==0` at upcall boundaries.

## 11. Open questions for Exp 1 (hardware only)

1. **INTSTAT/INTCNTL bits** other than 0, 1, 10: which fire (idle, page scan, connection, SSP, sniff), and what bits
   16–31 of `0xFFFF0003` enable. Today they are acked and dropped (F:282–290).
2. **Bottom-half latency tolerance**: the largest IRQ→`cscnt/rx_isr` delay the ROM survives (idle, connected,
   streaming ACL) before link loss, missed RX or AFH failure, and the IRQ rate under load (1.6 kHz idle).
3. **Worst-case duration** of one `run()` step / ROM handler and of `irq_service()` (P:205 asserts ≤ 1 ms).
4. **PRIMASK hold times**: `bt_malloc` walk and the ROM critical sections, against audio DMA (3 × 128 ms ring) and USB.
5. **g_bt_heap high-water mark** under connection + SSP + sustained ACL. Is 8 KiB enough when `bt_ctrl` frees promptly?
6. **EM pools**: confirm 9 TX / 8 RX (static reading of ROM `0x3052A`) and that pool B is used only for ACL TX. Find out
   what the baseband does when all 8 RX buffers are held (flow-stop vs drop). Can events still overtake (P:162)?
7. **TX completion**: when the ROM frees TX EM buffers, and whether `E462` counts match packets (sustained-ACL NOCP,
   P:339); what `1005` should advertise.
8. **Reply ids and layouts** for unmapped commands (`1005`, `1001`, `1002`, `0C14`, `1004`, …): first by
   disassembly (P:337), then verified on silicon stage A/B. Does `0C01` through hook `0x00803BBC[7]` change ROM
   behaviour, or only the open_fw copy at `0x00803D04`? What do hooks `[0..6]` filter?
9. **Clean restart**: does `stop()`+`start()` without a power cycle (real Reset, re-enable) return the controller to a
   fully working state, repeated N times?
10. **ROM stack depth** (watermark of the `bt_ctrl` stack during connection/SSP).
11. Is the 0x80..0x86 handle range stable, and does `conhdl_to_task` ever return a valid task for SCO handles
    (`type ≤ 2` branch, ROM `0x30774`)?
12. Has the legacy ACL/SPP path ever run on silicon? BT_HW_RESULTS records bring-up and HCI ready only. ACL is "not
    yet covered".
13. NVIC priority bits implemented (P:339) and the effect of moving IRQ 26 from priority 0 to 0x80..0xE0.

## 12. Discrepancies found (code/header/plan vs ROM)

| Item | Finding |
|---|---|
| H:105 `rom_hci_cmd_cmp_evt_send(..., uint8_t len)` | 4th arg is the **status**; `param_len` is fixed at 1 (ROM `0x32E40`). |
| F:933, F:973 `if (!buf)` after `rom_ke_msg_alloc` | Dead: the ROM never returns NULL; exhaustion writes near address 0 and returns `0x10`. |
| F:936 `bt_memzero` | Redundant: `ke_msg_alloc` zero-fills. |
| `ke_msg_free` vs `ke_msg_send` | Header vs param pointer (F:944 vs F:952). Easy to get wrong; `bt_rom_if_t` normalises. |
| P:148 "ROM `eif_write` callback" | `eif_write` is open_fw's transport. The ROM boundary is the TASK_HCI handler return plus frees. |
| P:239 "loops until the status register reads clear" | The register is acked in the top half; loop on the software pending word. |
| P:60 ISR in `.fast_ramtext` | Today it is `.ramtext` (F:241). |
| P:57–59 IRQ 26 priority | Never set today (default 0). |
| Unknown TASK_HCI ids | Consumed silently (F:851–852); `bt_ctrl` must count and log them. |
