import os

import ramulator

PIM_MODE = os.environ.get("HMC_PIM_MODE", "0") == "1"
VAULTS = 32

frontend = ramulator.frontend.External(clock_ratio=1)

vaults = [
    ramulator.controller.HMCVault(
        dram=ramulator.dram.HMC(org_preset="HMC_4GB", timing_preset="HMC_2500"),
        scheduler=ramulator.scheduler.FRFCFSRowHit(),
        refresh_manager=ramulator.refresh_manager.AllBank(),
        row_policy=ramulator.row_policy.Open(),
        addr_mapper=ramulator.addr_mapper.PassThroughAddrMapper(),
    )
    for _ in range(VAULTS)
]

mem = ramulator.memory_system.HMC(
    clock_ratio=1,
    controllers=vaults,
    pim_mode=PIM_MODE,
    host_links=4,
    link_width=16,
    lane_speed_gbps=30.0,
    payload_flits=16,
    max_block_bits=8,
)

sim = ramulator.Simulation(frontend, mem)
