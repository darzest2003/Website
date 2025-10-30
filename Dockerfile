# -------------------
# Stage 1: Build MongoDB C++ Driver
# -------------------
FROM ubuntu:22.04 as builder

# Install required build dependencies
RUN apt-get update && apt-get install -y \
    build-essential cmake pkg-config libssl-dev libsasl2-dev git wget

# Build MongoDB C Driver
RUN git clone https://github.com/mongodb/mongo-c-driver.git && \
    cd mongo-c-driver && \
    mkdir cmake-build && cd cmake-build && \
    cmake -DENABLE_AUTOMATIC_INIT_AND_CLEANUP=OFF .. && \
    make -j$(nproc) && make install

# Build MongoDB C++ Driver
RUN git clone https://github.com/mongodb/mongo-cxx-driver.git && \
    cd mongo-cxx-driver && \
    git checkout releases/stable && \
    mkdir build && cd build && \
    cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local && \
    make -j$(nproc) && make install

# -------------------
# Stage 2: Build and Run the Server
# -------------------
FROM ubuntu:22.04

# Install runtime dependencies
RUN apt-get update && apt-get install -y \
    build-essential libssl-dev libsasl2-dev && \
    rm -rf /var/lib/apt/lists/*

# Copy MongoDB C++ driver from builder
COPY --from=builder /usr/local /usr/local

# Create app directory
WORKDIR /app

# Copy your source code
COPY server.cpp .

# Compile the server
RUN g++ -std=c++17 -O2 server.cpp -o server \
    -I/usr/local/include/mongocxx/v_noabi \
    -I/usr/local/include/bsoncxx/v_noabi \
    -lmongocxx -lbsoncxx -lpthread

# Set environment and port
ENV PORT=8080
EXPOSE 8080

# Run the server
CMD ["./server"]
