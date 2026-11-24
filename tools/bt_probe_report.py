#!/usr/bin/env python3
"""
Report for the Bluetooth Exp 1 HCI probe, stages A and B (docs/BLUETOOTH_BLUEDROID_PLAN.md M5,
open_fw/bt_port/hci_probe.c).

Reads the probe's log (SD:/HCIPROBE.LOG, which holds every HCIP line of the run, or `tools/dev_cycle.py log` output,
whose 4 KiB ring cannot hold stage A + B), a file or stdin, extracts the "HCIP " lines of the LAST probe run and prints
a markdown report and/or JSON (per-opcode status, reply bytes, latency; stage B read-backs; IRQ-26 per-bit counts and
bt_ctrl CPU in the idle / discoverable / scan-off windows; NVIC, heap, timing and restart measurements).

  python3 tools/bt_probe_report.py /media/$USER/<label>/HCIPROBE.LOG   # the whole run
  sudo python3 tools/dev_cycle.py log > probe.log
  python3 tools/bt_probe_report.py probe.log                     # markdown to stdout
  python3 tools/bt_probe_report.py probe.log --json probe.json --md probe.md
  python3 tools/dev_cycle.py log | python3 tools/bt_probe_report.py --format json
  python3 tools/bt_probe_report.py --self-test

Line grammar (one result per line, "HCIP " may follow other text if the log ring interleaved):
  HCIP <KIND> [free words...] key=value key=value ...
  KIND = OFF BEGIN NOT_PROBED CMD LIVE TIMEOUT FAIL TEARDOWN HARDSTOP NVIC IDLE IRQBITS RESTART EVT HANG ABORT HEAP
         TIME CTRL STACK RELEASE SKIP END   (stage A)
         STAGE VERIFY WIN WINBITS            (stage B)   LOGFILE (after END: the SD copy was written)
  OFF = the image booted without the one-shot SD request file (HCIPROBE.RUN), so the probe did not run
        (trig=bad_content: the file named no stage; it must be empty, "A" or "B"; trig=qemu_only_on_silicon: a
        QEMU-only image (HCI_PROBE_AUTORUN / HCI_PROBE_WAIVE_LIVENESS) refused to run on a part with eFuses).
  IRQBITS / WINBITS values are b<N>=<top-half occurrences>/<bottom-half services>/<latched again while pending>.
  Stage B CMD lines add fwd= (forwarded to the ROM), romd= (ROM messages meanwhile), hw= (g_bt_heap high-water),
  ctrl_free= (bt_ctrl stack bytes never used so far). STAGE s=B (start) repeats trig= so a wrapped ring still shows
  which request ran; WIN adds cpu_pk= (highest bt_ctrl CPU share in one 100 ms sample, permille) and ctrl_free=, and
  end=fault|hang|cpu when it stopped early. HANG why=run (one bt_ctrl_run() stuck) | cpu (CPU-share guard); HANG and
  HARDSTOP carry cntl= (RWBT_CNTL after the register-only radio stop: bit 8 RWBTEN must be 0).

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
    0x0C56: "Write Simple Pairing Mode",
    0x0C01: "Set Event Mask",
    0x0C24: "Write Class of Device",
    0x0C13: "Write Local Name",
    0x0C1A: "Write Scan Enable",
}

KINDS = {"OFF", "BEGIN", "NOT_PROBED", "CMD", "LIVE", "TIMEOUT", "FAIL", "TEARDOWN", "HARDSTOP", "NVIC", "IDLE",
         "IRQBITS", "RESTART", "EVT", "HANG", "ABORT", "HEAP", "TIME", "CTRL", "STACK", "RELEASE", "SKIP", "END",
         "STAGE", "VERIFY", "WIN", "WINBITS", "LOGFILE"}


# bare hex digits: keep as text (e.g. rx=040100, st=00, p=FFFFFFFFFFFFBF1D)
HEX_TEXT_KEYS = {"rx", "op", "p", "st", "bd", "got", "want", "was"}


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
        warnings.append("no HCIP BEGIN in the log: the 4 KiB ring wrapped (a stage B run always wraps it: report "
                        "SD:/HCIPROBE.LOG instead) or the probe image did not run")
        run = records
    else:
        if len(starts) > 1:
            warnings.append(f"{len(starts)} probe runs in the log; reporting the last one")
        run = records[starts[-1]:]
    if run and run[0]["kind"] == "OFF":
        if run[0].get("trig") == "qemu_only_on_silicon":
            warnings.append("a QEMU-only probe image (HCI_PROBE_AUTORUN / HCI_PROBE_WAIVE_LIVENESS) was flashed and "
                            "refused to run: build the silicon image (BT=1 HCI_PROBE=1, update-up) and flash that")
        elif run[0].get("trig") == "bad_content":
            warnings.append(f"the probe did not run: {run[0].get('file')} named no stage (it must be empty, \"A\" or "
                            "\"B\"); it was consumed, create it again")
        else:
            warnings.append(f"the probe did not run (trig={run[0].get('trig')}): create {run[0].get('file')} (empty "
                            "or \"A\" = stage A, \"B\" = stages A + B) in the SD root and reboot")
        return run, warnings
    if not any(r["kind"] == "END" for r in run):
        warnings.append("no HCIP END: the probe was still running when the log was read, or the device reset")
    return run, warnings


def _bits(body: dict) -> list[dict]:
    out = []
    for key, val in body.items():
        if key.startswith("b") and key[1:].isdigit() and isinstance(val, str) and val.count("/") == 2:
            occ, svc, coal = (int(x) for x in val.split("/"))
            out.append({"bit": int(key[1:]), "occurrences": occ, "serviced": svc, "coalesced": coal})
    return out


def _rates(body: dict) -> None:
    us = body.get("us") or 0
    if isinstance(us, int) and us > 0:
        body["irq_rate_hz"] = round(body.get("irq", 0) * 1e6 / us, 1)
        body["cpu_pct"] = round(100.0 * body.get("cpu_us", 0) / us, 2)


def build_report(text: str) -> dict:
    records = [r for r in (parse_line(ln) for ln in text.splitlines()) if r is not None]
    run, warnings = last_run(records)
    rep: dict[str, Any] = {
        "status": None, "begin": None, "commands": [], "not_probed": [], "liveness": [], "failures": [],
        "teardowns": [], "hardstops": [], "nvic": None, "idle": None, "irq_bits": [], "restarts": [], "events": [],
        "hang": None, "heap": None, "time": None, "ctrl": None, "stack": None, "release": None, "skip": None,
        "off": None, "end": None, "stage_b": None, "verify": [], "windows": {}, "logfile": None,
        "warnings": warnings,
    }
    stage = "A"
    for r in run:
        k = r["kind"]
        body = {key: val for key, val in r.items() if key != "kind"}
        if k == "OFF":
            rep["off"] = body
            rep["status"] = "OFF"
        elif k == "BEGIN":
            rep["begin"] = body
            stage = "A"
            if body.get("trig") == "autorun" or body.get("waive_live"):
                warnings.append("QEMU-only image (HCI_PROBE_AUTORUN / HCI_PROBE_WAIVE_LIVENESS): never flash it")
        elif k == "NOT_PROBED":
            ops = str(body.get("ops", "")).split(",") if "ops" in body else []
            rep["not_probed"].append({
                "reason": body.get("text", ""), "exp": body.get("exp"),
                "opcodes": [o for o in ops if o], "xcheck": body.get("xcheck"),
            })
        elif k == "CMD":
            body["stage"] = stage
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
        elif k == "NVIC":
            rep["nvic"] = body
        elif k == "IDLE":
            _rates(body)
            rep["idle"] = body
        elif k == "IRQBITS":
            rep["irq_bits"] += _bits(body)
            rep["irq_unhandled"] = body.get("unh")
        elif k == "STAGE":
            stage = str(body.get("s", stage))
            if "st" in body:
                rep["stage_b"] = {**(rep["stage_b"] or {}), **body}
            else:
                rep["stage_b"] = body
                if body.get("trig") == "autorun" and not rep.get("begin"):
                    warnings.append("QEMU-only image (HCI_PROBE_AUTORUN, from STAGE: BEGIN not in the log): never "
                                    "flash it")
        elif k == "VERIFY":
            rep["verify"].append(body)
            if body.get("res") != "match":
                detail = (f"got {body.get('got')}, want {body.get('want')}" if "want" in body else
                          f"0C14 did not return {body.get('name')}")
                warnings.append(f"stage B read-back {body.get('what')} did not match: {detail}")
        elif k == "WIN":
            _rates(body)
            w = rep["windows"].setdefault(str(body.get("w")), {"bits": []})
            w.update(body)
            if "end" in body:
                why = {"fault": "a bt_ctrl fault", "hang": "a bt_ctrl hang (one run stuck > 3 s)",
                       "cpu": "the CPU-share guard (bt_ctrl kept the CPU)"}.get(str(body["end"]), str(body["end"]))
                warnings.append(f"window {body.get('w')} ended early on {why}")
        elif k == "WINBITS":
            w = rep["windows"].setdefault(str(body.get("w")), {"bits": []})
            w["bits"] += _bits(body)
            if "unh" in body:
                w["unh"] = body["unh"]
        elif k == "LOGFILE":
            rep["logfile"] = body
            if body.get("st") != "OK":
                warnings.append("the probe could not write SD:/HCIPROBE.LOG (no card, read-only, full?)")
            elif body.get("lost"):
                warnings.append(f"HCIPROBE.LOG is missing {body.get('lost')} lines (probe log copy full)")
        elif k == "RESTART":
            rep["restarts"].append(body)
        elif k == "EVT":
            rep["events"].append(body)
        elif k == "HANG":
            rep["hang"] = body
            warnings.append("bt_ctrl was demoted (HANG): reboot the device now, before anything else; the radio stop "
                            "after a hang is register-only and unverified")
        elif k == "HARDSTOP":
            rep["hardstops"].append(body)
            warnings.append("teardown missed its deadline (HARDSTOP): reboot the device now, before anything else")
        elif k == "HEAP":
            rep["heap"] = body
        elif k == "TIME":
            rep["time"] = body
        elif k == "CTRL":
            rep["ctrl"] = body
        elif k == "STACK":
            if isinstance(body.get("ctrl_size"), int) and isinstance(body.get("ctrl_free"), int):
                body["ctrl_used"] = body["ctrl_size"] - body["ctrl_free"]
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
    b = rep.get("begin") or {}
    if b.get("stage") == "B" and rep["status"] == "DONE" and not (rep["stage_b"] or {}).get("st"):
        warnings.append("stage B was requested but did not run to its end")
    if any(c.get("waived") for c in rep["commands"]):
        warnings.append("stage B ROM replies were waived (QEMU-only image): nothing was verified")
    return rep


def _md_kv(title: str, d: dict | None) -> list[str]:
    if not d:
        return []
    return [f"**{title}:** " + ", ".join(f"`{k}={v}`" for k, v in d.items()), ""]


def to_markdown(rep: dict) -> str:
    b = rep.get("begin") or {}
    out = [f"# Exp 1 HCI probe report (stage {b.get('stage', 'B' if rep.get('stage_b') else 'A')})", ""]
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
                "| n | stage | opcode | command | param | want | got | plen | status | latency (us) | result | "
                "reply (H4) |",
                "|---|---|---|---|---|---|---|---|---|---|---|---|"]
        for c in rep["commands"]:
            rx = c.get("rx", "")
            if c.get("truncated"):
                rx = f"{rx}... ({c.get('trunc')} B shown, CRC-32 0x{c.get('crc', 0):08X})"
            note = f" bd={c['bd']}" if "bd" in c else ""
            for key in ("fwd", "romd", "hw", "ctrl_free", "waived"):
                if key in c:
                    note += f" {key}={c[key]}"
            prm = str(c.get("p", "")) + (f" ({c['pl']} B)" if "pl" in c else "")
            out.append(f"| {c.get('n')} | {c.get('stage')} | {c.get('opcode')} | {c.get('name')} | {prm} | "
                       f"{c.get('want')} | {c.get('got')} | {c.get('plen', '')} | {c.get('st', '')} | {c.get('lat')} | "
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
            out.append(f"- HARD STOP: {h.get('why')}, RWBT_CNTL after the register-only stop 0x{h.get('cntl', 0):08X}")
        if rep.get("hang"):
            h = rep["hang"]
            what = (f"bt_ctrl above {h.get('cpu_pm')} permille CPU for {h.get('cpu_n')} samples (peak {h.get('cpu_pk')})"
                    if h.get("why") == "cpu" else f"bt_ctrl stuck > {h.get('limit')} us")
            out.append(f"- HANG: {what}; {h.get('action')}"
                       + (f", RWBT_CNTL 0x{h['cntl']:08X}" if isinstance(h.get("cntl"), int) else ""))
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

    if rep.get("stage_b") or rep["verify"] or rep["windows"]:
        sb = rep.get("stage_b") or {}
        out += ["## Stage B (verified writes)", "",
                f"Probe name `{sb.get('name')}`, discoverable window {sb.get('scan_ms')} ms: **{sb.get('st', 'not finished')}**"
                + (f", read-back mismatches {sb.get('vfail')}, {sb.get('ms')} ms" if "vfail" in sb else ""), ""]
        if rep["verify"]:
            out += ["| read-back | after | got | want | latency (us) | result |", "|---|---|---|---|---|---|"]
            for v in rep["verify"]:
                got = v.get("got", v.get("name", ""))
                want = v.get("want", v.get("name", ""))
                after = {"ssp": "0C56 (1004 page 1, features[0])", "name": "0C13 (0C14)",
                         "evtmask": "0C01 (hci_tl_env.evt_msk, was " + str(v.get("was", "?")) + ")"}.get(
                             str(v.get("what")), "")
                out.append(f"| {v.get('what')} | {after} | `{got}` | `{want}` | {v.get('lat', '')} | {v.get('res')} |")
            out.append("")
        wins = [("idle", rep.get("idle"))] + [(k, w) for k, w in rep["windows"].items()]
        wins = [(k, w) for k, w in wins if w]
        if rep["windows"]:
            out += ["### IRQ 26 and bt_ctrl load: idle (stage A) vs discoverable (0C1A 03) vs scan off (0C1A 00)", "",
                    "| window | length (us) | IRQs | rate (Hz) | bt_ctrl CPU (%) | wakes | IRQ->BH max/avg (us) | "
                    "bt_ctrl_run max (us) | events | ROM msgs | unknown ROM ids | g_bt_heap hw / used | "
                    "CPU peak per 100 ms (permille) | bt_ctrl stack free (B) | ended |",
                    "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|"]
            for name, w in wins:
                out.append(f"| {name} | {w.get('us')} | {w.get('irq')} | {w.get('irq_rate_hz')} | {w.get('cpu_pct')} | "
                           f"{w.get('wakes')} | {w.get('lat_max')}/{w.get('lat_avg')} | {w.get('ctrl_max')} | "
                           f"{w.get('evts', '')} | {w.get('romd', '')} | {w.get('unk', '')} | "
                           f"{w.get('hw', '')}{'/' + str(w['used']) if 'used' in w else ''} | "
                           f"{w.get('cpu_pk', '')} | {w.get('ctrl_free', '')} | {w.get('end', '')} |")
            out.append("")
            per_bit: dict[int, dict[str, Any]] = {}
            for name, w in wins:
                bits = rep["irq_bits"] if name == "idle" else w.get("bits", [])
                for bit in bits:
                    per_bit.setdefault(bit["bit"], {})[name] = bit
            if per_bit:
                names = [n for n, _ in wins]
                out += ["| INTSTAT bit | " + " | ".join(f"{n}: occ/svc/coal (Hz)" for n in names) + " |",
                        "|---|" + "---|" * len(names)]
                for bit in sorted(per_bit):
                    cells = []
                    for n, w in wins:
                        d = per_bit[bit].get(n)
                        us = w.get("us") or 0
                        hz = round(d["occurrences"] * 1e6 / us, 1) if d and us else 0
                        cells.append(f"{d['occurrences']}/{d['serviced']}/{d['coalesced']} ({hz})" if d else "-")
                    out.append(f"| {bit} | " + " | ".join(cells) + " |")
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
    out += _md_kv("SD log copy", rep.get("logfile"))

    if rep["events"]:
        out += ["## Unsolicited events", ""]
        for e in rep["events"]:
            out.append(f"- len {e.get('len')}: `{e.get('rx', '')}`")
        out.append("")

    if rep["not_probed"]:
        out += ["## Not probed (never sent)", "", "| reason | Exp 1 class | opcodes |", "|---|---|---|"]
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


# Stage B on silicon (synthetic numbers): A's lines trimmed to what the B checks need.
SYNTH_B = """\
HCIP BEGIN v=1 bd=02CA5500011D src=efuse trig=taken tblerr=0 dl=500000 dl_reset=2000000 hang=3000000 stage=B
HCIP NOT_PROBED needs disassembly exp=G-B ops=0C18,0C1C,0C1E,0C3A,0C52
HCIP CMD n=1 op=0C03 want=CC got=CC plen=4 st=00 lat=205000 res=OK rx=040E0401030C00
HCIP IDLE us=5000000 irq=8000 spur=0 wakes=8000 runs=8000 cpu_us=265000 lat_max=40 lat_avg=8 svc_max=41 \
disp_max=40 disp_ev=1 run_max=8 ctrl_max=80 bh_max=1
HCIP IRQBITS b0=8000/8000/0 unh=0x00000000
HCIP RESTART i=5 stop=OK stop_us=15 reset=OK rlat=205000 irq=3 bd=match blat=200 retries=0
HCIP STAGE s=B name=Cassini-Probe-011D scan_ms=30000 trig=taken cpu_pm=700 cpu_n=10
HCIP CMD n=19 op=0C56 p=01 want=CC got=CC plen=4 st=00 lat=410 res=OK fwd=1 romd=1 hw=32 ctrl_free=3008 \
rx=040E0401560C00
HCIP CMD n=20 op=1004 p=01 want=CC got=CC plen=14 st=00 lat=300 res=OK rx=040E0E0104100001010100000000000000
HCIP VERIFY what=ssp op=1004 p=01 got=01 want=01 lat=300 res=match
HCIP CMD n=21 op=0C01 p=FFFFFFFFFFFFBF1D want=CC got=CC plen=4 st=00 lat=380 res=OK fwd=1 romd=1 hw=32 \
rx=040E0401010C00
HCIP VERIFY what=evtmask was=FFFFFFFFFFFFFFFF got=FFFFFFFFFFFFFFFF want=FFFFFFFFFFFFFFFF lat=0 res=match
HCIP CMD n=22 op=0C24 p=040420 want=CC got=CC plen=4 st=00 lat=390 res=OK fwd=1 romd=1 hw=32 rx=040E0401240C00
HCIP CMD n=23 op=0C13 p=43 pl=248 want=CC got=CC plen=4 st=00 lat=520 res=OK fwd=1 romd=1 hw=272 ctrl_free=2900 \
rx=040E0401130C00
HCIP VERIFY what=name op=0C14 name=Cassini-Probe-011D lat=350 res=match
HCIP CMD n=24 op=0C1A p=03 want=CC got=CC plen=4 st=00 lat=400 res=OK fwd=1 romd=1 hw=272 rx=040E04011A0C00
HCIP WIN w=scan us=30000000 irq=60000 spur=0 wakes=60000 runs=60000 cpu_us=3000000 lat_max=60 lat_avg=9 \
svc_max=50 disp_max=45 run_max=12 ctrl_max=95 bh_max=2 evts=0 romd=0 unk=0 hw=272 used=0 cpu_pk=130 ctrl_free=2900
HCIP WINBITS w=scan b0=48000/48000/0 b1=11000/11000/3 b10=1000/1000/0 unh=0x00000000
HCIP CMD n=25 op=0C1A p=00 want=CC got=CC plen=4 st=00 lat=400 res=OK fwd=1 romd=1 hw=272 rx=040E04011A0C00
HCIP WIN w=off us=2000000 irq=3200 spur=0 wakes=3200 runs=3200 cpu_us=106000 lat_max=30 lat_avg=8 svc_max=40 \
disp_max=40 run_max=8 ctrl_max=80 bh_max=1 evts=0 romd=0 unk=0 hw=272 used=0
HCIP WINBITS w=off b0=3200/3200/0 unh=0x00000000
HCIP CMD n=26 op=0C01 p=0000000000000000 want=CC got=CC plen=4 st=00 lat=380 res=OK fwd=1 romd=1 hw=272 \
rx=040E0401010C00
HCIP VERIFY what=evtmask was=FFFFFFFFFFFFFFFF got=0000000000000000 want=00600400F80740E2 lat=0 res=MISMATCH
HCIP CMD n=27 op=0C01 p=FFFFFFFFFFFFBF1D want=CC got=CC plen=4 st=00 lat=380 res=OK fwd=1 romd=1 hw=272 \
rx=040E0401010C00
HCIP VERIFY what=evtmask was=0000000000000000 got=FFFFFFFFFFFFBF1D want=FFFFFFFFFFFFFFFF lat=0 res=MISMATCH
HCIP STAGE s=B st=DONE vfail=2 ms=34000
HCIP TEARDOWN why=end st=OK us=15
HCIP HEAP bytes=8192 hw=272 used=0 largest=8184 fail=0 refused=0 badfree=0 rtos_free=100000
HCIP CTRL rom_msgs=7 rom_unk=0 rom_short=0 late=0 resets=6 rfail=0 hung=0 faults=0 first=0 inv=0 lockv=0 eif=0
HCIP STACK ctrl_size=4096 ctrl_free=2900 dlv_free=1372 probe_free=1196
HCIP RELEASE ipr=0xF0 orig=0xF0 claimed=0 owner=0 lockout=0 legacy=allowed
HCIP END st=DONE fails=0 ms=40000 vfail=2
HCIP LOGFILE file=HCIPROBE.LOG st=OK bytes=4400 lost=0
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
    for needle in ("| 1 | A | 0x0C03 | Reset |", "CRC-32 0x1234ABCD", "| 0 | 7990 | 7980 | 10 |", "## Clean restart",
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

    # Stage B: commands tagged by stage, read-backs (a mismatch is a warning, not a failed run), window comparison.
    b = build_report(SYNTH_B.replace("\\\n", ""))
    assert b["status"] == "DONE" and b["begin"]["stage"] == "B", b["status"]
    stages = {c["opcode"] + str(c.get("p", "")): c["stage"] for c in b["commands"]}
    assert stages["0x0C03"] == "A" and stages["0x0C5601"] == "B" and stages["0x0C1A00"] == "B", stages
    w = {c["opcode"] + str(c.get("p", "")): c for c in b["commands"]}
    assert w["0x0C01FFFFFFFFFFFFBF1D"]["fwd"] == 1 and w["0x0C1343"]["pl"] == 248
    assert w["0x0C1343"]["hw"] == 272 and w["0x0C1A03"]["name"] == "Write Scan Enable"
    assert [v["res"] for v in b["verify"]] == ["match", "match", "match", "MISMATCH", "MISMATCH"]
    assert b["verify"][2]["name"] == "Cassini-Probe-011D" and b["verify"][3]["want"] == "00600400F80740E2"
    assert sum("read-back evtmask did not match" in x for x in b["warnings"]) == 2, b["warnings"]
    assert len(b["warnings"]) == 2, b["warnings"]
    assert b["stage_b"]["st"] == "DONE" and b["stage_b"]["vfail"] == 2 and b["stage_b"]["name"] == "Cassini-Probe-011D"
    sc, off = b["windows"]["scan"], b["windows"]["off"]
    assert sc["irq_rate_hz"] == 2000.0 and sc["cpu_pct"] == 10.0 and off["irq_rate_hz"] == 1600.0, (sc, off)
    assert [x["bit"] for x in sc["bits"]] == [0, 1, 10] and sc["bits"][1]["coalesced"] == 3
    assert b["logfile"]["bytes"] == 4400 and b["end"]["vfail"] == 2
    md = to_markdown(b)
    for needle in ("# Exp 1 HCI probe report (stage B)", "## Stage B (verified writes)",
                   "| 23 | B | 0x0C13 | Write Local Name | 43 (248 B) |", "fwd=1 romd=1 hw=272",
                   "| name | 0C13 (0C14) | `Cassini-Probe-011D` | `Cassini-Probe-011D` | 350 | match |",
                   "| scan | 30000000 | 60000 | 2000.0 | 10.0 |", "| idle | 5000000 | 8000 | 1600.0 | 5.3 |",
                   "| 1 | - | 11000/11000/3 (366.7) | - |", "| 0 | 8000/8000/0 (1600.0) | 48000/48000/0 (1600.0) | "
                   "3200/3200/0 (1600.0) |", "**SD log copy:**"):
        assert needle in md, needle
    json.loads(json.dumps(b))

    # Stage B aborted at its first write; the log copy could not be written.
    bad = (SYNTH_B.replace("\\\n", "").split("HCIP CMD n=20")[0]
           .replace("st=00 lat=410 res=OK", "st=12 lat=410 res=BAD")
           + "HCIP STAGE s=B st=FAIL vfail=0 ms=600\nHCIP FAIL n=1 why=stageB\nHCIP TEARDOWN why=stageB st=OK us=15\n"
           "HCIP ABORT fails=1\nHCIP END st=ABORT fails=1 ms=7000\nHCIP LOGFILE file=HCIPROBE.LOG st=FAIL bytes=3000 "
           "lost=0\n")
    ba = build_report(bad)
    assert ba["status"] == "ABORT" and ba["stage_b"]["st"] == "FAIL" and ba["failures"][0]["why"] == "stageB"
    assert any("HCIPROBE.LOG" in x for x in ba["warnings"]), ba["warnings"]

    # Stage requested, the run ended DONE without stage B's end (must not happen): flagged.
    nob = build_report(SYNTH_B.replace("\\\n", "").replace("HCIP STAGE s=B st=DONE vfail=2 ms=34000\n", ""))
    assert any("did not run to its end" in x for x in nob["warnings"]), nob["warnings"]

    # New stage B fields: per-command stack, window CPU peak / stack, stack used.
    assert w["0x0C1343"]["ctrl_free"] == 2900 and sc["cpu_pk"] == 130 and sc["ctrl_free"] == 2900
    assert b["stack"]["ctrl_used"] == 1196 and b["stage_b"]["trig"] == "taken"
    assert "| 130 | 2900 |  |" in md and "ctrl_free=2900" in md, md

    # Ring wrapped past BEGIN (HCIPROBE.LOG not written): STAGE still identifies the run and a QEMU-only image.
    tail = SYNTH_B.replace("\\\n", "").split("HCIP STAGE s=B name")[1]
    wr = build_report("HCIP STAGE s=B name" + tail.replace("trig=taken", "trig=autorun"))
    assert wr["stage_b"]["trig"] == "autorun" and any("from STAGE" in x for x in wr["warnings"]), wr["warnings"]
    assert any("no HCIP BEGIN" in x for x in wr["warnings"])

    # CPU-share guard during the scan window: window ended early, HANG why=cpu with the radio register read-back.
    cpu = (SYNTH_B.replace("\\\n", "").split("HCIP WIN w=scan")[0]
           + "HCIP WIN w=scan us=1100000 irq=900000 spur=0 wakes=90000 runs=90000 cpu_us=900000 lat_max=60 lat_avg=9 "
           "svc_max=50 disp_max=45 run_max=12 ctrl_max=95 bh_max=2 evts=0 romd=0 unk=0 hw=272 used=0 cpu_pk=990 "
           "ctrl_free=2900 end=cpu\nHCIP STAGE s=B st=FAIL vfail=0 ms=1700\nHCIP FAIL n=1 why=scan_window\n"
           "HCIP TEARDOWN why=scan_window st=PARKED us=300\nHCIP HANG why=cpu limit=3000000 cpu_pm=700 cpu_n=10 "
           "cpu_pk=990 action=bt_stop+irq26_masked+bt_ctrl_demoted+rwbt_silenced cntl=0x00000000\nHCIP ABORT fails=1\n"
           "HCIP RELEASE ipr=0x80 orig=0xF0 claimed=1 owner=1 lockout=1 legacy=refused\nHCIP END st=ABORT fails=1 "
           "ms=9000\n")
    c = build_report(cpu)
    assert c["status"] == "ABORT" and c["windows"]["scan"]["end"] == "cpu" and c["hang"]["why"] == "cpu"
    assert any("CPU-share guard" in x for x in c["warnings"]) and any("reboot the device now" in x
                                                                     for x in c["warnings"]), c["warnings"]
    md = to_markdown(c)
    assert "above 700 permille CPU for 10 samples (peak 990)" in md and "RWBT_CNTL 0x00000000" in md, md

    # A QEMU-only image flashed by mistake refuses to run.
    qs = build_report("HCIP OFF trig=qemu_only_on_silicon file=HCIPROBE.RUN\n")
    assert qs["status"] == "OFF" and "QEMU-only probe image" in qs["warnings"][0], qs["warnings"]

    # A request file with bad content.
    bc = build_report("HCIP OFF trig=bad_content file=HCIPROBE.RUN\n")
    assert bc["status"] == "OFF" and "named no stage" in bc["warnings"][0], bc["warnings"]

    # The CRC the firmware prints is plain CRC-32 (zlib): keep both sides in agreement.
    assert zlib.crc32(b"123456789") == 0xCBF43926
    print("bt_probe_report.py self-test PASSED (parse, last-run selection, wrap warnings, stage B read-backs and "
          "windows, markdown, JSON)")
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
