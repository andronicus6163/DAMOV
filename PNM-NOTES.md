# DAMOV — build & run notes

Verified 2026-09-08 on Ubuntu 22.04, kernel 5.15, 80 cores.
Status: **simulator build OK / simulator run OK. Workload suite UNOBTAINABLE.**

## THE BLOCKER: the workload archive is gone
`get_workloads.sh` (and the fallback link in the README) point at one Mega file:
`https://mega.nz/file/Mz51xJyY#J_ai3_Pl5kVvFETurKmBuMIrOagUK4sadyahOzUYQVE`

It returns **`EBLOCKED`**; the Mega API returns error `-16` for that node, i.e. the
file has been taken down. This is not a quota/rate problem (that would be
`EOVERQUOTA`/`ETEMPUNAVAIL`) and it does not recover on retry. The repo is at its
latest upstream commit (`7a2147b`) and offers no alternative mirror, and the
`workloads/` tree is not in git — only `simulator/command_files/` (which reference
`../workloads/...` paths) survive.

Consequence: the 144 DAMOV functions / 74 applications cannot be reproduced as
packaged. They are all from public suites (BWA, Chai, Darknet, GASE, Hardware
Effects, Hashjoin, HPCC, HPCG, Ligra, PARSEC, Parboil, PolyBench, Phoenix, Rodinia,
SPLASH-2, STREAM), so they could in principle be re-assembled from upstream and
re-instrumented with `simulator/misc/hooks/zsim_hooks.h`, but the DAMOV-specific
instrumentation, datasets and `compile.py` scripts lived in that tarball.

**The simulator itself is fully working** — see below; it just needs your own
ROI-instrumented binaries.

## Source fix required
`simulator/src/locality.h` uses `uint64_t`/`uint32_t` without including
`<cstdint>`. It compiled on gcc <= 5 via transitive includes from `<map>`; on
gcc >= 6 (definitely on 9 and 11) it fails with ~50 errors. One line:
```c
#include <set>
#include <cstdint>     /* <-- add this */
```

## Why a container is needed to RUN
Bundled Pin is 2.14 and SIGABRTs on glibc >= 2.31 (`ExecuteSysArchPrct1`); it works
on glibc <= 2.27. The `SConstruct` is also Python 2. An ubuntu:18.04 image solves
both. See `.toolchains/zsim-docker/Dockerfile`.
(`scripts/setup.sh` is a root-only `apt-get` script — skip it; every dep is in the
image, and `/usr/include/asm` already exists on 22.04.)

## Build (in the container)
```bash
docker build -t zsim-bionic .toolchains/zsim-docker

docker run --rm --privileged -v $PWD:/work -w /work/simulator zsim-bionic bash -c '
  cd ramulator  && make -j16 libramulator.so && cd ..
  cd libconfig  && ./configure --prefix=$PWD && make -j16 install && cd ..
  export PINPATH=$PWD/pin LIBCONFIGPATH=$PWD/libconfig RAMULATORPATH=$PWD/ramulator
  scons -j24'
# -> simulator/build/opt/zsim, build/opt/libzsim.so
```
`scripts/compile.sh` does the same thing with the same three env vars.

Native 22.04 build also succeeds (repo-local Python 2 SCons at
`.toolchains/scons2`), but the binary cannot run anything — use the container.

## Data prep — your own ROI workload
DAMOV's `zsim_hooks.h` only provides `zsim_roi_begin()` / `zsim_roi_end()`
(the `zsim_PIM_function_*` hooks belong to ramulator-pim, not here).
`simulator/smoke_test/stream_roi.c` is a minimal STREAM-style stand-in.

Config files come from `templates/` with three placeholders substituted —
that is all `scripts/generate_config_files.py` does (it also `cd`s to `../workloads`,
so it cannot run without the missing tarball; substitute by hand instead):
```bash
cd simulator
sed -e 's/NUMBER_CORES/1/g' \
    -e 's|STATS_PATH|zsim_stats/smoke/stream|' \
    -e 's|COMMAND_STRING|"smoke_test/stream_roi";|' \
    templates/template_host_ooo.cfg > smoke_test/host_ooo_stream.cfg
# same with template_pim_ooo.cfg for the PIM-core model
mkdir -p zsim_stats/smoke
```
`sim.stats` is a mandatory setting in this fork — a stock ZSim config such as
`tests/simple.cfg` panics with
`Mandatory setting sim.stats (string) not found`. Always start from `templates/`.

## Run
```bash
docker run --rm --privileged -v $PWD:/work -w /work/simulator zsim-bionic bash -c '
  gcc -O2 -o smoke_test/stream_roi smoke_test/stream_roi.c
  setarch x86_64 -R ./build/opt/zsim smoke_test/host_ooo_stream.cfg
  setarch x86_64 -R ./build/opt/zsim smoke_test/pim_ooo_stream.cfg'
```
Writes per-run `zsim_stats/smoke/stream{,_pim}.{zsim.out,ramulator.stats,dramRequestsPerPhase,out.cfg}`.
Measured on the STREAM stand-in: host OOO **63 436 813** cycles vs PIM OOO
**49 347 464** cycles for the same 29.36 M instructions — the host/PIM comparison
the framework exists for.

Templates available: `template_host_{ooo,inorder,accelerator}`,
`template_host_nuca{,_1_core}{,_inorder}`, `template_host_prefetch_*`,
`template_pim_{ooo,inorder,accelerator}`.
Ramulator memory models: `ramulator-configs/{HMC,HBM,DDR3,DDR4,GDDR5,LPDDR3,LPDDR4,PCM,ALDRAM,DSARP}-config.cfg`.

## Container file ownership
The `docker run` commands above run as root, so everything the build writes into the
bind-mounted repo comes out **root-owned** on the host, and later `git checkout` /
`rm` fail with `Permission denied`. Do **not** fix this with `--user $(id -u):$(id -g)` — Pin must ptrace the process it
instruments, and a non-root container aborts with a `ptrace_scope` error. Run as root
and hand the files back afterwards (no sudo needed, the container is root):
```bash
docker run --rm -v $PWD:/work zsim-bionic chown -R $(id -u):$(id -g) /work
```
