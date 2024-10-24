# PGS: C++ HTTP Static File Server

PGS (Pp3ng's Server) is a C++-based, multi-threaded HTTP static file server designed for efficient performance. It uses epoll for I/O multiplexing and a thread pool for handling requests concurrently. PGS supports features like MIME type detection, directory indexing, and JSON-based configuration. It's lightweight design and focus on simplicity.

## Architecture Overview

The server is built with a modular architecture, separating concerns into distinct classes for better maintainability and extensibility.

![Server Architecture](diagram/server-architecture.png)

### Core Components

1. **Server**: The main orchestrator that initializes and coordinates all components.
2. **Socket**: Handles low-level network operations.
3. **Router**: Manages request routing and static file serving.
4. **ThreadPool**: Manages worker threads for concurrent request processing.
5. **HTTP** Request/Response: Data structures for handling HTTP requests and responses.
6. **Logger**: Simple logging utility for human readable output.

### Utility Components

1. **Http**: Static utility class for HTTP protocol operations.
2. **Parser**: Configuration file parser using nlohmann/json.
3. **Config**: Structure for storing server configuration.

## Features

- ✨ Multi-threaded request handling
- 📁 Static file serving
- ⚡ Epoll-based I/O multiplexing
- 🔧 JSON-based configuration
- 🎯 MIME type detection
- 🚀 Thread pool for efficient concurrency
- 📌 Directory index support (serves index.html by default)

## Prerequisites

- C++17 or higher
- nlohmann/json library
- Linux environment (uses epoll)

## Installation

```bash
# Clone the repository
git clone https://github.com/Pp3ng/pgs.git

# Create build directory
cd pgs
make pgs
```

## Configuration

Create a `config.json` file in the server's root directory:

```json
{
  "port": 8080,
  "static_folder": "./static",
  "thread_count": 4
}
```

### Configuration Parameters

- `port`: Server listening port
- `static_folder`: Directory containing static files to serve
- `thread_count`: Number of worker threads in thread pool

## Usage

```bash
# Start the server
./pgs
```

The server will:

1. Log all actions to the console and log file
2. Load configuration from config.json
3. Initialize the thread pool
4. Start listening on the configured port
5. Serve static files from the configured directory

## Design Decisions

### ThreadPool Implementation

- Thread-safe queue for task management
- Condition variables for thread synchronization

### Socket Handling

- Non-blocking sockets with epoll for I/O multiplexing
- SO_REUSEADDR and SO_REUSEPORT options enabled
- IPv4 support

### Router Features

- Automatic MIME type detection
- Directory index support
- 404 handling for non-existent files

### Supported MIME Types

- HTML (.html)
- CSS (.css)
- JavaScript (.js)
- Images (.png, .jpg, .jpeg, .gif)
- Plain text (default)

## Error Handling

- Socket creation and binding failures
- Configuration file parsing errors
- File read/write errors
- Invalid request handling

## Performance Considerations

1. **Epoll Usage**

   - Event-driven I/O for better scalability
   - Efficient handling of multiple connections

2. **Thread Pool**

   - Prevents thread explosion
   - Reuses threads for better performance
   - Configurable thread count

3. **Static File Serving**
   - Efficient file reading
   - MIME type caching
   - Directory traversal prevention

### Response Codes

- 200: Successful file retrieval
- 404: File not found
- 500: Server error

## Safety and Security

- Directory traversal prevention
- Basic error handling
- Resource cleanup on shutdown

## Known Limitations

1. Only supports GET requests
2. No SSL/TLS support
3. Limited to static file serving
4. Linux-specific (uses epoll)

## Future Improvements

- [ ] Add SSL/TLS support
- [ ] Implement caching mechanisms
- [ ] Add support for dynamic content
- [ ] Cross-platform compatibility
- [ ] Configuration hot-reloading
- [ ] Better error reporting
- [ ] Performance metrics
