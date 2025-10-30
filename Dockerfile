FROM ubuntu:22.04

RUN apt-get update && apt-get install -y \
    cmake \
    g++ \
    git \
    libssl-dev \
    libsasl2-dev \
    pkg-config \
    && rm -rf /var/lib/apt/lists/*

# Build mongo-c driver
RUN git clone https://github.com/mongodb/mongo-c-driver.git && \
    cd mongo-c-driver && \
    mkdir cmake-build && cd cmake-build && \
    cmake -DENABLE_AUTOMATIC_INIT_AND_CLEANUP=OFF .. && \
    make -j$(nproc) && make install && \
    cd ../.. && rm -rf mongo-c-driver

# Build mongo-cxx driver
RUN git clone https://github.com/mongodb/mongo-cxx-driver.git && \
    cd mongo-cxx-driver && \
    git checkout releases/stable && \
    mkdir build && cd build && \
    cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local .. && \
    make -j$(nproc) && make install && \
    cd ../.. && rm -rf mongo-cxx-driver
