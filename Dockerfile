# Use the official GCC image to build the C++ project
FROM gcc:latest

# Set working directory inside the container
WORKDIR /app

# Copy all project files into the container
COPY . .

# Compile the C++ server (make sure server.cpp is present)
RUN g++ -std=c++17 -o server server.cpp

# Expose port (Railway/Render will map to $PORT automatically)
EXPOSE $PORT

# Run the compiled server binary
CMD ["./server"]