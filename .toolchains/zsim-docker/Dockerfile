FROM ubuntu:18.04
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      build-essential scons python2.7 python-minimal \
      libhdf5-dev libelf-dev zlib1g-dev libboost-all-dev \
      libxerces-c-dev autoconf automake libtool m4 perl flex bison byacc \
      ca-certificates make libcurl4-openssl-dev \
 && rm -rf /var/lib/apt/lists/*
RUN ln -sf /usr/include/asm-generic /usr/include/asm || true
WORKDIR /work
