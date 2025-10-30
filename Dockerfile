# Use Debian-based image with build tools
FROM debian:bullseye-slim AS build

# Install g++, MongoDB driver dependencies, and build tools
RUN apt-get update && apt-get install -y --no-install-recommends \
    g++ make cmake git pkg-config libssl-dev libbson-dev libmongoc-dev ca-certificates curl \
    && rm -rf /var/lib/apt/lists/*

# --- Install MongoDB C++ Driver (mongocxx / bsoncxx) ---
RUN git clone https://github.com/mongodb/mongo-cxx-driver.git && \
    cd mongo-cxx-driver && \
    cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local . && \
    make -j$(nproc) && make install && \
    cd .. && rm -rf mongo-cxx-driver

# Set work directory
WORKDIR /app

# Copy server code
COPY server.cpp .

# Compile the server with MongoDB libraries
RUN g++ -std=c++17 -O2 -pthread server.cpp -o server -lmongocxx -lbsoncxx

# Runtime image (small, only final binary)
FROM debian:bullseye-slim

WORKDIR /app

# Copy compiled binary from build stage
COPY --from=build /app/server .

# Create data & public folders
RUN mkdir -p /app/data /app/public

# Expose the port (Render will set PORT automatically)
EXPOSE 8080

# Run server, binding to $PORT
CMD ["sh", "-c", "./server"]
