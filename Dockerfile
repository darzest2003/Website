# Use official GCC image to build
FROM gcc:latest

# Set working directory
WORKDIR /app

# Copy project files
COPY . .

# Compile your server
RUN g++ -std=c++17 -O2 -pthread -o server server.cpp

# Expose a default port (Render injects $PORT at runtime)
EXPOSE 8080

# Run the server (it will use $PORT if set, otherwise 8080)
CMD ["./server"]
