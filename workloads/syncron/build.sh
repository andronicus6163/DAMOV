#!/bin/bash
# Build the SynCron workloads. The simulated binaries must be built inside the bionic container (Pin 2.14 +
# glibc 2.27); a 22.04 build links against GLIBC_2.34 and will not start under zsim.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
BINS=${*:-probe}
docker run --rm -v "$ROOT":/pnm -w /pnm/DAMOV/workloads/syncron zsim-bionic-r2 bash -c "
  for b in $BINS; do g++ -O2 -std=c++11 -I /pnm/DAMOV/simulator/misc/hooks -o \$b \$b.cpp -lpthread || exit 1; done
  chown $(id -u):$(id -g) $BINS"
echo "built: $BINS"
