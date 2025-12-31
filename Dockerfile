# =========================
# 1️⃣ Build Stage
# =========================
FROM ubuntu:22.04 AS builder

# Install build tools and PostgreSQL dev library
RUN apt-get update && apt-get install -y \
    g++ make git libpq-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY server.cpp .

# Compile your server with PostgreSQL library
RUN g++ -std=c++17 -O3 -pthread server.cpp -o server -lpq \
    && strip server

# =========================
# 2️⃣ Runtime Stage
# =========================
FROM ubuntu:22.04

# Install runtime dependencies
RUN apt-get update && apt-get install -y \
    ca-certificates libpq5 \
    && rm -rf /var/lib/apt/lists/*

# Create non-root user
RUN useradd -m appuser

WORKDIR /app
COPY --from=builder /app/server .

# Persistent disk directory
RUN mkdir -p /var/data && chown -R appuser:appuser /var/data

USER appuser

# Expose server port
EXPOSE 8080

# Environment variables for Render + server
ENV PORT=8080
ENV MAX_WORKERS=4
ENV DATA_DIR=/var/data

# PostgreSQL connection (set as envs so server can read them if needed)
ENV DB_HOST=dpg-d5ajkvu3jp1c73cm3le0-a
ENV DB_PORT=5432
ENV DB_NAME=websitedb_1jmq
ENV DB_USER=websitedb_1jmq_user
ENV DB_PASS=Bo0MkOa1e49wugCV7af2aOvvJE1TFOWn

# Start the server
ENTRYPOINT ["./server"]
