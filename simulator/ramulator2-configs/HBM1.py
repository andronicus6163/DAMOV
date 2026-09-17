"""SynCron's memory (HPCA'21 Table 5): HBM 1.0, 4 stacks, 4 GB, 500 MHz, 8 channels per stack.

  HBM_STACKS=4 python3 -m ramulator export HBM1.py -o HBM1-1Gb-4stack.yaml

One NDP unit sits on each stack, so `stacks` splits the controllers into per-unit groups, `source_mapping =
per_stack` makes a core's home the whole stack its unit owns (`cores_per_stack` cores per unit), and `addressing =
unit_major` puts the stack in the address's high bits so a contiguous region belongs to one unit -- which is what
per-unit data placement needs. Cross-unit accesses take the explicit inter-unit link (`inter_unit = link`): Table 5's
12.8 GB/s per direction and 40 ns per cache line, with the payload occupying the link so later transfers queue.

`base_die_hop_ps` is 0 on purpose: in SynCron the core-to-memory-controller cost is the compute die's crossbar, which
zsim's network models, so charging base-die hops here as well would count it twice.
"""
import os

import ramulator

ORG = os.environ.get("HBM1_ORG", "HBM1_1Gb")          # 1 Gb per channel x 8 channels = 1 GB per stack
TIMING = os.environ.get("HBM1_TIMING", "HBM1_1Gbps")  # 1000 Mbps = 500 MHz, Table 5
STACKS = int(os.environ.get("HBM_STACKS", "4"))
CHANNELS_PER_STACK = int(os.environ.get("HBM_CHANNELS_PER_STACK", "8"))

frontend = ramulator.frontend.External(clock_ratio=1)

controllers = [
    ramulator.controller.HBM12(
        dram=ramulator.dram.HBM1(org_preset=ORG, timing_preset=TIMING),
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
