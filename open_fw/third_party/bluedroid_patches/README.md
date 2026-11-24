# Local patches on top of vendored Bluedroid

`third_party/bluedroid/` is upstream ESP-IDF `ad20ded62a2d67a26e501fdc1905f43ba7ad6c81`
(`components/bt/host/bluedroid`, `components/bt/common`, `components/esp_ringbuf`, one header from
`components/bt/ble_profiles/common`). The first commit that added it is byte-identical to upstream; every
local change is a separate commit **and** a `.patch` file here, so an upgrade is: re-vendor, re-apply, rebuild.

| Patch | File | Why |
|---|---|---|
| 0001 | `host/bluedroid/common/include/common/bt_target.h` | Upstream hard-codes `BTA/BTC_AV_SINK_INCLUDED=TRUE` whenever A2DP is on. `OPENFW_A2DP_SOURCE_ONLY` (set in `bt_port/bt_port_prefix.h`) drops the sink and SBC decoder: -9.6 KB RAM (`a2dp_sink_local_param`), ~-10 KB flash. |
| 0002 | `host/bluedroid/btc/profile/std/a2dp/btc_a2dp_source.c` | `BTC_MEDIA_AA_BUF_SIZE` (4112) is allocated per encoded frame (up to 5 queued) although frames are <= the 1008 B A2DP MTU. Make it overridable (we use 1280): -14 KB peak heap. |
| 0003 | `host/bluedroid/bta/av/bta_av_api.c` | `BTA_GetDelayValue()` is built unconditionally but its event exists only with the sink: upstream does not compile source-only. |
| 0004 | `host/bluedroid/device/controller.c` | Host Buffer Size (`0C33`) announces `L2CAP_MTU_SIZE` (1691 B) as the host ACL length. Make it overridable (`L2CAP_HOST_FC_ACL_DATA_LEN`, we use 1021 = controller ACL length) so the controller->host window (4 x 1021 B, plan M1) fits bt_ctrl's RX ring; L2CAP reassembly MTU unchanged. |
| 0005 | `common/osi/alarm.c`, `osi/include/osi/alarm.h` | The esp_timer shim (`bt_port/bt_esp_timer.c` on `bt_alarm`) delivers expiries in BTC context after generation validation, so `alarm_cb_handler` calls the callback directly instead of `btc_transfer_context` (which allocates and can block). Adds `osi_alarm_generation()` / `osi_alarm_delivery_gen()` for re-validation under a caller's lock (plan M2 "Alarms"). |
| 0006 | `common/btc/core/btc_task.c` | Drain pending alarms after every BTC message, and `btc_post_alarm_drain()`: allocation-free, timeout-0 post of a drain work item. A failed post implies a full queue, so the drain-after-every-message guarantees delivery. |
| 0007 | `host/bluedroid/hci/hci_layer.c` | `command_timed_out` ignores an expiry whose generation no longer matches the command-response timer (checked under `commands_pending_response_lock`, where the timer is armed/cancelled), and reports surviving timeouts to `bt_port_on_hci_command_timeout()` (BT fault path). |
