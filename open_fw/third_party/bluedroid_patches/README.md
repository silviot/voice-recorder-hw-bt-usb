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

No functional Bluedroid logic is changed. Non-patch tuning lives in `bt_port/` (`sdkconfig.defaults`,
`bt_port_prefix.h`: `BTA_JV_SDP_DB_SIZE`/`BTA_JV_SDP_RAW_DATA_SIZE` are upstream `#ifndef` overrides).
