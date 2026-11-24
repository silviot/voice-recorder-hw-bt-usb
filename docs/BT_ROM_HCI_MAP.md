# RW-BT mask ROM: HCI command → KE message map (static analysis)

Status: **static disassembly only, nothing here has run on silicon.** This file resolves the `BT_CTRL_OPCODE_TABLE.md`
rows marked "needs disassembly" (G-B: `0C18 0C1C 0C1E 0C3A 0C52`; G-C: `0C45 0401 0402 0419 041A 0405 0408 041F 041D
041B 041C 040F 0C37 0C28 0411 0413 0434`), the stage-A cross-checks (`1009` via `E05D→E0C0`, `1001`/`LMP_version_res`)
and the reply ids of `1005 1001 1002 0C14 1004`. §9 decodes every stock event packer (ROM id → HCI event and
de-padding) and §10 lists the phone-mode path. The opcode table decides the actions: since 2026-11-24 it forwards the
phone-path rows of §2 (`BT_CTRL_OPCODE_TABLE.md` §7 item 11); the other G-x gates still need their hardware stage (§7).

Citation key: `ROM 0xNNNNN` = `dumps/bootrom_384K.bin` (base 0). `FW 0xNNNNNN` = stock `dumps/firmware_2M.bin` in XIP
addresses (`addr = 0xC00000 + file offset`). `RAM 0x81xxxx` = stock `.ramtext/.data`, load image at file `0x039470`
(`addr = file offset + 0x7D37D0`, `FIRMWARE_ANALYSIS.md` §1). `F:` = `open_fw/src/bt_hci.c`. `I:` = `BT_ROM_INTERFACE.md`.
Reproduction recipe: §8.

## 1. Where the mapping lives

The ROM has the RW-BT kernel and the LM/LC/LD tasks but **no HCI layer** (I §1). The HCI command → KE id mapping, the
HCI parameter unpacking and the KE reply → HCI event packing were all in the **stock flash firmware**. The stock
firmware's tables are the vendor's own record of how the ROM is driven. They were decoded here and every target entry
was cross-checked against the ROM handler that receives it.

| Piece | Location | Format |
|---|---|---|
| ROM TASK_LM descriptor | `.data` `0x00800660` (load image ROM `0x5BE60`) | `{state_handler 0x800620, default_handler 0x800618, state 0x80381C, state_max 8, idx_max 1}` |
| ROM TASK_LM default handlers | ROM `0xD96C`, 124 entries | `{u16 id, u16 pad, u32 fn}` |
| ROM TASK_LM per-state handlers | `0x800620[8]`: s1 `0xDD6C`(2), s2 `0xDD5C`(2), s3 `0xDD54`(1), s4 `0xDD4C`(1) | all entries = ROM `0x309E8` (`return 2` = **SAVED**) for `E004`, `E00C` |
| ROM TASK_LC descriptor | `0x008005F8`: `{0, default 0x8005F0, state 0x80137C, ..., idx_max 7}` | default table ROM `0xCC14`, 199 entries |
| ROM TASK_LD descriptor | `0x00800608` | no message handlers |
| Stock opcode → KE id | FW `0xCD6A28` (OGF1, 62), `0xCD6810` (OGF2, 17), `0xCD6854` (OGF3, 109), `0xCD6B20` (OGF4, 11), `0xCD6A08` (OGF5, 8) | per OGF: `u16 ke_id[n]`, then `u8 src[n]` (`0x3C` = supported), then `u8 size[n]` (ROM param struct size; OGF4 has no size array). Alloc: FW `0xD558BC` → `ke_msg_alloc(id, id>>10, 0x3C, max(size, rx_len))` |
| Stock zero-parameter commands | FW `0xD55970` (a switch, not a table) | `ke_msg_send_basic(id, id>>10, 0x3C)`. Notably `0402→E001`, `0404→E003`, `0C14→E026`, `0C57→E050`, OGF4 from the table above |
| Stock `1004` special case | FW `0xD555C4` | OGF4 OCF 4 is the only OGF4 command with a parameter: `E05B`, 1 byte |
| Stock per-opcode unpack ("finalize") | FW jump tables `0xD55C10` (OGF1), `0xD55D90` (OGF2), `0xD55E34` (OGF3), dispatcher FW `0xD55B90` | the HCI wire bytes are already in `param`; the finalizer rebuilds u16/u32 fields **in place** and, for LC-bound ids, sets `dest_id` from the handle (FW `0xD560CC`) |
| Stock TASK_HCI | descriptor RAM `0x8188C4`, default table RAM `0x818B3C`: `{0xFFFF → RAM 0x8188D4}`, `{0xF00B → FW 0xD596AC}` | handler 0x8188D4 indexes the reply tables below |
| Stock reply table, LM | RAM `0x8178E0`, index `id − 0xE065`, ids `E065..E0D4` | 8 B: `{i8 hdr_off (−5 = CC/CS, −2 = event), u8 ret_len (CC return bytes incl. status), u16 opcode, u32 packer}` |
| Stock reply table, LC | RAM `0x817628`, index `id − 0xE427`, ids `E427..E47C` | same |

Confidence key used below: **exact** = stock table entry + ROM handler-table entry + handler disassembly agree (ids,
offsets, reply id). **inferred** = read from code but the meaning (state names, side effects, timing) is an
interpretation.

### 1.1 Parameter layout rule (exact)

For every target opcode the stock finalizer only rebuilds fields at the offsets where they already sit (byte-wise
`u16`/`u32` composes, which are no-ops on little-endian), copies directly, or runs a byte loop that stores each byte back
onto itself (`0C52` FW `0xD58E5C`, and `040D` FW `0xD586DA`). **So the ROM struct of every target command is the
HCI wire layout, zero-padded up to the stock size.** The legacy verbatim copy (F:930–939, `min_len` = stock size) is
therefore correct for these opcodes. Only these stock finalizers really move fields, and none of them is a target:
`042E` (passkey u32 moved from wire offset 6 to 8), `0428`/`0429`/`043D`/`043E` (sync connection), `0C33`, `0C35`.

### 1.2 Reply delivery facts (exact)

* CC/CS helpers: ROM `0x32E40` (LM CC) and `0x32E54` (LM CS) = `ke_msg_alloc(id, dst, src, 1); param[0] = status;
  send`. ROM `0x29F4A` is the LC equivalent. Wider replies are allocated inline.
* Destination: most handlers hard-code `dst = 0x3C` (TASK_HCI). A few use the command's `src_id` (`E01C` 0C01 at ROM
  `0x423C4`, `E00C` 0419 CS at `0x3F3BA`, the `0x3F430` example in I §3.2). `src` is `0x38` for LM replies and the link's
  LC task `(link<<8)|0x39` for LC replies. **Correction to I §3.2**: replies do not generally go to `src_id`. All of
  them reach TASK_HCI only because `bt_ctrl` always sends with `src = 0x3C`.
* Three handler outcomes matter to `bt_ctrl`: (a) reply sent; (b) return `2` = **SAVED**, so the command waits on the
  saved queue until the task changes state and then runs again, and the reply is late; (c) return `0` **without any
  reply** ("silently consumed"). Case (c) exists only in LC handlers: in LC state `0` for all of the §2 LC rows except
  `0C28`, and also in states `0x20`/`0x24..0x26` for `040F 0411 0413 041C 041F` (inferred meaning: link idle /
  detach in progress; the handle can still pass `conhdl_to_task`). Bluedroid would wait forever for the CS/CC.

## 2. Per-opcode map (G-B and G-C rows)

Struct = ROM param struct (HCI wire order, offsets in bytes). "Initial reply" is the KE id the ROM sends for the CC/CS.
Its param layout is given and, unless noted, it is **identical to the HCI return parameters** (the stock generic
packer FW `0xD5860C` copies `ret_len` bytes verbatim).

| Opcode | KE id → task | Struct (size) | ROM handler: checks and behaviour | Initial reply (layout) | Terminal event | Conf. |
|---|---|---|---|---|---|---|
| `0C18` Write Page Timeout | `E02A` → LM | `u16 timeout@0` (2) | `0x4220C`: `0` → `0x12`, else stored (`0x3BC00`) | CC `E088` `{s}` | — | exact |
| `0C1C` Write Page Scan Activity | `E02E` → LM | `u16 interval@0, u16 window@2` (4) | `0x3EC3E`: interval `0x12..0x1000` and even, window `0x11..0x1000`, window ≤ interval, else `0x12`. Updates LD now (`0x3639C`) and sets the page-scan repetition mode (`0x3BC18`): R0 if window = interval, R1 if interval ≤ `0x800`, else R2 | CC `E08C` `{s}` | — | exact |
| `0C1E` Write Inquiry Scan Activity | `E030` → LM | as `0C1C` (4) | `0x3ECAC`: same range checks; LD update `0x36164` | CC `E08E` `{s}` | — | exact |
| `0C3A` Write Current IAC LAP | `E042` → LM | `u8 num@0, u8 lap[3]@1` (4; longer if sent) | `0x3EBA6`: **`num` must be exactly 1**, else `0x12`. Only one IAC is kept | CC `E0A2` `{s}` | — | exact |
| `0C52` Write EIR | `E04D` → LM | `u8 fec@0, u8 eir[240]@1` (241) | `0x41E66` → `0x3A9D0`: `fec > 1` → error; computes the significant length and picks the EIR packet type | CC `E0AF` `{s}` | — | exact |
| `0C45` Write Inquiry Mode | `E047` → LM | `u8 mode@0` (1) | `0x418C8`: `mode ≤ 2` stored at LM env `+0x219`, else `0x12`. The mode selects the result id (§3) | CC `E0A9` `{s}` | — | exact |
| `0401` Inquiry | `E000` → LM | `u8 lap[3]@0, u8 len@3, u8 num_rsp@4` (5) | `0x3ED44`: LM state 0: `len` `1..0x30` else `0x12`; starts LD inquiry `0x35F94(lap, len, num, mode)`, then LM state 4. State 1: SAVED. State 2: SAVED, or `0x3A` if env `+0x4b`. State ≥ 3: `0x0C` | CS `E0C7` `{s}` | results: `E0D1`/`E0D6`/`E0D7` (§3); completion `E0D0` `{s}` → `0x01` | exact |
| `0402` Inquiry Cancel | `E001` → LM, **no params** (send_basic) | — | `0x3F2BC`: only in LM state 4: LD cancel `0x36058`; on success LM state 7 and **no reply yet**. Otherwise CC `E066` with `0x0C` (or the LD error) | CC `E066` `{s}`, deferred: sent by the `EC07` handler `0x3EF8E` (LD inquiry stopped) in state 7, then LM state 0. No `E0D0` after a cancel | — | exact |
| `0419` Remote Name Request | `E00C` → LM | `bdaddr@0, u8 psrm@6, u8 rsvd@7, u16 clkoff@8` (10) | `0x3F348`: LM state ≠ 0 → SAVED (the s1–s4 tables also SAVE it). `psrm > 2` → `0x12`; RNR already running (env `+0x217`) → `0x0C`. Existing ACL → `E483{bdaddr}` to that LC (LMP name request); otherwise temporary page with DM1 (`0x32ED0`→`0x32E68`), `0x09` if no free link, other failure `0x12` | CS `E0CB` `{s}` (dst = `src_id`) | `E0D3` `{s@0, bdaddr@1, name[248]@7}` = wire `0x07` (255 B) | exact (ids, layout); inferred (temporary-link teardown) |
| `041A` RNR Cancel | `E00D` → LM, then forwarded to LC | `bdaddr@0` (6) | LM `0x41DA4`: no link for `bdaddr` or no RNR running → reply `0x12`. Else `ke_msg_forward` to LC `0x346CC`: LC state 2 (paging) → cancel page (`0x36298`), status 0, LC state `0x1A`; else `0x0C` | CC `E06E` `{s@0, bdaddr@1}` (7) | `E0D3` (status from the aborted page, inferred) | exact |
| `0405` Create Connection | `E004` → LM | `bdaddr@0, u16 pkt_type@6, u8 psrm@8, u8 rsvd@9, u16 clkoff@0xA, u8 allow_rs@0xC` (14) | `0x318BC`: LM state 1/2 → SAVED, ≥ 3 → `0x0C`. `psrm > 2` or `allow_rs > 1` → `0x12`; link exists → `0x0B`; `0x32E68` allocates the link (`0x09` = no free link), sends `E480{bdaddr, pkt_type, allow_rs}` to the new LC and starts paging (`0x3618C`). LM state 3 | CS `E0C8` `{s}` | `E0D2`/`E456` (packed today, F:763+) | exact |
| `0408` Create Connection Cancel | `E005` → LM, then `E47E` → LC | `bdaddr@0` (6) | LM `0x32734`: no link → CC with `0x02`. Else `E47E{bdaddr}` to the LC (`0x30E4C`): already connected → `0x0B`; else status 0, then cancel the page (`0x36298`) or start detach | CC `E069` `{s@0, bdaddr@1}` (7) | `E0D2`/`E456` with status `0x02`/`0x0B` (inferred) | exact |
| `040F` Change Conn Packet Type | `E401` → LC(handle) | `u16 h@0, u16 pkt_type@2` (4) | `0x28808`: LC states 0/`0x20`/`0x24–26`, or env `+0x46 == 3`: **silent**. State 5: ACL handle: requested types must equal the ROM's computed allowed set (`0x311CC`/`0x31214`), else `0x11`; then LMP. Other handle: SCO modify path (`0x33A4C`). Other states: SAVED | CS `E43D` `{s}` | `E46A` `{s@0, pad, h@2, pkt_type@4}` (6) → `0x1D` (5 B) | exact (ids); inferred (which type sets pass) |
| `0411` Authentication Requested | `E402` → LC | `u16 h@0` (2) | `0x201C4`: states 5/`0x59`/`0x7D` → start (`0x2C38C`); 0/`0x20`/`0x24–26` **silent**; else SAVED | CS `E43E` `{s}` | `E459` (packed) | exact |
| `0413` Set Connection Encryption | `E403` → LC | `u16 h@0, u8 enable@2` (4) | `0x20248`: state 5 only (0/`0x20`/`0x24–26` silent, else SAVED). Enable 1 while not encrypted → 0 and start; enable 0 while encrypted → 0 and stop; any no-op change → `0x12` | CS `E43F` `{s}` | `E45A` (packed) | exact |
| `041B` Read Remote Supported Features | `E405` → LC | `u16 h@0` (2) | `0x2052C`: state 0 **silent**. If remote features are cached (`0x2C9FC`): CS 0 and `E45D` immediately. Else **SAVED** until the LMP features exchange ends | CS `E441` `{s}` | `E45D` `{s@0, pad, h@2, feat[8]@4}` (12; packed today) | exact |
| `041C` Read Remote Extended Features | `E406` → LC | `u16 h@0, u8 page@2` (4) | `0x28710`: state 5: busy (env `+0x1e6`) → `0x0C`; remote page-0 bit 63 clear → `0x1A`; else 0 and LMP `features_req_ext`. Silent in 0/`0x20`/`0x24–26`. Other states: answer from cache, or SAVED | CS `E442` `{s}` | `E46D` `{s@0, pad, h@2, page@4, max_page@5, feat[8]@6}` (14) → `0x23` (13 B) | exact |
| `041D` Read Remote Version Info | `E407` → LC | `u16 h@0` (2) | `0x2046C`: state 5 → CS 0 and LMP version_req. States 2–4, `0x24–26` → `0x0C`. State 0 silent. Others: cached (env `+0x93`) → CS 0 + `E45E` now, else SAVED | CS `E443` `{s}` | `E45E` `{s@0, pad, h@2, lmp_ver@4, pad, manuf@6, subver@8}` (10; packed today) | exact |
| `041F` Read Clock Offset | `E408` → LC | `u16 h@0` (2) | `0x27F24`: states 5/7/`0xA`; 0/`0x20`/`0x24–26` silent; else SAVED. Slave (env `+0x3d`, inferred) → `E469` built from local clock, CS sent first, then `E469`. Master → CS 0 and LMP clkoffset_req (silent if env `+0x46 == 3`) | CS `E444` `{s}` | `E469` `{s@0, pad, h@2, u16 offset@4}` (6) → `0x1C` (5 B) | exact |
| `0C37` Write Link Supervision Timeout | `E41C` → LC | `u16 h@0, u16 to@2` (4) | `0x206C4`: state 0 silent, ≠ 5 SAVED. Slave → `0x0C`. With env `+0x46 == 2` (inferred: sniff) `to` ≠ 0 must exceed env `+0x12c`, else `0x12`. Else stores it and sends LMP supervision_timeout (`0x2BEB4`, LC state `0x15`). **CC is deferred**: `EC0E` handler `0x2755C` sends it when that LMP PDU (opcode `0x37`) completes | CC `E431` `{s@0, pad, h@2}` (4) → wire `s,h` | — | exact (ids); inferred (what completes `EC0E`) |
| `0C28` Write Automatic Flush Timeout | `E419` → LC | `u16 h@0, u16 to@2` (4) | `0x2841C`: **no state check**. `to ≥ 0x800` → `0x12`, else LD `0x3D2B0(link, to)` | CC `E42E` `{s@0, pad, h@2}` (4) | — | exact |
| `0434` IO Capability Request Negative Reply | `E018` → LM, then forwarded to LC | `bdaddr@0, u8 reason@6` (7) | LM `0x41D0A`: link found → forward to LC `0x2368E` and CC 0 **immediately**; no link → CC with **`0x01`** (not `0x12`, unlike `042B`). LC: stores `bdaddr`; in pairing states `0x5C`/`0x5D` sends LMP not_accepted with `reason` and ends pairing; in other states does nothing | CC `E076` `{s@0, bdaddr@1}` (7) | — | exact |

Stock `src`/`size` entries for all of these: `src = 0x3C`, sizes as in the Struct column. LC-bound ids that the stock
treats as **CS** when the handle is invalid (FW `0xD560CC`): `E019`, `E400..E408`, `E40A..E411`, `E415`, `E41D`, `E41E`.
The stock answers those with `E454` (generic LC CS: `{u16 opcode@0, u8 status@2}`, status `0x02`). The others (for
example `E419`, `E41C`) get `E455` (generic CC with handle: `{u16 opcode@0, u8 status@2, pad, u16 h@4}`). These two ids
are stock-synthesised (TASK_HCI → TASK_HCI), **not** sent by the ROM. `bt_ctrl` already validates handles itself.

## 3. Inquiry result ids (exact)

The LD → LM result handlers (`EC02` → `0x3EFC4`, `EC05` → `0x3F064`, LM states 4/5) choose the id from the inquiry mode
(`0C45`). Each message carries **one** response. In modes 1 and 2 the RSSI comes from the `rwip_rf` vtable
`0x00803BDC+0x1C`, which open_fw fills with `rf_rssi_convert` (F:537, F:1161).

| Mode | KE id | ROM struct | HCI event |
|---|---|---|---|
| 0 | `E0D1` (16 B) | `num@0, bdaddr@1, psrm@7, rsvd@8, rsvd@9, cod[3]@0xA, pad@0xD, u16 clkoff@0xE` | `0x02` (15 B): move `clkoff` 0xE → 0xD (stock packer FW `0xD594BA`) |
| 1, or 2 without EIR data (`EC02`) | `E0D6` (16 B) | `num@0, bdaddr@1, psrm@7, rsvd@8, cod[3]@9, u16 clkoff@0xC, i8 rssi@0xE, pad` | `0x22` (15 B) = struct bytes 0..14 |
| 2 with EIR data (`EC05`) | `E0D7` (256 B) | as `E0D6`, then `eir[240]@0xF` (length clamped to 240, `0x3130E`) | `0x2F` (255 B) = struct bytes 0..254 |

`num` is written as 1 on every path **except** `EC05` in mode 0 (`0x3F088`), which leaves `num@0 = 0`. The stock packer
would then emit a zero-response `0x02` event. That path is probably unreachable (EIR data only in mode 2, inferred), but
a `bt_ctrl` packer should force `num = 1` or drop the event.

The stock firmware has **no packer for `E0D6`/`E0D7`**. Its LM reply table ends at `E0D4`, and it never sent `0C45`.
These two layouts are therefore read from the ROM senders only (ROM `0x3F01E`, `0x3F0C6`, `0x3F10A`).

## 4. Cross-checks and read-only replies (exact unless noted)

`bt_ctrl` answers these locally (SYN). The ROM path exists and can serve as a cross-check:

| Opcode | KE id (params) | ROM handler | Reply id and ROM layout | Notes |
|---|---|---|---|---|
| `1009` | `E05D` (none) | `0x42406` | `E0C0` `{s@0, bdaddr@1}` (7) = wire | `bdaddr` read by `0x3BA68` from `0x00802C14` (written by `ld_bd_addr_set` `0x3BA30`). **Stage-A probe cross-check is valid as specified.** |
| `1001` | `E058` (none) | `0x41854` | `E0BB` `{s@0, hci_ver@1, u16 hci_rev@2, lmp_ver@4, pad, u16 manuf@6, u16 subver@8}` (10) → wire 9 B (stock packer FW `0xD590AC` packs `manuf`/`subver` down by 1) | `hci_ver = lmp_ver = *0x803C17`; `hci_rev = subver = (*0x803C16 << 8) \| *0x803C15`; `manuf = *(u16*)0x803C18`. The **`LMP_version_req/res` builders (ROM `0x21750`, `0x21C42`) read the same four bytes** (`VersNr`, `CompId`, `SubVersNr`). With F:1186–1189: version `0x06`, subversion `0x270F`. `0x803C18` has no writer in open_fw or in the stock prelude (literal search), so it stays at the Boot ROM `.bss` zero: **CompId `0x0000`** (inferred). Table §3.3 SYN (`0000` rev, `0060` manufacturer) therefore **disagrees** with what peers see over LMP. |
| `1005` | `E05C` (none) | `0x417BC` | `E0BF` `{s@0, pad, u16 acl_len@2, u8 sco_len@4, pad, u16 acl_num@6, u16 sco_num@8}` (10) → wire 8 B (FW `0xD590C8`) | Constants: `acl_len 0x3FD`, `sco_len 0xFF`, **`acl_num 9`**, `sco_num 4`. `acl_num = 9` agrees with the 9-buffer EM TX pool (I §6.1): static support for raising N at G-C. |
| `1002` | `E059` (none) | `0x423E0` | `E0BC` `{s@0, cmds[64]@1}` (65) = wire | `0x317D0` copies 30 octets from ROM `0xD2DC` and zeroes octets 30–63. |
| `1004` | **`E05B`, 1 B `{page@0}`** | `0x41F30` → `0x3BA7C` | `E0BE` `{s@0, page@1, max_page@2, feat[8]@3}` (11) = wire | Pages: page 0 `0x802C04`, page 1 `0x802C0C`. `max_page` = 1 if any page-1 byte ≠ 0, otherwise 0 (`0xFF` if page 0 is all zero too). `page > max_page` → status 0 with **zero features** (not `0x12`). `0C56` mode 1 (`E04F` `0x41E1E` → `0x31B78` → `0x3C5D4`) sets page-1 bit 0, so the ROM's own page-1 reply after SSP matches table §3. |
| `0C14` | **`E026`** (none) | `0x41738` | `E084` `{s@0, name[248]@1}` (249) = wire | Copies `len` bytes from LM env `+0x1D`. `len` is set by `0C13` (`E025` `0x3ED00`) = bytes before the first NUL, but up to **249** when there is no NUL. A 248-byte name with no NUL reads 1 byte past the `0C13` message and copies 249 bytes into the 249-byte `E084` param at offset 1: a 1-byte heap overrun (edge case, inferred effect). |

## 5. Corrections to existing documents (for the next editor of those files)

* `BT_CTRL_OPCODE_TABLE.md` / `BT_HCI_MATRIX.md` "stock slot zero / KE id unknown": `0402 → E001` (no params; reply
  `E066`, deferred), `0C14 → E026` (reply `E084`), `1004 → E05B` (1-byte page param; reply `E0BE`). The stock firmware
  sends them from code (FW `0xD55970`, `0xD555C4`), not from the per-OGF table. `0404 → E003` (reply `E068`).
* `0C52`: reply id is `E0AF`. The "candidate" ids of every row in §2 are confirmed (`E02A E02E E030 E042 E047 E000 E00C
  E00D E004 E005 E401 E402 E403 E405 E406 E407 E408 E41C E419 E018`).
* I §3.2 "replies go to `src_id`": only some do; most hard-code TASK_HCI (§1.2).
* I §11 Q8 (reply ids and layouts of `1005/1001/1002/0C14/1004`): answered statically in §4.
* `FIRMWARE_ANALYSIS.md` §14 "ACL ... × 9 buffers": `1005` returns the same 9 (ROM `0x417D6`).

## 6. Consequences for `bt_ctrl` (facts the opcode table must absorb; no change made here)

1. **Silent consumption** (§1.2 (c)): in LC state 0 the handlers for `040F 0411 0413 041B 041C 041D 041F 0C37` reply
   nothing, and so do `040F 0411 0413 041C 041F` in states `0x20`/`0x24..0x26` (`0C28` always replies). If any of these is forwarded around a disconnect, the one-command
   admission window stalls. `bt_ctrl` needs a per-command deadline that synthesises the CS/CC failure, or must refuse to
   forward to a handle whose `Disconnection Complete` is pending.
2. **SAVED commands reply late**: `0401`/`0405` in LM states 1–2, `0419` in any LM state ≠ 0 (for example during an
   inquiry: the RNR waits until the inquiry ends), and the LC reads while LMP data is not cached. Deadlines must allow
   for this (inquiry `len × 1.28 s`).
3. **Deferred replies by design**: `0402` (CC after the LD stops) and `0C37` (CC after the LMP PDU completes).
4. `0C3A` with two IACs (Bluedroid limited discoverable: LIAC + GIAC, `I:216`) → `0x12`. Only the general mode works.
5. `0434` without a link answers `0x01`. A REJ that also uses `0x01` is indistinguishable, which is harmless.
6. `040F`: a packet-type set that differs from the ROM's computed set fails with `0x11`. Bluedroid's request must match
   exactly (G-C).
7. The `1001` SYN and LMP `CompId`/`SubVersNr` should agree (§4). Writing `*(u16*)0x803C18` and `0x803C15..17` at
   start-up would make them agree. That is a design decision for M1, not done here.
8. `E0D6`/`E0D7` have never been exercised by any firmware. Enabling RSSI/EIR inquiry (page-0 bit 30, G-C) needs new
   packers with the §3 layouts, and `rf_rssi_convert` output checked against a reference RSSI.

Status of these items in `bt_ctrl` (2026-11-24): 1 → dead-handle rule and forwarded-command deadline
(`BT_CTRL_OPCODE_TABLE.md` §2 rules 8–9); 2 → the deadline is 7 s, so a command SAVED for longer (an RNR behind a
long inquiry) is answered `0x22`, which Bluedroid would otherwise turn into an 8 s command timeout and a BT fault;
3 → `0402`/`0C37` stay gated; 5 → `0434` is FWD; 6 → `040F` stays gated; 8 → still open (inquiry mode 0 only).

## 7. What remains uncertain and how stage B/C confirms it

Static facts are tied to ROM and FW addresses. None of them is a measured reply. Every row in §2 still needs its
golden vector. A probe step = send the command through `bt_ctrl` → ROM. Log the raw reply KE id, `param_len` and the
param bytes (before packing). Compare them with this file, then with the packed HCI bytes.

| Uncertain item | Stage | Probe that settles it |
|---|---|---|
| Write commands reach the handler and reply with the listed id | B (no peer) | `0C18 2000` → `E088 s=0`, `0C18 0000` → `E088 s=12` (negative case proves the routing); `0C1C 0800/0012` → `E08C 0`, odd interval → `12`; `0C1E` likewise → `E08E`; `0C3A` num=1, LAP GIAC `0x9E8B33` → `E0A2 0`, num=2 → `12`; `0C45 00` → `E0A9 0`, `03` → `12`; `0C52 00+240 B` → `E0AF 0`, `fec=02` → error |
| `0402` deferral and LM state machine | B | `0402` while idle → `E066 s=0C` at once. `0401` GIAC, len 1, num 0 (1.28 s) → `E0C7 0`, then `E0D0 0` with no peer nearby. `0401` + `0402` → `E0C7 0`, later `E066 0`, and **no** `E0D0` |
| `0419`/`041A`/`0408` without a peer | B | `0419` to an absent address → `E0CB 0`, then `E0D3` with a page-timeout status after `0C18`'s timeout. `041A` during it → `E06E 0` (LC path), and with no RNR → `E06E 12`. `0408` with no page → `E069 02` |
| `0434` without a link | B | `E076 s=01` |
| Mode 1/2 inquiry ids and layouts (`E0D6`/`E0D7`), RSSI scaling | C (any discoverable phone) | `0C45 01`/`02`, then `0401` → `E0D6`/`E0D7` with the §3 offsets; RSSI against a reference scan |
| LC handlers in state 5, SAVED behaviour, reply ordering | C (one peer) | `0405` → `E0C8` + `E0D2`/`E456`; then `041B` (`E441` + `E45D`), `041D` (`E443` + `E45E`), `041F` (`E444` + `E469`; CS before the event), `041C` (`E442` + `E46D` or `1A`), `040F` with Bluedroid's set (`E43D`, then `E46A` or `11`), `0411`/`0413` (`E43E`/`E43F` + `E459`/`E45A`), `0C28 h 0000` → `E42E`, `0C37 h 7D00` as master → `E431` (deferred; time it) and as slave → `0C` |
| Silent states | C | Send `041D` right after `0406`, before `E458`: expect **no** reply (validates §6.1). This is a negative test, so the probe must use its own deadline |
| `0x803C18` CompId, `0x803C15..17` | A/B | Forward `E058` once and read `E0BB` raw (one ROM message, read-only), or read the SRAM bytes in the probe summary. A peer's LMP version (stage C, `041D` from the phone side) shows the same value |
| `0C13` 248-byte unterminated name | B | Send 247/248-byte names and check heap integrity (`bt_rom_real` heap validation) after `0C14` forwarded once |

## 8. Reproduce

Tools: `llvm-objcopy-20`, `llvm-objdump-20`, Python 3. Nothing in the tree is needed except the gitignored dumps.

```sh
D=dumps; T=/tmp/btmap; mkdir -p $T
llvm-objcopy-20 -I binary -O elf32-littlearm --rename-section .data=.text,alloc,load,code,contents $D/bootrom_384K.bin $T/rom.elf
llvm-objcopy-20 -I binary -O elf32-littlearm --rename-section .data=.text,alloc,load,code,contents $D/firmware_2M.bin $T/fw.elf
dd if=$D/firmware_2M.bin of=$T/ram.bin bs=1 skip=$((0x39470)) count=$((0xD740))
llvm-objcopy-20 -I binary -O elf32-littlearm --rename-section .data=.text,alloc,load,code,contents $T/ram.bin $T/ram.elf
for f in rom fw ram; do llvm-objcopy-20 --strip-all $T/$f.elf; done
llvm-objdump-20 --triple=thumbv7em -d --no-show-raw-insn $T/rom.elf > $T/rom.dis                        # base 0
llvm-objdump-20 --triple=thumbv7em -d --no-show-raw-insn --adjust-vma=0xC00000 $T/fw.elf > $T/fw.dis    # XIP
llvm-objdump-20 --triple=thumbv7em -d --no-show-raw-insn --adjust-vma=0x80CC40 $T/ram.elf > $T/ram.dis  # stock .ramtext
```

```python
import struct
rom = open('dumps/bootrom_384K.bin','rb').read(); fw = open('dumps/firmware_2M.bin','rb').read()
# ROM handler tables: {u16 id, u16 pad, u32 fn}
for name, off, n in (('LM', 0xD96C, 0x7C), ('LC', 0xCC14, 0xC7)):
    for i in range(n):
        kid, _, fn = struct.unpack_from('<HHI', rom, off + 8*i); print(name, hex(kid), hex(fn))
# Stock opcode tables: ke_id[n], src[n], size[n] (OGF4: no size array)
for ogf, (b, n) in {1:(0xD6A28,0x3E), 2:(0xD6810,0x11), 3:(0xD6854,0x6D), 4:(0xD6B20,0x0B), 5:(0xD6A08,8)}.items():
    ids = struct.unpack_from('<%dH' % n, fw, b); src = fw[b+2*n:b+3*n]; sz = fw[b+3*n:b+4*n]
    for i in range(n): print('%04X' % (ogf<<10 | i+1), hex(ids[i]), src[i], sz[i] if ogf != 4 else '-')
# Stock reply tables (RAM image: file = addr - 0x7D37D0): {i8 hdr_off, u8 ret_len, u16 opcode, u32 packer}
for base, first, n in ((0x8178E0, 0xE065, 0x70), (0x817628, 0xE427, 0x56)):
    for i in range(n):
        off, rl, op, fn = struct.unpack_from('<bBHI', fw, base - 0x7D37D0 + 8*i); print(hex(first+i), off, rl, hex(op), hex(fn))
```

Lesson worth keeping: the per-OGF tables give the KE id but not the zero-parameter commands, which the stock
dispatches from a `switch` (FW `0xD55970`). Read the reply table before declaring a reply id "unknown", because every
LM/LC id from `E065` to `E47C` is listed there with its opcode and return length.

## 9. Event packers (stock reply tables, exact)

The stock reply tables (§1) give every LM/LC reply id with a packer. Each packer was read (FW `0xD591E0..0xD596AC`;
the message param starts at packer `r0 + 0x10`, the event code goes to `r0 + 0xE`, the length to `r0 + 0xF`). They
all follow one rule: the ROM struct is naturally aligned and the packer removes the pad byte after a leading status
and re-packs u16/u32 fields in place. `bt_ctrl_evt.c` implements every row below; host golden vectors:
`test_bt_ctrl.c` `test_event_packers`. **L** = the legacy `bt_hci.c:741-850` packer agrees byte for byte (legacy
phone mode exercises it on silicon whenever that event occurs).

| ROM id | Event | ROM struct → wire | Packer | |
|---|---|---|---|---|
| `E0D0` | `0x01` Inquiry Complete | `{s}` (1) | `0xD594B0` | |
| `E0D1` | `0x02` Inquiry Result | §3 (16 → 15, `num` forced to 1) | `0xD594BA` | |
| `E0D2`, `E456` | `0x03` Connection Complete | `{s, pad, h, pad, a@5, type@11, enc@12}` (13 → 11) | `0xD594D8` | L |
| `E457` | `0x04` Connection Request | `{a, cod[3], type}` (10) verbatim | `0xD59504` | L |
| `E458` | `0x05` Disconnection Complete | `{s, pad, h, reason@4}` (5 → 4) | `0xD59510` | L |
| `E459` | `0x06` Authentication Complete | `{s, pad, h}` (4 → 3) | `0xD59528` | L |
| `E0D3` | `0x07` Remote Name Req Complete | `{s, a, name[248]}` (255) verbatim | `0xD5953C` | |
| `E45A` | `0x08` Encryption Change | `{s, pad, h, enc@4}` (5 → 4) | `0xD5954A` | L |
| `E45B` | `0x09` Change Conn Link Key Complete | `{s, pad, h}` (4 → 3) | `0xD59208` | |
| `E45C` | `0x0A` Master Link Key Complete | `{s, pad, h, flag@4}` (5 → 4) | `0xD5921C` | |
| `E45D` | `0x0B` Read Remote Features Complete | `{s, pad, h, feat[8]@4}` (12 → 11) | `0xD59562` | L |
| `E45E` | `0x0C` Read Remote Version Complete | `{s, pad, h, ver@4, pad, manuf@6, subver@8}` (10 → 8) | `0xD59584` | L |
| `E45F` | `0x0D` QoS Setup Complete | `{s, pad, h, flags@4, type@5, pad[2], u32 ×4 @8}` (24 → 21) | `0xD59234` | |
| `E0D4` | `0x10` Hardware Error | `{code}` (1) | `0xD5927A` | |
| `E460` | `0x11` Flush Occurred | `{h}` (2) | `0xD59286` | |
| `E461` | `0x12` Role Change | `{s, a, role}` (8) verbatim | `0xD595AC` | |
| `E462` | `0x13` NOCP | `{n, pad, n × {h, count}}` → `{n, …}` | `0xD5929A` | L (bt_ctrl owns it) |
| `E463` | `0x14` Mode Change | `{s, pad, h, mode@4, pad, interval@6}` (8 → 6) | `0xD595B8` | |
| `E464`/`E465` | `0x16` PIN / `0x17` Link Key Request | `{a}` (6) | `0xD595D8`/`0xD595E4` | L |
| `E466` | `0x18` Link Key Notification | `{a, key[16], type}` (23) | `0xD595F0` | L |
| `E467` | `0x1A` Data Buffer Overflow | `{type}` (1) | `0xD592CC` | |
| `E468` | `0x1B` Max Slots Change | `{h, slots}` (3; the stock packer also zeroes `param[3]`) | `0xD595FC` | L |
| `E469` | `0x1C` Read Clock Offset Complete | `{s, pad, h, offset@4}` (6 → 5) | `0xD592D8` | |
| `E46A` | `0x1D` Conn Packet Type Changed | `{s, pad, h, types@4}` (6 → 5) | `0xD592F4` | |
| `E46B` | `0x1E` QoS Violation | `{h}` (2) | `0xD59310` | |
| `E46C` | `0x21` Flow Specification Complete | `{s, pad, h, flags, dir, type, pad, u32 ×4 @8}` (24 → 22) | `0xD59324` | |
| `E46D` | `0x23` Read Remote Ext Features Complete | `{s, pad, h, page@4, max@5, feat[8]@6}` (14 → 13) | `0xD5938C` | |
| `E46E`, `E46F`, `E470` | `0x2C`/`0x2D` sync link, `0x2E` sniff subrating | not packed by `bt_ctrl` (no SCO/eSCO, no sniff): counted as unknown | `0xD59614`, `0xD593B8`, `0xD593E4` | |
| `E471` | `0x30` Encryption Key Refresh Complete | `{s, pad, h}` (4 → 3) | `0xD59436` | |
| `E472` | `0x31` IO Capability Request | `{a}` (6) | `0xD5965C` | L |
| `E473` | `0x32` IO Capability Response | `{a, io, oob, auth}` (9) | `0xD5944A` | L |
| `E474` | `0x33` User Confirmation Request | `{a, pad[2], u32 value@8}` (12 → 10) | `0xD59668` | L |
| `E475`/`E476` | `0x34` Passkey / `0x35` OOB Data Request | `{a}` (6) | `0xD59456`/`0xD59462` | |
| `E477` | `0x36` Simple Pairing Complete | `{s, a}` (7) | `0xD59684` | L |
| `E478` | `0x38` LSTO Changed | `{h, timeout}` (4) | `0xD59690` | L |
| `E479` | `0x39` Enhanced Flush Complete | `{h}` (2) | `0xD5946E` | |
| `E47A` | `0x3B` User Passkey Notification | `{a, pad[2], u32 passkey@8}` (12 → 10) | `0xD59482` | |
| `E47B` | `0x3C` Keypress Notification | `{a, type}` (7) | `0xD59498` | |
| `E47C` | `0x3D` Remote Host Supported Features | `{a, feat[8]}` (14) | `0xD594A4` | |

Command replies: CC generic packer `0xD5860C` copies `ret_len` bytes (status first); CS generic packer `0xD59189`
takes the status from `param[0]`. The two CC-with-handle replies `E42E` (`0C28`) and `E431` (`0C37`) de-pad
`{s, pad, h}` first (`0xD5902C`, `0xD5905C`). `E454`/`E455` are stock-synthesised replies (§2 last paragraph).

## 10. Phone-mode path: commands and events (2026-11-24)

What Bluedroid (our `sdkconfig.h`, `ssp_en = true`, `sc_en = false`, local IO capability DisplayYesNo) sends and needs
when a phone or laptop pages us, pairs with numeric comparison, exchanges ACL, disconnects and re-connects with the
stored key. Send sites as in the opcode table (`A` = `btm_acl.c`, `S` = `btm_sec.c`, `L2` = `l2c_link.c`).

| Step | Host → controller | ROM path | Controller → host |
|---|---|---|---|
| Page from the phone | — | — | `E457` → `0x04` |
| Accept, remain slave (`L2:96-106`: no local role switch, so `btm_dev_support_switch()` is false; legacy `F:1592-1595` does the same) | `0409` | `E006` → CS `E0C9`/`E43B` | `E456` → `0x03` |
| ACL set-up reads (`A:435-436`, `A:1229`, `A:1246`) | `041F`, `041D`, `041B`, `041C` p1 | `E408`/`E407`/`E405`/`E406` → CS `E444`/`E443`/`E441`/`E442` | `E469` → `0x1C`, `E45E` → `0x0C`, `E45D` → `0x0B`, `E46D` → `0x23` |
| Name (`S:5002`, BTM name discovery) | `0419`, rarely `041A` | `E00C` → CS `E0CB`; `E00D` → CC `E06E` | `E47C` → `0x3D` (if the ROM sends it), `E0D3` → `0x07` |
| Phone authenticates (we respond) | `040C` or `040B` | `E009`/`E008` → CC `E06B`/`E06A` | `E465` → `0x17` first |
| SSP, responder order | `042B`, then `042C`/`042D` (or `0434`) | `E011` → `E06F`; `E012`/`E013` → `E070`/`E071`; `E018` → `E076` | `E473` → `0x32`, `E472` → `0x31`, `E474` → `0x33`, `E477` → `0x36`, `E466` → `0x18` |
| We authenticate (`S:5606`) | `0411`, then as above | `E402` → CS `E43E` | `0x17`, then **initiator order** `0x31` before `0x32`, … , `E459` → `0x06` |
| Encryption (`S:5624`, or by the phone) | `0413` | `E403` → CS `E43F` | `E45A` → `0x08`; `E471` → `0x30` on a key refresh |
| ACL both ways | ACL (`E426`) | EM TX, `E462` NOCP | `E47D` ACL RX, `0x13` NOCP |
| Link events the phone can cause | — | — | `E461` → `0x12` (role switch: page-0 LMP features still advertise it), `E463` → `0x14` (hold), `E468` → `0x1B`, `E478` → `0x38` |
| Disconnect (`S:1557`, `L2:788`) or by the phone | `0406` | `E400` → CS `E43A` | `E458` → `0x05` |

Legacy `bt_hci.c` ground truth for this path: it accepts as slave (`F:1595`), answers `0x17` with `040B`/`040C`
(`F:1650-1661`), `0x31` with `042B` NoInputNoOutput (`F:1670-1678`), `0x33` with `042C` (`F:1681-1684`), and stores
the key from `0x18` (`F:1664-1667`); it never sends `0411 0413 041B 041C 041D 041F 0419`. Its packers are reused
unchanged (§9 "L"); the commands legacy never sent are forwarded with the §2 layouts, which are the wire layout
zero-padded to the stock size (§1.1), so no command needed a repacking step.

**Lessons**
- Without `041C`, Bluedroid never reads the peer's page 1, `btm_sec_set_peer_sec_caps()` marks the peer
  `BTM_SM4_KNOWN` (legacy) and every service-level security check takes the legacy branch until an IO Capability
  Request happens to set `BTM_SM4_TRUE`. So `041C` is part of the SSP path, not an optional read; the `1002` bit for
  it must be set for Bluedroid to send it at all.
- The SSP event order differs by role: as responder the controller reports the initiator's IO capabilities (`0x32`)
  before asking ours (`0x31`); as initiator it asks first. Bluedroid handles both, the fake ROM peer
  (`tests/fake_rom.h` `fake_peer_*`) plays both so host tests cover both.
- Bluedroid's 8 s command timeout is a stack restart, not a retry: any ROM behaviour that delays an initial reply that
  long (SAVED commands, silent consumption) costs the whole session. `bt_ctrl` answers class-O commands at 7 s.
