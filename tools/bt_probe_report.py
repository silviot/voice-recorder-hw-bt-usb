#!/usr/bin/env python3
"""
Report for the Bluetooth Exp 1 stage A HCI probe (docs/BLUETOOTH_BLUEDROID_PLAN.md M5, open_fw/bt_port/hci_probe.c).

Reads `tools/dev_cycle.py log` output (a file or stdin), extracts the "HCIP " lines of the LAST probe run in the log
and prints a markdown report and/or JSON (per-opcode status, reply bytes, latency; IRQ-26, NVIC, heap, timing and
restart measurements).

  sudo python3 tools/dev_cycle.py log > probe.log
  python3 tools/bt_probe_report.py probe.log                     # markdown to stdout
  python3 tools/bt_probe_report.py probe.log --json probe.json --md probe.md
  python3 tools/dev_cycle.py log | python3 tools/bt_probe_report.py --format json
  python3 tools/bt_probe_report.py --self-test

Line grammar (one result per line, "HCIP " may follow other text if the log ring interleaved):
  HCIP <KIND> [free words...] key=value key=value ...
  KIND = OFF BEGIN NOT_PROBED CMD LIVE TIMEOUT FAIL TEARDOWN HARDSTOP NVIC IDLE IRQBITS RESTART EVT HANG ABORT HEAP
         TIME CTRL STACK RELEASE SKIP END
  OFF = the image booted without the one-shot SD request file (HCIPROBE.RUN), so the probe did not run.
  IRQBITS values are b<N>=<top-half occurrences>/<bottom-half services>/<latched again while pending>.

SPDX-License-Identifier: MIT
"""

from __future__ import annotations

import argparse
import json
import sys
import zlib
from typing import Any

OPCODE_NAMES = {
    0x0C03: "Reset",
    0x1005: "Read Buffer Size",
    0x1001: "Read Local Version Information",
    0x1009: "Read BD_ADDR",
    0x1002: "Read Local Supported Commands",
    0x1004: "Read Local Extended Features",
    0x0C14: "Read Local Name",
}

KINDS = {"OFF", "BEGIN", "NOT_PROBED", "CMD", "LIVE", "TIMEOUT", "FAIL", "TEARDOWN", "HARDSTOP", "NVIC", "IDLE",
         "IRQBITS", "RESTART", "EVT", "HANG", "ABORT", "HEAP", "TIME", "CTRL", "STACK", "RELEASE", "SKIP", "END"}


HEX_TEXT_KEYS = {"rx", "op", "p", "st", "bd"}   # bare hex digits: keep as text (e.g. rx=040100, st=00)


def _value(key: str, v: str) -> Any:
    """Decimal -> int, 0x.. -> int, anything else (and the bare-hex keys) stays a string."""
    if key in HEX_TEXT_KEYS:
        return v
    if v.startswith("0x"):
        try:
            return int(v, 16)
        except ValueError:
            return v
    if v.isdigit():
        return int(v)
    return v


def parse_line(line: str) -> dict | None:
    pos = line.find("HCIP ")
    if pos < 0:
        return None
    toks = line[pos + 5:].strip().split()
    if not toks or toks[0] not in KINDS:
        return None
    rec: dict[str, Any] = {"kind": toks[0]}
    words = []
    for t in toks[1:]:
        if "=" in t:
            k, v = t.split("=", 1)
            rec[k] = _value(k, v)
        else:
            words.append(t)
    if words:
        rec["text"] = " ".join(words)
    return rec


def last_run(records: list[dict]) -> tuple[list[dict], list[str]]:
    """Records of the last probe run (from its BEGIN/SKIP), plus warnings about the log."""
    warnings = []
    starts = [i for i, r in enumerate(records) if r["kind"] in ("OFF", "BEGIN", "SKIP")]
    if not starts:
        warnings.append("no HCIP BEGIN in the log: the 4 KiB ring wrapped or the probe image did not run")
        run = records
    else:
        if len(starts) > 1:
            warnings.append(f"{len(starts)} probe runs in the log; reporting the last one")
        run = records[starts[-1]:]
    if run and run[0]["kind"] == "OFF":
        warnings.append(f"the probe did not run (trig={run[0].get('trig')}): create an empty {run[0].get('file')} in "
                        "the SD root and reboot")
        return run, warnings
    if not any(r["kind"] == "END" for r in run):
        warnings.append("no HCIP END: the probe was still running when the log was read, or the device reset")
    return run, warnings


def build_report(text: str) -> dict:
    records = [r for r in (parse_line(ln) for ln in text.splitlines()) if r is not None]
    run, warnings = last_run(records)
    rep: dict[str, Any] = {
        "status": None, "begin": None, "commands": [], "not_probed": [], "liveness": [], "failures": [],
        "teardowns": [], "hardstops": [], "nvic": None, "idle": None, "irq_bits": [], "restarts": [], "events": [],
        "hang": None, "heap": None, "time": None, "ctrl": None, "stack": None, "release": None, "skip": None,
        "off": None, "end": None,
        "warnings": warnings,
    }
    for r in run:
        k = r["kind"]
        body = {key: val for key, val in r.items() if key != "kind"}
        if k == "OFF":
            rep["off"] = body
            rep["status"] = "OFF"
        elif k == "BEGIN":
            rep["begin"] = body
            if body.get("trig") == "autorun" or body.get("waive_live"):
                warnings.append("QEMU-only image (HCI_PROBE_AUTORUN / HCI_PROBE_WAIVE_LIVENESS): never flash it")
        elif k == "NOT_PROBED":
            ops = str(body.get("ops", "")).split(",") if "ops" in body else []
            rep["not_probed"].append({
                "reason": body.get("text", ""), "exp": body.get("exp"),
                "opcodes": [o for o in ops if o], "xcheck": body.get("xcheck"),
            })
        elif k == "CMD":
            op = body.get("op")
            opcode = int(str(op), 16) if op is not None else None
            body["opcode"] = f"0x{opcode:04X}" if opcode is not None else None
            body["name"] = OPCODE_NAMES.get(opcode, "")
            if "rx" in body:
                body["rx"] = str(body["rx"])
                body["truncated"] = "trunc" in body
            rep["commands"].append(body)
        elif k in ("LIVE", "TIMEOUT"):
            body["ok"] = k == "LIVE"
            rep["liveness"].append(body)
        elif k == "FAIL":
            rep["failures"].append(body)
        elif k == "TEARDOWN":
            rep["teardowns"].append(body)
        elif k == "HARDSTOP":
            rep["hardstops"].append(body)
        elif k == "NVIC":
            rep["nvic"] = body
        elif k == "IDLE":
            us = body.get("us") or 0
            if isinstance(us, int) and us > 0:
                body["irq_rate_hz"] = round(body.get("irq", 0) * 1e6 / us, 1)
                body["cpu_pct"] = round(100.0 * body.get("cpu_us", 0) / us, 2)
            rep["idle"] = body
        elif k == "IRQBITS":
            for key, val in body.items():
                if key.startswith("b") and key[1:].isdigit() and isinstance(val, str) and val.count("/") == 2:
                    occ, svc, coal = (int(x) for x in val.split("/"))
                    rep["irq_bits"].append({"bit": int(key[1:]), "occurrences": occ, "serviced": svc,
                                            "coalesced": coal})
            rep["irq_unhandled"] = body.get("unh")
        elif k == "RESTART":
            rep["restarts"].append(body)
        elif k == "EVT":
            rep["events"].append(body)
        elif k == "HANG":
            rep["hang"] = body
        elif k == "HEAP":
            rep["heap"] = body
        elif k == "TIME":
            rep["time"] = body
        elif k == "CTRL":
            rep["ctrl"] = body
        elif k == "STACK":
            rep["stack"] = body
        elif k == "RELEASE":
            rep["release"] = body
            if body.get("legacy") != "allowed":
                warnings.append("RW-BT left claimed (hang, hard stop or failed start/stop): legacy BT mode is refused "
                                "until reboot")
            if body.get("ipr") != body.get("orig"):
                warnings.append(f"IRQ 26 priority not restored (0x{body.get('ipr', 0):02X}, was "
                                f"0x{body.get('orig', 0):02X})")
        elif k == "SKIP":
            rep["skip"] = body
        elif k == "END":
            rep["end"] = body
            rep["status"] = body.get("st")
    if rep["status"] is None:
        rep["status"] = "INCOMPLETE"
    if rep["commands"]:
        ok = sum(1 for c in rep["commands"] if c.get("res") == "OK")
        rep["summary"] = {"commands": len(rep["commands"]), "ok": ok,
                          "not_probed": sum(len(n["opcodes"]) for n in rep["not_probed"])}
    return rep


def _md_kv(title: str, d: dict | None) -> list[str]:
    if not d:
        return []
    return [f"**{title}:** " + ", ".join(f"`{k}={v}`" for k, v in d.items()), ""]


def to_markdown(rep: dict) -> str:
    out = ["# Exp 1 stage A: HCI probe report", ""]
    b = rep.get("begin") or {}
    out.append(f"Status: **{rep['status']}**"
               + (f", failures {rep['end'].get('fails')}, {rep['end'].get('ms')} ms" if rep.get("end") and
                  "fails" in rep["end"] else ""))
    if b:
        out.append(f"BD_ADDR `{b.get('bd')}` ({b.get('src')}), table errors {b.get('tblerr')}, deadlines "
                   f"{b.get('dl')} us (Reset {b.get('dl_reset')} us), hang limit {b.get('hang')} us")
    if rep.get("off"):
        out.append(f"Not run: request file `{rep['off'].get('file')}` {rep['off'].get('trig')}")
    if rep.get("skip"):
        out.append(f"Skipped: previous boot left fault record `0x{rep['skip'].get('prev_fault', 0):08X}`")
    out.append("")
    for w in rep["warnings"]:
        out.append(f"> WARNING: {w}")
    if rep["warnings"]:
        out.append("")

    if rep["commands"]:
        out += ["## Commands", "",
                "| n | opcode | command | param | want | got | plen | status | latency (us) | result | reply (H4) |",
                "|---|---|---|---|---|---|---|---|---|---|---|"]
        for c in rep["commands"]:
            rx = c.get("rx", "")
            if c.get("truncated"):
                rx = f"{rx}... ({c.get('trunc')} B shown, CRC-32 0x{c.get('crc', 0):08X})"
            note = f" bd={c['bd']}" if "bd" in c else ""
            out.append(f"| {c.get('n')} | {c.get('opcode')} | {c.get('name')} | {c.get('p', '')} | {c.get('want')} | "
                       f"{c.get('got')} | {c.get('plen', '')} | {c.get('st', '')} | {c.get('lat')} | "
                       f"{c.get('res')}{note} | `{rx}` |")
        out.append("")

    if rep["liveness"] or rep["failures"] or rep["teardowns"] or rep["hardstops"] or rep.get("hang"):
        out += ["## Liveness, failures and recovery", ""]
        for lv in rep["liveness"]:
            out.append(f"- IRQ 26 liveness: {'OK' if lv['ok'] else 'TIMEOUT'} ({lv.get('irq')} IRQs, waited "
                       f"{lv.get('wait')} us, deadline {lv.get('deadline')} us)")
        for f in rep["failures"]:
            out.append(f"- failure {f.get('n')}: {f.get('why')}")
        for t in rep["teardowns"]:
            out.append(f"- teardown ({t.get('why')}): {t.get('st')} in {t.get('us')} us")
        for h in rep["hardstops"]:
            out.append(f"- HARD STOP: {h.get('why')}")
        if rep.get("hang"):
            out.append(f"- HANG: bt_ctrl stuck > {rep['hang'].get('limit')} us; {rep['hang'].get('action')}")
        out.append("")

    if rep.get("nvic"):
        n = rep["nvic"]
        out += ["## NVIC", "",
                f"IRQ {n.get('irq')}: implemented priority mask 0x{n.get('impl', 0):02X} ({n.get('bits')} bits), "
                f"written 0x{n.get('set', 0):02X}, read back 0x{n.get('rb', 0):02X} "
                f"(configMAX_SYSCALL_INTERRUPT_PRIORITY 0x{n.get('maxsys', 0):02X}), SHPR3 0x{n.get('shpr3', 0):08X}",
                ""]

    if rep.get("idle"):
        i = rep["idle"]
        out += ["## Idle window (controller up, no traffic)", "",
                "| window (us) | IRQs | rate (Hz) | spurious | bt_ctrl wakes | bt_ctrl runs | bt_ctrl CPU (%) | "
                "IRQ->BH max (us) | IRQ->BH avg (us) | irq_service max (us) | ROM dispatch max (us) | run() max (us) | "
                "bt_ctrl_run max (us) | BH loops max |",
                "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|",
                f"| {i.get('us')} | {i.get('irq')} | {i.get('irq_rate_hz')} | {i.get('spur')} | {i.get('wakes')} | "
                f"{i.get('runs')} | {i.get('cpu_pct')} | {i.get('lat_max')} | {i.get('lat_avg')} | {i.get('svc_max')} | "
                f"{i.get('disp_max')} (ev{i.get('disp_ev')}) | {i.get('run_max')} | {i.get('ctrl_max')} | "
                f"{i.get('bh_max')} |", ""]
    if rep["irq_bits"]:
        out += ["| INTSTAT bit | top-half occurrences | bottom-half services | latched while pending |",
                "|---|---|---|---|"]
        for bit in rep["irq_bits"]:
            out.append(f"| {bit['bit']} | {bit['occurrences']} | {bit['serviced']} | {bit['coalesced']} |")
        if rep.get("irq_unhandled") is not None:
            out.append(f"\nBits acked but not serviced (cumulative): 0x{rep['irq_unhandled']:08X}")
        out.append("")

    if rep["restarts"]:
        out += ["## Clean restart (stop + start + Reset + Read BD_ADDR)", "",
                "| # | stop | stop (us) | reset | Reset latency (us) | IRQs after start | BD_ADDR | 1009 latency (us) | "
                "retries |", "|---|---|---|---|---|---|---|---|---|"]
        for r in rep["restarts"]:
            out.append(f"| {r.get('i')} | {r.get('stop')} | {r.get('stop_us')} | {r.get('reset')} | {r.get('rlat')} | "
                       f"{r.get('irq')} | {r.get('bd')} | {r.get('blat')} | {r.get('retries')} |")
        out.append("")

    out += ["## Measurements", ""]
    out += _md_kv("g_bt_heap (bytes)", rep.get("heap"))
    out += _md_kv("Timing (us)", rep.get("time"))
    out += _md_kv("bt_ctrl / ROM counters", rep.get("ctrl"))
    out += _md_kv("Free stack (bytes)", rep.get("stack"))
    out += _md_kv("Left for the legacy path", rep.get("release"))

    if rep["events"]:
        out += ["## Unsolicited events", ""]
        for e in rep["events"]:
            out.append(f"- len {e.get('len')}: `{e.get('rx', '')}`")
        out.append("")

    if rep["not_probed"]:
        out += ["## Not probed in stage A (never sent)", "", "| reason | Exp 1 class | opcodes |", "|---|---|---|"]
        for n in rep["not_probed"]:
            ops = ", ".join(n["opcodes"]) if n["opcodes"] else (n.get("xcheck") or "")
            out.append(f"| {n['reason']} | {n.get('exp') or ''} | {ops} |")
        out.append("")
    return "\n".join(out).rstrip() + "\n"


# --------------------------------------------------------------------------------------------------------------------
SYNTH_LOG = """\
=== Cassini OpenFW (SL6801 Dual-Core CPU1 + Quad XIP + SRAM FastPath) ===
UI_FIRST_US=31000 LOAD=1 DRAW=2 BLIT=3 HEAP=0 WFE_W=7
HCIP BEGIN v=1 bd=02CA5500011D src=efuse trig=taken tblerr=0 dl=500000 dl_reset=2000000 hang=3000000
HCIP NOT_PROBED needs disassembly exp=G-B ops=0C18,0C1C,0C1E,0C3A,0C52
HCIP NOT_PROBED needs disassembly exp=G-C ops=0C45,0401,0402
HCIP NOT_PROBED needs stage B exp=V-B ops=0C56,0C01,0C24,0C13,0C1A
HCIP NOT_PROBED needs stage C exp=V-C ops=0409,040A
HCIP NOT_PROBED needs disassembly xcheck=1009:E05D->E0C0,1001:LMP_version_res
HCIP CMD n=1 op=0C03 want=CC got=CC plen=4 st=00 lat=301234 res=OK rx=040E0401030C00
HCIP LIVE what=irq26 irq=3 wait=2100 deadline=500000
HCIP NVIC irq=26 impl=0xE0 bits=3 set=0x80 rb=0x80 maxsys=0x80 shpr3=0xE0E00000
HCIP CMD n=2 op=1005 want=CC got=CC plen=11 st=00 lat=812 res=OK rx=040E0B01051000FD03000400000000
BOOT_US LCDB=1 SD=2
HCIP CMD n=3 op=1009 want=CC got=CC plen=10 st=00 lat=640 res=OK bd=match rx=040E0A010910001D010055CA02
HCIP CMD n=4 op=0C14 want=CC got=CC plen=252 st=00 lat=905 res=OK rx=040EFC01140C00000000000000000000 trunc=16 crc=0x1234ABCD
HCIP CMD n=5 op=1004 p=01 want=CC got=NONE lat=500000 res=TIMEOUT
HCIP FAIL n=1 why=cmd
HCIP TEARDOWN why=cmd st=OK us=900
HCIP EVT len=7 rx=040F0400010504
HCIP IDLE us=5000123 irq=8000 spur=0 wakes=7990 runs=7990 cpu_us=250000 lat_max=85 lat_avg=12 svc_max=30 \
disp_max=40 disp_ev=1 run_max=60 ctrl_max=95 bh_max=1
HCIP IRQBITS b0=7990/7980/10 b10=12/12/0 unh=0x00000000
HCIP RESTART i=1 stop=OK stop_us=210 reset=OK rlat=300100 irq=4 bd=match blat=700 retries=0
HCIP RESTART i=2 stop=OK stop_us=205 reset=OK rlat=300200 irq=4 bd=match blat=690 retries=0
HCIP TEARDOWN why=end st=OK us=800
HCIP HEAP bytes=8192 hw=512 used=0 largest=8184 fail=0 refused=0 badfree=0 rtos_free=100000
HCIP TIME rfcal=290000 start=9000 stop=200 ctrl_max_all=301000 starts=7 stops=7 stop_to=0
HCIP CTRL rom_msgs=0 rom_unk=0 rom_short=0 late=0 resets=7 rfail=0 hung=0 faults=0 first=0 inv=0 lockv=0 eif=0
HCIP STACK ctrl_free=900 dlv_free=1300 probe_free=1100
HCIP RELEASE ipr=0x00 orig=0x00 claimed=0 owner=0 lockout=0 legacy=allowed
HCIP END st=DONE fails=1 ms=14980
"""

SYNTH_QEMU = """\
HCIP BEGIN v=1 bd=02CA55000001 src=fixed trig=autorun tblerr=0 dl=500000 dl_reset=2000000 hang=3000000
HCIP CMD n=1 op=0C03 want=CC got=CC plen=4 st=00 lat=495197 res=OK rx=040E0401030C00
HCIP TIMEOUT what=irq26 irq=0 wait=924124 deadline=500000
HCIP FAIL n=1 why=irq26
HCIP TEARDOWN why=irq26 st=OK us=516
HCIP CMD n=2 op=0C03 want=CC got=CC plen=4 st=00 lat=327107 res=OK rx=040E0401030C00
HCIP TIMEOUT what=irq26 irq=0 wait=905674 deadline=500000
HCIP FAIL n=2 why=irq26
HCIP ABORT fails=2
HCIP RELEASE ipr=0x00 orig=0x00 claimed=0 owner=0 lockout=0 legacy=allowed
HCIP END st=ABORT fails=2 ms=2715
"""


def run_self_test() -> int:
    log = SYNTH_LOG.replace("\\\n", "")
    # A line the firmware split because another task was mid-line must still parse.
    log = log.replace("BOOT_US LCDB=1 SD=2\n", "BOOT_US LCDB=1 SD=2 HCIP EVT len=3 rx=040100\n")
    rep = build_report(log)
    assert rep["status"] == "DONE", rep["status"]
    assert rep["begin"]["bd"] == "02CA5500011D" and rep["begin"]["dl_reset"] == 2000000
    cmds = {c["opcode"] + c.get("p", ""): c for c in rep["commands"]}
    assert cmds["0x0C03"]["res"] == "OK" and cmds["0x0C03"]["lat"] == 301234
    assert cmds["0x1005"]["rx"] == "040E0B01051000FD03000400000000" and cmds["0x1005"]["name"] == "Read Buffer Size"
    assert cmds["0x1009"]["bd"] == "match"
    assert cmds["0x0C14"]["truncated"] and cmds["0x0C14"]["crc"] == 0x1234ABCD and cmds["0x0C14"]["plen"] == 252
    assert cmds["0x100401"]["res"] == "TIMEOUT" and cmds["0x100401"]["got"] == "NONE"
    assert rep["summary"] == {"commands": 5, "ok": 4, "not_probed": 15}, rep["summary"]
    assert rep["not_probed"][0]["reason"] == "needs disassembly" and rep["not_probed"][0]["exp"] == "G-B"
    assert rep["not_probed"][-1]["xcheck"] == "1009:E05D->E0C0,1001:LMP_version_res"
    assert rep["liveness"] == [{"what": "irq26", "irq": 3, "wait": 2100, "deadline": 500000, "ok": True}]
    assert rep["nvic"]["impl"] == 0xE0 and rep["nvic"]["bits"] == 3 and rep["nvic"]["rb"] == 0x80
    assert rep["idle"]["irq_rate_hz"] == 1600.0 and rep["idle"]["cpu_pct"] == 5.0, rep["idle"]
    assert rep["irq_bits"] == [{"bit": 0, "occurrences": 7990, "serviced": 7980, "coalesced": 10},
                               {"bit": 10, "occurrences": 12, "serviced": 12, "coalesced": 0}]
    assert len(rep["restarts"]) == 2 and rep["restarts"][1]["rlat"] == 300200
    assert len(rep["events"]) == 2 and rep["heap"]["hw"] == 512 and rep["stack"]["ctrl_free"] == 900
    assert rep["failures"] == [{"n": 1, "why": "cmd"}] and rep["warnings"] == []
    assert rep["release"]["legacy"] == "allowed" and rep["begin"]["trig"] == "taken"
    md = to_markdown(rep)
    for needle in ("| 1 | 0x0C03 | Reset |", "CRC-32 0x1234ABCD", "| 0 | 7990 | 7980 | 10 |", "## Clean restart",
                   "| needs disassembly | G-B | 0C18, 0C1C, 0C1E, 0C3A, 0C52 |", "Status: **DONE**",
                   "**Left for the legacy path:**"):
        assert needle in md, needle
    json.loads(json.dumps(rep))

    # QEMU (no RW-BT model): clean abort after the IRQ-26 liveness timeout.
    q = build_report("boot\n" + SYNTH_QEMU)
    assert q["status"] == "ABORT" and [lv["ok"] for lv in q["liveness"]] == [False, False]
    assert len(q["failures"]) == 2 and q["warnings"] == [
        "QEMU-only image (HCI_PROBE_AUTORUN / HCI_PROBE_WAIVE_LIVENESS): never flash it"], q["warnings"]

    # No request file: the probe did not run.
    off = build_report("boot\nHCIP OFF trig=absent file=HCIPROBE.RUN\n")
    assert off["status"] == "OFF" and off["off"]["trig"] == "absent" and len(off["warnings"]) == 1
    assert "HCIPROBE.RUN" in off["warnings"][0] and "Not run" in to_markdown(off)

    # A hang leaves RW-BT claimed and legacy BT refused; a priority not put back is flagged.
    hung = SYNTH_QEMU.replace("claimed=0 owner=0 lockout=0 legacy=allowed", "claimed=1 owner=1 lockout=1 legacy=refused")
    hung = hung.replace("ipr=0x00 orig", "ipr=0x80 orig")
    h = build_report(hung)
    assert any("legacy BT mode is refused" in x for x in h["warnings"])
    assert any("IRQ 26 priority not restored (0x80, was 0x00)" in x for x in h["warnings"]), h["warnings"]

    # Wrapped ring (no BEGIN), unfinished run, and two runs in one log.
    w = build_report("\n".join(SYNTH_QEMU.splitlines()[3:8]))
    assert w["status"] == "INCOMPLETE" and len(w["warnings"]) == 2, w["warnings"]
    two = build_report(SYNTH_QEMU + SYNTH_LOG.replace("\\\n", ""))
    assert two["status"] == "DONE" and any("2 probe runs" in x for x in two["warnings"])

    # The CRC the firmware prints is plain CRC-32 (zlib): keep both sides in agreement.
    assert zlib.crc32(b"123456789") == 0xCBF43926
    print("bt_probe_report.py self-test PASSED (parse, last-run selection, wrap warnings, markdown, JSON)")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="Report for the Exp 1 stage A HCI probe log (HCIP lines)")
    ap.add_argument("log", nargs="?", help="dev_cycle.py log output (default: stdin)")
    ap.add_argument("--format", choices=("md", "json"), default="md", help="what to print to stdout")
    ap.add_argument("--json", dest="json_out", metavar="FILE", help="also write the JSON report to FILE")
    ap.add_argument("--md", dest="md_out", metavar="FILE", help="also write the markdown report to FILE")
    ap.add_argument("--self-test", action="store_true", help="run the offline self-test with a synthetic log")
    args = ap.parse_args()
    if args.self_test:
        return run_self_test()
    if args.log:
        with open(args.log, encoding="utf-8", errors="replace") as f:
            text = f.read()
    else:
        text = sys.stdin.read()
    rep = build_report(text)
    md = to_markdown(rep)
    js = json.dumps(rep, indent=2)
    if args.json_out:
        with open(args.json_out, "w", encoding="utf-8") as f:
            f.write(js + "\n")
    if args.md_out:
        with open(args.md_out, "w", encoding="utf-8") as f:
            f.write(md)
    sys.stdout.write(md if args.format == "md" else js + "\n")
    return 0 if rep["status"] in ("DONE", "ABORT", "SKIP") else 1   # OFF / INCOMPLETE: no probe data


if __name__ == "__main__":
    raise SystemExit(main())
