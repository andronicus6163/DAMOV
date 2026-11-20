#!/bin/bash
# Builds the CPU-baseline apps for zsim (SynCron/HBM3E PIM baseline):
#   BFS (push), PageRank (1 push sweep), BellmanFord (pull), spmv
# from the gbbs fork's working tree, with the campaign's compiler + flags
# (clang 18, -march=x86-64-v3 -mtune=znver3, 64-bit IDs, OpenMP via LLVM libomp 18.1.3)
# plus -DZSIM (zsim ROI magic ops in gbbs/helpers/roi.h), linked against the
# Ubuntu 18.04 sysroot so Pin 2.14 can run them. libomp + libstdc++ are static.
# Output: bin/{BFS,PageRank,BellmanFord,spmv} + bin/BUILD_INFO.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
GBBS=${GBBS:-/media/data/andronicus/pnm-studies/gbbs}
mkdir -p "$HERE/src" "$HERE/bin"
rsync -a --delete --exclude .git --exclude 'external/deps' --exclude '*.o' --exclude '*.a' --exclude '/bin/' "$GBBS/" "$HERE/src/"
# keep deps between builds
mkdir -p "$HERE/deps"; ln -sfn /work/deps "$HERE/src/external/deps"
COMMIT=$(git -C "$GBBS" rev-parse --short HEAD)
DIRTY=$(git -C "$GBBS" status --porcelain -- gbbs benchmarks cpu_baselines external makefile.variables | wc -l)
docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$HERE:/work" -w /work/src gbbs-zsim-build bash -c '
set -euo pipefail
mkdir -p /tmp/bin
cat > /tmp/bin/clang++-sr <<"W"
#!/bin/sh
link=1
for a in "$@"; do case "$a" in -c|-E|-S|-M|-MM) link=0;; esac; done
if [ $link = 1 ]; then
  exec clang++-18 --sysroot=/sysroot -isystem /opt/omp/include "$@" -L/opt/omp/lib -static-libstdc++ -static-libgcc -ldl -lpthread
else
  exec clang++-18 --sysroot=/sysroot -isystem /opt/omp/include "$@"
fi
W
printf "#!/bin/sh\nexec clang-18 --sysroot=/sysroot \"\$@\"\n" > /tmp/bin/clang-sr
chmod +x /tmp/bin/*; export PATH=/tmp/bin:$PATH
export CXX_LLVM=clang++-sr CC_LLVM=clang-sr OPENMP_LLVM=1
export MARCH_FLAGS="-march=x86-64-v3 -mtune=znver3 -DZSIM"
[ -f external/deps/lib/libgtest.a ] || CXX_DEPS=clang++-sr CC_DEPS=clang-sr scripts/cpu_baseline/build_deps.sh
rm -f benchmarks/BFS/NonDeterministicBFS/BFS benchmarks/PageRank/PageRank benchmarks/GeneralWeightSSSP/BellmanFord/BellmanFord cpu_baselines/spmv
make -C gbbs -s -j16
for a in BFS/NonDeterministicBFS:BFS PageRank:PageRank GeneralWeightSSSP/BellmanFord:BellmanFord; do
  make -C benchmarks/${a%%:*} -s ${a##*:}
  cp benchmarks/${a%%:*}/${a##*:} /work/bin/
done
make -C cpu_baselines -s CXX=clang++-sr spmv
cp cpu_baselines/spmv /work/bin/
clang++-18 --version | head -1 > /work/bin/BUILD_INFO
'
{
  echo "gbbs commit $COMMIT (dirty files in build paths: $DIRTY)"
  echo "MARCH_FLAGS=-march=x86-64-v3 -mtune=znver3 -DZSIM; OpenMP LLVM libomp 18.1.3 static; sysroot ubuntu 18.04 (glibc 2.27)"
  date -u +%FT%TZ
} >> "$HERE/bin/BUILD_INFO"
cd "$HERE/bin"; sha256sum BFS PageRank BellmanFord spmv >> BUILD_INFO
cat BUILD_INFO
