import os

import ramulator

STANDARD = os.environ.get("HBM_STANDARD", "HBM3")
ORG = os.environ.get("HBM_ORG", "HBM3_16Gb_8hi")
TIMING = os.environ.get("HBM_TIMING", "HBM3_6400Mbps")
SYSTEM = os.environ.get("HBM_SYSTEM", "HBMStack")
# One PIM unit per channel; `HBM_STACKS` stacks of 16 channels each (a barrier over more than 16 PIM units needs
# more than one stack, and the host is the only path between stacks).
STACKS = int(os.environ.get("HBM_STACKS", "1"))
CHANNELS = 16 * STACKS

frontend = ramulator.frontend.External(clock_ratio=1)

controllers = [
    ramulator.controller.HBM34(
        dram=getattr(ramulator.dram, STANDARD)(org_preset=ORG, timing_preset=TIMING),
        scheduler=ramulator.scheduler.FRFCFSRowHit(),
        refresh_manager=ramulator.refresh_manager.AllBank(),
        row_policy=ramulator.row_policy.Open(),
        addr_mapper=ramulator.addr_mapper.PassThroughAddrMapper() if SYSTEM == "HBMStack"
        else ramulator.addr_mapper.ChRaBaRoCo(),
    )
    for _ in range(CHANNELS)
]

if SYSTEM == "HBMStack":
    mem = ramulator.memory_system.HBMStack(
        clock_ratio=1,
        controllers=controllers,
        # Ramulator guesstimates: host PHY + interposer per direction, base-die hop between channel sites.
        host_phy_latency_ps=int(os.environ.get("HBM_HOST_PHY_PS", "1000")),
        base_die_hop_ps=int(os.environ.get("HBM_HOP_PS", "200")),
        pim_mesh_width=4,
        request_bytes=64,
        stacks=STACKS,
        # Guesstimate: host-side switch/link hop to reach a stack, each direction.
        host_stack_link_ps=int(os.environ.get("HBM_STACK_LINK_PS", "0" if STACKS == 1 else "500")),
    )
else:
    mem = ramulator.memory_system.GenericDRAM(
        clock_ratio=1,
        controllers=controllers,
        channel_mapper=ramulator.channel_mapper.CacheLineInterleave(),
    )

sim = ramulator.Simulation(frontend, mem)
