#!/usr/bin/env python3
"""Generate the zsim network file for SynCron's NDP system (HPCA'21 Table 5).

Table 5's intra-NDP-unit network is a "buffered crossbar, 1-cycle arbitration, 1 cycle per hop, M/D/1 queuing
model". zsim's MeshNetworkMD1 is exactly an M/D/1 model: each router port charges rho/(2(1-rho)) cycles of queueing
on top of a fixed per-hop delay, with the load measured per phase. So a crossbar is one router, and everything
belonging to an NDP unit sits at that router's coordinate:

    unit u -> coordinate (u, 0): its 16 L1I + 16 L1D caches and LLC bank u
    memory -> coordinate (0, 1)

A core reaching its own unit's LLC bank therefore pays one traversal each way (`--hop-delay`, default 2 = 1 cycle
arbitration + 1 cycle hop) plus the M/D/1 queueing of the crossbar port. A core reaching another unit's bank pays
the extra hops, which is what the inter-unit links are for -- until placement (S2) makes bank selection follow the
unit, zsim hashes addresses across banks, so that traffic is real and visible rather than hidden.

File format (MeshNetworkMD1):
    xDim yDim hopDelay
    src dst 0 latency                     static: getRTT returns 2*latency
    src dst 1 srcX srcY dstX dstY         dynamic: routed, queued, round trip

The parser registers BOTH directions of every line it reads (and asserts if either is already present), so each pair
appears exactly once here -- the L1 -> LLC miss path and the LLC -> L1 invalidation path are the same entry.

  gen_network.py --units 4 --cores-per-unit 16 > network_4x16.mesh
"""
import argparse
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--units", type=int, default=4)
    ap.add_argument("--cores-per-unit", type=int, default=16)
    ap.add_argument("--hop-delay", type=int, default=2, help="cycles per traversal: 1 arbitration + 1 hop (Table 5)")
    ap.add_argument("--llc", default="llc", help="LLC cache-group name (one bank per unit)")
    a = ap.parse_args()

    out = [f"{a.units} 2 {a.hop_delay}"]
    l1s = {u: [f"l1i_u{u}-{c}" for c in range(a.cores_per_unit)] + [f"l1d_u{u}-{c}" for c in range(a.cores_per_unit)]
           for u in range(a.units)}
    banks = {u: f"{a.llc}-0b{u}" for u in range(a.units)}

    for u in range(a.units):
        for l1 in l1s[u]:
            for b in range(a.units):
                out.append(f"{l1} {banks[b]} 1 {u} 0 {b} 0")      # both directions, see above
    for u in range(a.units):
        out.append(f"{banks[u]} mem-0 0 0")                       # the bypassed LLC never queries this; present so
                                                                  # a stray lookup cannot panic
    print("\n".join(out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
