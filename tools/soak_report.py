#!/usr/bin/env python3
"""soak_report.py - summarise a soak test CSV from probes/soak_monitor.com.

    soak_report.py <node> [<node>...]     fetch [.SOAK]SOAK.CSV from each node and report
    soak_report.py --file <csv>           report a local copy

For each agent process: how long it ran, its CPU per hour, and the change in
working set, process pages and page-file use from the first hour to the last
(a leak shows as steady growth), plus any change of PID (a restart) or gap.
"""
import csv
import io
import os
import subprocess
import sys
from datetime import datetime

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def rows_from(text):
    out = []
    for r in csv.DictReader(io.StringIO(text)):
        try:
            r["t"] = datetime.strptime(r["time"][:19], "%Y-%m-%d %H:%M:%S")
        except (ValueError, KeyError):
            continue
        out.append(r)
    return out


def report(name, rows):
    print("== %s: %d samples" % (name, len(rows)))
    if not rows:
        return
    pids = []
    for r in rows:
        if not pids or pids[-1] != r["pid"]:
            pids.append(r["pid"])
    gaps = [(a["t"], b["t"]) for a, b in zip(rows, rows[1:]) if (b["t"] - a["t"]).total_seconds() > 1500]
    live = [r for r in rows if r["pid"] != "none"]
    print("  from %s to %s (%.1f h)" % (rows[0]["t"], rows[-1]["t"], (rows[-1]["t"] - rows[0]["t"]).total_seconds() / 3600))
    print("  processes: %s%s" % (", ".join(pids), "  <- RESTARTED" if len(pids) > 1 else "  (no restart)"))
    if any(r["pid"] == "none" for r in rows):
        print("  samples with no agent running: %d" % sum(r["pid"] == "none" for r in rows))
    for a, b in gaps:
        print("  gap in sampling: %s to %s" % (a, b))
    by_pid = {}
    for r in live:
        by_pid.setdefault(r["pid"], []).append(r)
    for pid, rs in by_pid.items():
        h = (rs[-1]["t"] - rs[0]["t"]).total_seconds() / 3600
        if h <= 0:
            continue
        cpu = (int(rs[-1]["cpu_10ms"]) - int(rs[0]["cpu_10ms"])) / 100.0
        print("  %s: %.1f h, CPU %.1f s (%.1f s an hour, %.2f s a minute)" % (pid, h, cpu, cpu / h, cpu / h / 60))
        # compare the first hour with the last hour, so start-up doesn't count as growth
        first = [r for r in rs if (r["t"] - rs[0]["t"]).total_seconds() <= 3600]
        last = [r for r in rs if (rs[-1]["t"] - r["t"]).total_seconds() <= 3600]
        for col, label in (("ws_pagelets", "working set"), ("proc_pages", "process pages"),
                           ("pgfl_used_pagelets", "page-file use"), ("virtpeak", "peak virtual size")):
            a = max(int(r[col]) for r in first)
            b = max(int(r[col]) for r in last)
            verdict = "flat" if b == a else ("+%d (%.1f%%)" % (b - a, 100.0 * (b - a) / a if a else 0))
            print("    %-18s first hour %8d   last hour %8d   %s" % (label, a, b, verdict))
        bio = (int(rs[-1]["bufio"]) - int(rs[0]["bufio"])) / h
        print("    buffered I/O       %.0f an hour (network traffic)" % bio)


def main():
    args = sys.argv[1:]
    if not args:
        sys.exit(__doc__)
    if args[0] == "--file":
        report(args[1], rows_from(open(args[1]).read()))
        return
    for node in args:
        out = subprocess.run([os.path.join(ROOT, "tools", "vms.sh"), node, "dcl", "type [.soak]soak.csv"],
                             capture_output=True, text=True).stdout
        text = "\n".join(l for l in out.splitlines() if l.startswith(("time,", "20")))
        report(node, rows_from(text))


if __name__ == "__main__":
    main()
