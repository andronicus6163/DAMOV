#!/bin/bash
# Build zsim (DAMOV fork, with the SynCron engine) against the ramulator2 fork, inside the bionic container.
# Mirrors MultiPIM/pimgraph/scripts/build_sim.sh; the paths are the ones baked into the build (/pnm = pnm-repos).
set -eu
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
JOBS=${JOBS:-32}
docker run --rm --privileged -v "$ROOT":/pnm -w /pnm/DAMOV/simulator "${IMAGE:-zsim-bionic-r2}" bash -c "
  trap 'chown -R $(id -u):$(id -g) /pnm/DAMOV/simulator/build 2>/dev/null' EXIT
  set -e
  export PINPATH=/pnm/DAMOV/simulator/pin LIBCONFIGPATH=/pnm/DAMOV/simulator/libconfig \
         RAMULATORPATH=/pnm/DAMOV/simulator/ramulator RAMULATOR2PATH=/pnm/ramulator2
  scons -j$JOBS
"
