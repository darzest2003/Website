# Use Debian-based image with build tools
FROM debian:bullseye-slim AS build

# Install g++ and other dependencies
RUN apt-get update && apt-get install -y --no-install-recommends \
    g++ make ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Set work directory
WORKDIR /app

# Copy server code
COPY server.cpp .

# Compile the server
RUN g++ -std=c++17 -O2 -pthread server.cpp -o server

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
