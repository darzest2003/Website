# -------------------
# Stage 1: Build MongoDB C and C++ Drivers
# -------------------
FROM ubuntu:22.04 AS builder

RUN apt-get update && apt-get install -y \
    build-essential cmake pkg-config libssl-dev libsasl2-dev git wget

# -------- Build MongoDB C Driver --------
RUN echo "=== Building MongoDB C Driver ===" && \
    git clone https://github.com/mongodb/mongo-c-driver.git && \
    cd mongo-c-driver && \
    mkdir cmake-build && cd cmake-build && \
    cmake -DENABLE_AUTOMATIC_INIT_AND_CLEANUP=OFF -DCMAKE_BUILD_TYPE=Release .. && \
    make -j$(nproc) && make install && \
    cd ../.. && rm -rf mongo-c-driver

# Ensure pkg-config can find installed libs
ENV PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:$PKG_CONFIG_PATH

# -------- Build MongoDB C++ Driver --------
RUN echo "=== Building MongoDB C++ Driver ===" && \
    git clone https://github.com/mongodb/mongo-cxx-driver.git && \
    cd mongo-cxx-driver && \
    git checkout releases/stable && \
    mkdir build && cd build && \
    cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local && \
    make -j$(nproc) && make install && \
    cd ../.. && rm -rf mongo-cxx-driver

# -------------------
# Stage 2: Build and Run Server
# -------------------
FROM ubuntu:22.04

RUN apt-get update && apt-get install -y \
    build-essential libssl-dev libsasl2-dev && \
    rm -rf /var/lib/apt/lists/*

# Copy MongoDB libs
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
