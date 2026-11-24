# `bt_ctrl` HCI opcode policy table

Companion to `docs/BLUETOOTH_BLUEDROID_PLAN.md` (v18), section M1. It lists every HCI command that the
vendored Bluedroid can send in the open_fw build (Classic only: GAP, SDP, L2CAP, RFCOMM/SPP, AVDTP/A2DP source,
AVRCP, SSP), and for each one: the policy class, what `bt_ctrl` does with it, and the exact initial reply.
This is a static analysis. Rows marked **G-x** below need hardware Exp 1 results before their action is final.

Sources: `docs/BT_HCI_MATRIX.md` (send sites, stock KE candidate ids), `open_fw/src/bt_hci.c` (ROM paths that
have a packer today), and these Bluedroid files under `open_fw/third_party/bluedroid/host/bluedroid`:
`device/controller.c` (start-up order), `hci/hci_packet_parser.c` (minimum lengths for start-up replies),
`hci/hci_layer.c` (CC/CS matching), `stack/btu/btu_hcif.c` (runtime CC/CS dispatch), `stack/hcic/hcicmds.c`
(encoders), and `stack/btm/*`. Configuration: `bt_port/sdkconfig.h`, `bt_port/bt_port_prefix.h`,
`BT_BLUEDROID_INIT_CONFIG_DEFAULT()` (`ssp_en = true`, `sc_en = false`).

**Which opcodes are sendable:** the `btsnd_hcic_*` encoders and `make_*` factory functions present in the
`BT=1` image (`--gc-sections`, with the real entry points kept by `.bt_link_roots`), checked with `nm` on
`/tmp/sl6801_openfw_bt/open_fw.elf`. Each one was then traced to its call sites. Encoders that are not in the
image cannot be sent (§6). `bt_ctrl` host tests should repeat this `nm` check, so that a change in the linked
set fails the build (§7, item 10).

## 1. Notation

- **Opcode** is hex, `OGF<<10 | OCF`, little-endian on the wire.
- **Class** `M` = mandatory-success: never REJECT, reply status `0x00`. `O` = optional: may fail.
- **Action**
  - `FWD` = forward to the ROM KE handler and pack the ROM's reply.
  - `SYN` = `bt_ctrl` builds the reply itself.
  - `REJ` = reply status `0x01` (Unknown HCI Command) with the full, correctly sized reply.
  - `LOCAL` = handled inside `bt_ctrl`, no reply.
  - `X → Y` = action X once the Exp 1 gate passes; Y until then, or if the gate fails.
- **Reply**
  - `CC(n)` = Command Complete with `Parameter_Total_Length = n`, laid out as
    `Num_HCI_Command_Packets:u8, Opcode:u16, Status:u8, return parameters`, so `n = 4 + return bytes`.
  - `CS` = Command Status, always `n = 4`: `Status:u8, Num_HCI_Command_Packets:u8, Opcode:u16`. On success a
    terminal event follows (`→ 0x03` means Connection Complete, and so on).
  - In the layout columns, `s` = status, `h` = `Connection_Handle:u16`, `a` = `BD_ADDR[6]` in wire order.
- **Exp 1** (plan M5)
  - `—` = no hardware dependency.
  - `V-A/B/C` = the action is fixed; the stage only verifies the golden vector.
  - `G-A/B/C` = the action depends on that stage. A = reset and read-only commands, B = verified writes,
    C = procedures with a peer.
- **Send-site abbreviations:** `C` = `device/controller.c`, `D` = `stack/btm/btm_devctl.c`,
  `I` = `stack/btm/btm_inq.c`, `A` = `stack/btm/btm_acl.c`, `S` = `stack/btm/btm_sec.c`, `P` = `stack/btm/btm_pm.c`,
  `U` = `stack/btu/btu_hcif.c`, `F` = `open_fw/src/bt_hci.c`.

## 2. Policy rules (apply to every row)

1. **Exactly one initial reply per command, except `0C35`.** Every reply that `bt_ctrl` builds (SYN or REJ) uses
   `Num_HCI_Command_Packets = 1`, matching the ROM packers (`F:676,691`). `bt_ctrl` admits one command at a time
   to the ROM, so 1 is always the truth.
2. **CC versus CS follows the HCI specification for the opcode, never convenience.** All start-up commands in
   `controller.c` are awaited through futures. In `hci_layer.c:600–641`, a Command Status runs only
   `command_status_cb`, which is NULL for futured commands, so a CS reply to a start-up command stalls
   `start_up()` forever.
3. **REJ replies keep the full reply length.** `read_command_complete_header()` checks
   `parameter_length >= 4 + minimum` **before** it looks at the status (`hci_packet_parser.c:283,299`), and
   `btu_hcif` handlers read their fixed fields. A REJ fills the reply as follows:
   - `s = 0x01`
   - if the command's first parameter is `h` or `a`, that value is echoed at the same position in the reply
     (`echo` column in §8)
   - every other return byte is zero.
4. **Status-only replies are never sent for read commands.** For example, a 4-byte CC for `1005` would assert in
   `parse_read_buffer_size_response`.
5. **The table is closed.** An opcode not in §8 gets `CS` with `s = 0x01` and increments an
   `unknown_opcode[opcode]` counter. Host tests fail if this happens in any scenario. Bluedroid accepts a CS for
   any pending BTU command (`hci_layer.c:600–641`), and every futured command is in the table, so this default
   cannot stall start-up. Nothing is ever silently dropped (plan M1).
6. **SYN is allowed in class O only when it is exact**, meaning the reply matches what a real controller in our
   state would return (`0C05`, `0C12`). In every other case class O is FWD, or REJ when no verified ROM path
   exists.
7. **FWD with a nonzero ROM status is passed through unchanged** for class O. For class M rows, the ROM status
   is checked against the golden vectors in Exp 1. A class M FWD reply with a nonzero status on target counts as
   a `bt_ctrl` invariant violation (BT fault, plan M1 recovery ladder). It is never rewritten to `0x00`.
   **One exception (2026-11-24, silicon bench):** `0C1A` answered `0x0C` is a defined run-time refusal, not a broken
   controller: the ROM refuses page scan (`02`/`03`) while more than one ACL link is up (`BT_ROM_HCI_MAP.md` §11).
   It is passed through, counted (`scan_refused`) and noted for the session log. The session sends its scan mode
   again every `BT_XREC_SCAN_RETRY_MS` (3 s) while refused, at once when a link goes or the wanted mode changes, and
   fails closed: a refused write without inquiry scan, after a discoverable one was sent, is followed by scan off
   (`0x00`, never refused), so a closing window cannot leave the device discoverable. Any other `0C1A` status is
   still a fault.
8. **Dead handles.** After a successful Disconnection Complete for handle `h` (and until a successful Connection
   Complete reuses it), an LC-destined FWD command for `h` is answered locally with `0x02` (Unknown Connection
   Identifier, `cmd_dead_handle`) and host ACL for `h` is dropped (`acl_tx_dead_handle`). Reason: the ROM can still
   map `h` to a dying LC instance whose handlers consume commands silently (`BT_ROM_HCI_MAP.md` §1.2 (c)) and
   whose ACL is never completed by a NOCP, which would hold one of the `1005` credits for good.
9. **Forwarded-command deadline.** With a clock (`bt_ctrl_set_clock()`, set by `bt_ctrl_rtos.c`), a class O FWD
   command that has no ROM reply after `BT_CTRL_FWD_DEADLINE_MS` = 7 s is answered locally with `0x22` in its
   reserved slot (`fwd_deadline`); the ROM's late reply for that opcode is consumed once (`fwd_late_reply`), never
   delivered twice. 7 s is just under Bluedroid's own 8 s command timeout (`hci_layer.c` `COMMAND_PENDING_TIMEOUT`),
   which would otherwise post a BT fault and restart the whole stack; so the deadline only turns a stack restart into
   one failed command. Class M commands keep waiting: their failure must reach the recovery ladder.

## 3. Synthesized content

### 3.1 `1005` Read Buffer Size → `CC(11)`

| Field | Value | Reason |
|---|---|---|
| `s` | `0x00` | |
| `HC_ACL_Data_Packet_Length:u16` | `0x03FD` (1021) | RW-BT DH5/3-DH5 payload. Already enforced on transmit (`F:961`, `len > 0x03FD` rejected). |
| `HC_SCO_Data_Packet_Length:u8` | `0x00` | No SCO over HCI (`BTM_SCO_HCI_INCLUDED = FALSE`). |
| `HC_Total_Num_ACL_Data_Packets:u16` | `BT_CTRL_H2C_ACL_CREDITS`, **provisionally 4** | Must not exceed the ACL packets `bt_ctrl` can hand to the ROM immediately, which is the number of free EM TX buffers (`rom_em_buf_tx_alloc`). `FIRMWARE_ANALYSIS.md` §14 says 9, from static analysis only. Each packet counted here holds at most one EM TX buffer until its NOCP arrives. So `N ≤ EM TX pool` guarantees that every ACL entry in the unified TX ring can be drained at once, which is plan M1's invariant. A failed `rom_em_buf_tx_alloc` with a credit available is a `bt_ctrl` invariant violation. **G-C:** raise N toward the verified pool size only after the stage-C sustained-ACL NOCP accounting run. Static assert `N <= BT_CTRL_EM_TX_BUFS`. |
| `HC_Total_Num_SCO_Data_Packets:u16` | `0x0000` | |

Bluedroid reads all 7 bytes after the status (`hci_packet_parser.c:48–53`).

### 3.2 `0C31` / `0C33` / `0C35`: controller-to-host credit window, owned by `bt_ctrl`

- **`0C31` Set Controller To Host Flow Control:** `SYN CC(4)`. The command parameter must be `0x01` (ACL on,
  `C:264`). It arms the `bt_ctrl` RX dispatcher's credit gate. It is not forwarded: the ROM does no C2H gating of
  its own (stock candidate `E03D` is unverified), and gating in two places would double-count.
- **`0C33` Host Buffer Size:** `SYN CC(4)`.
  - Bluedroid sends: `ACL_len:u16 = 1021` (`L2CAP_HOST_FC_ACL_DATA_LEN`, patch 0004), `SCO_len:u8 = 0xFF`,
    `ACL_num:u16 = 4` (`L2CAP_HOST_FC_ACL_BUFS`), `SCO_num:u16 = 10`.
  - `bt_ctrl` enforces **4 × 1021**: `c2h_len = min(ACL_len, 1021)`, `c2h_credits = min(ACL_num, 4)`. The SCO
    fields are ignored.
  - A value other than 1021/4 still gets `s = 0x00` (class M) but increments a config-mismatch invariant counter.
    Host tests assert it stays 0.
  - `bt_ctrl` fragments controller-to-host ACL to at most `c2h_len` and spends one credit per H4 ACL packet. The
    ACL ring is statically asserted to hold `4 × (1021 + 5)` bytes.
- **`0C35` Host Number Of Completed Packets:** `LOCAL`, **no reply**.
  - It does not consume or require a command credit and is never forwarded to the ROM.
  - Wire format from `hcicmds.c:1322–1352`: `n:u8`, then `n` interleaved pairs `(h:u16, count:u16)`;
    `Parameter_Total_Length = 1 + 4n`.
  - For each pair: `outstanding[h] -= count`. If `count` is more than is outstanding, clamp at 0 and increment an
    invariant counter.
  - Recompute the free credits and kick the RX dispatcher.
  - Malformed input (length mismatch, unknown handle) is counted and ignored, never answered.
  - The host side relies on the `hci_layer.c` NOACK-queue patch (plan M1).

### 3.3 `1001` Read Local Version Information → `CC(12)`

`s = 0x00, HCI_Version = 0x06, HCI_Revision = 0x0000, LMP_Version = 0x06, Manufacturer_Name = 0x0060
(RivieraWaves), LMP_Subversion = BT_CTRL_LMP_SUBVER`.

These values are packed byte by byte, never from a native struct (`hci_packet_parser.c:62–68` reads 8 bytes).

`Manufacturer_Name` **must not be `0x000F` (Broadcom)**. `BTM_IS_BRCM_CONTROLLER()` would then enable the
Broadcom vendor command `FC57` (`l2c_utils.c:2782–2794`) on every A2DP priority change.

**V-A:** check `LMP_Version`, `Manufacturer_Name` and `LMP_Subversion` against the constants the ROM sends in
`LMP_version_res`, so that HCI and air agree. Until then `LMP_Subversion = 0x0000`. Only the version is
cosmetic; the manufacturer rule above is not.

### 3.4 `1009` Read BD_ADDR → `CC(10)`

`s = 0x00, BD_ADDR[6]` in wire order. The address is the identity that `bt_ctrl` programmed into the ROM at
reset (plan M1 "Local identity"). `bt_ctrl` owns that value, so a SYN reply cannot disagree with it. **V-A:**
the stage-A probe forwards `E05D → E0C0` (`F:748,891`) once and compares the result byte for byte.

### 3.5 `1002` Read Local Supported Commands → `CC(68)`

`s = 0x00`, then 64 bytes. The bitmap is computed from §8: a bit is set if and only if the row's **effective**
action (after the Exp 1 gates) is FWD, SYN or LOCAL.

Bluedroid reads only two bits for Classic:

| Octet / mask | Command | Effect |
|---|---|---|
| 2 / `0x40` | `041C` Read Remote Extended Features | **Set** (FWD since the phone-path promotion). When clear, `041C` is never sent (`A:1286–1287`) and every peer is treated as legacy (no page 1, `S:6249–6258`). |
| 45 / `0x80` | `0C84` Set Min Encryption Key Size | **Always clear.** Then `0C84` is never sent (`D:1027`). |

All other bits are informative and are still kept truthful. Octets 25–28 (LE) are zero.

### 3.6 `1004` Read Local Extended Features → `CC(14)` = `s, page:u8, max_page:u8, features[8]`

`max_page = 0x01` in every reply, so Bluedroid requests pages 0 and 1 only (`C:349–361`), and page 2 is never read.

**Page 0** is the ROM's LMP page 0 (`F:1273–1274`: `7F FE CD FA DB FD 7B 87`) AND-masked as follows:

| Byte | ROM | Synth | Bits cleared (bit number in the 64-bit mask) | Why |
|---|---|---|---|---|
| 0 | `7F` | `1F` | 5 role switch, 6 hold, 7 sniff (already 0) | Plan: no role switch, no sniff. Hold has no use. Clearing them makes `080B`/`0801`/`0803`/`0804` unreachable (`A:812`, `P:220–222`). |
| 1 | `FE` | `06` | 8 park (already 0), 11 SCO, 12 HV2, 13 HV3, 14 µ-law, 15 A-law | Park unreachable. No SCO in the build. |
| 2 | `CD` | `C4` | 16 CVSD, 19 transparent sync data | No SCO. |
| 3 | `FA` | `0A` | 28 interlaced inquiry scan, 29 interlaced page scan, 30 RSSI with inquiry results, 31 EV3 | 28/29: Bluedroid then never sends `0C43`/`0C47` (`I:293–295,333–335`). 30: no `0C45` at reset (`D:437`), so inquiry stays in standard mode and only event `0x02` needs a packer. Re-enable after G-C. 31: no eSCO. |
| 4 | `DB` | `98` | 32 EV4, 33 EV5, 38 LE supported (template quirk, `FIRMWARE_ANALYSIS.md` §14) | No eSCO, no LE. |
| 5 | `FD` | `1D` | 41 sniff subrating (already 0), 45/46/47 EDR eSCO | |
| 6 | `7B` | `19` (or `18`) | 49 simultaneous LE/BR (quirk), 53 erroneous data reporting, 54 non-flushable PB flag. Bit 48 EIR stays set **only if `0C52` passes G-B**; otherwise the byte is `18`. | 54 clear: L2CAP never sends PB = `00` packets and Enhanced Flush stays unused (`l2c_utils.c:992`, `l2c_api.c:2409`). Re-enable only after Exp 1 C proves both in the ROM. 48 clear: BTA never sends `0C52` (`I:2362`). |
| 7 | `87` | `85` | 57 inquiry TX power level | Unused by the host; nothing proves the ROM implements it. |

Bits kept:

- BR 3/5-slot, encryption, slot offset, timing accuracy (bits 0–4)
- power control and channel-quality bits 9, 10, 18
- flow-control lag, broadcast encryption (22, 23)
- EDR ACL 2 Mb/s (25), enhanced inquiry scan (27)
- AFH (35, 36, 43, 44), 3/5-slot EDR ACL (39, 40)
- pause encryption (42), SSP (51), encapsulated PDU (52)
- LSTO-changed event (56), enhanced power control (58), extended features (63)

The EDR bits 25/39/40 decide which packet types the host requests in `0405`/`040F` (`D:325–349`). **V-C:** if
the ROM link layer does not deliver EDR ACL on air, clear them. **No Secure Connections:** page 2 is never
offered, `sc_en = false`, so `0C7A` is never sent (`C:364–369`).

Resulting page 0 (EIR variant): **`1F 06 C4 0A 98 1D 19 85`**. Without EIR: **`1F 06 C4 0A 98 1D 18 85`**.

**Page 1:** `features[0] = 0x01` if the last `0C56` forwarded with `s = 0x00` enabled SSP, else `0x00`. Bytes
1–7 are `0x00`: no LE host, no SC host.

**Page ≥ 2** (never requested): `CC(14)` with `s = 0x12`, the requested page echoed, `max_page = 0x01`, and zero
features. The parser accepts it because the length check passes (`hci_packet_parser.c:117–127`).

**LMP alignment (needs a decision, §7 item 3):** the same masked page-0 value should be written to the ROM's
LMP feature table at `0x00802C04/08`. That is the table peers see; today `F:1273–1274` writes the unmasked
value. If the ROM keeps advertising role switch at LMP level, a peer can start a role switch on its own, and
the resulting event `0x12` Role Change has no packer.

### 3.7 `0C14` Read Local Name → `CC(252)`

`s = 0x00, Local_Name[248]`, taken from a `bt_ctrl` cache:

- `bt_ctrl` Reset sets the cache to 248 zero bytes and writes no name to the ROM.
- A `0C13` whose ROM reply has `s = 0x00` copies its 248 parameter bytes into the cache.

This read is the BTA enable barrier (`bta_dm_act.c:578–585`; `D:589–597`; `U:1308–1310`): the callback must
always arrive, including on first boot before BTC sets a name. Size check: the H4 event is
`1 + 2 + 252 = 255` bytes, which fits a 260-byte event-ring slot.

### 3.8 `0C03` Reset → `CC(4)`

`bt_ctrl` performs the real controller reset: ROM reinit, all `bt_ctrl` state cleared, including credits, the
name cache, the SSP flag and the per-handle `outstanding[]`. It checks that the reset completed, then replies
`s = 0x00`. A reset failure is a BT fault (plan M1 "Reset"). The same path is used on re-enable.

## 4. Mandatory-success opcodes (class M)

The plan's list is `0C03, 1005, 0C31, 0C33, 0C35, 1001, 1009, 1002, 1004 (pages 0–1), 0C56, 0C01, 0C14` plus
"the BTA setup writes". The plan does not list the setup writes; the code shows them to be `0C18`, `080F`,
`0C24`, `0C13` and `0C1A` (§7 item 2). `0C0A`, `0C43`, `0C45`, `0C47` and `0C52` would also be setup writes, but
the feature masks in §3.6 make them unreachable or conditional, so they are class O.

| Opcode | Command | Send site (order) | Action | Reply | Return parameters | ROM path | Exp 1 |
|---|---|---|---|---|---|---|---|
| `0C03` | Reset | `C:251` (1st) | SYN after real reset | `CC(4)` | `s=00` | `bt_ctrl` reset sequence (legacy fake CC `E07A`, `F:912–915`) | V-A |
| `1005` | Read Buffer Size | `C:256` (2nd) | SYN | `CC(11)` | `s=00, acl_len=03FD, sco_len=00, acl_num=N(4), sco_num=0000` (§3.1) | none (stock `E05C` has no reply packer) | G-C (raise N) |
| `0C31` | Set Controller To Host Flow Control | `C:264` | SYN | `CC(4)` | `s=00` | none (local gate) | — |
| `0C33` | Host Buffer Size | `C:279–286` | SYN, enforce 4 × 1021 | `CC(4)` | `s=00` (§3.2) | none (local) | — |
| `1001` | Read Local Version Information | `C:292` | SYN | `CC(12)` | `s=00, 06, 0000, 06, 0060, subver` (§3.3) | none (stock `E058` has no reply packer) | V-A |
| `1009` | Read BD_ADDR | `C:296` | SYN | `CC(10)` | `s=00, a` (§3.4) | cross-check `E05D→E0C0` | V-A |
| `1002` | Read Local Supported Commands | `C:300` | SYN | `CC(68)` | `s=00, bitmap[64]` (§3.5) | none | — (bits follow gates) |
| `1004` | Read Local Extended Features, page 0 | `C:310` | SYN | `CC(14)` | `s=00, 00, 01, 1F 06 C4 0A 98 1D 19/18 85` | none (stock slot zero) | G-B (EIR bit), V-C (EDR bits) |
| `0C56` | Write Simple Pairing Mode (`01`) | `C:331` (page-0 bit 51 and `ssp_en`) | FWD | `CC(4)` | `s` | `E04F → E0B1` (`F:747,888`) | V-B |
| `1004` | Read Local Extended Features, page 1 | `C:351` | SYN | `CC(14)` | `s=00, 01, 01, ssp?01:00, 00×7` | none | — |
| `0C01` | Set Event Mask | `C:477` (last start-up command) | FWD | `CC(4)` | `s` | `E01C → E079` (`F:742,883`); the ROM hook ORs mandatory bits (`F:481–487`) | V-B |
| `0C18` | Write Page Timeout (`0x2000`) | `D:230` (reset), `bta_dm_act.c:572` | FWD → SYN | `CC(4)` | `s` | `E02A` (2 B) → CC `E088`. SYN is exact while the value is `0x2000`, the HCI default; G-A confirms the ROM default after reset. | G-B |
| `0C24` | Write Class of Device | `bta_dm_act.c:546` | FWD | `CC(4)` | `s` | `E034 → E094` (`F:746,886`) | V-B |
| `080F` | Write Default Link Policy Settings | `bta_dm_act.c:574` → `A:1082`; BTM masks the value to `0000` (`A:1063–1076`) | SYN | `CC(4)` | `s=00` | none (exact: no LMP link modes are advertised, §3.6) | — |
| `0C14` | Read Local Name | `bta_dm_act.c:584` → `D:594` (BTA enable barrier) | SYN | `CC(252)` | `s=00, name[248]` (§3.7) | none (stock slot zero) | — |
| `0C13` | Write Local Name | `D:514` (BTC after enable; name changes) | FWD, update cache on `s=00` | `CC(4)` | `s` | `E025 → E083` (`F:744,884`) | V-B |
| `0C1A` | Write Scan Enable | `I:262,648`; BTA disable path | FWD | `CC(4)` | `s` | `E02C → E08A` (`F:745,885`) | V-B |
| `0C35` | Host Number Of Completed Packets | `stack/btu/c2h_credit.c:87` → `hcicmds.c:1322` | LOCAL | **none** | — (§3.2) | never forwarded | — |

## 5. Optional opcodes (class O)

### 5.1 Visibility, inquiry and name

| Opcode | Command | Send site / reachability | Action | Reply | Return parameters | ROM path | Exp 1 |
|---|---|---|---|---|---|---|---|
| `0C1C` | Write Page Scan Activity | `I:636`, when interval/window change | FWD → REJ | `CC(4)` | `s` | `E02E` (4 B) → CC `E08C` (map §2) | G-B |
| `0C1E` | Write Inquiry Scan Activity | `I:231`, when changed | FWD → REJ | `CC(4)` | `s` | `E030` (4 B) → CC `E08E` | G-B |
| `0C3A` | Write Current IAC LAP | `I:216,220`, discoverability mode change | FWD → REJ (controller keeps GIAC) | `CC(4)` | `s` | `E042` (4 B) → CC `E0A2`; `num` must be 1, so Bluedroid's LIAC+GIAC write fails `0x12` | G-B |
| `0C52` | Write Extended Inquiry Response | `bta_dm_act.c:4656` → `I:2364`, only with page-0 bit 48 | FWD → bit 48 cleared (unreachable; REJ if it arrives anyway) | `CC(4)` | `s` | `E04D` (241 B) → CC `E0AF` | G-B |
| `0C45` | Write Inquiry Mode | `D:439–441`, only with page-0 bit 30 (cleared) | REJ → FWD after G-C | `CC(4)` | `s` | `E047` (1 B) → CC `E0A9`; needs `0x22`/`0x2F` packers (no stock packer) | G-C |
| `0C43` | Write Inquiry Scan Type | `D:453`, unreachable (bit 28 cleared) | REJ | `CC(4)` | `s` | candidate `E045` | — |
| `0C47` | Write Page Scan Type | `D:452`, unreachable (bit 29 cleared) | REJ | `CC(4)` | `s` | candidate `E049` | — |
| `0C05` | Set Event Filter | `I:870,1567`: type `01`, condition `00` (all devices), or type `00` (clear) | SYN when the parameters are exactly `(01,00)` or `(00)`, else REJ | `CC(4)` | `s` | none. Exact: `bt_ctrl` never forwards a filter, so the controller is always at its default (no filter). A failure would end inquiry (`U:1293–1298`, `I:1620–1647`). | — |
| `0401` | Inquiry | `I:1718` | FWD → REJ | `CS` → `0x02`…, `0x01` | `s, 01, 0401` | `E000` (5 B) → CS `E0C7`; `0x02` (`E0D1`, mode 0) and `0x01` (`E0D0`) now packed. A CS failure tells BTM the inquiry is done (`U` default case). | G-C |
| `0402` | Inquiry Cancel | `I:748,1314` | FWD → REJ | `CC(4)` | `s` | `E001` (no parameters; zero-length layout in `bt_ctrl_cmds.c` `k_fwd_zero_len`, as the stock `ke_msg_send_basic`) → CC `E066`, deferred until the LD stops. After a REJ, BTU still treats any CC as cancel-complete (`U:1290–1292`), while the inquiry runs until its duration ends. | G-C |
| `0403` | Periodic Inquiry Mode | `I:1702`, explicit API only (unused) | REJ | `CC(4)` | `s` | candidate `E002` | — |
| `0404` | Exit Periodic Inquiry Mode | `I:559`, only after `0403` | REJ | `CC(4)` | `s` | stock slot zero | — |
| `0419` | Remote Name Request | `I:2116,2131,2137`, `S:5002` | FWD | `CS` → `0x07` | `s, 01, 0419` | `E00C` (10 B) → CS `E0CB`; terminal `E0D3` → `0x07` (255 B verbatim), optionally preceded by `E47C` → `0x3D`. LM SAVES it while LM state ≠ 0 (e.g. during an inquiry). A CS failure completes the name and security paths (`U` default case). | V-C |
| `041A` | Remote Name Request Cancel | `I:963` | FWD | `CC(10)` | `s, a` | `E00D` (6 B) → CC `E06E` `{s, a}`; no RNR running → `0x12` | V-C |

### 5.2 Connection set-up and teardown

| Opcode | Command | Send site / reachability | Action | Reply | Return parameters | ROM path | Exp 1 |
|---|---|---|---|---|---|---|---|
| `0405` | Create Connection | `stack/l2cap/l2c_utils.c:2660` (headphones mode) | FWD → REJ | `CS` → `0x03` | `s, 01, 0405` | `E004` (14 B; the struct is the wire layout zero-padded, map §1.1) → CS `E0C8`; terminal `E0D2/E456` packed. Allow-role-switch = no, because page-0 bit 5 is clear. | G-C |
| `0408` | Create Connection Cancel | `S:1313` | FWD → REJ | `CC(10)` | `s, a` (echo) | `E005` (6 B) → CC `E069` `{s, a}` | G-C |
| `0409` | Accept Connection Request | `l2c_link.c:106,126` | FWD | `CS` → `0x03` | `s, 01, 0409` | `E006 → E0C9/E43B` (`F:757–758,895`) | V-C |
| `040A` | Reject Connection Request | `l2c_link.c:73,133,139`, `S:2731` | FWD | `CS` → `0x03` (failure) | `s, 01, 040A` | `E007 → E0CA/E43C` (`F:759–760,896`) | V-C |
| `0406` | Disconnect | `S:1557`, `l2c_link.c:788` | FWD | `CS` → `0x05` | `s, 01, 0406` | `E400 → E43A` (`F:761,894`); terminal `E458` packed | V-C |

### 5.3 Automatic per-connection reads (`btm_acl.c`) and link settings

| Opcode | Command | Send site / reachability | Action | Reply | Return parameters | ROM path | Exp 1 |
|---|---|---|---|---|---|---|---|
| `041F` | Read Clock Offset | `A:435`, each new BR ACL | FWD | `CS` → `0x1C` | `s, 01, 041F` | `E408` (2 B) → CS `E444`; terminal `E469` → `0x1C`. As slave (phone mode) the ROM answers from the local clock, CS first. Master path: silent if LC env `+0x46 == 3` (map §2), covered by the deadline (§2 rule 9). | V-C |
| `041D` | Read Remote Version Information | `A:436,490` | FWD | `CS` → `0x0C` | `s, 01, 041D` | `E407` (2 B) → CS `E443`; terminal `E45E` (legacy packer) | V-C |
| `041B` | Read Remote Supported Features | `A:474,497` → `A:1229` | FWD (**required by procedure**) | `CS` → `0x0B` | `s, 01, 041B` | `E405` (2 B) → CS `E441`; terminal `E45D` (legacy packer). SAVED until the LMP features exchange ends. **A REJ stalls `btm_establish_continue`** (`A:1270–1300`; the BTU CS failure branch has no case for it). | V-C |
| `041C` | Read Remote Extended Features | `A:1246`, when the peer's page 0 has bit 63 (and `1002` octet 2 bit `0x40`, now set) | FWD | `CS` → `0x23` | `s, 01, 041C` | `E406` (4 B) → CS `E442`; terminal `E46D` → `0x23`. **Needed for SSP**: without page 1 `btm_sec_set_peer_sec_caps()` (`S:6249–6258`) marks every peer `BTM_SM4_KNOWN` (legacy) and the access checks take the legacy branch (`S:2300`, `S:2571`). A CS failure (`0x1A` when the peer lacks page-0 bit 63) is handled (`btm_read_remote_ext_features_failed`). | V-C |
| `040F` | Change Connection Packet Type | `A:1840` via `A:1420` (establish) and `A:1682` (role change) | FWD → REJ (the link keeps the packet types from create/accept) | `CS` → `0x1D` | `s, 01, 040F` | `E401` (4 B) → CS `E43D`; terminal `E46A` → `0x1D` (packed). Stays gated: the ROM fails any set that differs from its own computed set with `0x11`, and is silent if LC env `+0x46 == 3`; no Bluedroid path depends on it (a CS failure has no handler). | G-C |
| `080D` | Write Link Policy Settings | `A:1040` via `A:362`, `bta_dm_act.c:1499` (value masked to `0000`) | REJ (no BTU CC handler; harmless) | `CC(6)` | `s, h` (echo `h`) | candidate `E414` | — |
| `0C37` | Write Link Supervision Timeout | `A:1507`, master role only; re-sent after a role change (`A:1678`) | FWD → REJ (ROM default stays, 20 s) | `CC(6)` | `s, h` (echo) | `E41C` (4 B) → CC `E431` `{s, pad, h}` (de-padded), **deferred** until the LMP PDU completes. Not on the phone path (slave). | G-C |
| `0C28` | Write Automatic Flush Timeout | `bta_av_aact.c:2155,2468,2701` → `l2c_api.c:1181–1200` (A2DP) | FWD → REJ (no BTU handler; flush stays infinite) | `CC(6)` | `s, h` (echo) | `E419` (4 B) → CC `E42E` `{s, pad, h}` (de-padded) | G-C |

### 5.4 Security and SSP

| Opcode | Command | Send site / reachability | Action | Reply | Return parameters | ROM path | Exp 1 |
|---|---|---|---|---|---|---|---|
| `0411` | Authentication Requested | `S:5606` | FWD | `CS` → `0x06` | `s, 01, 0411` | `E402` (2 B) → CS `E43E`; then `0x17`, SSP events, terminal `E459` → `0x06` (legacy packer). SAVED while the LC is in a pairing state (e.g. a peer-started pairing waiting for the user): the deadline (§2 rule 9) bounds it. | V-C |
| `0413` | Set Connection Encryption | `S:5624`, `A:874,1695` | FWD | `CS` → `0x08` | `s, 01, 0413` | `E403` (4 B) → CS `E43F`; terminal `E45A` → `0x08` (legacy packer). A no-op change answers `0x12`. | V-C |
| `040B` | Link Key Request Reply | `S:5082` | FWD | `CC(10)` | `s, a` | `E008 → E06A` (`F:749,897`) | V-C |
| `040C` | Link Key Request Negative Reply | `S:5063,5078,5090` | FWD | `CC(10)` | `s, a` | `E009 → E06B` (`F:750,898`) | V-C |
| `040D` | PIN Code Request Reply | `S:994,5261` (legacy; the policy rejects it, so Bluedroid normally sends `040E`) | FWD | `CC(10)` | `s, a` | `E00A → E06C` (`F:751,899`) | V-C |
| `040E` | PIN Code Request Negative Reply | `S:930,5249` | FWD | `CC(10)` | `s, a` | `E00B → E06D` (`F:752,900`) | V-C |
| `042B` | IO Capability Request Reply | `S:1736,3547` | FWD | `CC(10)` | `s, a` | `E011 → E06F` (`F:753,901`) | V-C |
| `0434` | IO Capability Request Negative Reply | `S:3390,3473` (pairing refused outside the pairing window, plan M4) | FWD | `CC(10)` | `s, a` | `E018` (7 B) → CC `E076` `{s, a}`; the LC sends LMP not_accepted with the reason. No link → `0x01` (not `0x12`). | V-C |
| `042C` | User Confirmation Request Reply | `S:1603` | FWD | `CC(10)` | `s, a` | `E012 → E070` (`F:754,902`) | V-C |
| `042D` | User Confirmation Request Negative Reply | `S:1607,3725` | FWD | `CC(10)` | `s, a` | `E013 → E071` (`F:755,903`) | V-C |
| `042E` | User Passkey Request Reply | `S:1670`; unreachable: local IO capability is DisplayYesNo, so `0x34` never arrives | REJ | `CC(10)` | `s, a` (echo) | candidate `E014` | — |
| `042F` | User Passkey Request Negative Reply | `S:1667,3737`; unreachable (same reason) | REJ | `CC(10)` | `s, a` (echo) | candidate `E015` | — |
| `0430` | Remote OOB Data Request Reply | `S:1790`; unreachable: OOB-present is never advertised | REJ | `CC(10)` | `s, a` (echo) | candidate `E016` | — |
| `0433` | Remote OOB Data Request Negative Reply | `S:1787,3890`; unreachable (same reason) | REJ | `CC(10)` | `s, a` (echo) | candidate `E017` | — |
| `0C57` | Read Local OOB Data | `S:1752`, explicit API only | REJ | `CC(36)` | `s, C[16]=0, R[16]=0` | stock slot zero | — |
| `0C20` | Write Authentication Enable | `S:954,5923,6136`, security mode 3 or legacy dedicated bonding only | REJ | `CC(4)` | `s` | candidate `E032` | — |
| `0C0A` | Write PIN Type | `S:434–436`, only when the PIN type changes (the reset-time call is unchanged) | REJ | `CC(4)` | `s` | candidate `E020` | — |
| `0C12` | Delete Stored Link Key | `D:1216`, `S:1059`, `btm_dev.c:203` (bond removal) | SYN | `CC(6)` | `s=00, num_deleted:u16=0000` | none. Exact: `bt_ctrl` never forwards Write Stored Link Key, so the controller holds no keys. | — |

### 5.5 Link-policy modes (unreachable with the §3.6 masks) and explicit-API-only commands

| Opcode | Command | Send site / reachability | Action | Reply | Return parameters | Notes |
|---|---|---|---|---|---|---|
| `080B` | Switch Role | `A:883,949,2674`, `bta_av_main.c:1237,1276`, `l2c_utils.c:2565`; refused locally by `A:812` | REJ | `CS` | `s=01, 01, 080B` | A CS failure calls `btm_acl_role_changed` (`U` default) |
| `0801` | Hold Mode | `P:724`; refused by `P:220–222` | REJ | `CS` | `s=01, 01, 0801` | `btm_pm_proc_cmd_status` |
| `0803` | Sniff Mode | `P:731`; refused by `P:220–222` | REJ | `CS` | `s=01, 01, 0803` | Plan: no sniff |
| `0804` | Exit Sniff Mode | `P:708`; never in sniff | REJ | `CS` | `s=01, 01, 0804` | |
| `0805` | Park State | `P:739`; refused | REJ | `CS` | `s=01, 01, 0805` | |
| `0806` | Exit Park State | `P:713`; never parked | REJ | `CS` | `s=01, 01, 0806` | |
| `0807` | QoS Setup | `A:2085`, explicit GAP QoS API only | REJ | `CS` | `s=01, 01, 0807` | Upstream quirk: the failure branch matches on the event code `HCI_QOS_SETUP_COMP_EVT`, not the opcode, so the caller's callback is not invoked. The app never calls it. |
| `0C3F` | Set AFH Host Channel Classification | `D:1307`, explicit API | REJ | `CC(4)` | `s` | |
| `0C58` | Read Inquiry Response TX Power Level | `I:1119`, explicit API | REJ | `CC(5)` | `s, tx_power:i8=00` | |
| `0C59` | Write Inquiry Transmit Power Level | `I:1163`, explicit API | REJ | `CC(4)` | `s` | |
| `1405` | Read RSSI | `A:2194`, explicit API | REJ | `CC(7)` | `s, h (echo), rssi:i8=00` | |
| `0C84` | Set Min Encryption Key Size | `D:1028`, only if `1002` octet 45 bit `0x80` (always clear) | REJ | `CC(4)` | `s` | |
| `FCxx` | Vendor specific (any OCF) | `D:744`: ESP vendor commands from explicit APIs (`A:137`, `btm_bredr_pwr_ctrl.c`); Broadcom `FC57` gated on manufacturer `0x000F` (§3.3) | REJ | `CC(4)` | `s` | `btm_vsc_complete` passes the reply to the caller |

### 5.6 Never sent in this configuration, but kept in the table as REJ (no stray default)

| Opcode | Command | Why it is not sent | Reply |
|---|---|---|---|
| `0C7A` | Write Secure Connections Host Support | `sc_en = false` and `max_page = 1` (`C:364–369`) | `CC(4)`, `s=01` |
| `0C2F` | Write Synchronous Flow Control Enable | `BTM_SCO_HCI_INCLUDED = FALSE` (`C:480–486`) | `CC(4)`, `s=01` |
| `0C5B` | Write Default Erroneous Data Reporting | same | `CC(4)`, `s=01` |
| `1003` | Read Local Supported Features | factory only; Classic start-up uses `1004` | `CC(12)`, `s=01, 00×8` |

## 6. Encoders not in the `BT=1` image (cannot be sent)

These encoders are absent from `nm`:

- `0C5F` Enhanced Flush (`btsnd_hcic_enhanced_flush`)
- `0C60` Send Keypress Notification
- `0811` Sniff Subrating
- `0C26` Write Voice Setting
- `0C2D` Read Transmit Power Level
- `1403` Get Link Quality
- `0407` Add SCO
- `0428`/`0429`/`042A` (e)SCO set-up, accept and reject
- `0420` Read LMP Handle
- `1803` Enable Device Under Test
- every `08xx` LE command except the link-policy group above

`BT_HCI_MATRIX.md` lists `0C5F` and `0C60` as reachable; in this build they are compiled out or garbage-collected.
If any of them ever arrives, the closed-table default applies: `CS`, `s = 0x01`, counter incremented, host test
fails. Clearing page-0 bit 54 also keeps `0C5F` unreachable if a future change links it back in.

## 7. Ambiguities and contradictions (stated, not resolved silently)

1. **Two classes versus FWD/SYN in class O.** Plan M1 says "Optional: REJECT". Taken literally, that would
   reject commands the profiles need, such as `0405`, `0406` and the link-key replies. This table reads it as:
   class O *may* fail. Its action is FWD when a ROM path is verified, SYN only when exact (§2 rule 6), and REJ
   otherwise. This needs confirmation in review.
2. **"The BTA setup writes" are not listed in the plan.** This table uses `0C18, 080F, 0C24, 0C13, 0C1A` plus the
   read `0C14`, all sent unconditionally during reset, enable, name and visibility set-up. `0C0A/0C43/0C45/0C47/0C52`
   are made conditional or unreachable through §3.6 instead of being promoted to class M.
3. **Where feature masking happens.** The plan says "feature masks: no sniff, no role switch, no SC" without
   saying HCI-reported or LMP-advertised. This table masks the HCI page (required) and *recommends* writing the
   same value to the ROM LMP table at `0x802C04/08`. Otherwise peers can start role switches whose `0x12` event
   has no packer. Decide this together with Exp 1 stage B, which must confirm the ROM honours the table.
4. **Value of `1005` `HC_Total_Num_ACL_Data_Packets`.** "No larger than bt_ctrl can immediately hand to the ROM"
   has no measured value yet. The static analysis says 9 EM TX buffers, unverified. The provisional value is 4;
   it is raised only after the G-C NOCP run. If the ROM holds EM buffers per link beyond the HCI packet (for
   example on retransmission), the bound must come from that measurement, not from the pool size.
5. **`1009` FWD or SYN.** The plan allows either. This table chooses SYN from the identity that `bt_ctrl`
   programmed, plus a stage-A cross-check, because `E05D` was never exercised by the legacy chain.
6. **Default reply for an opcode that is not in the table.** The HCI specification lets a controller answer an
   unknown command with CS or CC. This table chooses CS (§2 rule 5), which is safe for every non-futured
   Bluedroid command.
7. **`0C33` mismatch inside class M.** A Host Buffer Size other than 1021/4 cannot be rejected (class M). It gets
   `s = 0x00`, `bt_ctrl` enforces `min()`, and an invariant counter catches the configuration drift.
8. **`041B` is "optional" but required by procedure.** No CS failure branch covers it, so REJ is not a working
   fallback. The same applies in practice to `0405`, `0411` and `0413` for the profiles. Their G-C gate is a
   profile gate, not a shippable fallback.
9. **Stale evidence risk.** The linked-encoder list (§6) comes from the `BT=1` ELF at
   `/tmp/sl6801_openfw_bt/open_fw.elf` (Nov 24 12:27), not from a fresh build of the current tree.
10. **Proposed test.** A `test-bt-port` check should compare `nm | grep btsnd_hcic_` (plus the `make_*` factory)
    against `BT_CTRL_OPCODE_TABLE`, so that a newly linked encoder without a table row fails the build.
11. **Phone-path promotion (2026-11-24).** `0419 041A 041B 041C 041D 041F 0411 0413 0434` moved from `REJ → FWD`
    (G-C) to `FWD` (V-C): ids, payload sizes and reply ids from `BT_ROM_HCI_MAP.md` §2 ("exact": stock opcode table
    + ROM handler table + handler disassembly agree), event packers from the stock reply tables (map §9). Legacy
    `bt_hci.c` sends none of these commands, so there was nothing to disagree with; every event they produce that
    legacy also packs (`E459 E45A E45D E45E E465 E466 E472 E473 E474 E477`) uses the legacy packer unchanged, and the
    stock packers agree with it byte for byte. Silicon only has to confirm the golden vectors (V-C, plan step 6).
    Still gated: `040F 0C37 0C28` (not needed by the phone path, risky ROM behaviour, see their rows), the inquiry and
    connection-creation rows (headphones path) and the G-B visibility writes. All gated rows now carry their reply id,
    and `bt_ctrl_init()` checks that every reply id in the table is packed back to its own opcode.

12. **Headphones rows (2026-11-25, `docs/BT_A2DP_DESIGN.md` §7 milestone H2).** `0401 0402 0405 0408` keep `REJ` in
    the default build and are forwarded with `-DBT_CTRL_EXP1_PASSED_LIST=BT_CTRL_HP_GATE_LIST` (host tests; bench
    images with `make BT=1 BENCH=1 HP_GATES=1` for Exp 4a/4b). Every one of them validates with the gate on: ids,
    payload sizes and reply ids are "exact" in `BT_ROM_HCI_MAP.md` §2, and `0402` now has its zero-length layout.
    `test-bt-ctrl` runs both builds; `test-bt-host` runs the headphones smoke (`test_bt_host_hp hp`) on the gated
    build. What the real Bluedroid sends around an outgoing link (harness, master role): `0C05` (SYN) and `0401`
    for discovery, `0419` one device at a time after Inquiry Complete (BTA's name pass, never during the inquiry),
    `0402` on cancel, then `0405`, the set-up reads `041F 041D 041B 041C` (FWD), and `080D`, `0C37`, `040F`, which
    stay `REJ`: `080D` and `040F` have no handler for a failure, `0C37` (master only) leaves the ROM's 20 s supervision
    timeout, which the design's credit-stall watchdog covers (§4.6). `040F` stays refused until Exp 4b decides it
    (design §7 "role switch decision"); after a `0405` the link keeps the packet types the ROM computed from the
    `0405` request. `080B` and `0C45` stay `REJ` too; `E0D6`/`E0D7` have packers (`bt_ctrl_evt.c`, ROM-sender
    layouts) but stay unreachable while page-0 bit 30 is masked. Deadline facts for these rows (§2 rule 9): `0401`
    and `0405` are SAVED in LM states 1/2, so their CS can come only at the 7 s deadline (`0x22`); the late CS is
    consumed and a retry with the same opcode waits for it (`fwd_late_hold`); results or a Connection Complete of the
    saved command still reach the host as events, so a `0405` answered `0x22` can still produce a link, which
    Bluedroid has to disconnect (its role in `bt_ctrl_link_info` is then unknown). `0402`'s deferred CC is bounded
    the same way.

## 8. Machine-readable list (C X-macro)

Field meaning:

- `op`: opcode.
- `cls`: `BTC_M` or `BTC_O`.
- `act`: action before or without the Exp 1 gate.
- `gated`: action once the gate passes (equal to `act` when there is no gate).
- `rsp`: `BTC_CC`, `BTC_CS` or `BTC_NONE`.
- `plen`: `Parameter_Total_Length` of the initial reply.
- `echo`: first command parameter copied into a SYN or REJ reply.
- `rom_req`, `rom_rsp`: KE message ids, `0` = none or unknown.
- `so`, `sm`: `1002` supported-commands octet and mask (`0xFF` = none).
- `exp`: Exp 1 dependency.

A `bt_ctrl` build option (`BT_CTRL_EXP1_PASSED_<op>`) selects `gated` over `act`. The `1002` bitmap and the page-0
bits 30 and 48 are derived from the effective actions. Rows whose action depends on parameters (`0C05`, `1004`)
carry the parameter rule in `bt_ctrl_cmds.c`; here they show their success action.

```c
/* docs/BT_CTRL_OPCODE_TABLE.md §8 — generated by hand from §4–§5; keep in sync. */
/* X(op,     cls,   act,       gated,     rsp,      plen, echo,           rom_req, rom_rsp, so,   sm,   exp)        */
#define BT_CTRL_OPCODE_TABLE(X) \
  /* ---- class M: controller.c start-up (futured, parsed by hci_packet_parser.c) ---- */ \
  X(0x0C03u, BTC_M, BTC_SYN,   BTC_SYN,   BTC_CC,     4u, BTC_ECHO_NONE,  0x0000u, 0x0000u,  5u, 0x80u, BTC_EXP_V_A) \
  X(0x1005u, BTC_M, BTC_SYN,   BTC_SYN,   BTC_CC,    11u, BTC_ECHO_NONE,  0x0000u, 0x0000u, 14u, 0x80u, BTC_EXP_G_C) \
  X(0x0C31u, BTC_M, BTC_SYN,   BTC_SYN,   BTC_CC,     4u, BTC_ECHO_NONE,  0x0000u, 0x0000u, 10u, 0x20u, BTC_EXP_NONE) \
  X(0x0C33u, BTC_M, BTC_SYN,   BTC_SYN,   BTC_CC,     4u, BTC_ECHO_NONE,  0x0000u, 0x0000u, 10u, 0x40u, BTC_EXP_NONE) \
  X(0x1001u, BTC_M, BTC_SYN,   BTC_SYN,   BTC_CC,    12u, BTC_ECHO_NONE,  0x0000u, 0x0000u, 14u, 0x08u, BTC_EXP_V_A) \
  X(0x1009u, BTC_M, BTC_SYN,   BTC_SYN,   BTC_CC,    10u, BTC_ECHO_NONE,  0x0000u, 0x0000u, 15u, 0x02u, BTC_EXP_V_A) \
  X(0x1002u, BTC_M, BTC_SYN,   BTC_SYN,   BTC_CC,    68u, BTC_ECHO_NONE,  0x0000u, 0x0000u, 14u, 0x10u, BTC_EXP_NONE) \
  X(0x1004u, BTC_M, BTC_SYN,   BTC_SYN,   BTC_CC,    14u, BTC_ECHO_NONE,  0x0000u, 0x0000u, 14u, 0x40u, BTC_EXP_G_B) \
  X(0x0C56u, BTC_M, BTC_FWD,   BTC_FWD,   BTC_CC,     4u, BTC_ECHO_NONE,  0xE04Fu, 0xE0B1u, 17u, 0x40u, BTC_EXP_V_B) \
  X(0x0C01u, BTC_M, BTC_FWD,   BTC_FWD,   BTC_CC,     4u, BTC_ECHO_NONE,  0xE01Cu, 0xE079u,  5u, 0x40u, BTC_EXP_V_B) \
  /* ---- class M: BTM reset / BTA enable / visibility set-up writes ---- */ \
  X(0x0C18u, BTC_M, BTC_SYN,   BTC_FWD,   BTC_CC,     4u, BTC_ECHO_NONE,  0xE02Au, 0xE088u,  7u, 0x20u, BTC_EXP_G_B) \
  X(0x0C24u, BTC_M, BTC_FWD,   BTC_FWD,   BTC_CC,     4u, BTC_ECHO_NONE,  0xE034u, 0xE094u,  9u, 0x02u, BTC_EXP_V_B) \
  X(0x080Fu, BTC_M, BTC_SYN,   BTC_SYN,   BTC_CC,     4u, BTC_ECHO_NONE,  0x0000u, 0x0000u,  5u, 0x10u, BTC_EXP_NONE) \
  X(0x0C14u, BTC_M, BTC_SYN,   BTC_SYN,   BTC_CC,   252u, BTC_ECHO_NONE,  0x0000u, 0x0000u,  7u, 0x02u, BTC_EXP_NONE) \
  X(0x0C13u, BTC_M, BTC_FWD,   BTC_FWD,   BTC_CC,     4u, BTC_ECHO_NONE,  0xE025u, 0xE083u,  7u, 0x01u, BTC_EXP_V_B) \
  X(0x0C1Au, BTC_M, BTC_FWD,   BTC_FWD,   BTC_CC,     4u, BTC_ECHO_NONE,  0xE02Cu, 0xE08Au,  7u, 0x80u, BTC_EXP_V_B) \
  X(0x0C35u, BTC_M, BTC_LOCAL, BTC_LOCAL, BTC_NONE,   0u, BTC_ECHO_NONE,  0x0000u, 0x0000u, 10u, 0x80u, BTC_EXP_NONE) \
  /* ---- class O: visibility, inquiry, names ---- */ \
  X(0x0C1Cu, BTC_O, BTC_REJ,   BTC_FWD,   BTC_CC,     4u, BTC_ECHO_NONE,  0xE02Eu, 0xE08Cu,  8u, 0x02u, BTC_EXP_G_B) \
  X(0x0C1Eu, BTC_O, BTC_REJ,   BTC_FWD,   BTC_CC,     4u, BTC_ECHO_NONE,  0xE030u, 0xE08Eu,  8u, 0x08u, BTC_EXP_G_B) \
  X(0x0C3Au, BTC_O, BTC_REJ,   BTC_FWD,   BTC_CC,     4u, BTC_ECHO_NONE,  0xE042u, 0xE0A2u, 11u, 0x10u, BTC_EXP_G_B) \
  X(0x0C52u, BTC_O, BTC_REJ,   BTC_FWD,   BTC_CC,     4u, BTC_ECHO_NONE,  0xE04Du, 0xE0AFu, 17u, 0x02u, BTC_EXP_G_B) \
  X(0x0C45u, BTC_O, BTC_REJ,   BTC_FWD,   BTC_CC,     4u, BTC_ECHO_NONE,  0xE047u, 0xE0A9u, 13u, 0x01u, BTC_EXP_G_C) \
  X(0x0C43u, BTC_O, BTC_REJ,   BTC_REJ,   BTC_CC,     4u, BTC_ECHO_NONE,  0x0000u, 0x0000u, 12u, 0x20u, BTC_EXP_NONE) \
  X(0x0C47u, BTC_O, BTC_REJ,   BTC_REJ,   BTC_CC,     4u, BTC_ECHO_NONE,  0x0000u, 0x0000u, 13u, 0x02u, BTC_EXP_NONE) \
  X(0x0C05u, BTC_O, BTC_SYN,   BTC_SYN,   BTC_CC,     4u, BTC_ECHO_NONE,  0x0000u, 0x0000u,  6u, 0x01u, BTC_EXP_NONE) \
  X(0x0401u, BTC_O, BTC_REJ,   BTC_FWD,   BTC_CS,     4u, BTC_ECHO_NONE,  0xE000u, 0xE0C7u,  0u, 0x01u, BTC_EXP_G_C) \
  X(0x0402u, BTC_O, BTC_REJ,   BTC_FWD,   BTC_CC,     4u, BTC_ECHO_NONE,  0xE001u, 0xE066u,  0u, 0x02u, BTC_EXP_G_C) \
  X(0x0403u, BTC_O, BTC_REJ,   BTC_REJ,   BTC_CC,     4u, BTC_ECHO_NONE,  0x0000u, 0x0000u,  0u, 0x04u, BTC_EXP_NONE) \
  X(0x0404u, BTC_O, BTC_REJ,   BTC_REJ,   BTC_CC,     4u, BTC_ECHO_NONE,  0x0000u, 0x0000u,  0u, 0x08u, BTC_EXP_NONE) \
  X(0x0419u, BTC_O, BTC_FWD,   BTC_FWD,   BTC_CS,     4u, BTC_ECHO_NONE,  0xE00Cu, 0xE0CBu,  2u, 0x08u, BTC_EXP_V_C) \
  X(0x041Au, BTC_O, BTC_FWD,   BTC_FWD,   BTC_CC,    10u, BTC_ECHO_BDADDR,0xE00Du, 0xE06Eu,  2u, 0x10u, BTC_EXP_V_C) \
  /* ---- class O: connection set-up / teardown ---- */ \
  X(0x0405u, BTC_O, BTC_REJ,   BTC_FWD,   BTC_CS,     4u, BTC_ECHO_NONE,  0xE004u, 0xE0C8u,  0u, 0x10u, BTC_EXP_G_C) \
  X(0x0408u, BTC_O, BTC_REJ,   BTC_FWD,   BTC_CC,    10u, BTC_ECHO_BDADDR,0xE005u, 0xE069u,  0u, 0x80u, BTC_EXP_G_C) \
  X(0x0409u, BTC_O, BTC_FWD,   BTC_FWD,   BTC_CS,     4u, BTC_ECHO_NONE,  0xE006u, 0xE0C9u,  1u, 0x01u, BTC_EXP_V_C) \
  X(0x040Au, BTC_O, BTC_FWD,   BTC_FWD,   BTC_CS,     4u, BTC_ECHO_NONE,  0xE007u, 0xE0CAu,  1u, 0x02u, BTC_EXP_V_C) \
  X(0x0406u, BTC_O, BTC_FWD,   BTC_FWD,   BTC_CS,     4u, BTC_ECHO_NONE,  0xE400u, 0xE43Au,  0u, 0x20u, BTC_EXP_V_C) \
  /* ---- class O: automatic per-connection reads and link settings ---- */ \
  X(0x041Fu, BTC_O, BTC_FWD,   BTC_FWD,   BTC_CS,     4u, BTC_ECHO_NONE,  0xE408u, 0xE444u,  3u, 0x01u, BTC_EXP_V_C) \
  X(0x041Du, BTC_O, BTC_FWD,   BTC_FWD,   BTC_CS,     4u, BTC_ECHO_NONE,  0xE407u, 0xE443u,  2u, 0x80u, BTC_EXP_V_C) \
  X(0x041Bu, BTC_O, BTC_FWD,   BTC_FWD,   BTC_CS,     4u, BTC_ECHO_NONE,  0xE405u, 0xE441u,  2u, 0x20u, BTC_EXP_V_C) \
  X(0x041Cu, BTC_O, BTC_FWD,   BTC_FWD,   BTC_CS,     4u, BTC_ECHO_NONE,  0xE406u, 0xE442u,  2u, 0x40u, BTC_EXP_V_C) \
  X(0x040Fu, BTC_O, BTC_REJ,   BTC_FWD,   BTC_CS,     4u, BTC_ECHO_NONE,  0xE401u, 0xE43Du,  1u, 0x40u, BTC_EXP_G_C) \
  X(0x080Du, BTC_O, BTC_REJ,   BTC_REJ,   BTC_CC,     6u, BTC_ECHO_HANDLE,0x0000u, 0x0000u,  5u, 0x04u, BTC_EXP_NONE) \
  X(0x0C37u, BTC_O, BTC_REJ,   BTC_FWD,   BTC_CC,     6u, BTC_ECHO_HANDLE,0xE41Cu, 0xE431u, 11u, 0x02u, BTC_EXP_G_C) \
  X(0x0C28u, BTC_O, BTC_REJ,   BTC_FWD,   BTC_CC,     6u, BTC_ECHO_HANDLE,0xE419u, 0xE42Eu,  9u, 0x20u, BTC_EXP_G_C) \
  /* ---- class O: security / SSP ---- */ \
  X(0x0411u, BTC_O, BTC_FWD,   BTC_FWD,   BTC_CS,     4u, BTC_ECHO_NONE,  0xE402u, 0xE43Eu,  1u, 0x80u, BTC_EXP_V_C) \
  X(0x0413u, BTC_O, BTC_FWD,   BTC_FWD,   BTC_CS,     4u, BTC_ECHO_NONE,  0xE403u, 0xE43Fu,  2u, 0x01u, BTC_EXP_V_C) \
  X(0x040Bu, BTC_O, BTC_FWD,   BTC_FWD,   BTC_CC,    10u, BTC_ECHO_BDADDR,0xE008u, 0xE06Au,  1u, 0x04u, BTC_EXP_V_C) \
  X(0x040Cu, BTC_O, BTC_FWD,   BTC_FWD,   BTC_CC,    10u, BTC_ECHO_BDADDR,0xE009u, 0xE06Bu,  1u, 0x08u, BTC_EXP_V_C) \
  X(0x040Du, BTC_O, BTC_FWD,   BTC_FWD,   BTC_CC,    10u, BTC_ECHO_BDADDR,0xE00Au, 0xE06Cu,  1u, 0x10u, BTC_EXP_V_C) \
  X(0x040Eu, BTC_O, BTC_FWD,   BTC_FWD,   BTC_CC,    10u, BTC_ECHO_BDADDR,0xE00Bu, 0xE06Du,  1u, 0x20u, BTC_EXP_V_C) \
  X(0x042Bu, BTC_O, BTC_FWD,   BTC_FWD,   BTC_CC,    10u, BTC_ECHO_BDADDR,0xE011u, 0xE06Fu, 18u, 0x80u, BTC_EXP_V_C) \
  X(0x0434u, BTC_O, BTC_FWD,   BTC_FWD,   BTC_CC,    10u, BTC_ECHO_BDADDR,0xE018u, 0xE076u, 20u, 0x08u, BTC_EXP_V_C) \
  X(0x042Cu, BTC_O, BTC_FWD,   BTC_FWD,   BTC_CC,    10u, BTC_ECHO_BDADDR,0xE012u, 0xE070u, 19u, 0x01u, BTC_EXP_V_C) \
  X(0x042Du, BTC_O, BTC_FWD,   BTC_FWD,   BTC_CC,    10u, BTC_ECHO_BDADDR,0xE013u, 0xE071u, 19u, 0x02u, BTC_EXP_V_C) \
  X(0x042Eu, BTC_O, BTC_REJ,   BTC_REJ,   BTC_CC,    10u, BTC_ECHO_BDADDR,0x0000u, 0x0000u, 19u, 0x04u, BTC_EXP_NONE) \
  X(0x042Fu, BTC_O, BTC_REJ,   BTC_REJ,   BTC_CC,    10u, BTC_ECHO_BDADDR,0x0000u, 0x0000u, 19u, 0x08u, BTC_EXP_NONE) \
  X(0x0430u, BTC_O, BTC_REJ,   BTC_REJ,   BTC_CC,    10u, BTC_ECHO_BDADDR,0x0000u, 0x0000u, 19u, 0x10u, BTC_EXP_NONE) \
  X(0x0433u, BTC_O, BTC_REJ,   BTC_REJ,   BTC_CC,    10u, BTC_ECHO_BDADDR,0x0000u, 0x0000u, 19u, 0x80u, BTC_EXP_NONE) \
  X(0x0C57u, BTC_O, BTC_REJ,   BTC_REJ,   BTC_CC,    36u, BTC_ECHO_NONE,  0x0000u, 0x0000u, 17u, 0x80u, BTC_EXP_NONE) \
  X(0x0C20u, BTC_O, BTC_REJ,   BTC_REJ,   BTC_CC,     4u, BTC_ECHO_NONE,  0x0000u, 0x0000u,  8u, 0x20u, BTC_EXP_NONE) \
  X(0x0C0Au, BTC_O, BTC_REJ,   BTC_REJ,   BTC_CC,     4u, BTC_ECHO_NONE,  0x0000u, 0x0000u,  6u, 0x08u, BTC_EXP_NONE) \
  X(0x0C12u, BTC_O, BTC_SYN,   BTC_SYN,   BTC_CC,     6u, BTC_ECHO_NONE,  0x0000u, 0x0000u,  6u, 0x80u, BTC_EXP_NONE) \
  /* ---- class O: link-policy modes (unreachable with masks) and explicit-API-only ---- */ \
  X(0x080Bu, BTC_O, BTC_REJ,   BTC_REJ,   BTC_CS,     4u, BTC_ECHO_NONE,  0x0000u, 0x0000u,  5u, 0x01u, BTC_EXP_NONE) \
  X(0x0801u, BTC_O, BTC_REJ,   BTC_REJ,   BTC_CS,     4u, BTC_ECHO_NONE,  0x0000u, 0x0000u,  4u, 0x02u, BTC_EXP_NONE) \
  X(0x0803u, BTC_O, BTC_REJ,   BTC_REJ,   BTC_CS,     4u, BTC_ECHO_NONE,  0x0000u, 0x0000u,  4u, 0x04u, BTC_EXP_NONE) \
  X(0x0804u, BTC_O, BTC_REJ,   BTC_REJ,   BTC_CS,     4u, BTC_ECHO_NONE,  0x0000u, 0x0000u,  4u, 0x08u, BTC_EXP_NONE) \
  X(0x0805u, BTC_O, BTC_REJ,   BTC_REJ,   BTC_CS,     4u, BTC_ECHO_NONE,  0x0000u, 0x0000u,  4u, 0x10u, BTC_EXP_NONE) \
  X(0x0806u, BTC_O, BTC_REJ,   BTC_REJ,   BTC_CS,     4u, BTC_ECHO_NONE,  0x0000u, 0x0000u,  4u, 0x20u, BTC_EXP_NONE) \
  X(0x0807u, BTC_O, BTC_REJ,   BTC_REJ,   BTC_CS,     4u, BTC_ECHO_NONE,  0x0000u, 0x0000u,  4u, 0x40u, BTC_EXP_NONE) \
  X(0x0C3Fu, BTC_O, BTC_REJ,   BTC_REJ,   BTC_CC,     4u, BTC_ECHO_NONE,  0x0000u, 0x0000u, 12u, 0x02u, BTC_EXP_NONE) \
  X(0x0C58u, BTC_O, BTC_REJ,   BTC_REJ,   BTC_CC,     5u, BTC_ECHO_NONE,  0x0000u, 0x0000u, 18u, 0x01u, BTC_EXP_NONE) \
  X(0x0C59u, BTC_O, BTC_REJ,   BTC_REJ,   BTC_CC,     4u, BTC_ECHO_NONE,  0x0000u, 0x0000u, 18u, 0x02u, BTC_EXP_NONE) \
  X(0x1405u, BTC_O, BTC_REJ,   BTC_REJ,   BTC_CC,     7u, BTC_ECHO_HANDLE,0x0000u, 0x0000u, 15u, 0x20u, BTC_EXP_NONE) \
  X(0x0C84u, BTC_O, BTC_REJ,   BTC_REJ,   BTC_CC,     4u, BTC_ECHO_NONE,  0x0000u, 0x0000u, 45u, 0x80u, BTC_EXP_NONE) \
  /* ---- never sent in this configuration; explicit REJ rows ---- */ \
  X(0x0C7Au, BTC_O, BTC_REJ,   BTC_REJ,   BTC_CC,     4u, BTC_ECHO_NONE,  0x0000u, 0x0000u, 0xFFu,0x00u, BTC_EXP_NONE) \
  X(0x0C2Fu, BTC_O, BTC_REJ,   BTC_REJ,   BTC_CC,     4u, BTC_ECHO_NONE,  0x0000u, 0x0000u, 10u, 0x10u, BTC_EXP_NONE) \
  X(0x0C5Bu, BTC_O, BTC_REJ,   BTC_REJ,   BTC_CC,     4u, BTC_ECHO_NONE,  0x0000u, 0x0000u, 18u, 0x08u, BTC_EXP_NONE) \
  X(0x1003u, BTC_O, BTC_REJ,   BTC_REJ,   BTC_CC,    12u, BTC_ECHO_NONE,  0x0000u, 0x0000u, 14u, 0x20u, BTC_EXP_NONE)

/* Vendor-specific OGF 0x3F (opcode & 0xFC00 == 0xFC00): BTC_O, BTC_REJ, BTC_CC, plen 4 — matched by OGF, not listed.
 * Any other opcode: CS, status 0x01, unknown_opcode counter++ (host tests assert zero).                                */

/* Synthesized constants (§3). */
#define BT_CTRL_H2C_ACL_LEN          1021u   /* 1005 HC_ACL_Data_Packet_Length */
#define BT_CTRL_H2C_ACL_CREDITS      4u      /* 1005 HC_Total_Num_ACL_Data_Packets; <= EM TX pool (Exp 1 C) */
#define BT_CTRL_C2H_ACL_LEN          1021u   /* enforced from 0C33 */
#define BT_CTRL_C2H_ACL_CREDITS      4u      /* enforced from 0C33 */
#define BT_CTRL_HCI_VERSION          0x06u
#define BT_CTRL_HCI_REVISION         0x0000u
#define BT_CTRL_LMP_VERSION          0x06u
#define BT_CTRL_MANUFACTURER         0x0060u /* RivieraWaves; must never be 0x000F */
#define BT_CTRL_LMP_SUBVER           0x0000u /* Exp 1 A: replace with ROM LMP_version_res value */
#define BT_CTRL_EXT_FEAT_MAX_PAGE    1u
#define BT_CTRL_FEAT_P0_ROM          { 0x7Fu, 0xFEu, 0xCDu, 0xFAu, 0xDBu, 0xFDu, 0x7Bu, 0x87u }
#define BT_CTRL_FEAT_P0_AND_MASK     { 0x1Fu, 0x06u, 0xC4u, 0x0Au, 0x98u, 0x1Du, 0x19u, 0x85u }
#define BT_CTRL_FEAT_P0_EIR_BIT      { 6u, 0x01u }   /* cleared unless 0x0C52 effective action is FWD */
#define BT_CTRL_FEAT_P0_INQ_RSSI_BIT { 3u, 0x40u }   /* stays clear until 0x0C45 + 0x22/0x2F packers pass G-C */
#define BT_CTRL_FEAT_P1_SSP_HOST     0x01u   /* features[0] of page 1 iff last 0C56 (enable) returned s=0 */
```
