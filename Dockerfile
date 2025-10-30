# -------------------
# Stage 1: Build MongoDB C and C++ Drivers
# -------------------
FROM ubuntu:22.04 AS builder

# Install build dependencies + Python3 for C++ driver version detection
RUN apt-get update && apt-get install -y \
    build-essential cmake pkg-config libssl-dev libsasl2-dev git wget python3 && \
    rm -rf /var/lib/apt/lists/*

# -------- Build MongoDB C Driver --------
RUN echo "=== Building MongoDB C Driver ===" && \
    rm -rf mongo-c-driver && \
    git clone --depth 1 https://github.com/mongodb/mongo-c-driver.git && \
    cd mongo-c-driver && \
    mkdir -p cmake-build && cd cmake-build && \
    cmake -DENABLE_AUTOMATIC_INIT_AND_CLEANUP=OFF -DCMAKE_BUILD_TYPE=Release .. && \
    make -j2 && make install && \
    cd ../.. && rm -rf mongo-c-driver

# Ensure pkg-config can find installed libs
ENV PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:$PKG_CONFIG_PATH

# -------- Build MongoDB C++ Driver --------
RUN echo "=== Building MongoDB C++ Driver ===" && \
    rm -rf mongo-cxx-driver && \
    git clone --depth 1 https://github.com/mongodb/mongo-cxx-driver.git && \
    cd mongo-cxx-driver && \
    git checkout releases/stable && \
    mkdir -p build && cd build && \
    export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:$PKG_CONFIG_PATH && \
    cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local && \
    make -j1 && make install && \
    cd ../.. && rm -rf mongo-cxx-driver

# -------------------
# Stage 2: Build and Run Server
# -------------------
FROM ubuntu:22.04

# Install runtime dependencies
RUN apt-get update && apt-get install -y \
    build-essential libssl-dev libsasl2-dev libcurl4-openssl-dev zlib1g && \
    rm -rf /var/lib/apt/lists/*

COPY --from=builder /usr/local /usr/local
ENV PKG_CONFIG_PATH=/usr/local/lib/pkgconfig

WORKDIR /app
COPY server.cpp .

RUN echo "=== Compiling Server ===" && \
    g++ -std=c++17 -O2 server.cpp -o server \
        $(pkg-config --cflags --libs libmongocxx libbsoncxx) \
        -lpthread

EXPOSE 8080
ENV PORT=8080
CMD ["./server"]
