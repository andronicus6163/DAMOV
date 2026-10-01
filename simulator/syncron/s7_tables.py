#!/usr/bin/env python3
"""S7 tables: the barrier under the paper's four systems, swept over participants, placement and link latency.

  s7_tables.py            # print every table from zsim_stats/syncron (run `run.py --collect` first if in doubt)

Reads the same logs and stats run.py writes. Every SynCron point is also checked against the protocol: its modelled
cost (the engine's barrierReleaseCycles / barriers) must lie between the local-only and the remote-last bounds of
Sec 4.3's arithmetic -- N departures serialised at 24 core cycles each, plus one link traversal and the remote SE's
service when the last departure goes to another unit. When every core participates, Sec 4.1.3's two-level protocol
applies instead, and its cost is exact: (units-1) global departures + link + the remote SE's service + that unit's
local fan-out + the last hop to the core.
"""
import pathlib
import re
import sys

SIM = pathlib.Path(__file__).resolve().parent.parent
BASE = SIM / "zsim_stats" / "syncron"
SCHEMES = ("ideal", "syncron", "hier", "central")
SPU = 24          # 12 SPU cycles at 1 GHz, in 2 GHz core cycles
LOCAL = 2         # core <-> local SE
MHZ = 2000
PHASE = 100
WARMUP = 50


def run_name(scope, scheme, cpu, used, link, groups=1, st=64):
    return (f"barrier_{scope}__{scheme}__u4x16__c{cpu}x{used}__g{groups}__st{st}__l{link}__w{WARMUP}"
            f"__p{PHASE}__f{MHZ}__timing")


def load(name):
    log = BASE / "logs" / f"{name}.log"
    if not log.exists():
        return None
    text = log.read_text(errors="replace")
    if not (BASE / "logs" / f"{name}.time").exists() and not any(
            m in text for m in ("Panic", "Failed assertion", "VERIFY FAIL")):
        return None  # still running: run.py writes the .time file when zsim exits
    if "BARRIER done" not in text or "VERIFY PASS" not in text:
        return {"broken": True}
    r = {k: float(v) for k, v in re.findall(r"(period_cycles|period_min|period_max)=([\d.]+)", text)}
    out = BASE / f"{name}.zsim.out"
    if out.exists():
        d = [x for x in out.read_text().split("===") if x.strip()][-1]
        block = re.search(r"^ syncron:.*?(?=^ \S)", d, re.M | re.S)
        if block:
            for key in ("barriers", "barrierReleaseCycles", "localMsgs", "globalMsgs", "swMsgs", "swGlobalMsgs",
                        "linkMsgs", "linkQueueCycles", "linkMaxQueue", "twoLevelBarriers", "spuInversions",
                        "spuInversionCycles", "arrivals"):
                m = re.search(rf"^\s+{key}: (\d+)", block.group(0), re.M)
                if m:
                    r[key] = int(m.group(1))
    if r.get("barriers"):
        r["modelled"] = r["barrierReleaseCycles"] / r["barriers"]
    return r


def cell(r, key="period_cycles"):
    if r is None:
        return "—"
    if r.get("broken"):
        return "FAIL"
    return f"{r[key]:,.0f}" if key in r else "—"


def spread(r):
    if not r or r.get("broken") or "period_min" not in r:
        return ""
    lo, hi, mid = r["period_min"], r["period_max"], r["period_cycles"]
    return f"±{100.0 * max(mid - lo, hi - mid) / mid:.0f}%" if mid else ""


def syncron_bounds(n, used, link):
    """Sec 4.3 arithmetic for the modelled cost (last release - last arrival)."""
    link_cycles = link * MHZ // 1000
    if used == 4 and n == 64:  # every core: two-level (Sec 4.1.3), global departures first
        exact = (used - 1) * SPU + link_cycles + SPU + (n // used) * SPU + LOCAL
        return exact, exact
    lo = n * SPU + LOCAL                       # the last departure goes to a core in the Master's own unit
    hi = n * SPU + (link_cycles + SPU + LOCAL if used > 1 else LOCAL)  # ... or to another unit
    return lo, hi


def participants_table(scope, used, cpus, title):
    print(f"\n### {title}\n")
    print("| N | per unit | Ideal | SynCron | Hier | Central | SynCron modelled | protocol bound | check | SPU out-of-order |")
    print("|---:|---:|---:|---:|---:|---:|---:|---:|:---:|---:|")
    for cpu in cpus:
        rs = {s: load(run_name(scope, s, cpu, used, 40)) for s in SCHEMES}
        n = cpu * used
        sy = rs["syncron"]
        mod, bound, ok = "—", "—", ""
        if sy and not sy.get("broken") and "modelled" in sy:
            lo, hi = syncron_bounds(n, used, 40)
            mod = f"{sy['modelled']:,.1f}"
            bound = f"{lo}" if lo == hi else f"{lo}–{hi}"
            ok = "✓" if lo - 0.5 <= sy["modelled"] <= hi + 0.5 else "✗"
        inv = "—"
        if sy and not sy.get("broken") and "spuInversions" in sy and sy.get("arrivals"):
            inv = (f"{100.0 * sy['spuInversions'] / (sy['localMsgs'] + sy['globalMsgs']):.1f}% "
                   f"({sy['spuInversionCycles'] / max(sy['spuInversions'], 1):.0f} cyc)")
        print(f"| {n} | {cpu} | {cell(rs['ideal'])} | {cell(sy)} {spread(sy)} | {cell(rs['hier'])} {spread(rs['hier'])} "
              f"| {cell(rs['central'])} {spread(rs['central'])} | {mod} | {bound} | {ok} | {inv} |")


def placement_table():
    print("\n### Same participant count, packed into one unit vs spread over four\n")
    print("| N | placement | Ideal | SynCron | Hier | Central |")
    print("|---:|---|---:|---:|---:|---:|")
    for n, packed, spread_cpu in ((4, 4, 1), (8, 8, 2), (16, 16, 4)):
        for label, scope, cpu, used in (("1 unit", "unit", packed, 1), ("4 units", "units", spread_cpu, 4)):
            rs = {s: load(run_name(scope, s, cpu, used, 40)) for s in SCHEMES}
            print(f"| {n} | {label} ({cpu}×{used}) | {cell(rs['ideal'])} | {cell(rs['syncron'])} | "
                  f"{cell(rs['hier'])} | {cell(rs['central'])} |")


def link_table(cpu):
    n = cpu * 4
    print(f"\n### Link latency, {n} participants across 4 units ({cpu} per unit)\n")
    print("| link | Ideal | SynCron | Hier | Central | Hier÷SynCron | Central÷SynCron | SynCron modelled | bound | "
          "link queue (max) |")
    print("|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
    for link in (40, 100, 200, 300, 500):
        rs = {s: load(run_name("units", s, cpu, 4, link)) for s in SCHEMES}
        sy = rs["syncron"]

        def ratio(a):
            if not a or a.get("broken") or not sy or sy.get("broken"):
                return "—"
            return f"{a['period_cycles'] / sy['period_cycles']:.2f}"

        mod, bound = "—", "—"
        if sy and not sy.get("broken") and "modelled" in sy:
            lo, hi = syncron_bounds(n, 4, link)
            mod = f"{sy['modelled']:,.0f}" + (" ✓" if lo - 0.5 <= sy["modelled"] <= hi + 0.5 else " ✗")
            bound = f"{lo}–{hi}"
        q = "—"
        if sy and not sy.get("broken") and "linkMsgs" in sy:
            q = f"{sy['linkQueueCycles'] / max(sy['linkMsgs'], 1):.1f} ({sy['linkMaxQueue']})"
        print(f"| {link} ns | {cell(rs['ideal'])} | {cell(sy)} | {cell(rs['hier'])} | {cell(rs['central'])} | "
              f"{ratio(rs['hier'])} | {ratio(rs['central'])} | {mod} | {bound} | {q} |")


def main():
    participants_table("unit", 1, (1, 2, 4, 8, 15, 16), "Participants inside one NDP unit (barrier_wait_within_unit)")
    participants_table("units", 4, (1, 2, 4, 8, 15, 16), "Participants spread over 4 NDP units (barrier_wait_across_units)")
    placement_table()
    link_table(16)
    link_table(15)
    link_table(4)
    return 0


if __name__ == "__main__":
    sys.exit(main())
