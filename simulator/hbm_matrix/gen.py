import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
SIM = os.path.dirname(HERE)
W = "../workloads/damov-original"

MEMORIES = {
    "ddr4": ("DDR4-2400.yaml", "DDR4-2400.yaml"),
    "hmc": ("HMC.yaml", "HMC-PIM.yaml"),
    "hbm3": ("HBM3-16Gb_8hi.yaml", "HBM3-16Gb_8hi.yaml"),
    "hbm3e": ("HBM3E-24Gb_12hi.yaml", "HBM3E-24Gb_12hi.yaml"),
    "hbm4": ("HBM4-32Gb_8Hi-8000.yaml", "HBM4-32Gb_8Hi-8000.yaml"),
}

WORKLOADS = {
    "stream": f"{W}/STREAM/stream_triad_4M",
    "hpcg": f"{W}/hpcg/build-spm/bin/xhpcg --nx=32 --ny=32 --nz=32 --rt=1",
    "bfs": f"{W}/ligra/apps/binaries/BFS_Ems -rounds 1 -r 0 {W}/ligra/inputs/rMat_100K",
    "pr": f"{W}/ligra/apps/binaries/PageRank_Emd -rounds 1 -maxiters 5 {W}/ligra/inputs/rMat_100K",
}

MEM_BLOCK = re.compile(r'type = "Ramulator";\s*ramulatorConfig = "[^"]*";')


def main():
    os.makedirs(os.path.join(HERE, "configs"), exist_ok=True)
    names = []
    for mode in ("host", "pim"):
        template = open(os.path.join(SIM, "templates", f"template_{mode}_ooo.cfg")).read()
        for mem, yamls in MEMORIES.items():
            yaml = yamls[0] if mode == "host" else yamls[1]
            for wl, cmd in WORKLOADS.items():
                name = f"{wl}_{mem}_{mode}"
                cfg = template.replace("NUMBER_CORES", "1")
                cfg = cfg.replace("STATS_PATH", f"zsim_stats/hbm_matrix/{name}")
                cfg = cfg.replace("COMMAND_STRING", f'"{cmd}";')
                cfg, n = MEM_BLOCK.subn(
                    f'type = "Ramulator2";\n        clockMode = "ns";\n        ramulatorConfig = "ramulator2-configs/{yaml}";', cfg)
                assert n == 1, (mode, n)
                open(os.path.join(HERE, "configs", f"{name}.cfg"), "w").write(cfg)
                names.append(name)
    print("\n".join(names))


if __name__ == "__main__":
    main()
