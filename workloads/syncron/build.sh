#!/bin/bash
# Build the SynCron workloads. The simulated binaries must be built inside the bionic container (Pin 2.14 +
# glibc 2.27); a 22.04 build links against GLIBC_2.34 and will not start under zsim.
#
#   ./build.sh probe fetchadd monitor ...                one binary per source, named after it
#   OUT=monitor_spin DEFS=-DMON_SPIN ./build.sh monitor  one source, a variant name and extra defines
#   ./build.sh variants                                  every _spin / _ph variant the runs use
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
BINS=${*:-probe}
if [ "$BINS" = "variants" ]; then
  for v in "monitor_spin -DMON_SPIN" "monitor_ph -DMON_PHASES" "monitor_spin_ph -DMON_SPIN -DMON_PHASES" \
           "fetchadd_spin -DFA_SPIN" "ff_spin -DFF_SPIN"; do
    set -- $v; out=$1; shift
    OUT=$out DEFS="$*" "$0" "${out%%_*}"
  done
  exit 0
fi
if [ -n "${OUT:-}" ]; then
  docker run --rm -v "$ROOT":/pnm -w /pnm/DAMOV/workloads/syncron zsim-bionic-r2 bash -c "
    g++ -O2 -std=c++11 ${DEFS:-} -I /pnm/DAMOV/simulator/misc/hooks -o $OUT $BINS.cpp -lpthread &&
    chown $(id -u):$(id -g) $OUT"
  echo "built: $OUT ($BINS.cpp ${DEFS:-})"
  exit 0
fi
docker run --rm -v "$ROOT":/pnm -w /pnm/DAMOV/workloads/syncron zsim-bionic-r2 bash -c "
  for b in $BINS; do g++ -O2 -std=c++11 -I /pnm/DAMOV/simulator/misc/hooks -o \$b \$b.cpp -lpthread || exit 1; done
  chown $(id -u):$(id -g) $BINS"
echo "built: $BINS"
