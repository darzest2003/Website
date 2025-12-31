# =========================
# 1️⃣ Build Stage
# =========================
FROM ubuntu:22.04 AS builder

# Install build tools and SQLite development library
RUN apt-get update && apt-get install -y \
    g++ cmake make git libsqlite3-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY server.cpp .

# Compile server with SQLite
RUN g++ -std=c++17 -O3 -pthread server.cpp -o server -lsqlite3 \
    && strip server

# =========================
# 2️⃣ Runtime Stage
# =========================
FROM ubuntu:22.04

# Install runtime dependencies including SQLite
RUN apt-get update && apt-get install -y \
    ca-certificates sqlite3 \
    && rm -rf /var/lib/apt/lists/*

# Create non-root user
RUN useradd -m appuser

WORKDIR /app
COPY --from=builder /app/server .

# Persistent disk directory for Render
RUN mkdir -p /var/data && chown -R appuser:appuser /var/data

USER appuser

# Expose server port
EXPOSE 8080

# Environment variables for server
ENV PORT=8080
ENV MAX_WORKERS=4
ENV DATA_DIR=/var/data

# Start the server
ENTRYPOINT ["./server"]
