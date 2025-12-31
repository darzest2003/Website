# =========================
# 1️⃣ Build Stage
# =========================
FROM ubuntu:22.04 AS builder

# Install build tools and PostgreSQL client dev library
RUN apt-get update && apt-get install -y \
    g++ cmake make git libpq-dev \
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

# Install runtime dependencies including PostgreSQL client
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

# Expose your application port
EXPOSE 8080

# Environment variables
ENV PORT=8080
ENV MAX_WORKERS=4
ENV DATA_DIR=/var/data

# Start the server
ENTRYPOINT ["./server"]
