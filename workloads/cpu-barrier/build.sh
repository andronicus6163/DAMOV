#!/bin/bash
# Build the CPU-baseline barrier. Simulated binaries must be built inside the bionic container (Pin 2.14 +
# glibc 2.27); a 22.04 build links against GLIBC_2.34 and will not start under zsim.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
docker run --rm -e OUT -e DEFS -v "$ROOT":/pnm -w /pnm/DAMOV/workloads/cpu-barrier zsim-bionic-r2 bash -c "
  g++ -O2 -std=c++11 -I /pnm/DAMOV/simulator/misc/hooks -o \${OUT:-cpu_barrier} \${DEFS:-} cpu_barrier.cpp -lpthread &&
  chown $(id -u):$(id -g) \${OUT:-cpu_barrier}"
echo "built: ${OUT:-cpu_barrier}"
