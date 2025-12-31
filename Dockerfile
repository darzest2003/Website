# =========================
# 1️⃣ Build Stage
# =========================
FROM ubuntu:22.04 AS builder

RUN apt-get update && apt-get install -y \
    g++ cmake make git libpq-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY server.cpp .
RUN g++ -std=c++17 -O3 -pthread server.cpp -o server -lpq \
    && strip server


# =========================
# 2️⃣ Runtime Stage
# =========================
FROM ubuntu:22.04

RUN apt-get update && apt-get install -y \
    ca-certificates libpq5 \
    && rm -rf /var/lib/apt/lists/*
RUN useradd -m appuser

WORKDIR /app
COPY --from=builder /app/server .

# 🔑 Render persistent disk path
RUN mkdir -p /var/data && chown -R appuser:appuser /var/data

USER appuser

EXPOSE 8080

ENV PORT=8080
ENV MAX_WORKERS=4
ENV DATA_DIR=/var/data

ENTRYPOINT ["./server"]
