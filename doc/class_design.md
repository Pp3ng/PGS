# PGS Architecture Class Diagram

## Overview

This document provides a detailed description of the classes in the PGS architecture, as represented in the class diagram. The architecture is organized into several packages, each serving a specific purpose within the system.

## Packages

### 1. Configuration

This package contains classes that define the configuration structure for the server.

#### Config

- **Description**: Plain configuration structure initialized from `pgs_conf.json`.
- **Attributes**:
  - `port: int`: The port for the server.
  - `staticFolder: std::string`: The path for static files.
  - `threadCount: int`: The number of worker threads.
  - `rateLimit: RateLimitConfig`: Configuration for rate limiting.
    - `maxRequests: int`: Maximum number of requests allowed within the time window.
    - `timeWindow: int`: Duration of the time window for rate limiting.
  - `cache: CacheConfig`: Configuration for caching.
    - `sizeMB: size_t`: Maximum size of the cache in MB.
    - `maxAgeSeconds: int`: Maximum age of cache entries in seconds.

---

### 2. Data Structures

This package contains classes that define various data structures used in the server.

#### ConnectionInfo

- **Description**: Tracks individual client connection metrics and maintains connection-specific log buffers.
- **Attributes**:
  - `startTime: std::chrono::steady_clock::time_point`: The time when the connection started.
  - `ip: std::string`: The IP address of the client.
  - `isLogged: bool`: Indicates if the connection has been logged.
  - `isClosureLogged: bool`: Indicates if the closure has been logged.
  - `bytesReceived: uint64_t`: Total bytes received from the client.
  - `bytesSent: uint64_t`: Total bytes sent to the client.
- **Constructor**:
  - `ConnectionInfo(const std::chrono::steady_clock::time_point &time, const std::string &ipAddr, bool logged = false, bool closureLogged = false, uint64_t received = 0, uint64_t sent = 0)`: Initializes a new instance with the provided parameters.

---

### 3. Core Components

This package contains the main components of the server.

#### Server

- **Description**: Central server component that orchestrates all other components and manages the lifecycle of client connections.
- **Attributes**:
  - `socket: Socket`: The server socket.
  - `router: Router`: The router instance.
  - `pool: ThreadPool`: The thread pool for handling requests.
  - `epoll: EpollWrapper`: Epoll instance for managing events.
  - `rateLimiter: RateLimiter`: Rate limiter for managing request rates.
  - `cache: Cache`: Cache for storing content.
  - `connectionsMutex: std::mutex`: Mutex to protect the connections map.
  - `connections: std::map<int, ConnectionInfo>`: Map to store connection information.
  - `shouldStop: std::atomic<bool>`: Flag to indicate if the server should stop.
  - `cacheCleanupThread: std::thread`: Thread for cleaning up the cache.
  - `CACHE_CLEANUP_INTERVAL: std::chrono::minutes`: Interval for cache cleanup (5 minutes).
- **Methods**:
  - `Server(int port, const std::string &staticFolder, int threadCount, int maxRequests, int timeWindow, int cacheSizeMB, int maxAgeSeconds)`: Constructor to initialize the server with the specified parameters.
  - `void start()`: Starts the server.
  - `void stop()`: Stops the server.
  - `void handleClient(int client_socket, const std::string &clientIp)`: Handles communication with a connected client.
  - `void closeConnection(int client_socket)`: Closes the specified client connection.
  - `void startCacheCleanup()`: Starts the cache cleanup thread.

#### Socket

- **Description**: RAII wrapper for TCP socket operations with dual-stack (IPv4/IPv6) support and connection management.
- **Attributes**:
  - `server_fd: int`: File descriptor for the server socket.
  - `port: int`: The port number for the socket.
- **Methods**:
  - `Socket(int port)`: Constructor to create a socket on the specified port.
  - `~Socket()`: Destructor to close the socket.
  - `void bind()`: Binds the socket to the specified port.
  - `void listen()`: Listens for incoming connections.
  - `void closeSocket()`: Closes the socket.
  - `int acceptConnection(std::string &clientIp)`: Accepts a new connection and retrieves the client's IP address.
  - `int getSocketFd() const`: Returns the socket file descriptor.
  - `static std::string durationToString(const std::chrono::steady_clock::duration &duration)`: Converts a duration to a string representation.

#### Router

- **Description**: Handles routing of HTTP requests to appropriate static files with MIME type detection and 404 handling.
- **Attributes**:
  - `staticFolder: std::string`: The folder for static files.
- **Methods**:
  - `Router(const std::string &staticFolder)`: Constructor to initialize the router with the specified static folder.
  - `void route(const std::string &path, int client_socket, const std::string &clientIp, Middleware *middleware, Cache *cache, const std::string &request)`: Routes the request to the appropriate handler.
  - `std::string getMimeType(const std::string &path)`: Retrieves the MIME type based on the file extension.

#### ThreadPool

- **Description**: High-performance thread pool with various optimizations.
- **Attributes**:
  - `thread_data: std::vector<std::unique_ptr<ThreadData>>`: Thread-local data for each worker thread.
  - `workers: std::vector<std::thread>`: Vector of worker threads.
  - `global_queue: LockFreeQueue<std::function<void()>>`: Global queue for tasks.
- **Methods**:
  - `ThreadPool(size_t numThreads)`: Constructor to create a thread pool with the specified number of threads.
  - `~ThreadPool()`: Destructor to clean up resources.
  - `template<typename F, typename... Args> std::future<std::invoke_result_t<F, Args...>> enqueue(F&& f, Args&&... args)`: Enqueues a task for execution.
  - `void stop()`: Stops the thread pool and joins all threads.
  - `size_t get_active_threads() const`: Returns the number of active threads.
  - `size_t get_thread_tasks_processed(size_t) const`: Returns the number of tasks processed by a specific thread.

#### Http

- **Description**: Handles HTTP request processing and response generation.
- **Attributes**:
  - `BUFFER_SIZE: size_t`: Buffer size for I/O operations.
  - `ALIGNMENT: size_t`: Alignment for memory operations.
  - `SENDFILE_CHUNK: size_t`: Chunk size for sending files.
  - `MAX_IOV: int`: Maximum number of I/O vectors.
- **Methods**:
  - `static std::string getRequestPath(const std::string &request)`: Extracts the request path from the HTTP request.
  - `static void sendResponse(int client_socket, const std::string &content, const std::string &mimeType, int statusCode, const std::string &clientIp, bool isIndex = false, Middleware *middleware = nullptr, Cache *cache = nullptr, const std::string &request = "")`: Sends an HTTP response to the client.
  - `static bool isAssetRequest(const std::string &path)`: Checks if the request is for an asset (e.g., CSS, JS).
  - `static bool handleClientCache(int client_socket, const std::string &request, time_t lastModified, const std::string &mimeType, const std::string &filePath, const std::string &clientIp, std::chrono::steady_clock::time_point startTime)`: Handles client-side caching.

#### Logger

- **Description**: Modern thread-safe singleton logger with memory pool and async logging.
- **Attributes**:
  - `messageQueue: pmr::deque<LogMessage>`: Queue for log messages.
- **Methods**:
  - `static Logger* getInstance()`: Returns the singleton instance of the logger.
  - `void log(const std::string &message, LogLevel level = LogLevel::INFO, const std::string &ip = "-")`: Logs a message with the specified log level and IP address.
  - `void error(const std::string &message, const std::string &ip = "-")`: Logs an error message.
  - `void warning(const std::string &message, const std::string &ip = "-")`: Logs a warning message.
  - `void info(const std::string &message, const std::string &ip = "-")`: Logs an informational message.

#### EpollWrapper

- **Description**: RAII wrapper for Linux epoll API with optimized inline operations.
- **Attributes**:
  - `epoll_fd: int`: File descriptor for the epoll instance.
- **Methods**:
  - `EpollWrapper()`: Constructor to create an epoll instance.
  - `int get() const`: Returns the epoll file descriptor.
  - `int wait(epoll_event* events, int maxEvents, int timeout)`: Waits for events on the epoll instance.
  - `bool add(int fd, uint32_t events)`: Adds a file descriptor to the epoll instance.
  - `bool modify(int fd, uint32_t events)`: Modifies the events for a file descriptor.
  - `bool remove(int fd)`: Removes a file descriptor from the epoll instance.

---

### 4. Utility Classes

This package contains utility classes that provide helper functions and formatting.

#### Middleware

- **Description**: Abstract class for middleware components.
- **Methods**:
  - `virtual ~Middleware() = default`: Virtual destructor for proper cleanup in derived classes.
  - `virtual std::string process(const std::string &data) = 0`: Processes the input data and returns the processed result.

#### SocketOptionGuard

- **Description**: Manages socket options using RAII.
- **Attributes**:
  - `int &cork`: Reference to the cork option.
  - `int client_socket`: The client socket file descriptor.
- **Methods**:
  - `SocketOptionGuard(int &c, int cs)`: Constructor to initialize the guard with the cork option and client socket.
  - `~SocketOptionGuard()`: Destructor that resets the cork option.

#### FileGuard

- **Description**: RAII wrapper for file descriptors.
- **Attributes**:
  - `int fd`: File descriptor.
- **Methods**:
  - `FileGuard()`: Default constructor that initializes the file descriptor to -1.
  - `explicit FileGuard(int f)`: Constructor that initializes the file descriptor with the provided value.
  - `~FileGuard()`: Destructor that closes the file descriptor if it is valid.
  - `int get() const`: Returns the file descriptor.
  - `void reset(int f = -1)`: Resets the file descriptor, closing the previous one if necessary.

#### MMapGuard

- **Description**: RAII wrapper for memory-mapped files.
- **Attributes**:
  - `void *addr`: Address of the mapped memory.
  - `size_t length`: Length of the mapped memory.
- **Methods**:
  - `MMapGuard(void *a, size_t l)`: Constructor to initialize the guard with the address and length.
  - `~MMapGuard()`: Destructor that unmaps the memory if the address is valid.

#### Compression

- **Description**: Middleware for compressing response data.
- **Attributes**:
  - `static const std::unordered_set<std::string> nonCompressibleTypes`: Set of MIME types that should not be compressed.
  - `static const std::unordered_set<std::string> compressibleTypes`: Set of MIME types that can be compressed.
  - `static constexpr size_t COMPRESSION_BUFFER_SIZE = 32768`: Buffer size for compression (32KB).
  - `static constexpr size_t MIN_COMPRESSION_SIZE = 1024`: Minimum size for compression (1KB).
- **Methods**:
  - `static bool shouldCompress(const std::string &mimeType, size_t contentLength)`: Checks if content should be compressed based on MIME type and length.
  - `std::string process(const std::string &data) override`: Processes and compresses the input data.

#### Parser

- **Description**: Parses configuration files.
- **Methods**:
  - `static Parser* getInstance()`: Returns the singleton instance of the parser.
  - `Config parseConfig(const std::string &configFilePath)`: Parses the configuration file and returns a Config object.

---

## Relationships

- **Server** manages **Socket**, **Router**, **ThreadPool**, **EpollWrapper**, **RateLimiter**, and **Cache**.
- **Config** contains **RateLimitConfig** and **CacheConfig**.
- **Parser** creates **Config**.
- **Http** processes requests with **SocketSettings**, **SocketOptionGuard**, **FileGuard**, and **MMapGuard**.
- **Router** uses **Http** and **Middleware**.
- **Logger** contains **LogMessage** and formats logs with **TerminalUtils**.

This architecture provides a robust framework for handling HTTP requests efficiently while maintaining a clean separation of concerns among different components.
