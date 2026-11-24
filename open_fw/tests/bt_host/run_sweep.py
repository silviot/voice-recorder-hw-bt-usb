#!/usr/bin/env python3
"""Allocation-failure sweep over one Bluedroid lifecycle call (plan M1 "general rule", M3 harness).

--mode selects the call: esp_bluedroid_init(), _enable() (default), _disable() or _deinit(). Runs
`test_bt_host sweep-count MODE` to learn how many allocation attempts the call makes (all tasks), then one process per
attempt N (`test_bt_host sweep-one N MODE`, parallel, each under a timeout) with the N-th attempt failing. A point
passes when the call completed (OK) or failed cleanly (an error with the API state unchanged: CLEAN_FAIL), teardown
from there (a failed disable/deinit is retried) returned heap, tasks and esp_timer slots to the baseline, a following
clean cycle still worked, and there was no sanitizer report, contract error or BT fault.

Every non-passing point is classified (CRASH = assert/sanitizer abort, HANG = watchdog deadline, CHECK = leak or
contract check failed) and grouped by the failing allocation's call site, symbolised from the INJECT backtrace the
harness prints (addr2line; the binary is non-PIE).

A point that passes but whose N-th attempt never happened (`injected=0`: the run made fewer attempts than sweep-count
reported) tested nothing and is classified NOT_INJECTED, which fails the run.

Known upstream findings may be listed in a waiver file (one `mode  site-function[<-caller]  CLASS  COUNT  reason`
per line, '#' comments; only the lines of the swept mode apply): matching points are reported as WAIVED and do not fail the run. COUNT is the exact number of points
the waiver is expected to cover. A waiver matching MORE points than its COUNT fails the run (a new failing path ended
at a waived site); one matching fewer (or none: stale) prints a warning so the file gets updated. With --max the
upper bound still applies, the stale warnings are skipped. Exit status: 0 when every point passes or is waived within
its count.
"""
import argparse
import concurrent.futures
import os
import re
import subprocess
import sys

HARNESS_FRAMES = ("host_heap.c", "allocator.c", "/queue.c", "/tasks.c", "/list.c", "/timers.c", "semaphore.c",
                  "mutex.c", "execinfo", "??")


def run(binary, args, timeout):
    try:
        p = subprocess.run([binary] + args, capture_output=True, text=True, timeout=timeout,
                           env=dict(os.environ, ASAN_OPTIONS=os.environ.get("ASAN_OPTIONS", "") + ":abort_on_error=0"))
        return p.returncode, p.stdout + p.stderr
    except subprocess.TimeoutExpired as e:
        out = (e.stdout or b"").decode() if isinstance(e.stdout, bytes) else (e.stdout or "")
        return 124, out + "\nHANG: runner timeout"


_sym_cache = {}


def symbolise(binary, addrs):
    """[(function, file:line)] for a backtrace, inlined frames expanded. Return addresses are moved back one byte so
    the call instruction (not the next line) is reported."""
    keys = [int(a, 16) - 1 for a in addrs if a.startswith("0x")]
    todo = sorted(set(k for k in keys if k not in _sym_cache))
    if todo:
        out = subprocess.run(["addr2line", "-f", "-i", "-a", "-e", binary] + ["0x%x" % k for k in todo],
                             capture_output=True, text=True).stdout.splitlines()
        cur, frames = None, []
        for line in out:
            if re.fullmatch(r"0x[0-9a-f]+", line):
                if cur is not None:
                    _sym_cache[cur] = frames
                cur, frames = int(line, 16), []
            else:
                frames.append(line)
        if cur is not None:
            _sym_cache[cur] = frames
    res = []
    for k in keys:
        f = _sym_cache.get(k, [])
        for i in range(0, len(f) - 1, 2):
            res.append((f[i], f[i + 1].split(" ")[0]))
    return res


def site_of(frames):
    """(function, location, caller) of the first frame outside the allocator / harness / kernel plumbing."""
    for i, (fn, loc) in enumerate(frames):
        if not any(h in loc for h in HARNESS_FRAMES) and fn not in ("osi_malloc_func", "osi_calloc_func"):
            return fn, loc, frames[i + 1][0] if i + 1 < len(frames) else "?"
    return (frames[0][0], frames[0][1], "?") if frames else ("?", "?", "?")


def waiver_matches(w, fn, caller, cls):
    """A waiver names `function` or `function<-caller` (for generic sites such as btc_transfer_context)."""
    site = w[0].split("<-")
    return w[1] == cls and site[0] == fn and (len(site) == 1 or site[1] == caller)


def classify(rc, out):
    m = re.search(r"SWEEP mode=\w+ n=\d+ injected=(\d+) .*outcome=(\w+)", out)
    if m and int(m.group(1)) == 0:
        return ("NOT_INJECTED", "", "the N-th allocation attempt never happened: nothing was tested")
    if rc == 0 and m:
        return ("PASS", m.group(2), "")
    if rc == 124 or "HANG:" in out:
        hang = re.search(r"HANG: (.*)", out)
        return ("HANG", "", hang.group(1) if hang else "")
    for pat in (r"Assertion .*", r"ERROR: AddressSanitizer: [^\n]*", r"runtime error: [^\n]*",
                r"configASSERT failed[^\n]*", r"bt_host: [^\n]*reached[^\n]*", r"bt_host heap: [^\n]*"):
        mm = re.search(pat, out)
        if mm:
            return ("CRASH", "", mm.group(0)[:160])
    fails = re.findall(r"FAIL [^\n]*\n([^\n]*)", out)
    if fails or rc == 1:
        f = re.search(r"FAIL ([^\n]*)", out)
        return ("CHECK", "", (f.group(1) if f else "rc=1")[:160])
    return ("CRASH", "", "rc=%d" % rc)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("binary")
    ap.add_argument("--mode", default="enable", choices=("init", "enable", "disable", "deinit"))
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--timeout", type=int, default=90)
    ap.add_argument("--max", type=int, default=0, help="sweep only attempts 1..max (0 = all)")
    ap.add_argument("--waivers", default="")
    ap.add_argument("--verbose", action="store_true")
    a = ap.parse_args()

    rc, out = run(a.binary, ["sweep-count", a.mode], a.timeout)
    m = re.search(r"SWEEP_ALLOCS %s (\d+)" % a.mode, out)
    if rc != 0 or not m:
        print(out)
        print("sweep: sweep-count %s failed (rc=%d)" % (a.mode, rc))
        return 1
    total = int(m.group(1))
    n_max = min(total, a.max) if a.max else total
    print("sweep: esp_bluedroid_%s() makes %d allocation attempts; failing each of 1..%d in turn" % (a.mode, total,
                                                                                                     n_max))

    waivers = []
    if a.waivers:
        for line in open(a.waivers):
            line = line.split("#", 1)[0].strip()
            if line:
                parts = line.split(None, 4)
                if len(parts) < 4 or not parts[3].isdigit() or \
                        parts[0] not in ("init", "enable", "disable", "deinit"):
                    print("sweep: bad waiver line (want `mode  function[<-caller]  CLASS  COUNT  reason`): %s" % line)
                    return 1
                if parts[0] == a.mode:
                    waivers.append((parts[1], parts[2], parts[4] if len(parts) > 4 else "", int(parts[3])))

    results = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as ex:
        futs = {ex.submit(run, a.binary, ["sweep-one", str(n), a.mode], a.timeout): n for n in range(1, n_max + 1)}
        for f in concurrent.futures.as_completed(futs):
            results[futs[f]] = f.result()

    groups = {}
    counts = {"OK": 0, "CLEAN_FAIL": 0, "WAIVED": 0, "HANG": 0, "CRASH": 0, "CHECK": 0, "NOT_INJECTED": 0}
    waiver_hits = {i: 0 for i in range(len(waivers))}
    for n in sorted(results):
        rc, out = results[n]
        cls, outcome, msg = classify(rc, out)
        inj = re.search(r"INJECT attempt=\d+ bt=([0-9a-fx,]+)", out)
        frames = symbolise(a.binary, inj.group(1).split(",")) if inj else []
        fn, loc, caller = site_of(frames)
        if cls == "PASS":
            counts[outcome] = counts.get(outcome, 0) + 1
            if a.verbose:
                print("  n=%-4d %-10s %s (%s)" % (n, outcome, fn, loc))
            continue
        wi = next((i for i, w in enumerate(waivers) if waiver_matches(w, fn, caller, cls)), None) \
            if cls in ("CRASH", "HANG", "CHECK") else None
        waived = waivers[wi] if wi is not None else None
        if wi is not None:
            waiver_hits[wi] += 1
        key = ("WAIVED" if waived else cls, fn + "<-" + caller, loc, msg if not waived else waived[2])
        counts["WAIVED" if waived else cls] += 1
        groups.setdefault(key, []).append(n)
        if a.verbose:
            chain = " <- ".join("%s" % f for f, _ in frames[:8])
            print("  n=%-4d %-6s %s: %s\n         %s" % (n, cls, fn, msg, chain))

    print("sweep %s: %d points: %d OK, %d CLEAN_FAIL, %d WAIVED, %d CRASH, %d HANG, %d CHECK, %d NOT_INJECTED" % (
        a.mode, n_max, counts["OK"], counts["CLEAN_FAIL"], counts["WAIVED"], counts["CRASH"], counts["HANG"],
        counts["CHECK"], counts["NOT_INJECTED"]))
    for (cls, fn, loc, msg), ns in sorted(groups.items(), key=lambda kv: (kv[0][0], kv[1][0])):
        shown = ",".join(str(n) for n in ns[:8]) + (",..." if len(ns) > 8 else "")
        print("  %-6s n=%-24s %s (%s)\n         %s" % (cls, shown, fn, os.path.relpath(loc) if loc != "?" else loc,
                                                     msg))
    over = 0
    for i, w in enumerate(waivers):
        hits, want = waiver_hits[i], w[3]
        if hits > want:
            over += 1
            print("  WAIVER EXCEEDED: %s %s matched %d points, expected %d: new failing points at a waived site" % (
                w[0], w[1], hits, want))
        elif hits < want and n_max == total:
            print("  warning: waiver %s %s matched %d of %d expected points%s: update sweep_waivers.txt" % (
                w[0], w[1], hits, want, " (STALE)" if hits == 0 else ""))
    bad = counts["CRASH"] + counts["HANG"] + counts["CHECK"] + counts["NOT_INJECTED"] + over
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
