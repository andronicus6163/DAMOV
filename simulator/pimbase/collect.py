#!/usr/bin/env python3
"""Summarise PIM-baseline cells (zsim_stats/pimbase/<cell>.zsim.out + logs/<cell>.log) into one CSV row each.

  ./collect.py [cell ...]      (default: every cell with a zsim.out) -> zsim_stats/pimbase/summary.csv + stdout

ROI time: the app's own "Running Time" (zsim virtualises the clock, so it is simulated time at 2 GHz); it is
cross-checked against the busiest core's cycle count. Memory: Ramulator 2 HBMStack's PIM request counters.
Atomics: SyncronSystem's routeAtomics counters (src 0 app, 1 OpenMP runtime, 2 libc).
"""
import csv
import re
import sys
from pathlib import Path

D = Path(__file__).resolve().parents[1] / "zsim_stats" / "pimbase"
FREQ_HZ = 2.0e9


def num(text, key):
    m = re.search(r"\n\s+" + re.escape(key) + r": (\d+)", text)
    return int(m.group(1)) if m else None


def vec(text, key):
    m = re.search(re.escape(key) + r":.*?\n((?:\s+\d+: \d+.*\n)+)", text)
    return [int(x) for x in re.findall(r"\d+: (\d+)", m.group(1))] if m else []


def cell_row(cell):
    out = (D / f"{cell}.zsim.out").read_text()
    log = (D / "logs" / f"{cell}.log").read_text(errors="replace")
    y = (D / f"{cell}.ramulator2.stats.yaml")
    ytxt = y.read_text() if y.exists() else ""
    syn = out[out.index(" syncron:"):out.index(" sched:")] if " syncron:" in out else ""
    cyc = [int(x) for x in re.findall(r"\n\s+cycles: (\d+)", out[:out.index(" l1i_u0:")])]
    ins = [int(x) for x in re.findall(r"\n\s+instrs: (\d+)", out[:out.index(" l1i_u0:")])]
    rt = re.findall(r"Running Time: ([\d.]+)", log)
    roi_s = float(rt[-1]) if rt else None
    checks = re.findall(r"(Reachable: \d+|PR sum: [\d.e+-]+|SSSP dist sum: \d+|SpMV check \w+[^\n]*|ATOMIC_UBENCH[^\n]*)", log)
    ym = lambda k: sum(int(float(x)) for x in re.findall(r"\n\s+" + k + r": ([\d.e+]+)", ytxt))
    pim, loc, rem = ym("pim_requests"), ym("pim_local_requests"), ym("pim_remote_stack_requests")
    src = vec(syn, "atomicsPerSrc") + [0, 0, 0]
    a = num(syn, "atomics") or 0
    wall = (D / "logs" / f"{cell}.time")
    wall_s = None
    if wall.exists():
        m = re.search(r"real\s+(\d+)m([\d.]+)s", wall.read_text())
        wall_s = int(m.group(1)) * 60 + float(m.group(2)) if m else None
    return {
        "cell": cell,
        "check": "; ".join(checks),
        "roi_s": roi_s,
        "roi_cycles_app": round(roi_s * FREQ_HZ) if roi_s else None,
        "max_core_cycles": max(cyc) if cyc else None,
        "instrs_total": sum(ins),
        "instrs_max_core": max(ins) if ins else None,
        "atomics": a,
        "atomics_app": src[0], "atomics_omp": src[1], "atomics_libc": src[2],
        "atomic_remote": num(syn, "atomicRemote"),
        "atomic_contended": num(syn, "atomicContended"),
        "atomic_avg_grant_cycles": round((num(syn, "atomicGrantCycles") or 0) / a, 1) if a else None,
        "se_msgs": sum(vec(syn, "msgsPerUnit")),
        "se_queue_cycles": num(syn, "queueCycles"),
        "spu_inversions": num(syn, "spuInversions"),
        "pim_requests": pim,
        "pim_local": loc,
        "pim_remote": rem,
        "remote_frac": round(rem / pim, 3) if pim else None,
        "dram_bytes": pim * 64,
        "dram_GBps": round(pim * 64 / roi_s / 1e9, 1) if pim and roi_s else None,
        "inter_unit_bytes": ym("inter_unit_bytes"),
        "sim_wall_s": wall_s,
        "sim_minstr_per_s": round(sum(ins) / wall_s / 1e6, 3) if wall_s else None,
    }


def main():
    cells = sys.argv[1:] or sorted(p.name[:-len(".zsim.out")] for p in D.glob("*.zsim.out")
                                   if p.stat().st_size > 100)
    rows = [cell_row(c) for c in cells]
    with open(D / "summary.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0]))
        w.writeheader()
        w.writerows(rows)
    for r in rows:
        print(r)


if __name__ == "__main__":
    main()
