"""SynCron's system on HBM3E: the same 4 NDP units as HBM1.py, with each unit's stack an HBM3E 24Gb 12-hi.

  PYTHONPATH=<ramulator2>/python python3 -m ramulator export HBM3E-4unit.py -o HBM3E-24Gb_12hi-4unit.yaml

Only the memory device changes: 16 channels per stack at 9.6 Gbps/pin (36 GiB per stack) instead of HBM 1.0's 8 at
1 Gbps. Everything that makes it SynCron's system is kept from HBM1.py -- one NDP unit per stack (`per_stack`),
unit-major addressing, Table 5's inter-unit links, and no base-die hops (the crossbar is zsim's). The HBM-PIM study's
HBM3E-24Gb_12hi-4stack.yaml is a different system (one PIM unit per channel, stacks joined through the host).
"""
import os

import ramulator

ORG = os.environ.get("HBM3E_ORG", "HBM3E_24Gb_12hi")        # 18432 Mb per channel x 16 = 36 GiB per stack
TIMING = os.environ.get("HBM3E_TIMING", "HBM3E_9600Mbps")
STACKS = int(os.environ.get("HBM_STACKS", "4"))
CHANNELS_PER_STACK = int(os.environ.get("HBM_CHANNELS_PER_STACK", "16"))

frontend = ramulator.frontend.External(clock_ratio=1)

controllers = [
    ramulator.controller.HBM34(
        dram=ramulator.dram.HBM3E(org_preset=ORG, timing_preset=TIMING),
        scheduler=ramulator.scheduler.FRFCFSRowHit(),
        refresh_manager=ramulator.refresh_manager.AllBank(),
        row_policy=ramulator.row_policy.Open(),
        addr_mapper=ramulator.addr_mapper.PassThroughAddrMapper(),  # HBMStack does the mapping itself
    )
    for _ in range(STACKS * CHANNELS_PER_STACK)
]

mem = ramulator.memory_system.HBMStack(
    clock_ratio=1,
    controllers=controllers,
    host_phy_latency_ps=int(os.environ.get("HBM_HOST_PHY_PS", "1000")),
    base_die_hop_ps=int(os.environ.get("HBM_HOP_PS", "0")),
    pim_mesh_width=int(os.environ.get("HBM_MESH_WIDTH", "4")),
    request_bytes=64,
    stacks=STACKS,
    host_stack_link_ps=int(os.environ.get("HBM_STACK_LINK_PS", "0")),
    source_mapping=os.environ.get("HBM_SOURCE_MAPPING", "per_stack"),
    addressing=os.environ.get("HBM_ADDRESSING", "unit_major"),
    cores_per_stack=int(os.environ.get("NDP_CORES_PER_UNIT", "16")),
    inter_unit=os.environ.get("HBM_INTER_UNIT", "link"),
    inter_unit_latency_ps=int(os.environ.get("INTER_UNIT_LATENCY_PS", "40000")),   # Table 5: 40 ns per cache line
    inter_unit_bw_gbps=float(os.environ.get("INTER_UNIT_BW_GBPS", "12.8")),        # Table 5: per direction
)

sim = ramulator.Simulation(frontend, mem)
