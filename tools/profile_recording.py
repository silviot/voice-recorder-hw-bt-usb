#!/usr/bin/env python3
"""Read CPU0 recording profile windows from an OpenFW bench log and write CSV.

One log per recording. Use BTBENCH.LOG or the SRAM ring, whichever covers
the desired interval. Each row is one window. The final window can be short.
All durations include preemption. idle_us is FreeRTOS idle-task residency.
"""
# SPDX-License-Identifier: MIT
import argparse
import csv
from pathlib import Path
import re
import sys


WINDOW = re.compile(
    r"^PROFILE frames=(\d+) dsp_cyc=(\d+) dsp_us=(\d+) sd_us=(\d+) "
    r"wall_us=(\d+) idle_us=(\d+) gets_b=(\d+)$"
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", nargs="+", type=Path)
    args = parser.parse_args()
    writer = csv.writer(sys.stdout, lineterminator="\n")
    writer.writerow(("log", "window", "frames", "wall_us", "dsp_cycles",
                     "dsp_us", "sd_us", "idle_us", "gets_bytes",
                     "cycles_per_frame", "cycles_per_us", "idle_pct"))
    for path in args.log:
        count = 0
        for line in path.read_text(errors="replace").splitlines():
            if not line.startswith("PROFILE "):
                continue
            match = WINDOW.fullmatch(line)
            if not match:
                parser.error(f"{path}: incomplete profile line: {line}")
            frames, cycles, dsp_us, sd_us, wall_us, idle_us, gets = map(int, match.groups())
            if not frames or not wall_us or not dsp_us:
                parser.error(f"{path}: zero frames or elapsed time")
            count += 1
            writer.writerow((path.name, count, frames, wall_us, cycles, dsp_us,
                             sd_us, idle_us, gets,
                             round(cycles / frames, 2) if cycles else "",
                             round(cycles / dsp_us, 2),
                             round(idle_us * 100 / wall_us, 2)))
        if not count:
            parser.error(f"{path}: no recording profile windows")


if __name__ == "__main__":
    main()
