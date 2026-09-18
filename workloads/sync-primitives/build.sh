#!/bin/bash
# The simulated binary must be built inside the bionic container (Pin 2.14 + glibc 2.27); a 22.04 build links
# against GLIBC_2.34 and will not start under zsim. The .native build is only for protocol checks on the host.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)          # pnm-repos
docker run --rm -v "$ROOT":/pnm -w /pnm/DAMOV/workloads/sync-primitives zsim-bionic-r2 bash -c "
  g++ -O2 -std=c++11 -I /pnm/DAMOV/simulator/misc/hooks -o barrier_host barrier_host.cpp -lpthread &&
  chown $(id -u):$(id -g) barrier_host"
g++ -O2 -std=c++11 -I "$HERE/../../simulator/misc/hooks" -o "$HERE/barrier_host.native" "$HERE/barrier_host.cpp" -lpthread
echo "built: barrier_host (bionic, for zsim), barrier_host.native (host, --map index only)"
