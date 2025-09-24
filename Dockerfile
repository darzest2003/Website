# Use GCC with C++17 support
FROM gcc:12

# Set working directory inside container
WORKDIR /app

# Copy all project files
COPY . .

# Compile the C++ server
RUN g++ -std=c++17 -O2 -pthread -o server server.cpp

# Expose a default port (Render injects $PORT at runtime)
EXPOSE 8080

# Run the server (it reads $PORT automatically)
CMD ["./server"]
