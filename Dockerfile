# Use a stable and lightweight base image
FROM ubuntu:22.04

# Install build tools (g++, make, pthreads, libc headers, netcat for debugging)
RUN apt-get update && apt-get install -y \
    g++ \
    make \
    netcat \
    && rm -rf /var/lib/apt/lists/*

# Set working directory
WORKDIR /app

# Copy project files into container
COPY . .

# Ensure required folders exist for your server
RUN mkdir -p data public

# Build the C++ server (optimized with pthread support)
RUN g++ -std=c++17 -O2 -pthread -o server server.cpp

# Render injects PORT automatically (we’ll map to 8080 in server.cpp if unset)
EXPOSE 8080

# Run the server
CMD ["./server"]
