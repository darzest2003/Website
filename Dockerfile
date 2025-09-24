# Use GCC with C++17 support
FROM gcc:12

# Set working directory
WORKDIR /app

# Copy source code and public assets
COPY server.cpp .
COPY public ./public

# Build server (compile inside image)
RUN g++ -std=c++17 -O2 -pthread -o server server.cpp

# Create data directory
RUN mkdir -p data

# Expose Render's runtime port
EXPOSE 8080

# Render injects $PORT, so we pass it to the server
CMD ["sh", "-c", "./server"]
