# Use the official GCC image
FROM gcc:latest

# Set working directory inside container
WORKDIR /app

# Copy project files
COPY . .

# Ensure required folders exist
RUN mkdir -p data public

# Compile the C++ server (adjust filename!)
RUN g++ -std=c++17 -o server server.cpp

# Expose port (Render uses $PORT)
EXPOSE 8080

# Run the server
CMD ["./server"]
