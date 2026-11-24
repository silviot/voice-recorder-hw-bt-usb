# bt_ctrl ↔ Bluedroid HCI driver contract

What `bt_ctrl` (plan M1, `docs/BLUETOOTH_BLUEDROID_PLAN.md` v18) must do at the `esp_bluedroid_attach_hci_driver()`
boundary (plan D3). Every statement comes from reading the vendored sources, not from a trace. Line numbers refer to
the tree on `feat/freertos-bluedroid` as it stands now, with patches 0001–0007 applied. Path prefixes:

- `BD/` = `open_fw/third_party/bluedroid/host/bluedroid/`
- `OSI/` = `open_fw/third_party/bluedroid/common/osi/`
- `PORT/` = `open_fw/bt_port/`

Opcode-level reply formats for every command (startup, BTA enable, pairing, links) are in `docs/BT_HCI_MATRIX.md`.
This document covers the transport: who calls what, from which task, what blocks, how credits move, and what the
host asserts on. §12 lists the requirements as a checklist. §11 lists where the plan and the code disagree. §13 gives
the three upstream patches that are still missing, as diffs. They are **not applied**.

Build facts this document relies on: `BT_CONTROLLER_INCLUDED` is FALSE (`PORT/sdkconfig.h:10`
`CONFIG_BT_CONTROLLER_DISABLED`), so the driver-ops path of `BD/api/esp_bluedroid_hci.c` is used. BLE is off
(`UC_BT_BLE_ENABLED`, `UC_BT_BLE_42_SCAN_EN` both FALSE: `BD/common/include/common/bluedroid_user_config.h:258,328`).
`C2H_FLOW_CONTROL_INCLUDED` is TRUE unconditionally (`BD/common/include/common/bt_target.h:1637`). `assert()` is live,
because `bt/Makefile.bluedroid:33-41` has no `-DNDEBUG`. `OPENFW_BLUEDROID=1` is defined for all Bluedroid objects
(`bt/Makefile.bluedroid:40`). `PORT/bt_port_prefix.h:25-26` sets `L2CAP_HOST_FC_ACL_BUFS=4` and
`L2CAP_HOST_FC_ACL_DATA_LEN=1021`.

---

## 1. The interface

`BD/api/include/api/esp_bluedroid_hci.h`:

| Member | Signature | Line | Implemented by | Called by / task |
|---|---|---|---|---|
| ops `send` | `void (*)(uint8_t *data, uint16_t len)` | :43 | bt_ctrl | hciT only (§3.1) |
| ops `check_send_available` | `bool (*)(void)` | :50 | bt_ctrl | hciT only (§3.1) |
| ops `register_host_callback` | `esp_err_t (*)(const esp_bluedroid_hci_driver_callbacks_t *)` | :57 | bt_ctrl | BTC task, during `esp_bluedroid_init()` (§2) |
| cb `notify_host_send_available` | `void (*)(void)` | :22 | Bluedroid `host_send_pkt_available_cb` (`BD/hci/hci_hal_h4.c:577-582`) | bt_dlv (plan M1). Never bt_ctrl. |
| cb `notify_host_recv` | `int (*)(uint8_t *data, uint16_t len)` | :32 | Bluedroid `host_recv_pkt_cb` (`BD/hci/hci_hal_h4.c:618-712`) | bt_dlv (plan M1). Never bt_ctrl. |

The header's only stated return contract is "0 received successfully, failed otherwise" (:30). Upstream always
returns 0 (§5.4).

## 2. Registration and lifecycle

- `esp_bluedroid_attach_hci_driver()` copies the three op pointers into a static table. It succeeds only while
  `esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_UNINITIALIZED`, i.e. before `esp_bluedroid_init()`
  (`BD/api/esp_bluedroid_hci.c:31-52`). `detach` has the same precondition (:54-69). So bt_ctrl attaches once per
  init cycle, and it cannot swap ops while the stack is initialised.
- `esp_bluedroid_init()` runs `btc_init_bluetooth()` in the **BTC task** (`BD/btc/core/btc_main.c:95-117`).
  That calls `bte_main_boot_entry()`, then `bte_main_enable()` → `hci_start_up()` (`BD/main/bte_main.c:98,137-151`).
  `hci_start_up()` creates hciT (`BD/hci/hci_layer.c:122-158`). `hal_open()` → `hci_host_register_callback()` →
  bt_ctrl's `register_host_callback()` (`BD/hci/hci_hal_h4.c:183-194`, `BD/api/esp_bluedroid_hci.c:116-134`).
  - The callbacks argument points to a `static const` table (`BD/hci/hci_hal_h4.c:714-717`). bt_ctrl may keep the
    pointer.
  - bt_ctrl must return `ESP_OK`. Anything else makes `hal_open()` fail, and so does `hci_start_up()`
    (`BD/hci/hci_layer.c:143-145`). The result is that `esp_bluedroid_init()` fails.
  - The RX queue already exists when `register_host_callback` is called (`hci_hal_env_init()`,
    `BD/hci/hci_hal_h4.c:134`), so `notify_host_recv` is valid from that point. No command is sent before
    `esp_bluedroid_enable()` (§9). Anything bt_ctrl delivers before then is unsolicited.
- There is **no unregister**. `hci_shut_down()` → `hal->close()` → `hci_hal_env_deinit()` frees `rx_q` and
  memsets the environment while the callback pointers in bt_ctrl stay live (`BD/hci/hci_layer.c:197-220`,
  `BD/hci/hci_hal_h4.c:149-181,196-199`). `host_recv_pkt_cb()` tests `rx_q == NULL` without a lock
  (`BD/hci/hci_hal_h4.c:633-635`). A delivery that races with `hci_shut_down()` is therefore a use-after-free.
  **bt_dlv must be quiesced (plan M1: session id bumped, late deliveries rejected) before `esp_bluedroid_deinit()`
  runs `bte_main_shutdown()`** (`BD/btc/core/btc_main.c:72,133`). `notify_host_send_available` after shutdown
  is harmless: `hci_downstream_data_post()` returns false when `downstream_data_ready == NULL`
  (`BD/hci/hci_layer.c:227-234`).
- Command credits are re-initialised to 1 only in `hci_layer_init_env()`, i.e. on `esp_bluedroid_init()`
  (`BD/hci/hci_layer.c:253`). A disable/enable cycle without deinit keeps the last value the host learned.
  bt_ctrl's synthesized Reset CC (plan M1 "Reset") sets that value again (§6.1).

## 3. Host → controller: `check_send_available()` and `send()`

### 3.1 Single caller, single task

The only call sites are `hci_host_check_send_available()` and `hci_host_send_packet()` (`BD/api/esp_bluedroid_hci.c:75-86,
88-114`). There are no others in the tree (`grep` over `third_party/bluedroid`). Their only callers are:

- `hci_downstream_data_handler()` loop head: `while (hci_host_check_send_available())` (`BD/hci/hci_layer.c:335`).
- `transmit_data()` → `hci_host_send_packet()` (`BD/hci/hci_hal_h4.c:207-235`). It is reached only through
  `transmit_fragment()` (`BD/hci/hci_layer.c:453-463`), which is reached only through
  `packet_fragmenter->fragment_and_dispatch()`. The fragmenter is called only from the downstream handler and from
  `event_command_ready()` / `event_packet_ready()`, which the handler itself invokes (`BD/hci/hci_layer.c:337-344,
  416-450`).

`hci_downstream_data_handler` runs only as the `downstream_data_ready` event on **hciT** work queue 0
(`BD/hci/hci_layer.c:140,262-264`). hciT has priority `BT_TASK_MAX_PRIORITIES - 3` = 7 (`BD/hci/hci_layer.c:53`,
static-asserted in `open_fw/src/rtos/rtos_port.c:32`). **Both ops are therefore called only by hciT, never
concurrently with each other, and never from ISR context.** No Bluedroid lock is held during either call (the handler
takes none). If no driver is attached, `check_send_available` reports false (`BD/api/esp_bluedroid_hci.c:81-83`).

### 3.2 Loop semantics (what a `true` means)

```
while (check_send_available()) {                         // hci_layer.c:335
    pkt = fragmenter->fragment_current_packet();         // pending ACL continuation, if any
    if (pkt)                       fragment_and_dispatch(pkt);         // :338-339
    else if (cmdq non-empty && command_credits > 0) event_command_ready(); // :340-342
    else if (pktq non-empty)       event_packet_ready();               // :343-344
    else break;                                                        // :345-347
}
```

- **At most one `send()` per `true`.** A command produces one `fragmented(packet, true)` → one `transmit_data`
  (`BD/hci/packet_fragmenter.c:131-133`). An ACL packet produces one fragment per call. When the packet is larger
  than one controller buffer, the rest is parked in `current_fragment_packet` and the loop checks again before the
  next fragment (`BD/hci/packet_fragmenter.c:157-185`).
- **Zero `send()` after a `true` is normal.** The loop exits with nothing to do (:345-347). A command sits in the queue
  while `command_credits == 0` (:340-341). An ACL packet with `len > max_packet_size && layer_specific == 0` is bounced
  back to L2CAP as `MSG_HC_TO_STACK_L2C_SEG_XMIT` without a send (`BD/hci/packet_fragmenter.c:147-152`), and so is a
  segment whose `layer_specific` budget runs out (:171-184). `transmit_data` rejects an invalid type with no send
  (`BD/hci/hci_hal_h4.c:215-218`).
- Hence (plan M1 "TX admission", confirmed): `check_send_available()` must be a **pure query** with no reservation.
  A true answer must stay true until the next `send()`. That holds when only hciT fills the TX ring and only bt_ctrl
  frees slots. Then `send()` cannot fail: it returns `void`, and nothing in Bluedroid would retry or free on failure.
- The check carries **no packet type**. A true answer must cover the largest packet hciT might send next, which is
  any of: command ≤ 1 + 3 + 255 = 259 B, ACL ≤ 1 + 4 + `acl_data_size_classic` = 1026 B with the 1021 B that bt_ctrl
  reports in `1005`. Hence the unified ≤ 1026 B slot (plan M1, §4 table).
- If `check_send_available()` returns false, hciT leaves the handler and does not poll. TX resumes only on the next
  `downstream_data_ready` post (§4).

### 3.3 `send(data, len)` framing and buffer rules

- `data[0]` is the H4 packet indicator, written by `transmit_data` into the byte *before* the HCI packet
  (`--data; *data = type; ++length`), and restored **after `send()` returns** (`BD/hci/hci_hal_h4.c:220-232`).
  `len` = HCI packet length + 1.
  - Types: `0x01` command, `0x02` ACL, `0x03` SCO (`DATA_TYPE_*`, mapped by `event_to_data_type()`,
    `BD/hci/hci_layer.c:740-753`). SCO is not compiled into this build (`BTM_SCO_HCI_INCLUDED` FALSE because
    `UC_BT_HFP_AUDIO_DATA_PATH_HCI` is unset, `BD/common/include/common/bt_target.h:1237-1243`). bt_ctrl should reject
    `0x03` and any other value with an invariant violation (BT fault, plan M1 recovery ladder).
  - Command: `opcode:u16 LE, plen:u8, params[plen]`. ACL: `handle|PB|BC:u16 LE, len:u16 LE, payload[len]`.
- **The buffer is borrowed.** bt_ctrl must copy all `len` bytes into its ring before returning and must not keep
  `data`. Byte `data[0]` belongs to the preceding field of a `BT_HDR` and is overwritten again after the return.
  There is no alignment guarantee: `data` is `packet->data + offset - 1`.
- **ACL from the host is already fragmented** to `controller->get_acl_data_size_classic()`, which is bt_ctrl's own
  `1005` `ACL_len` (`BD/hci/packet_fragmenter.c:141-146`, `BD/device/controller.c:640-644`). The first fragment carries
  the PB flags that L2CAP set. Continuations get PB=`01` (`APPLY_CONTINUATION_FLAG`, `BD/hci/packet_fragmenter.c:32,
  156,168`). An ACL longer than the advertised length is a host bug. bt_ctrl should treat it as an invariant violation,
  not truncate it.
- ACL admission to the *controller* is L2CAP's job. It is initialised from `1005`'s `ACL_count`
  (`l2c_link_process_num_bufs(controller->get_acl_buffer_count_classic())`, `BD/stack/btm/btm_devctl.c:202`) and
  replenished by Number Of Completed Packets events (`0x13`, `BD/stack/btu/btu_hcif.c:389`,
  `BD/stack/l2cap/l2c_link.c:1527`). bt_ctrl must emit exactly one completed-packet count per HCI ACL packet it
  accepted, and must advertise in `1005` no more buffers than it can hand to the ROM immediately (plan M1). Otherwise
  the unified ring fills with ACL that cannot drain.

## 4. `notify_host_send_available()`

Implementation: `host_send_pkt_available_cb()` → `hci_downstream_data_post(OSI_THREAD_MAX_TIMEOUT)`
(`BD/hci/hci_hal_h4.c:577-582`, `BD/hci/hci_layer.c:227-244`) → `osi_thread_post_event()`
(`OSI/thread.c:799-839`).

- It schedules one run of `hci_downstream_data_handler` on hciT. Posts are **coalesced**: if the event is already
  queued (`OSI_EVENT_FLAG_QUEUED`), the post returns false at once (`OSI/thread.c:554-568,815-820`). `QUEUED` is
  cleared *before* the handler runs (`OSI/thread.c:760`), so a notify that arrives while the handler is running queues
  another run. No wakeup is lost if bt_ctrl frees the slot **before** the notify is issued.
- **It can block.** It takes the event's mutex (`OSI/thread.c:814,829`). If the event is not queued, it does
  `xQueueSend(..., portMAX_DELAY)` into hciT work queue 0 (`OSI/thread.c:153-171,457-478`). That queue has length 3
  (`BD/hci/hci_layer.c:56`) and is shared with the C2H `flush_ready` event (`BD/stack/btu/c2h_credit.c:674-683`) and
  with non-coalesced `flush_reassembly_work` posts from BTU (`BD/hci/hci_layer.c:187-191`). It is unbounded in
  principle, and bounded in practice by hciT (priority 7) draining its queue. This is why the plan routes it through
  bt_dlv (priority 6) and never calls it from bt_ctrl.
- **When bt_ctrl must cause one**: whenever a TX ring slot is freed after `check_send_available()` might have returned
  false. Simplest rule: notify on every full→not-full transition. Other events also re-run the handler: any enqueue by
  the host (`BD/hci/hci_layer.c:377,398,411`), and a CC/CS that leaves `command_credits > 0` with a non-empty command
  queue (:654-658). An ACL packet blocked only by a full ring is woken by nothing else.
- It must **not** be called from inside `send()` or `check_send_available()`. Those run on hciT, and hciT would then
  post to its own work queue with `portMAX_DELAY`. That self-deadlocks when the queue is full.
- The return type is void. A false post result is dropped silently (:236-243). This is harmless, because false only
  means "already queued" or "stack shut down".

## 5. Controller → host: `notify_host_recv(data, len)`

### 5.1 What one call must contain

Exactly one complete H4 packet: `data[0]` = `0x04` event or `0x02` ACL, followed by the whole HCI packet, with
`len` = 1 + HCI length. Validation happens later on hciT. Every failure there is a **silent drop**, with only a trace
log, compiled out by `CONFIG_BT_STACK_NO_LOG`:

| Condition | Code | Effect |
|---|---|---|
| `len < 1` | `BD/hci/hci_hal_h4.c:462-466` | dropped |
| type `0x3E` (H4 type compared with the LE meta event code) | :470-483 | dropped |
| type ∉ {2,3,4} (e.g. `0x01`) | :484-491 | dropped |
| shorter than the HCI preamble (event 2, ACL 4) | :492-503 | dropped. ACL with ≥ 2 bytes returns a C2H credit. |
| length field ≠ remaining bytes (also catches two packets concatenated) | :504-523 | dropped. ACL returns a C2H credit. |
| event `0x0E` with params < 3, or `0x0F` with params < 4 | `BD/hci/hci_layer.c:570-574,581-585,614-618` | dropped |

A dropped Command Complete or Command Status stalls command dispatch. After `COMMAND_PENDING_TIMEOUT` = 8 s
(`BD/hci/hci_layer.c:87`), patch 0007 reports it to `bt_port_on_hci_command_timeout()` (:532-534). bt_ctrl's
packers must therefore emit exact lengths. They are tested against these checks (plan M1 golden vectors).

- **Never deliver an LE advertising report** (`04 3E xx 02` / `0B`). `host_recv_adv_packet()`
  (`BD/hci/hci_hal_h4.c:293-309`) routes it to the adv path. With `BLE_42_SCAN_EN` FALSE that path is `assert(0)`
  while the stack is enabled (:703), and a silent drop otherwise (:668-672). The RW-BT controller is BR/EDR-only,
  but bt_ctrl should filter `0x3E` anyway.
- ACL to the host must use PB=`10` (start) or `01` (continuation). Reassembly treats **every value other than `10`
  as a continuation** (`GET_BOUNDARY_FLAG` / `START_PACKET_BOUNDARY`, `BD/hci/packet_fragmenter.c:35,38,221-226`).
  A PB=`00` start would be dropped as "continuation for unknown packet" (:310-317). ACL payload must be
  ≤ 1021 B, the host ACL length announced in `0C33` (§7).
- Ring sizing check (plan §4): event ≤ 1 + 2 + 255 = 258 B (ring slot 260), ACL ≤ 1 + 4 + 1021 = 1026 B (ring slot
  1021+5). Both are consistent.

### 5.2 What the callback does (allocation, queues)

`host_recv_pkt_cb()` (`BD/hci/hci_hal_h4.c:618-712`), for non-adv packets:

1. `rx_q == NULL || data == NULL` → `return 0` (:633-635): the packet is discarded and reported as success (§11 D6).
2. `pkt = osi_calloc(BT_HDR_SIZE + len)`. On failure: **`assert(0)`** (:653-658). Patch 0008 changes this.
3. `memcpy(pkt->data, data, len)` (:663). **The callback copies**, so the caller keeps ownership of `data` and may
   reuse the ring entry as soon as the call returns. (The ISO branch at :639-648 `free()`s the caller's buffer. It is
   compiled out: `BLE_FEAT_ISO_EN` is off.)
4. `fixed_queue_enqueue(rx_q, pkt, FIXED_QUEUE_MAX_TIMEOUT)`. The **result is ignored** (:664-666). Patch 0008
   changes this.
   - `rx_q` has capacity `QUEUE_SIZE_MAX` = 254 (`BD/hci/hci_hal_h4.c:134`, `OSI/include/osi/fixed_queue.h:27`).
   - The enqueue takes `enqueue_sem` with `OSI_SEM_MAX_TIMEOUT` (**blocks forever while 254 packets are queued**),
     then takes the queue mutex and calls `list_append()`. That **allocates a list node** (`OSI/list.c:152-160`) and
     can fail *after* the slot was taken. Upstream then neither gives the slot back nor reports the failure to this
     caller (`OSI/fixed_queue.c:137-156`). Patch 0009 changes this.
5. `hci_upstream_data_post(OSI_THREAD_MAX_TIMEOUT)` (:707) posts `upstream_data_ready` to hciT work queue 1. The queue
   has length 1 and only this coalesced event uses it (`BD/hci/hci_layer.c:57`, `BD/hci/hci_hal_h4.c:141-144`), so in
   practice the post does not wait on the queue. It does take the event mutex.
6. `return 0` (:711).

### 5.3 Blocking points (summary)

Heap lock and allocation (`osi_calloc`, `list_append`), `rx_q` slot semaphore (forever), queue mutex, event mutex,
and a work-queue send. The caller must be a task that is allowed to block. bt_dlv qualifies; bt_ctrl and ISRs do not.
hciT (7) preempts bt_dlv (6) as soon as the event is posted, so processing normally finishes before the call returns.
hciT itself can block in `btu_task_post(SIG_BTU_HCI_MSG, ..., OSI_THREAD_MAX_TIMEOUT)` for events
(`BD/hci/hci_layer.c:692`). When it does, `rx_q` fills and bt_dlv eventually blocks on step 4. That is the intended
place for backpressure (plan R11).

### 5.4 Return value

Upstream always returns 0. bt_ctrl cannot learn of a dropped packet today. With patch 0008, the return is `-1` when
the copy cannot be allocated or the enqueue fails. The packet was then **not** accepted, and no host state changed:
the copy is freed and the slot is restored (0009). bt_dlv keeps the ring entry and retries (plan M1: 1-tick backoff,
BT fault after N consecutive failures). The `rx_q == NULL` case stays `0` (§11 D6).

### 5.5 What happens next on hciT (relevant to credits)

`hci_upstream_data_handler()` drains `rx_q` (`BD/hci/hci_hal_h4.c:245-286`):

- Events go to `filter_incoming_event()`. CC/CS are consumed there (§6). Other events go to BTU
  (`BD/hci/hci_layer.c:539-546,680-696`).
- ACL goes to `reassemble_and_dispatch()` (`BD/hci/packet_fragmenter.c:192-371`), then to
  `btu_hci_acl_data_post()` (`BD/stack/btu/btu_task.c:483-518`). That call has a 100 ms enqueue timeout and a
  `osi_malloc`. On failure, `dispatch_reassembled()` **frees the ACL and returns its C2H credit**
  (`BD/hci/hci_layer.c:698-709`). This is an upstream silent data loss path (§11 D7).
- After every batch, `c2h_credit_flush()` runs (`BD/hci/hci_hal_h4.c:262-267`).

## 6. Command flow control

### 6.1 Num_HCI_Command_Packets

- The initial value is 1 (`BD/hci/hci_layer.c:250-253`).
- Every Command Complete **assigns** (not adds) the event's `Num_HCI_Command_Packets` to `command_credits`
  (`STREAM_TO_UINT8(hci_host_env.command_credits, stream)`, `BD/hci/hci_layer.c:586`). Every Command Status does
  the same (:620). This happens **before** the opcode is matched. So a CC/CS with no matching command, including a
  `0x0000` NOP CC, still updates the window (:589-592,625-629 → `intercepted`).
- Each non-NOACK command decrements the count when dequeued (:432). Commands are dequeued only while
  `command_credits > 0` (:340-341). That is the only gate. bt_ctrl's own ROM-side command credit is invisible to the
  host.
- After a CC/CS, hciT re-runs the TX loop only if `command_credits != 0` and the command queue is non-empty
  (:654-658).
- **bt_ctrl obligation**: every CC/CS it emits (forwarded, SYNTH or REJECT) carries the window it wants the host to
  use. The plan does not say what that value is (§11 D4). Recommendation: always `1`. It keeps at most one ordinary
  command in the unified ring, so commands can never fill it ahead of ACL. bt_ctrl then holds that command until its
  ROM credit allows. A forwarded ROM value of `0` must not reach the host unless bt_ctrl later sends a CC (a NOP CC is
  enough) that reopens the window. Otherwise the host stalls until the 8 s timeout.

### 6.2 Response matching and the pending list

- Sent commands are appended to `commands_pending_response` (:433-436). The 8 s response timer is restarted after
  each send (:441) and after every consumed CC/CS (:652). The timer only reports (patch 0007 → BT fault). It never
  frees the entry or resolves a future (:501-536).
- CC/CS are matched by **opcode only**, first match wins (`get_waiting_command()`, :755-776). bt_ctrl must echo the
  exact opcode of the command it answers. A wrong opcode leaves the real command pending, and the reply is consumed as
  "no matching command".
- Command Status: `command_status_cb(status, ...)` is invoked and no CC is expected afterwards (:612-647). A **futured**
  command (all of `controller.c` startup, `transmit_command_futured()`, :381-400) is only ever resolved by a **Command
  Complete** (:607-608). A CS for a futured command frees the entry and leaves `future_await()` blocked forever. The
  wait is `osi_sem_take(..., OSI_SEM_MAX_TIMEOUT)` (`OSI/future.c:72-84`) on BTU (§9). So **every startup command must
  be answered with CC, never CS.**
- Ownership: after a CC with a callback or future, Bluedroid frees the event (:660-671). bt_ctrl only gives a copy
  (§5.2).

### 6.3 NOACK commands (`0C35`)

- `btsnd_hcic_host_num_xmitted_pkts()` builds `0C35` and sets `HCI_CMD_MSG_F_SRC_NOACK`
  (`BD/stack/hcic/hcicmds.c:1322-1353`). It is the only NOACK command in a Classic build. The other one,
  `hciblecmds.c:1050`, is BLE.
- When dequeued, a NOACK command is sent and freed. It does **not** decrement `command_credits`, is not added to the
  pending list, and arms no timer (`BD/hci/hci_layer.c:426-431`).
- **But it is queued in the same FIFO and dequeued only when `command_credits > 0`** (:376, :340-342). If command
  credits are 0, or an ordinary command ahead of it uses the last credit, the credit return never leaves the host. With
  bt_ctrl gating inbound ACL on host credits (plan M1 RX path), that is a deadlock. The HCI spec allows Host Number Of
  Completed Packets regardless of Num_HCI_Command_Packets (Core Vol 4 Part E §7.3.40). → **Patch 0010** (§13.3).
- **bt_ctrl obligation**: produce **no** CC/CS for `0C35`. A spurious CC would be "no matching command", but it would
  still overwrite `command_credits` (§6.1). Handle `0C35` locally (return the host credits to the C2H window, §7)
  without consuming a ROM command credit. Validate `num_handles ≥ 1`, the length, known handles, and that the sum does
  not exceed the outstanding window. A violation is an invariant fault.

## 7. Controller → host ACL flow control (`0C31` / `0C33` / `0C35`, `c2h_credit.c`)

### 7.1 Enabling it

`start_up()` always sends **`0C31` Set Controller To Host Flow Control** with `HCI_HOST_FLOW_CTRL_ACL_ON`
(`BD/device/controller.c:262-266`) and then **`0C33` Host Buffer Size** (:274-288,
`BD/hci/hci_packet_factory.c:68-79`) with:

| Field | Value | Source |
|---|---|---|
| Host_ACL_Data_Packet_Length | **1021** | `L2CAP_HOST_FC_ACL_DATA_LEN` (patch 0004, `PORT/bt_port_prefix.h:26`). Upstream would send `L2CAP_MTU_SIZE` = 1691. |
| Host_SCO_Data_Packet_Length | 255 | `SCO_HOST_BUFFER_SIZE` (`BD/device/controller.c:181`) |
| Host_Total_Num_ACL_Data_Packets | **4** | `L2CAP_HOST_FC_ACL_BUFS` (`PORT/bt_port_prefix.h:25`). Upstream default is 100 (`bt_target.h:1801-1803`). |
| Host_Total_Num_SCO_Data_Packets | 10 | literal |

bt_ctrl must enforce both sides: (a) it never delivers an ACL packet with payload > 1021; (b) it never has more than
4 ACL packets delivered to the host and not yet returned by `0C35`, counted across **all** handles. Plan M1 ties the
ACL RX ring (4 × 1026 B) to these values with a static assert. Both replies are generic CCs (§9).

### 7.2 Where the host returns credits

The host owes one credit per ACL packet delivered. Credits are returned:

- On drop paths inside HCI: malformed ACL in hal (`BD/hci/hci_hal_h4.c:496-500,516-520`); fragmenter error and
  duplicate-start paths (`BD/hci/packet_fragmenter.c:205-212,232-244,256-260,278-283,298-303,312-339`); BTU post
  failure (`BD/hci/hci_layer.c:698-707`). These are accounted on hciT via `hci_c2h_credit_account()`
  (`BD/stack/btu/c2h_credit.c:816-847`).
- When the ACL is consumed: continuation fragments return their credit as soon as they are copied into the partial
  packet (`BD/hci/packet_fragmenter.c:354-357`). **The START fragment's credit is held until L2CAP finishes the whole
  PDU** (:305-308,364-365). On BTU these go to `btu_c2h_credit_add/post/flush` (`BD/stack/btu/btu_task.c:149,180,275,
  323,330`, `BD/stack/l2cap/l2c_main.c:1184,1194`). A link teardown purges with `btu_c2h_credit_flush_and_purge()`
  (`BD/stack/l2cap/l2c_utils.c:254`).
- Batching: below `C2H_CREDIT_FLUSH_MIN` = 4 pending credits, the send is deferred 10 ms. If building the command
  fails, the retry is 100 ms (`BD/stack/btu/c2h_credit.c:28-30,510-569,1030-1048`). With a window of 4, a fully
  consumed window is returned immediately. A partial window is returned after ≤ 10 ms plus alarm latency. The handoff
  goes BTU tab → `mail` → hciT `flush_ready` (WQ0) → `btsnd_hcic_host_num_xmitted_pkts()` **on hciT** →
  `transmit_command()` → command queue (§6.3).
- One `0C35` carries up to `C2H_CREDIT_TAB_CAP` handles: min(`MAX_L2CAP_LINKS + BTM_MAX_SCO_LINKS`, 12)
  (`BD/stack/include/stack/c2h_credit.h:24-28`).
- At shutdown: `hci_c2h_credit_close()` flushes before teardown (`BD/hci/hci_layer.c:201-204`,
  `BD/stack/btu/c2h_credit.c:733-772`). Any `0C35` still queued when the queues are freed is discarded (§2). bt_ctrl
  resets its C2H window on controller reset/stop.

### 7.3 Liveness argument (with patch 0010)

ACL is admitted only with a host credit. Events are never gated (plan M1 RX dispatcher). So the host can always
receive the events it needs. Every delivered ACL eventually yields a credit through §7.2. That credit becomes a
NOACK `0C35`, which patch 0010 sends whenever the unified TX ring has a slot, whatever the command credits. The
number of queued NOACK commands is bounded by the window (≤ 4 outstanding credits, so ≤ 4 `0C35` without new
inbound ACL) plus purge one-shots. The NOACK queue (capacity 254) therefore never blocks its producer.

Residual: a held START credit belongs to a partially reassembled PDU, and the peer must send the continuations to
release it. With a window of 4, up to 4 concurrent partial PDUs on 4 handles can exhaust the window. With
`MAX_L2CAP_LINKS` links that is reachable only with ≥ 4 links mid-PDU at once. That is not a target scenario, but
the plan's "ACL-ring saturation → controlled link disconnect" (M1) covers it.

## 8. Host → controller ACL size and count (`1005`)

`parse_read_buffer_size_response()` reads `ACL_len:u16, SCO_len:u8, ACL_count:u16, SCO_count:u16` and asserts
(`BD/hci/hci_packet_parser.c:40-55`). `ACL_len` drives host fragmentation (§3.3) and the L2CAP segment size.
`ACL_count` drives L2CAP's controller window (`BD/stack/btm/btm_devctl.c:202`). Both are bt_ctrl's choice (plan M1:
not more than the ROM can take immediately). All 7 bytes after status must be present even without SCO.

## 9. Startup sequence and which replies assert

`esp_bluedroid_enable()` → BTA → `bta_sys_hw_evt_enabled()` → `BTM_DeviceReset()` → `controller->start_up()` runs
on **BTU** (`BD/bta/sys/bta_sys_main.c:435-439`, `BD/stack/btm/btm_devctl.c:240-251`). Each command is
`AWAIT_COMMAND` = `future_await(transmit_command_futured(...))` (`BD/device/controller.c:237`), so BTU blocks forever
on each reply (§6.2). Every reply first passes `read_command_complete_header()` (`BD/hci/hci_packet_parser.c:265-305`),
which **asserts** three things: event code == `0x0E` (:282), parameter length ≥ 4 + the parser's minimum (:283), and,
for the typed parsers, opcode == expected (:292-294). It returns `NULL` on nonzero status (:299-302).

| # | Opcode | Sent at | Parser | Nonzero status / short reply |
|---|---|---|---|---|
| 1 | `0C03` Reset | `controller.c:251-252` | generic (`parser.c:33-38`, min 0) | logged, ignored |
| 2 | `1005` Read Buffer Size | :256-259 | min 7, **`assert(stream != NULL)`** (`parser.c:48-49`) | **assert** |
| 3 | `0C31` Set C2H Flow Control | :264-265 | generic | logged, ignored (but the host still runs C2H accounting and sends `0C35`) |
| 4 | `0C33` Host Buffer Size | :279-288 | generic | logged, ignored |
| 5 | `1001` Read Local Version | :292-293 | min 8, **`assert(stream != NULL)`** (`parser.c:62-63`) | **assert** |
| 6 | `1009` Read BD_ADDR | :296-297 | min 6, **`assert(stream != NULL)`** (`parser.c:78-79`) | **assert** |
| 7 | `1002` Read Local Supported Commands | :300-305 | min 64, **`assert(stream != NULL)`** (`parser.c:91-92`) | **assert** |
| 8 | `1004` Read Local Extended Features, page 0 | :309-320 | min 10. On NULL it logs and keeps zeros (`parser.c:117-128`). `assert(page < 3)` (:122). `assert(page_number == 0)` (`controller.c:319`). | features stay zero |
| 9 | `0C56` Write Simple Pairing Mode | :327-333 | generic. Sent only if page 0 has the SSP bit and `cfg.ssp_en`. | logged |
| 10 | `1004` pages 1..max_page | :349-361 | as #8. The loop runs while `page ≤ max_page && page < 3`. The reply's `page_number` must equal the request. | features stay zero |
| 11 | `0C7A` Write SC Host Support | :364-370 | generic. Sent only if `cfg.sc_en` **and** page 2 has the SC bit. | logged |
| 12 | `0C01` Set Event Mask | :477-478 | generic | logged |

BLE (:335-344,372-475) and SCO (:480-486) blocks are compiled out. `controller->readable` becomes true at :487.
Then `reset_complete()` (`BD/stack/btm/btm_devctl.c:160-237`) queues the first BTA/BTM writes (`0C18`, `0C0A`, …), and
BTA's `0C14` Read Local Name gates `BTA_DM_ENABLE_EVT` (`BD/bta/dm/bta_dm_act.c:578-585`). Those are ordinary
callback commands. A CS or a REJECT is survivable there, but missing replies stall enable (`docs/BT_HCI_MATRIX.md`).

Contract: every row is answered with a **CC** that echoes the opcode, carries a credit ≥ 1 (§6.1), and has at least
the parser's minimum length. Rows 2, 5, 6 and 7 must have status 0 or the stack asserts. All others need status 0
for correct operation even though the parser tolerates failure. That matches the plan's "mandatory-success" class
(§11 D1).

## 10. Task and blocking map

| Context | Calls into the driver boundary | May block on |
|---|---|---|
| hciT (7) | `check_send_available`, `send` | nothing inside bt_ctrl (both must be non-blocking). Upstream, hciT blocks on BTU's queue (`btu_task_post` forever, ACL 100 ms) and posts to its own WQ0 with `portMAX_DELAY` in `transmit_command` (`BD/hci/hci_layer.c:377`, §11 D11). |
| bt_dlv (6) | `notify_host_recv`, `notify_host_send_available` | heap, `rx_q` slot (forever), mutexes, hciT WQ0 (§4, §5.3) |
| bt_ctrl (8) | none. Never calls the callbacks. | nothing in Bluedroid |
| BTC (4) | `register_host_callback` (once per init) | n/a |
| BTU (5) | none directly. It blocks in `future_await` during startup. | replies from bt_ctrl |

## 11. Discrepancies with plan v18 M1 (flagged, not silently resolved)

- **D1 – "parsers `assert(stream != NULL)` on nonzero status"** applies only to `1005`, `1001`, `1009`, `1002`
  (§9 rows 2, 5–7). `1004` has an error branch. Generic CCs (`0C03`, `0C31`, `0C33`, `0C56`, `0C7A`, `0C01`) only log.
  On the other hand, *every* startup reply asserts on event code, length and (typed parsers) opcode, and a CS
  deadlocks BTU. The plan's policy (FORWARD/SYNTH with status 0, never REJECT) is still right. Its stated reason is
  imprecise.
- **D2 – Mandatory list.** `0C35` is listed among the mandatory-success opcodes. It is never sent at startup and must
  produce **no** reply (§6.3). `0C14` is a BTA command, not a `controller.c` one (§9). `0C7A` is absent from the list.
  It is sent if `cfg.sc_en` is set and bt_ctrl's page-2 reply advertises SC. The plan's feature masking ("no SC")
  avoids it. bt_ctrl should report `max_page ≤ 1` or clear the SC bit, and the mandatory-opcode unit test should cover
  both `sc_en` settings.
- **D3 – NOACK ordering vs ACL fragments.** The plan says "drained ahead of the credit check (and ahead of
  credit-blocked ordinary commands)" but says nothing about a partially sent ACL packet (`current_fragment_packet`).
  Patch 0010 puts NOACK **first**, so a `0C35` may go out between two fragments of one ACL packet. HCI allows this,
  because packet types are framed independently. bt_ctrl's ACL handling must therefore not assume that fragments of
  one ACL packet arrive back to back.
- **D4 – Host-visible command window.** The plan gates commands on the ROM credit inside bt_ctrl, but does not say
  what `Num_HCI_Command_Packets` bt_ctrl reports to the host. §6.1 recommends a constant 1. This needs a decision.
- **D5 – RX allocation failure.** Confirmed as described: `assert(0)` on copy failure, enqueue result ignored, slot
  leaked on `list_append()` failure (§5.2). Patches 0008 and 0009 implement the plan's wording.
- **D6 – `rx_q == NULL` returns 0.** After `hal_close()` (or before `hal_open()`), `notify_host_recv` drops the packet
  and reports success (`BD/hci/hci_hal_h4.c:633-635`). Patch 0008 keeps that, because returning failure would make
  bt_dlv retry against a closed host. The plan's session-id gating, not the return value, must keep deliveries out of
  that window, and §2 shows it must anyway (use-after-free).
- **D7 – Silent ACL loss after the driver boundary is not covered by the three patches.** `btu_hci_acl_data_post()`
  can fail on `osi_malloc` or on its 100 ms enqueue timeout (`BD/stack/btu/btu_task.c:492-505`). The packet is then
  freed with its credit returned (`BD/hci/hci_layer.c:698-709`). The start fragment's reassembly buffer `osi_calloc`
  failure is `assert(0)` (`BD/hci/packet_fragmenter.c:273-283`). `transmit_downward()` ignores
  `fixed_queue_enqueue()`'s result for outbound ACL (`BD/hci/hci_layer.c:408`). With 0009 the slot comes back, but
  the ACL buffer still leaks. Plan M1's "general rule (steps 2–3)" and the allocation-failure sweep will hit all three.
  They need further documented patches or an explicit waiver.
- **D8 – C2H credit flush failures are retried, not lost**, except `tab_add()` on the BTU host tab under OOM
  (`BD/stack/btu/c2h_credit.c:1013-1016`, logged drop) and `tab_add` dynamic-node OOM (`assert(0)`, :244-248). With
  `C2H_CREDIT_TAB_CAP` ≥ the number of links, the dynamic path is unreachable. A lost credit permanently shrinks the
  4-packet window, which is again in scope of the sweep.
- **D9 – "`1004 (pages 0–1)`".** The pages requested are 0..`max_page` from bt_ctrl's own reply, capped at 2 (§9
  rows 8, 10). bt_ctrl decides how many pages there are.
- **D10 – Patch numbering.** README lists 0001–0007. This document proposes 0008 (`hci_hal_h4.c`), 0009
  (`fixed_queue.c`) and 0010 (`hci_layer.c`). The plan does not number them.
- **D11 – Upstream self-post hazard (not in scope of the three patches).** When `0C35` is built on hciT (C2H flush),
  `transmit_command()` calls `hci_downstream_data_post(OSI_THREAD_MAX_TIMEOUT)` on hciT's own WQ0
  (`BD/hci/hci_layer.c:377`). If `downstream_data_ready` is not already queued and WQ0 (length 3) is full of
  non-coalesced `flush_reassembly_work` items (`BD/hci/hci_layer.c:187-191`), hciT blocks on itself. That needs ≥ 3
  link purges queued while hciT runs a flush, which is unlikely with ≤ 2 links. It should be covered by a harness test
  or a timeout-0 post in a later patch.

## 12. Contract checklist for bt_ctrl / bt_dlv

1. Attach before `esp_bluedroid_init()`. `register_host_callback` returns `ESP_OK` and stores the pointer.
2. `check_send_available()`: non-blocking, side-effect-free, true ⇔ the unified TX ring has a free ≤ 1026 B slot.
   Called only by hciT.
3. `send()`: copies `len` bytes before returning, never fails (assert on a full ring = invariant violation), and
   accepts types `0x01`/`0x02` only. It never calls any host callback.
4. Free slot after a possible `false` → bt_dlv calls `notify_host_send_available()` after the slot is freed.
5. `notify_host_*` are called only from bt_dlv, never with a lock held or PRIMASK set, and never after the session is
   closed or after `esp_bluedroid_deinit()` starts.
6. One complete, exactly-sized H4 packet per `notify_host_recv()`, type `0x04` or `0x02`. No `0x3E` adv reports.
   ACL PB ∈ {`10`, `01`}, payload ≤ 1021.
7. With patch 0008: nonzero return = not accepted. Keep the entry and retry with backoff, and post a BT fault after N
   failures.
8. Every non-NOACK command gets exactly one CC or CS with the echoed opcode within 8 s. Futured/startup commands get
   CC only. Rows 2, 5–7 of §9 have status 0 and their minimum lengths.
9. `Num_HCI_Command_Packets` in every CC/CS is the host window (recommended: 1). Never leave the host at 0 without a
   later reopening CC.
10. `0C35`: no reply. Credits go back to the C2H window. Validate them. No ROM command credit is used. Kick the RX
    dispatcher.
11. C2H window: ≤ 4 ACL packets outstanding to the host across all handles. Events are never gated.
12. `1005`: `ACL_len` = 1021 (or what the ROM path accepts), `ACL_count` ≤ what the ROM can take immediately, all 7
    bytes present. One completed-packet count per accepted ACL packet.

## 13. Required upstream patches (not yet written / not applied)

All three follow the existing patch style: `#if defined(OPENFW_BLUEDROID) && OPENFW_BLUEDROID`, as in 0007
(`BD/hci/hci_layer.c:18-20,508-515`), so the pristine upstream behaviour is kept in the `#else` branches. The diffs
below were generated against copies of the current files. The patched copies pass `clang -fsyntax-only -Werror`
with the exact `bt/Makefile.bluedroid` ARM flags. To apply them later, each one becomes a commit plus a `.patch` file
and a README row (plan D6).

### 13.1 0008 – `hci_hal_h4.c`: fail-soft RX, failure returned to the driver

```diff
--- a/open_fw/third_party/bluedroid/host/bluedroid/hci/hci_hal_h4.c
+++ b/open_fw/third_party/bluedroid/host/bluedroid/hci/hci_hal_h4.c
@@ -654,16 +654,29 @@
         pkt = (BT_HDR *) osi_calloc(pkt_size);
         if (!pkt) {
             HCI_TRACE_ERROR("%s couldn't acquire memory for inbound data buffer.\n", __func__);
+#if defined(OPENFW_BLUEDROID) && OPENFW_BLUEDROID
+            return -1; /* open_fw patch 0008: fail soft; the driver keeps its copy and retries */
+#else
             assert(0);
+#endif
         }
 
         pkt->offset = 0;
         pkt->len = len;
         pkt->layer_specific = 0;
         memcpy(pkt->data, data, len);
+#if defined(OPENFW_BLUEDROID) && OPENFW_BLUEDROID
+        /* open_fw patch 0008: list_append() inside fixed_queue_enqueue() can fail after a slot was taken (patch 0009
+         * gives the slot back); free our copy and report failure instead of leaking it and dropping the packet. */
+        if (!fixed_queue_enqueue(hci_hal_env.rx_q, pkt, FIXED_QUEUE_MAX_TIMEOUT)) {
+            osi_free(pkt);
+            return -1;
+        }
+#else
         {
             fixed_queue_enqueue(hci_hal_env.rx_q, pkt, FIXED_QUEUE_MAX_TIMEOUT);
         }
+#endif
     } else {
         if (esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_ENABLED) {
             // Prevent race condition during host deinit/disable
```

Notes:

- The early returns skip `hci_upstream_data_post()`. That is correct, because nothing was queued, and any earlier
  packet already posted the event.
- `bt_record_hci_data()` (:637) has already run for the failed attempt. It is a no-op here (`BT_HCI_LOG_INCLUDED`
  FALSE, `open_fw/third_party/bluedroid/common/include/bt_common.h:98-100`), and if it were enabled a retry would log
  the packet twice.
- `osi_calloc` / `fixed_queue_enqueue` still block where they did before. The patch only removes the assert and the
  silent loss.

### 13.2 0009 – `fixed_queue.c`: give the slot back when `list_append()` fails

```diff
--- a/open_fw/third_party/bluedroid/common/osi/fixed_queue.c
+++ b/open_fw/third_party/bluedroid/common/osi/fixed_queue.c
@@ -151,6 +151,10 @@
 
     if(status == true )
         osi_sem_give(&queue->dequeue_sem);
+#if defined(OPENFW_BLUEDROID) && OPENFW_BLUEDROID
+    else
+        osi_sem_give(&queue->enqueue_sem); /* open_fw patch 0009: list node allocation failed, return the slot */
+#endif
 
     return status;
 }
```

Notes:

- Without this patch, each failed `list_append()` permanently loses one of the 254 slots of the affected queue. Every
  `fixed_queue_t` user in the stack benefits. Callers that ignore the result still lose their item (D7), but they no
  longer leak capacity.
- Giving back a slot that was just taken cannot exceed the semaphore's maximum (`OSI/fixed_queue.c:61`).
- `fixed_pkt_queue` is intrusive and does not allocate (`OSI/fixed_pkt_queue.c:95-113`), so it needs no change.

### 13.3 0010 – `hci_layer.c`: NOACK queue drained before the command-credit check

```diff
--- a/open_fw/third_party/bluedroid/host/bluedroid/hci/hci_layer.c
+++ b/open_fw/third_party/bluedroid/host/bluedroid/hci/hci_layer.c
@@ -73,6 +73,9 @@
 typedef struct {
     int command_credits;
     fixed_pkt_queue_t *command_queue;
+#if defined(OPENFW_BLUEDROID) && OPENFW_BLUEDROID
+    fixed_pkt_queue_t *noack_queue; /* open_fw patch 0010: NOACK commands (0C35), sent without a command credit */
+#endif
     fixed_queue_t *packet_queue;
     struct osi_event *downstream_data_ready;
     command_waiting_response_t cmd_waiting_q;
@@ -258,6 +261,15 @@
         HCI_TRACE_ERROR("%s unable to create pending command queue.", __func__);
         return -1;
     }
+#if defined(OPENFW_BLUEDROID) && OPENFW_BLUEDROID
+    hci_host_env.noack_queue = fixed_pkt_queue_new(QUEUE_SIZE_MAX);
+    if (hci_host_env.noack_queue) {
+        fixed_pkt_queue_register_dequeue(hci_host_env.noack_queue, event_command_ready);
+    } else {
+        HCI_TRACE_ERROR("%s unable to create NOACK command queue.", __func__);
+        return -1;
+    }
+#endif
 
     struct osi_event *event = osi_event_create(hci_downstream_data_handler, NULL);
     assert(event != NULL);
@@ -303,6 +315,12 @@
         fixed_pkt_queue_free(hci_host_env.command_queue, (fixed_pkt_queue_free_cb)osi_free_func);
         hci_host_env.command_queue = NULL;
     }
+#if defined(OPENFW_BLUEDROID) && OPENFW_BLUEDROID
+    if (hci_host_env.noack_queue) {
+        fixed_pkt_queue_free(hci_host_env.noack_queue, (fixed_pkt_queue_free_cb)osi_free_func);
+        hci_host_env.noack_queue = NULL;
+    }
+#endif
     if (hci_host_env.packet_queue) {
         fixed_queue_free(hci_host_env.packet_queue, osi_free_func);
         hci_host_env.packet_queue = NULL;
@@ -333,6 +351,15 @@
     UNUSED(arg);
 
     while (hci_host_check_send_available()) {
+#if defined(OPENFW_BLUEDROID) && OPENFW_BLUEDROID
+        /* open_fw patch 0010: Host Number Of Completed Packets may be sent regardless of Num_HCI_Command_Packets
+         * (Core Vol 4 Part E 7.3.40), so NOACK commands go first: ahead of the command-credit check, ahead of
+         * credit-blocked ordinary commands and between fragments of an ACL packet. One send per iteration. */
+        if (!fixed_pkt_queue_is_empty(hci_host_env.noack_queue)) {
+            fixed_pkt_queue_process(hci_host_env.noack_queue);
+            continue;
+        }
+#endif
         /*Now Target only allowed one packet per TX*/
         BT_HDR *pkt = packet_fragmenter->fragment_current_packet();
         if (pkt != NULL) {
@@ -373,7 +400,15 @@
     HCI_TRACE_DEBUG("HCI Enqueue Command opcode=0x%x\n", metadata->opcode);
     BTTRC_DUMP_BUFFER(NULL, command->data + command->offset, command->len);
 
+#if defined(OPENFW_BLUEDROID) && OPENFW_BLUEDROID
+    /* open_fw patch 0010: NOACK commands bypass the credit-gated FIFO (event_command_ready() already sends them
+     * without consuming a credit or waiting for a response). */
+    fixed_pkt_queue_enqueue((metadata->flags_src & HCI_CMD_MSG_F_SRC_NOACK) ? hci_host_env.noack_queue
+                                                                             : hci_host_env.command_queue,
+                            linked_pkt, FIXED_PKT_QUEUE_MAX_TIMEOUT);
+#else
     fixed_pkt_queue_enqueue(hci_host_env.command_queue, linked_pkt, FIXED_PKT_QUEUE_MAX_TIMEOUT);
+#endif
     hci_downstream_data_post(OSI_THREAD_MAX_TIMEOUT);
 
 }
```

Notes:

- The patch reuses `event_command_ready()` unchanged. Its NOACK branch (`BD/hci/hci_layer.c:426-431`) already sends
  and frees without touching credits, the pending list or the timer. The loop still performs at most one send per
  `true` (§3.2).
- `transmit_command_futured()` is left alone. Futured commands are never NOACK.
- No change is needed in `filter_incoming_event()`. `transmit_command()` already posts the downstream event, and a
  NOACK command never waits for a CC.
- Teardown matches the command queue: queued NOACK commands are freed, not sent, as `0C35` in the ordinary queue is
  upstream.
- Capacity is `QUEUE_SIZE_MAX`. The bound argument is in §7.3. Producers are the hciT flush (`c2h_credit.c:549`) and
  the BTU one-shots (:835,1080).

### 13.4 Tests these patches need (host harness, plan M1/M3)

- 0008/0009: fail the copy allocation, then fail the `list_append` node. Each time `notify_host_recv` returns ≠ 0, the
  heap returns to baseline, `fixed_queue_length` and free-slot count are unchanged, and the retried packet (a CC) is
  processed exactly once, with the command not timing out. Run it through the allocation-failure sweep.
- 0010 (plan M1 test): command credits 0, C2H window exhausted, an ordinary command queued ahead → `0C35` reaches
  `send()` and inbound ACL resumes. A variant has an ACL packet mid-fragmentation: `0C35` is sent between fragments
  and the fragments stay in order. A further variant checks that no CC/CS is required for `0C35` and that
  `command_credits` is unchanged by it.
