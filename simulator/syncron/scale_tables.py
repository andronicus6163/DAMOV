#!/usr/bin/env python3
"""Scaling tables: SynCron's barrier as the machine grows from 4 to 128 NDP units (64 to 2048 cores).

  scale_tables.py

Reads every `barrier_units__*__u<U>x16__*__xbu__*` run (the per-unit-crossbar series) and prints, per machine size:
period, the engine's modelled cost and the protocol's own arithmetic for it, the network it crossed (hops per message,
link queueing) and what the run cost to simulate. The arithmetic assumes the Master SE is unit 0 (where the harness
places the barrier variable) and units numbered row-major on a mesh `mesh_x` wide, routed X then Y:

  two-level (every core):  the Master sends barrier_depart_global to units 1..U-1 in order, 24 cycles apart; unit u's
                           arrives 80 cycles per hop later, its SE takes it (24) and fans out 16 departures (16 x 24),
                           and the last core is 2 cycles further:  max_u(24 * u + 80 * hops(u)) + 24 + 16*24 + 2
  one-level (fewer):       N departures from the Master, 24 apart, the last one reaching some unit:
                           between N*24 + 2 (the Master's own unit) and N*24 + 80*max_hops + 24 + 2
"""
import pathlib
import re
import sys

SIM = pathlib.Path(__file__).resolve().parent.parent
BASE = SIM / "zsim_stats" / "syncron"
SPU, LOCAL, LINK, CPU = 24, 2, 80, 16


def hops(u, w):
    if not w:
        return 1 if u else 0
    return (u % w) + (u // w)  # from unit 0 at (0, 0)


def expected(n, units, w, cpu):
    if n == units * CPU:  # two-level
        last = max(SPU * i + LINK * hops(u, w) for i, u in enumerate(range(1, units), start=1))
        return last + SPU + cpu * SPU + LOCAL, last + SPU + cpu * SPU + LOCAL
    far = max(hops(u, w) for u in range(units))
    return n * SPU + LOCAL, n * SPU + LINK * far + SPU + LOCAL


def load(path):
    name = path.name[: -len(".zsim.out")]
    log = BASE / "logs" / f"{name}.log"
    if not log.exists() or not (BASE / "logs" / f"{name}.time").exists():
        return None
    text = log.read_text(errors="replace")
    r = {"name": name, "ok": "BARRIER done" in text and "VERIFY PASS" in text,
         "panic": next((l for l in text.splitlines() if "Panic" in l or "Failed assertion" in l), "")}
    for k, v in re.findall(r"(period_cycles|period_min|period_max)=([\d.]+)", text):
        r[k] = float(v)
    tm = re.search(r"real\s+(\d+)m([\d.]+)s", (BASE / "logs" / f"{name}.time").read_text())
    r["wall"] = int(tm.group(1)) * 60 + float(tm.group(2)) if tm else 0
    out = BASE / f"{name}.zsim.out"
    if out.exists():
        d = [x for x in out.read_text().split("===") if x.strip()][-1]
        cycles = [int(x) for x in re.findall(r"^\s+cycles: (\d+)", d, re.M)]
        r["sim_cycles"] = max(cycles) if cycles else 0
        b = re.search(r"^ syncron:.*?(?=^ \S)", d, re.M | re.S)
        if b:
            for k in ("barriers", "barrierReleaseCycles", "linkMsgs", "linkHops", "linkQueueCycles", "linkMaxQueue",
                      "twoLevelBarriers", "earlyReleases", "spuInversions", "localMsgs", "globalMsgs"):
                m = re.search(rf"^\s+{k}: (\d+)", b.group(0), re.M)
                if m:
                    r[k] = int(m.group(1))
    m = re.search(r"__(\w+?)__u(\d+)x16__c(\d+)x(\d+)__", name)
    r["scheme"], r["units"], r["cpu"], r["used"] = m.group(1), int(m.group(2)), int(m.group(3)), int(m.group(4))
    m = re.search(r"__mesh(\d+)__", name)
    r["mesh_x"] = int(m.group(1)) if m else 0
    return r


def main():
    rows = [r for r in (load(p) for p in sorted(BASE.glob("barrier_units__*__u*x16__*__xbu__*.zsim.out"))) if r]
    rows.sort(key=lambda r: (r["mesh_x"] > 0, r["units"], r["scheme"], r["cpu"]))
    print("| network | units | cores | scheme | N | period (cyc) | period (ns) | modelled | protocol | check | "
          "hops/msg | link queue avg (max) | early | sim cycles | wall | cycles/s |")
    print("|---|---:|---:|---|---:|---:|---:|---:|---:|:---:|---:|---:|---:|---:|---:|---:|")
    for r in rows:
        n = r["cpu"] * r["used"]
        net = f"mesh {r['mesh_x']}x{r['units'] // r['mesh_x']}" if r["mesh_x"] else "full"
        if not r["ok"]:
            print(f"| {net} | {r['units']} | {r['units'] * CPU} | {r['scheme']} | {n} | **BROKE** {r['panic'][:60]} |"
                  + " |" * 10)
            continue
        mod, proto, chk = "—", "—", ""
        if r.get("barriers") and r["scheme"] == "syncron":
            mod_v = r["barrierReleaseCycles"] / r["barriers"]
            lo, hi = expected(n, r["units"], r["mesh_x"], r["cpu"])
            mod, proto = f"{mod_v:,.0f}", (f"{lo:,}" if lo == hi else f"{lo:,}–{hi:,}")
            chk = "✓" if lo - 0.5 <= mod_v <= hi + 0.5 else "✗"
        hpm = f"{r['linkHops'] / r['linkMsgs']:.2f}" if r.get("linkMsgs") else "—"
        q = (f"{r['linkQueueCycles'] / r['linkHops']:.1f} ({r['linkMaxQueue']})" if r.get("linkHops") else "—")
        cps = f"{r['sim_cycles'] / r['wall']:,.0f}" if r.get("wall") else "—"
        print(f"| {net} | {r['units']} | {r['units'] * CPU} | {r['scheme']} | {n} | {r['period_cycles']:,.0f} | "
              f"{r['period_cycles'] / 2:,.0f} | {mod} | {proto} | {chk} | {hpm} | {q} | {r.get('earlyReleases', '—')} | "
              f"{r.get('sim_cycles', 0):,} | {r['wall']:.0f}s | {cps} |")
    return 0


if __name__ == "__main__":
    sys.exit(main())
