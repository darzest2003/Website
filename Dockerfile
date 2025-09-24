# Use a small official image with g++
FROM debian:bullseye-slim AS build

# Install compiler & tools
RUN apt-get update && apt-get install -y --no-install-recommends \
    g++ make && rm -rf /var/lib/apt/lists/*

# Set workdir
WORKDIR /app

# Copy only the source file
COPY server.cpp .

# Build the C++ server
RUN g++ -std=c++17 -O2 -pthread -o server server.cpp

# ================= Runtime Stage =================
FROM debian:bullseye-slim

WORKDIR /app

# Copy compiled binary
COPY --from=build /app/server .

# Create data & public dirs (to avoid missing paths)
RUN mkdir -p data public

# Expose port (Render/Heroku will inject $PORT)
EXPOSE 8080

# Run the server
CMD ["./server"]
