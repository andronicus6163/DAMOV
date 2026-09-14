FROM zsim-bionic
RUN apt-get update \
 && apt-get install -y --no-install-recommends software-properties-common gnupg dirmngr git wget \
 && add-apt-repository -y ppa:ubuntu-toolchain-r/test \
 && apt-get update \
 && apt-get install -y --no-install-recommends g++-11 gcc-11 libstdc++6 \
 && rm -rf /var/lib/apt/lists/*
RUN wget -q https://github.com/Kitware/CMake/releases/download/v3.27.9/cmake-3.27.9-linux-x86_64.tar.gz -O /tmp/cmake.tgz \
 && tar xzf /tmp/cmake.tgz -C /opt && rm /tmp/cmake.tgz \
 && ln -s /opt/cmake-3.27.9-linux-x86_64/bin/cmake /usr/local/bin/cmake
