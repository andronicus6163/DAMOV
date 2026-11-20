# Builder for the zsim (Pin 2.14) versions of the CPU-baseline apps.
# Same compiler as the CPU campaign (clang 18 on Ubuntu 24.04), but every binary is linked against the
# Ubuntu 18.04 sysroot taken from zsim-bionic-r2 (glibc 2.27, libstdc++ 11), because Pin 2.14 cannot follow
# glibc >= 2.34's clone3 thread creation. The OpenMP runtime is LLVM libomp 18.1.3 (the campaign's runtime
# version), built from source against the same sysroot and linked statically.
FROM ubuntu:24.04
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      clang-18 make cmake python3 perl xz-utils git ca-certificates file \
 && rm -rf /var/lib/apt/lists/*
COPY sysroot /sysroot
COPY openmp-18.1.3.src.tar.xz cmake-18.1.3.src.tar.xz /src/
RUN cd /src && tar xf openmp-18.1.3.src.tar.xz && tar xf cmake-18.1.3.src.tar.xz && mv cmake-18.1.3.src cmake \
 && cmake -S openmp-18.1.3.src -B build -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=clang-18 -DCMAKE_CXX_COMPILER=clang++-18 \
      -DCMAKE_SYSROOT=/sysroot -DLIBOMP_ENABLE_SHARED=OFF -DOPENMP_ENABLE_LIBOMPTARGET=OFF \
      -DLIBOMP_OMPD_SUPPORT=OFF -DOPENMP_ENABLE_OMPT_TOOLS=OFF -DCMAKE_INSTALL_PREFIX=/opt/omp \
 && cmake --build build -j16 && cmake --install build && rm -rf /src/build
# Compiler wrappers: clang 18 with the bionic sysroot; libomp.a found before clang's own (24.04) libomp.
RUN printf '#!/bin/sh\nexec clang-18 --sysroot=/sysroot "$@"\n' > /usr/local/bin/clang-sr \
 && printf '#!/bin/sh\nexec clang++-18 --sysroot=/sysroot -L/opt/omp/lib -isystem /opt/omp/include "$@" -ldl -lpthread\n' > /usr/local/bin/clang++-sr \
 && chmod +x /usr/local/bin/clang-sr /usr/local/bin/clang++-sr
