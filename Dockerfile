# =========================
# 1️⃣ Build Stage
# =========================
FROM ubuntu:22.04 AS builder

# Install required packages for building C++ code
RUN apt-get update && apt-get install -y \
    g++ cmake make git \
    && rm -rf /var/lib/apt/lists/*

# Create working directory
WORKDIR /app

# Copy source code
COPY server.cpp .

# Compile the server (static build for portability)
RUN g++ -std=c++17 -O3 -pthread server.cpp -o server \
    && strip server

# =========================
# 2️⃣ Runtime Stage
# =========================
FROM ubuntu:22.04

# Minimal runtime dependencies
RUN apt-get update && apt-get install -y \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Create non-root user for safety
RUN useradd -m appuser
USER appuser

WORKDIR /app

# Copy compiled binary from builder
COPY --from=builder /app/server .

# Copy public assets (if any)
COPY public ./public

# Ensure data directory exists
RUN mkdir -p data

# Expose default port
EXPOSE 8080

# Environment variables (can override at runtime)
ENV PORT=8080
ENV MAX_WORKERS=4
ENV DATA_DIR=data

# Run the server
ENTRYPOINT ["./server"]
