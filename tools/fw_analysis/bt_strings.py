#!/usr/bin/env python3
import re
import pathlib

fw = pathlib.Path("dumps/firmware_2M.bin").read_bytes()

print("=== ALL BT / HFP / A2DP / AVRCP / SPP / BLE / SDP / AT STRINGS IN FIRM ===")
rx = re.compile(r"(^AT[\+\=A-Z]|^\+[A-Z]{3,}|bt_|bluetooth|hfp|hf_|a2dp|avrcp|avdtp|avctp|rfcomm|sdp|l2cap|spp|ble|gatt|obex|pbap|hid|sbc|msbc|cvsd|sco|call_|phone|HW_|btrlk|btlink|pair|inq|pin_code|ssp)", re.I)
for m in re.finditer(rb"[ -~]{4,}", fw[0x00E000:0x173E8C]):
    s = m.group().decode("ascii")
    off = 0xE000 + m.start()
    if rx.search(s):
        # skip non-English long translations in 0x51000..0x54E00
        if 0x51000 <= off <= 0x54D00:
            continue
        print(f"  0x{off:06X}: {s!r}")
