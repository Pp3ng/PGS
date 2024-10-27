/*
 * File: pgs.cpp
 * Author:Z.LuoPeng
 * Date: 2024/10/~~~~~~
 * Description: A static file server with middlewares, rate limiting, and caching
 *
 * 42 is the ultimate answer to life, the universe, and everything!!
 */

#include <iostream>           // std::cout, std::cerr
#include <cstring>            // strlen()
#include <sys/socket.h>       // socket(), bind(), listen(), accept()
#include <arpa/inet.h>        // inet_ntoa
#include <netinet/tcp.h>      // TCP_KEEPIDLE, TCP_KEEPINTVL, TCP_KEEPCNT
#include <netinet/in.h>       // sockaddr_in
#include <unistd.h>           // close() function
#include <sys/epoll.h>        // Epoll model
#include <thread>             // std::thread
#include <vector>             // std::vector
#include <queue>              // std::queue
#include <mutex>              // std::mutex
#include <condition_variable> // std::condition_variable
#include <functional>         // std::function
#include <fstream>            // File reading
#include <sstream>            // String stream
#include <filesystem>         // Filesystem
#include <ctime>              // Timestamp
#include <fcntl.h>            // File control
#include <nlohmann/json.hpp>  // Parse JSON
#include <map>                // For storing connection info
#include <deque>              // For rate limiting
#include <unordered_map>      // For rate limiting
#include <unordered_set>      // For asset requests
#include <chrono>             // For time measurement
#include <shared_mutex>       // Shared mutex
#include <csignal>            // Signal handling
#include <atomic>             // Atomic bool flag
#include <zlib.h>             // zlib compression
#include <algorithm>          // std::transform
#include <stdexcept>          // std::runtime_error
#include <sys/sendfile.h>     // sendfile
#include <sys/stat.h>         // fstat
#include <sys/uio.h>          // writev
#include <sys/mman.h>         // mmap
#include "terminal_utils.h"

namespace fs = std::filesystem; // Alias for filesystem namespace
using json = nlohmann::json;    // Alias for JSON namespace

struct Config
{
    int port;                 // prot for server
    std::string staticFolder; // path for static files
    int threadCount;          // thread count of worker threads
    struct
    {
        int maxRequests; // Maximum number of requests allowed within the time window
        int timeWindow;  // Duration of the time window for rate limiting
    } rateLimit;
    struct
    {
        size_t sizeMB;     // Maximum size of the cache in MB
        int maxAgeSeconds; // Maximum age of cache entries in seconds
    } cache;
};

// Connection information structure
struct ConnectionInfo
{
    std::chrono::steady_clock::time_point startTime; // Connection start time
    std::string ip;                                  // Client IP address
    bool isLogged;                                   // Flag to track if connection is logged
    bool isClosureLogged;                            // Flag to track if connection closure is logged
    uint64_t bytesReceived;                          // Bytes received from client
    uint64_t bytesSent;                              // Bytes sent to client
    std::vector<std::string> logBuffer;              // Buffer for storing logs

    ConnectionInfo(const std::chrono::steady_clock::time_point &time,
                   const std::string &ipAddr,
                   bool logged = false,
                   bool closureLogged = false,
                   uint64_t received = 0,
                   uint64_t sent = 0)
        : startTime(time), ip(ipAddr), isLogged(logged),
          isClosureLogged(closureLogged), bytesReceived(received),
          bytesSent(sent) {}
};
class Logger
{
private:
    // Basic logger members
    std::mutex logMutex;   // Mutex to protect log file access
    std::ofstream logFile; // Log file stream
    static Logger *instance;
    bool isWaitingForEvents;                                                 // New flag to track event waiting state
    std::chrono::steady_clock::time_point lastEventWaitLog;                  // Track last event wait log time
    static constexpr auto EVENT_WAIT_LOG_INTERVAL = std::chrono::seconds(5); // Log interval for waiting events

    // Structure to hold log message data
    struct LogMessage
    {
        std::string message;                             // The actual log message
        std::string level;                               // Log level (INFO, ERROR, etc.)
        std::string ip;                                  // IP address associated with the log
        std::chrono::system_clock::time_point timestamp; // When the log was created

        LogMessage(const std::string &msg, const std::string &lvl, const std::string &clientIp)
            : message(msg), level(lvl), ip(clientIp),
              timestamp(std::chrono::system_clock::now()) {}
    };

    // async logging members
    std::queue<LogMessage> messageQueue; // queue to store pending log messages
    std::mutex queueMutex;               // mutex to protect message queue
    std::condition_variable queueCV;     // condition variable for queue synchronization
    std::thread loggerThread;            // background thread for processing logs
    std::atomic<bool> running{true};     // flag to control background thread

    Logger() : isWaitingForEvents(false)
    {
        logFile.open("pgs.log", std::ios::app); // Open log file in append mode
        // start the background logging thread
        loggerThread = std::thread(&Logger::processLogs, this);
        // note: We deliberately don't call info() here to avoid recursion
        writeLogMessage(LogMessage("Logger initialized", "INFO", "-"));
    }

    // process logs in background thread
    void processLogs()
    {
        while (running)
        {
            std::vector<LogMessage> messages; // batch of log messages to write
            {
                // wait for new messages or shutdown signal
                std::unique_lock<std::mutex> lock(queueMutex);
                queueCV.wait_for(lock, std::chrono::seconds(1),
                                 [this]
                                 { return !messageQueue.empty() || !running; });

                // batch process up to 100 messages at once for better performance
                while (!messageQueue.empty() && messages.size() < 100)
                {
                    messages.push_back(std::move(messageQueue.front()));
                    messageQueue.pop();
                }
            }

            // write batched messages to file
            if (!messages.empty())
            {
                std::lock_guard<std::mutex> lock(logMutex);
                for (const auto &msg : messages)
                {
                    writeLogMessage(msg);
                }
                logFile.flush();
            }
        }
    }

    // format and write a single log message
    void writeLogMessage(const LogMessage &msg)
    {
        std::string logMessage = formatLogMessage(msg);

        logFile << logMessage << std::endl;

        // Output to console with appropriate color
        if (msg.level == "ERROR")
        {
            std::cout << TerminalUtils::error(logMessage) << std::endl;
        }
        else if (msg.level == "WARNING")
        {
            std::cout << TerminalUtils::warning(logMessage) << std::endl;
        }
        else if (msg.level == "SUCCESS")
        {
            std::cout << TerminalUtils::success(logMessage) << std::endl;
        }
        else
        {
            std::cout << TerminalUtils::info(logMessage) << std::endl;
        }
    }

    // format timestamp and log message
    [[nodiscard]]
    std::string formatLogMessage(const LogMessage &msg)
    {
        auto time = std::chrono::system_clock::to_time_t(msg.timestamp);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      msg.timestamp.time_since_epoch()) %
                  1000;

        char timestamp[32]; // buffer for timestamp
        std::strftime(timestamp, sizeof(timestamp), "[%Y-%m-%d %H:%M:%S]",
                      std::localtime(&time)); // format timestamp

        return std::string(timestamp) + "." + std::to_string(ms.count()) +
               " [" + msg.level + "] [" + msg.ip + "] " + msg.message;
    }

public:
    // singleton pattern implementation
    static Logger *getInstance()
    {
        if (instance == nullptr)
        {
            instance = new Logger();
        }
        return instance;
    }

    static void destroyInstance() // clean up
    {
        delete instance;
        instance = nullptr;
    }

    // ensure proper cleanup in destructor
    ~Logger()
    {
        running = false;      // signal background thread to stop
        queueCV.notify_one(); // wake up background thread

        if (loggerThread.joinable())
        {
            loggerThread.join(); // wait for background thread to finish
        }

        if (logFile.is_open())
        {
            logFile.close();
        }
    }

    // main logging function
    void log(const std::string &message, const std::string &level = "INFO", const std::string &ip = "-")
    {
        // handle special case for "Waiting for events" messages
        if (message == "Waiting for events...")
        {
            std::lock_guard<std::mutex> lock(logMutex);
            auto now = std::chrono::steady_clock::now();

            if (!isWaitingForEvents ||
                (now - lastEventWaitLog) >= EVENT_WAIT_LOG_INTERVAL) // check if interval has passed
            {
                isWaitingForEvents = true;
                lastEventWaitLog = now;
            }
            else
            {
                return; // skip logging if we're already waiting and interval hasn't passed
            }
        }
        else
        {
            isWaitingForEvents = false; // reset the waiting flag
        }

        // queue the log message for async processing
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            messageQueue.emplace(message, level, ip);
        }
        queueCV.notify_one(); // notify the background thread
    }

    // convenience methods for different log levels
    void error(const std::string &message, const std::string &ip = "-")
    {
        log(message, "ERROR", ip);
    }

    void warning(const std::string &message, const std::string &ip = "-")
    {
        log(message, "WARNING", ip);
    }

    void success(const std::string &message, const std::string &ip = "-")
    {
        log(message, "SUCCESS", ip);
    }

    void info(const std::string &message, const std::string &ip = "-")
    {
        log(message, "INFO", ip);
    }
};

Logger *Logger::instance = nullptr; // initialize the static singleton instance

class Cache
{
private:
    // Structure to hold cache entry data
    struct CacheEntry
    {
        std::vector<char> data;                             // Actual content of the cached file
        std::string mimeType;                               // MIME type of the cached content
        time_t lastModified;                                // Last modification time of the file
        std::chrono::steady_clock::time_point lastAccessed; // Last access time of this cache entry
    };

    std::unordered_map<std::string, CacheEntry> cache; // Main cache storage
    mutable std::shared_mutex mutex;                   // Mutex for thread-safe operations
    size_t maxSize;                                    // Maximum size of the cache in bytes
    size_t currentSize;                                // Current size of the cache in bytes
    std::chrono::seconds maxAge;                       // Maximum age of cache entries

public:
    // Constructor
    explicit Cache(size_t maxSizeMB, std::chrono::seconds maxAge)
        : maxSize(static_cast<size_t>(maxSizeMB) * 1024 * 1024),
          currentSize(0),
          maxAge(maxAge)
    {
        // Check for cache size overflow
        if (maxSize / (1024 * 1024) != maxSizeMB)
        {
            throw std::overflow_error("Cache size overflow");
        }
    }

    // Retrieve an item from the cache
    bool get(const std::string &key, std::vector<char> &data, std::string &mimeType, time_t &lastModified)
    {
        std::shared_lock<std::shared_mutex> lock(mutex);
        auto it = cache.find(key);
        if (it != cache.end())
        {
            auto now = std::chrono::steady_clock::now();
            // Check if the cache entry has expired
            if (now - it->second.lastAccessed > maxAge)
            {
                return false; // Cache entry has expired
            }
            data = it->second.data;
            mimeType = it->second.mimeType;
            lastModified = it->second.lastModified;
            it->second.lastAccessed = now; // Update last access time
            return true;
        }
        return false; // Cache miss
    }

    // Add or update an item in the cache
    void set(const std::string &key, const std::vector<char> &data, const std::string &mimeType, time_t lastModified)
    {
        std::unique_lock<std::shared_mutex> lock(mutex);

        // If cache is full, remove oldest entries until there's enough space
        while (currentSize + data.size() > maxSize && !cache.empty())
        {
            auto oldest = std::min_element(cache.begin(), cache.end(),
                                           [](const auto &a, const auto &b)
                                           {
                                               return a.second.lastAccessed < b.second.lastAccessed;
                                           });
            currentSize -= oldest->second.data.size();
            cache.erase(oldest);
        }

        // If the new entry is too large, don't cache it
        if (data.size() > maxSize)
        {
            return;
        }

        // Add or update the cache entry
        auto &entry = cache[key];
        entry.data = data;
        entry.mimeType = mimeType;
        entry.lastModified = lastModified;
        entry.lastAccessed = std::chrono::steady_clock::now();
        currentSize += data.size();
    }

    // Clear all items from the cache
    void clear()
    {
        std::unique_lock<std::shared_mutex> lock(mutex);
        cache.clear();
        currentSize = 0;
    }

    // Get the current size of the cache in bytes
    size_t size() const
    {
        std::shared_lock<std::shared_mutex> lock(mutex);
        return currentSize;
    }

    // Get the number of items in the cache
    size_t count() const
    {
        std::shared_lock<std::shared_mutex> lock(mutex);
        return cache.size();
    }

    // Get the maximum age of cache entries
    std::chrono::seconds getMaxAge() const
    {
        return maxAge;
    }

    // Set a new maximum age for cache entries
    void setMaxAge(std::chrono::seconds newMaxAge)
    {
        std::unique_lock<std::shared_mutex> lock(mutex);
        maxAge = newMaxAge;
    }

    // Remove a specific item from the cache
    bool remove(const std::string &key)
    {
        std::unique_lock<std::shared_mutex> lock(mutex);
        auto it = cache.find(key);
        if (it != cache.end())
        {
            currentSize -= it->second.data.size();
            cache.erase(it);
            return true;
        }
        return false;
    }

    // Check if an item exists in the cache and is not expired
    bool exists(const std::string &key)
    {
        std::shared_lock<std::shared_mutex> lock(mutex);
        auto it = cache.find(key);
        if (it != cache.end())
        {
            auto now = std::chrono::steady_clock::now();
            return (now - it->second.lastAccessed <= maxAge);
        }
        return false;
    }

    // Get cache statistics
    struct CacheStats
    {
        size_t currentSize;
        size_t maxSize;
        size_t itemCount;
        std::chrono::seconds maxAge;
    };

    CacheStats getStats() const
    {
        std::shared_lock<std::shared_mutex> lock(mutex);
        return {currentSize, maxSize, cache.size(), maxAge};
    }
};

class Middleware
{
public:
    virtual ~Middleware() = default;
    virtual std::string process(const std::string &data) = 0;
};

class RateLimiter : public Middleware
{
public:
    // Constructor to initialize the maximum number of requests and the time window
    RateLimiter(size_t maxRequests, std::chrono::seconds timeWindow)
        : maxRequests(maxRequests), timeWindow(timeWindow) {}

    std::string process(const std::string &data) override // override the process method
    {
        std::lock_guard<std::mutex> lock(rateMutex); // lock the mutex to protect the clientRequests map

        auto now = std::chrono::steady_clock::now(); // get the current timestamp

        auto &timestamps = clientRequests[data]; // retrieve the reference to the list of timestamps

        while (!timestamps.empty() && now - timestamps.front() > timeWindow) // remove expired timestamps
        {
            timestamps.pop_front(); // Remove the oldest timestamp
        }

        // Check if the number of requests in the time window exceeds the allowed limit
        if (timestamps.size() >= maxRequests)
        {
            // If exceeded, return a 429 Too Many Requests response
            return "HTTP/1.1 429 Too Many Requests\r\n"
                   "Content-Type: text/plain\r\n"
                   "Content-Length: 19\r\n"
                   "\r\n"
                   "Too Many Requests";
        }

        timestamps.push_back(now); // add the current timestamp to the list

        return data; // return the original data if the rate limit is not exceeded
    }

private:
    size_t maxRequests;              // Maximum number of requests allowed within the time window
    std::chrono::seconds timeWindow; // Duration of the time window for rate limiting
    // Map to store timestamps of requests for each client, identified by their data
    std::unordered_map<std::string, std::deque<std::chrono::steady_clock::time_point>> clientRequests;
    std::mutex rateMutex; // Mutex to protect access to clientRequests
};

class Compression : public Middleware
{
public:
    [[nodiscard]]
    static bool shouldCompress(const std::string &mimeType, size_t contentLength)
    {
        // Check if it's an image type
        if (mimeType.find("image/") != std::string::npos)
        {
            return false; // Don't compress images
        }

        static const std::vector<std::string> compressibleTypes = {
            "text/", "application/javascript", "application/json",
            "application/xml", "application/x-yaml"};

        if (contentLength < 1024) // 1KB
        {
            return false;
        }

        return std::any_of(compressibleTypes.begin(), compressibleTypes.end(),
                           [&mimeType](const std::string &type)
                           {
                               return mimeType.find(type) != std::string::npos; // Check if the MIME type is compressible
                           });
    }

    [[nodiscard]]
    static bool clientAcceptsGzip(const std::string &request)
    {
        return request.find("Accept-Encoding: ") != std::string::npos &&
               request.find("gzip") != std::string::npos;
    }

    [[nodiscard]]
    std::string process(const std::string &data) override
    {
        std::string compressed = compressData(data);
        if (!compressed.empty()) // check if compression was successful
        {
            Logger::getInstance()->info("Compressed data: " + std::to_string(data.size()) + " -> " + std::to_string(compressed.size()));
            return compressed;
        }
        return data; // Ensure a string is returned in all cases
    }

private:
    [[nodiscard]]
    std::string compressData(const std::string &data)
    {
        z_stream zs;                // Create a z_stream object for compression
        memset(&zs, 0, sizeof(zs)); // Zero-initialize the z_stream structure

        // Initialize the zlib compression
        if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION,  // set the compression level
                         Z_DEFLATED,                  // use the deflate compression method
                         15 | 16,                     // 15 | 16 for gzip encoding
                         8,                           // set the window size
                         Z_DEFAULT_STRATEGY) != Z_OK) // use the default compression strategy
        {
            throw std::runtime_error("Failed to initialize zlib");
        }

        // Set the input data for compression
        zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data.data())); // input data
        zs.avail_in = data.size();                                               // size of input data

        int ret;                         // variable to hold the return status of compression
        const size_t bufferSize = 32768; // define the size of the output buffer (32KB)
        std::string compressed;          // string to hold the final compressed data
        char outbuffer[bufferSize];      // buffer for compressed output

        // compress the data in a loop until all data is processed
        do
        {
            zs.next_out = reinterpret_cast<Bytef *>(outbuffer); // output buffer for compressed data
            zs.avail_out = bufferSize;                          // size of the output buffer

            ret = deflate(&zs, Z_FINISH); // perform the compression operation

            if (compressed.size() < zs.total_out) // append the compressed data to the output string
            {
                compressed.append(outbuffer, zs.total_out - compressed.size());
            }
        } while (ret == Z_OK); // continue until no more data to compress

        // check if compression completed successfully
        if (ret != Z_STREAM_END)
        {
            deflateEnd(&zs); // clean up the z_stream object
            throw std::runtime_error("Failed to compress data");
        }

        deflateEnd(&zs);   // clean up and free resources allocated by zlib
        return compressed; // return the compressed data
    }
};

class ThreadPool
{
public:
    ThreadPool(size_t numThreads);
    ~ThreadPool();
    void enqueue(std::function<void()> task);
    void stop();

private:
    std::vector<std::thread> workers;        // worker threads
    std::queue<std::function<void()>> tasks; // task queue
    std::mutex queueMutex;                   // mutex to protect task queue
    std::condition_variable condition;       // condition variable for task queue
    std::atomic<bool> stop_flag{false};      // flag to stop the worker threads

    void workerThread();
};

ThreadPool::ThreadPool(size_t numThreads)
{
    for (size_t i = 0; i < numThreads; ++i)
    {
        workers.emplace_back(&ThreadPool::workerThread, this); // create worker threads
    }
}

ThreadPool::~ThreadPool()
{
    stop();
}

void ThreadPool::stop()
{
    // lock scope
    {
        std::unique_lock<std::mutex> lock(queueMutex); // acquire lock to ensure exclusive access to queue and stop_flag

        stop_flag = true; // set stop flag to indicate that the thread pool should stop accepting tasks

        while (!tasks.empty()) // clear all pending tasks to prevent threads from picking up new work
            tasks.pop();
    }

    condition.notify_all(); // notify all threads to wake up and check the stop flag

    for (std::thread &worker : workers) // join all worker threads to ensure they have finished executing
    {
        if (worker.joinable())
            worker.join();
    }
}

void ThreadPool::enqueue(std::function<void()> task)
{
    // lock scope
    {
        std::unique_lock<std::mutex> lock(queueMutex); // acquire lock to ensure exclusive access to task queue and stop_flag

        if (stop_flag) // if stopping is initiated, ignore new tasks and return immediately
            return;

        tasks.push(std::move(task)); // move the task into the queue to avoid unnecessary copying
    }

    condition.notify_one(); // notify one thread that a new task is available
}

void ThreadPool::workerThread()
{
    while (true)
    {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(queueMutex); // acquire lock to ensure exclusive access to task queue and stop_flag
            condition.wait(lock, [this]
                           { return stop_flag || !tasks.empty(); }); // wait until stop flag is set or there are tasks in the queue
            if (stop_flag && tasks.empty())                          // if stopping is initiated and there are no tasks in the queue, exit the thread
                return;
            if (!tasks.empty()) // if there are tasks in the queue, pick up the next task
            {
                task = std::move(tasks.front());
                tasks.pop();
            }
        }
        if (task)
            task(); // execute the task outside the lock to avoid blocking other threads
    }
}

class Socket
{
public:
    Socket(int port);
    ~Socket();
    void bind();
    void listen();
    void closeSocket();
    int acceptConnection(std::string &clientIp);
    int getSocketFd() const;

    static std::string durationToString(const std::chrono::steady_clock::duration &duration)
    {
        auto seconds = std::chrono::duration_cast<std::chrono::seconds>(duration).count();
        auto minutes = seconds / 60;
        seconds %= 60;
        return std::to_string(minutes) + "m " + std::to_string(seconds) + "s";
    }

private:
    int server_fd; // Server socket file descriptor
    int port;      // Port number
};

Socket::Socket(int port) : port(port)
{
    Logger::getInstance()->info("Creating dual-stack socket on port: " + std::to_string(port));

    // Create a dual-stack socket
    server_fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (server_fd == -1)
    {
        Logger::getInstance()->error("Socket creation failed: " + std::string(strerror(errno)));
        throw std::runtime_error("Socket creation failed!");
    }

    int no = 0;
    if (setsockopt(server_fd, IPPROTO_IPV6, IPV6_V6ONLY, &no, sizeof(no)) < 0)
    {
        Logger::getInstance()->error("Failed to set IPV6_V6ONLY to 0: " + std::string(strerror(errno)));
        close(server_fd);
        throw std::runtime_error("Failed to configure dual-stack socket!");
    }
}

Socket::~Socket()
{
    Logger::getInstance()->info("Closing server socket");
    close(server_fd);
}

void Socket::bind()
{
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt)) < 0) // Set socket options
    {
        Logger::getInstance()->error("Failed to set socket options: " + std::string(strerror(errno)));
        throw std::runtime_error("Failed to set socket options");
    }

    struct sockaddr_in6 address;
    memset(&address, 0, sizeof(address));
    address.sin6_family = AF_INET6;  // Set address family to IPv6
    address.sin6_addr = in6addr_any; // Bind to all available interfaces
    address.sin6_port = htons(port); // Set port

    if (::bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) // Bind the socket to the address and port
    {
        std::string errorMsg = "Bind failed: " + std::string(strerror(errno));
        Logger::getInstance()->error(errorMsg);
        throw std::runtime_error(errorMsg);
    }

    Logger::getInstance()->success("Socket successfully bound to port " + std::to_string(port));
}

void Socket::listen()
{
    ::listen(server_fd, 42); // 42 is the ultimate answer to life, the universe, and everything
    Logger::getInstance()->info("Server listening on port " + std::to_string(port));
}
void Socket::closeSocket()
{
    if (server_fd != -1) // Check if the socket is valid
    {
        close(server_fd);
        server_fd = -1;
    }
}

[[nodiscard]]
int Socket::acceptConnection(std::string &clientIp)
{
    struct sockaddr_in6 address;
    socklen_t addrlen = sizeof(address);
    int new_socket = accept(server_fd, (struct sockaddr *)&address, &addrlen); // Accept a new connection

    if (new_socket >= 0)
    {
        char ipstr[INET6_ADDRSTRLEN];
        if (address.sin6_family == AF_INET6)
        {
            inet_ntop(AF_INET6, &address.sin6_addr, ipstr, sizeof(ipstr)); // Convert IPv6 address to string
        }
        clientIp = ipstr;
        Logger::getInstance()->success("New connection accepted from " + clientIp);

        int flags = fcntl(new_socket, F_GETFL, 0);
        if (flags == -1)
        {
            Logger::getInstance()->error("Failed to get socket flags");
            close(new_socket);
            return -1;
        }
        if (fcntl(new_socket, F_SETFL, flags | O_NONBLOCK) == -1)
        {
            Logger::getInstance()->error("Failed to set socket to non-blocking mode");
            close(new_socket);
            return -1;
        }
    }
    else
    {
        clientIp = "-";
        Logger::getInstance()->error("Failed to accept connection");
    }
    return new_socket;
}

[[nodiscard]]
int Socket::getSocketFd() const
{
    return server_fd;
}

class Http
{
public:
    static std::string getRequestPath(const std::string &request);
    static void sendResponse(int client_socket, const std::string &content,
                             const std::string &mimeType, int statusCode,
                             const std::string &clientIp, bool isIndex = false,
                             Middleware *middleware = nullptr, Cache *cache = nullptr);
    static bool isAssetRequest(const std::string &path)
    {
        // undordered_set is faster than vector for lookups
        static const std::unordered_set<std::string> assetExtensions = {
            ".css", ".js", ".png", ".jpg", ".jpeg", ".gif", ".ico",
            ".svg", ".woff", ".woff2", ".ttf", ".eot", ".map",
            ".webp", ".pdf", ".mp4", ".webm", ".mp3", ".wav",
            ".json", ".xml"};

        static const std::unordered_set<std::string> assetDirs = {
            "/assets/", "/static/", "/images/", "/img/",
            "/css/", "/js/", "/fonts/", "/media/"};

        // Check if the path ends with an asset extension
        for (const auto &ext : assetExtensions)
        {
            if (path.length() >= ext.length() &&
                path.compare(path.length() - ext.length(), ext.length(), ext) == 0) // compare file extension
            {
                return true;
            }
        }

        // Check if the path contains an asset directory
        for (const auto &dir : assetDirs)
        {
            if (path.find(dir) != std::string::npos) // check if the path contains an asset directory
            {
                return true;
            }
        }

        return false;
    }
};

[[nodiscard]]
std::string Http::getRequestPath(const std::string &request)
{
    size_t pos1 = request.find("GET ");
    size_t pos2 = request.find(" HTTP/");
    if (pos1 == std::string::npos || pos2 == std::string::npos) // Check if the request is valid
    {
        return "/";
    }
    return request.substr(pos1 + 4, pos2 - (pos1 + 4));
}

void Http::sendResponse(int client_socket, const std::string &filePath,
                        const std::string &mimeType, int statusCode,
                        const std::string &clientIp, bool isIndex,
                        Middleware *middleware, Cache *cache)
{
    // constants for optimized I/O
    static const size_t BUFFER_SIZE = 65536;      // 64KB buffer size
    static const size_t ALIGNMENT = 512;          // memory alignment boundary
    static const size_t SENDFILE_CHUNK = 1048576; // 1MB sendfile chunk size
    static const int MAX_IOV = IOV_MAX;           // maximum iovec array size

    // set socket options for keep-alive and performance
    int keepAlive = 1;
    int keepIdle = 60;
    int keepInterval = 10;
    int keepCount = 3;
    int cork = 1; // enable TCP_CORK to optimize packet transmission

    auto setSocketOption = [&](int level, int optname, const void *optval, socklen_t optlen)
    {
        if (setsockopt(client_socket, level, optname, optval, optlen) < 0)
        {
            Logger::getInstance()->error("Failed to set socket option: " + std::string(strerror(errno)), clientIp);
            close(client_socket);
            return false;
        }
        return true;
    };

    // set socket options including TCP_CORK
    if (!setSocketOption(SOL_SOCKET, SO_KEEPALIVE, &keepAlive, sizeof(keepAlive)) ||
        !setSocketOption(IPPROTO_TCP, TCP_KEEPIDLE, &keepIdle, sizeof(keepIdle)) ||
        !setSocketOption(IPPROTO_TCP, TCP_KEEPINTVL, &keepInterval, sizeof(keepInterval)) ||
        !setSocketOption(IPPROTO_TCP, TCP_KEEPCNT, &keepCount, sizeof(keepCount)) ||
        !setSocketOption(IPPROTO_TCP, TCP_CORK, &cork, sizeof(cork)))
    {
        return;
    }

    std::vector<char> fileContent;
    size_t fileSize;
    time_t lastModified;
    bool cacheHit = false;

    // try to get content from cache
    if (cache && statusCode == 200)
    {
        std::string cachedMimeType;
        cacheHit = cache->get(filePath, fileContent, cachedMimeType, lastModified);
        if (cacheHit)
        {
            fileSize = fileContent.size();
            Logger::getInstance()->info("Cache hit for: " + filePath, clientIp);
        }
    }

    int fd = -1;
    if (!cacheHit)
    {
        // open file with O_DIRECT for large files to bypass system cache
        int flags = O_RDONLY;
        struct stat statBuf;
        if (stat(filePath.c_str(), &statBuf) == 0 && statBuf.st_size > 10 * 1024 * 1024) // 10MB threshold
        {
            flags |= O_DIRECT;
        }

        fd = open(filePath.c_str(), flags);
        if (fd == -1 && errno == EINVAL) // O_DIRECT not supported, fallback to normal open
        {
            fd = open(filePath.c_str(), O_RDONLY);
        }

        if (fd == -1)
        {
            Logger::getInstance()->error(
                "Failed to open file: errno=" + std::to_string(errno),
                clientIp);
            return;
        }

        // advise the kernel about access pattern for optimal I/O performance
        posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);

        struct stat file_stat;
        if (fstat(fd, &file_stat) == -1)
        {
            Logger::getInstance()->error(
                "Fstat failed: errno=" + std::to_string(errno),
                clientIp);
            close(fd);
            return;
        }

        fileSize = file_stat.st_size;
        lastModified = file_stat.st_mtime;
    }

    bool isCompressed = false;
    std::string compressedContent;

    // compress file content if needed
    if (middleware && Compression::shouldCompress(mimeType, fileSize) && mimeType.find("image/") == std::string::npos)
    {
        if (cacheHit)
        {
            compressedContent = middleware->process(std::string(fileContent.begin(), fileContent.end()));
        }
        else
        {
            // allocate aligned buffer for optimal I/O performance
            void *alignedBuffer;
            if (posix_memalign(&alignedBuffer, ALIGNMENT, BUFFER_SIZE) != 0)
            {
                Logger::getInstance()->error("Failed to allocate aligned memory", clientIp);
                close(fd);
                return;
            }

            std::vector<char> buffer;
            buffer.reserve(fileSize); // pre-allocate to avoid reallocation

            size_t totalRead = 0;
            while (totalRead < fileSize)
            {
                ssize_t bytesRead = read(fd, alignedBuffer, std::min(BUFFER_SIZE, fileSize - totalRead));
                if (bytesRead <= 0)
                {
                    free(alignedBuffer);
                    Logger::getInstance()->error(
                        "Read file failed: errno=" + std::to_string(errno),
                        clientIp);
                    close(fd);
                    return;
                }
                buffer.insert(buffer.end(), static_cast<char *>(alignedBuffer),
                              static_cast<char *>(alignedBuffer) + bytesRead);
                totalRead += bytesRead;
            }
            free(alignedBuffer);

            compressedContent = middleware->process(std::string(buffer.begin(), buffer.end()));
        }
        isCompressed = true;
        fileSize = compressedContent.size();
        if (fd != -1)
        {
            close(fd);
            fd = -1;
        }
    }

    // pre-allocate header string capacity
    std::string headerStr;
    headerStr.reserve(1024); // reserve reasonable space for headers

    // generate response headers
    char timeBuffer[128], lastModifiedBuffer[128];
    time_t now = time(nullptr);
    struct tm *tm_info = gmtime(&now);
    strftime(timeBuffer, sizeof(timeBuffer), "%a, %d %b %Y %H:%M:%S GMT", tm_info);

    tm_info = gmtime(&lastModified);
    strftime(lastModifiedBuffer, sizeof(lastModifiedBuffer), "%a, %d %b %Y %H:%M:%S GMT", tm_info);

    const char *statusMessage;
    switch (statusCode)
    {
    case 200:
        statusMessage = "OK";
        break;
    case 304:
        statusMessage = "Not Modified";
        break;
    case 400:
        statusMessage = "Bad Request";
        break;
    case 401:
        statusMessage = "Unauthorized";
        break;
    case 403:
        statusMessage = "Forbidden";
        break;
    case 404:
        statusMessage = "Not Found";
        break;
    case 500:
        statusMessage = "Internal Server Error";
        break;
    case 503:
        statusMessage = "Service Unavailable";
        break;
    default:
        statusMessage = "Unknown Status";
        break;
    }

    // assemble response headers
    std::ostringstream headers;
    headers << "HTTP/1.1 " << statusCode << " " << statusMessage << "\r\n"
            << "Server: RobustHTTP/1.0\r\n"
            << "Date: " << timeBuffer << "\r\n"
            << "Content-Type: " << mimeType << "\r\n"
            << "Content-Length: " << fileSize << "\r\n"
            << "Last-Modified: " << lastModifiedBuffer << "\r\n"
            << "Connection: keep-alive\r\n"
            << "Keep-Alive: timeout=60, max=1000\r\n"
            << "Accept-Ranges: bytes\r\n"
            << "Cache-Control: public, max-age=31536000\r\n"
            << "X-Content-Type-Options: nosniff\r\n"
            << "X-Frame-Options: SAMEORIGIN\r\n"
            << "X-XSS-Protection: 1; mode=block\r\n";

    if (isCompressed)
    {
        headers << "Content-Encoding: gzip\r\n"
                << "Vary: Accept-Encoding\r\n";
    }
    headers << "\r\n";

    headerStr = headers.str();

    // optimize writev using maximum allowed iovec structures
    struct iovec iov[MAX_IOV];
    int iovcnt = 0;

    // add header to iovec
    iov[iovcnt].iov_base = const_cast<char *>(headerStr.c_str());
    iov[iovcnt].iov_len = headerStr.size();
    iovcnt++;

    // add content to iovec if compressed or cached
    if (isCompressed)
    {
        iov[iovcnt].iov_base = const_cast<char *>(compressedContent.c_str());
        iov[iovcnt].iov_len = compressedContent.size();
        iovcnt++;
    }
    else if (cacheHit)
    {
        iov[iovcnt].iov_base = fileContent.data();
        iov[iovcnt].iov_len = fileContent.size();
        iovcnt++;
    }

    // send headers and content using writev
    size_t totalSent = 0;
    const size_t totalSize = headerStr.size() +
                             (isCompressed ? compressedContent.size() : (cacheHit ? fileContent.size() : 0));

    while (totalSent < totalSize)
    {
        ssize_t sent = writev(client_socket, iov, iovcnt);
        if (sent <= 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                usleep(1000);
                continue;
            }
            Logger::getInstance()->error(
                "Failed to send response: errno=" + std::to_string(errno),
                clientIp);
            if (fd != -1)
                close(fd);
            return;
        }
        totalSent += sent;

        // update iovec structures
        while (sent > 0 && iovcnt > 0)
        {
            if (static_cast<size_t>(sent) >= iov[0].iov_len) // check if entire iovec buffer was sent
            {
                sent -= iov[0].iov_len; // update remaining bytes to send
                iovcnt--;
                for (int i = 0; i < iovcnt; i++)
                {
                    iov[i] = iov[i + 1]; // shift iovec array
                }
            }
            else
            {
                iov[0].iov_base = static_cast<char *>(iov[0].iov_base) + sent; // update iovec base pointer
                iov[0].iov_len -= sent;                                        // update iovec length
                break;
            }
        }
    }

    // handle large file transfer using sendfile or mmap
    if (!isCompressed && !cacheHit && fd != -1)
    {
        off_t offset = 0;
        bool useMmap = false;

        // try sendfile with optimal chunk size
        while (offset < static_cast<off_t>(fileSize))
        {
            size_t chunk = std::min(SENDFILE_CHUNK, fileSize - offset);
            ssize_t sent = sendfile(client_socket, fd, &offset, chunk);

            if (sent == -1)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                {
                    usleep(1000);
                    continue;
                }
                else if (errno == EINVAL || errno == ENOSYS)
                {
                    useMmap = true;
                    break;
                }
                Logger::getInstance()->error(
                    "Failed to send file: errno=" + std::to_string(errno),
                    clientIp);
                close(fd);
                return;
            }
            else if (sent == 0)
            {
                break;
            }
            totalSent += sent;
        }

        // fall back to mmap for large files
        if (useMmap)
        {
            // use huge pages for large files
            int flags = MAP_PRIVATE;
            if (fileSize >= 2 * 1024 * 1024) // 2MB threshold for huge pages
            {
                flags |= MAP_HUGETLB;
            }

            void *mmapAddr = mmap(nullptr, fileSize, PROT_READ, flags, fd, 0);
            if (mmapAddr == MAP_FAILED)
            {
                // retry without huge pages if it failed
                if (flags & MAP_HUGETLB)
                {
                    mmapAddr = mmap(nullptr, fileSize, PROT_READ, MAP_PRIVATE, fd, 0);
                }
                if (mmapAddr == MAP_FAILED)
                {
                    Logger::getInstance()->error(
                        "Mmap failed: errno=" + std::to_string(errno),
                        clientIp);
                    close(fd);
                    return;
                }
            }

            // advise sequential access pattern
            madvise(mmapAddr, fileSize, MADV_SEQUENTIAL);

            const char *fileContent = static_cast<const char *>(mmapAddr);
            size_t remainingBytes = fileSize;
            size_t bytesSent = 0;

            while (remainingBytes > 0)
            {
                size_t chunk = std::min(BUFFER_SIZE, remainingBytes);
                ssize_t sent = send(client_socket, fileContent + bytesSent, chunk, MSG_NOSIGNAL);
                if (sent == -1)
                {
                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                    {
                        usleep(1000);
                        continue;
                    }
                    Logger::getInstance()->error(
                        "Failed to send mmap data: errno=" + std::to_string(errno),
                        clientIp);
                    munmap(mmapAddr, fileSize);
                    close(fd);
                    return;
                }
                bytesSent += sent;
                remainingBytes -= sent;
                totalSent += sent;
            }

            munmap(mmapAddr, fileSize);
        }

        // update cache if needed
        if (cache && statusCode == 200)
        {
            // allocate aligned buffer for cache update
            void *alignedBuffer;
            if (posix_memalign(&alignedBuffer, ALIGNMENT, BUFFER_SIZE) == 0)
            {
                std::vector<char> content;
                content.reserve(fileSize); // pre-allocate to avoid reallocation

                lseek(fd, 0, SEEK_SET);
                size_t totalRead = 0;
                while (totalRead < fileSize)
                {
                    ssize_t bytesRead = read(fd, alignedBuffer,
                                             std::min(BUFFER_SIZE, fileSize - totalRead)); // read file in chunks
                    if (bytesRead <= 0)
                        break;
                    content.insert(content.end(), static_cast<char *>(alignedBuffer),
                                   static_cast<char *>(alignedBuffer) + bytesRead); // append to content vector
                    totalRead += bytesRead;
                }
                free(alignedBuffer);

                if (totalRead == fileSize)
                {
                    cache->set(filePath, content, mimeType, lastModified); // update cache with file content
                }
            }
        }

        close(fd);
    }

    // Disable TCP_CORK to flush any remaining data
    cork = 0;
    setSocketOption(IPPROTO_TCP, TCP_CORK, &cork, sizeof(cork));

    if (isIndex)
    {
        Logger::getInstance()->info(
            "Response sent: status=" + std::to_string(statusCode) +
                ", path=" + filePath +
                ", size=" + std::to_string(fileSize) +
                ", type=" + mimeType +
                ", cache=" + (cacheHit ? "HIT" : "MISS"),
            clientIp);
    }
}

class Router
{
public:
    Router(const std::string &staticFolder) : staticFolder(staticFolder)
    {
        Logger::getInstance()->info("Router initialized with static folder: " + staticFolder);
    }
    void route(const std::string &path, int client_socket, const std::string &clientIp, Middleware *middleware, Cache *cache);
    [[nodiscard]]
    std::string getStaticFolder() const
    {
        return staticFolder;
    }

private:
    std::string staticFolder;                         // path to static files
    std::string getMimeType(const std::string &path); // get MIME type based on file extension
};

[[nodiscard]]
std::string Router::getMimeType(const std::string &path) // calculate MIME type based on file extension
{
    // Define a map of file extensions to MIME types
    static const std::unordered_map<std::string, std::string> mimeTypes = {
        {".html", "text/html"},
        {".css", "text/css"},
        {".js", "application/javascript"},
        {".png", "image/png"},
        {".jpg", "image/jpeg"},
        {".jpeg", "image/jpeg"},
        {".gif", "image/gif"}};

    std::string lowerPath = path;
    std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(), ::tolower); // convert path to lowercase
    // Use ends_with to find the appropriate MIME type
    for (const auto &entry : mimeTypes)
    {
        if (lowerPath.ends_with(entry.first)) // check if path ends with file extension
        {
            return entry.second; // return MIME type
        }
    }

    return "text/plain"; // Default MIME type
}

void Router::route(const std::string &path, int client_socket, const std::string &clientIp, Middleware *middleware, Cache *cache)
{
    std::string filePath = staticFolder + path;
    bool isIndex = (path == "/index.html" || path == "/" || fs::is_directory(filePath));
    bool isAsset = Http::isAssetRequest(path);

    if (fs::is_directory(filePath))
    {
        filePath += "/index.html";
    }

    if (!isAsset && isIndex)
    {
        Logger::getInstance()->info("Processing request: " + path, clientIp);
    }

    if (!fs::exists(filePath) || fs::is_directory(filePath))
    {
        if (!isAsset)
        {
            Logger::getInstance()->warning("File not found: " + filePath, clientIp);
        }

        // read 404.html
        std::ifstream file("404.html");
        if (!file)
        {
            // if 404.html not found, send a simple 404 response
            const char *response =
                "HTTP/1.1 404 Not Found\r\n"
                "Content-Type: text/plain\r\n"
                "Content-Length: 9\r\n"
                "\r\n"
                "Not Found";
            send(client_socket, response, strlen(response), 0);
            return;
        }

        // read 404.html content
        std::ostringstream buffer;
        buffer << file.rdbuf();
        std::string content = buffer.str();

        // assemble 404 response
        std::string response =
            "HTTP/1.1 404 Not Found\r\n"
            "Content-Type: text/html\r\n"
            "Content-Length: " +
            std::to_string(content.size()) + "\r\n"
                                             "Connection: close\r\n" // Close the connection after sending the response
                                             "\r\n" +
            content;

        // send 404 response
        send(client_socket, response.c_str(), response.size(), 0);
        return;
    }

    std::string mimeType = getMimeType(filePath);
    Http::sendResponse(client_socket, filePath, mimeType, 200, clientIp, !isAsset && isIndex, middleware, cache);
}

class Parser
{
public:
    static Config parseConfig(const std::string &configFilePath);
};

Config Parser::parseConfig(const std::string &configFilePath)
{
    // Log the start of configuration reading process
    Logger::getInstance()->info("Reading configuration from: " + configFilePath);

    // Open the configuration file
    std::ifstream file(configFilePath);
    if (!file.is_open())
    {
        Logger::getInstance()->error("Could not open config file: " + configFilePath);
        throw std::runtime_error("Could not open config file!");
    }

    // Parse the JSON configuration
    json configJson;
    try
    {
        file >> configJson;
    }
    catch (const json::parse_error &e)
    {
        Logger::getInstance()->error("JSON parsing error: " + std::string(e.what()));
        throw std::runtime_error("Failed to parse configuration file");
    }

    // Check if all required fields exist and are not null
    // This includes the new rate_limit fields and cache fields
    if (!configJson.contains("port") || configJson["port"].is_null() ||
        !configJson.contains("static_folder") || configJson["static_folder"].is_null() ||
        !configJson.contains("thread_count") || configJson["thread_count"].is_null() ||
        !configJson.contains("rate_limit") || configJson["rate_limit"].is_null() ||
        !configJson["rate_limit"].contains("max_requests") || configJson["rate_limit"]["max_requests"].is_null() ||
        !configJson["rate_limit"].contains("time_window") || configJson["rate_limit"]["time_window"].is_null() ||
        !configJson.contains("cache") || configJson["cache"].is_null() ||
        !configJson["cache"].contains("size_mb") || configJson["cache"]["size_mb"].is_null() ||
        !configJson["cache"].contains("max_age_seconds") || configJson["cache"]["max_age_seconds"].is_null())
    {
        Logger::getInstance()->error("Configuration file is missing required fields or contains null values");
        throw std::runtime_error("Incomplete configuration file");
    }

    // Create a Config object and populate it with values from the JSON
    Config config;
    config.port = configJson["port"];
    config.staticFolder = configJson["static_folder"];
    config.threadCount = configJson["thread_count"];
    config.rateLimit.maxRequests = configJson["rate_limit"]["max_requests"];
    config.rateLimit.timeWindow = configJson["rate_limit"]["time_window"];
    config.cache.sizeMB = configJson["cache"]["size_mb"].get<size_t>();
    config.cache.maxAgeSeconds = configJson["cache"]["max_age_seconds"].get<int>();

    // Validate the port number
    if (config.port <= 0 || config.port > 65535)
    {
        Logger::getInstance()->error("Invalid port number: " + std::to_string(config.port));
        throw std::runtime_error("Invalid port number");
    }

    // Check if the specified static folder exists
    if (!fs::exists(config.staticFolder))
    {
        Logger::getInstance()->error("Static folder does not exist: " + config.staticFolder);
        throw std::runtime_error("Invalid static folder path");
    }

    // Validate the thread count
    if (config.threadCount <= 0 || config.threadCount > 1000)
    {
        Logger::getInstance()->error("Invalid thread count: " + std::to_string(config.threadCount));
        throw std::runtime_error("Invalid thread count");
    }

    // Validate the maximum requests for rate limiting
    if (config.rateLimit.maxRequests <= 0)
    {
        Logger::getInstance()->error("Invalid max requests for rate limiting: " + std::to_string(config.rateLimit.maxRequests));
        throw std::runtime_error("Invalid max requests for rate limiting");
    }

    // Validate the time window for rate limiting
    if (config.rateLimit.timeWindow <= 0)
    {
        Logger::getInstance()->error("Invalid time window for rate limiting: " + std::to_string(config.rateLimit.timeWindow));
        throw std::runtime_error("Invalid time window for rate limiting");
    }

    // Validate cache size
    if (config.cache.sizeMB <= 0 || config.cache.sizeMB > std::numeric_limits<size_t>::max() / (1024 * 1024))
    {
        Logger::getInstance()->error("Invalid cache size: " + std::to_string(config.cache.sizeMB) + " MB");
        throw std::runtime_error("Invalid cache size");
    }

    // Validate cache max age
    if (config.cache.maxAgeSeconds <= 0)
    {
        Logger::getInstance()->error("Invalid cache max age: " + std::to_string(config.cache.maxAgeSeconds) + " seconds");
        throw std::runtime_error("Invalid cache max age");
    }

    // Log successful configuration loading
    Logger::getInstance()->success("Configuration loaded successfully");

    // Return the populated and validated Config object
    return config;
}

class EpollWrapper
{
public:
    EpollWrapper() : epoll_fd(epoll_create1(0))
    {
        if (epoll_fd == -1)
        {
            throw std::runtime_error("Failed to create epoll instance: " + std::string(strerror(errno)));
        }
    }

    ~EpollWrapper()
    {
        if (epoll_fd != -1)
        {
            close(epoll_fd);
        }
    }

    // forbid copy
    EpollWrapper(const EpollWrapper &) = delete;
    EpollWrapper &operator=(const EpollWrapper &) = delete;

    // allow move
    EpollWrapper(EpollWrapper &&other) noexcept : epoll_fd(other.epoll_fd)
    {
        other.epoll_fd = -1;
    }

    EpollWrapper &operator=(EpollWrapper &&other) noexcept
    {
        if (this != &other)
        {
            if (epoll_fd != -1)
            {
                close(epoll_fd);
            }
            epoll_fd = other.epoll_fd;
            other.epoll_fd = -1;
        }
        return *this;
    }

    int get() const { return epoll_fd; }

    int wait(struct epoll_event *events, int maxEvents, int timeout)
    {
        return epoll_wait(epoll_fd, events, maxEvents, timeout);
    }

    void add(int fd, uint32_t events)
    {
        struct epoll_event ev = {};
        ev.events = events;
        ev.data.fd = fd;
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) == -1)
        {
            throw std::runtime_error("Failed to add fd to epoll: " + std::string(strerror(errno)));
        }
    }

    void modify(int fd, uint32_t events)
    {
        struct epoll_event ev = {};
        ev.events = events;
        ev.data.fd = fd;
        if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev) == -1) // modify fd in epoll
        {
            throw std::runtime_error("Failed to modify fd in epoll: " + std::string(strerror(errno)));
        }
    }

    void remove(int fd)
    {
        if (epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr) == -1) // remove fd from epoll
        {
            throw std::runtime_error("Failed to remove fd from epoll: " + std::string(strerror(errno)));
        }
    }

private:
    int epoll_fd; // epoll file descriptor
};

class Server
{
public:
    Server(int port, const std::string &staticFolder, int threadCount, int maxRequests, int timeWindow, int cacheSizeMB, int maxAgeSeconds);
    void start();
    void stop();

private:
    Socket socket;                             // server socket
    Router router;                             // server router instance
    ThreadPool pool;                           // server thread pool
    EpollWrapper epoll;                        // server epoll instance
    RateLimiter rateLimiter;                   // server rate limiter
    Cache cache;                               // server cache
    std::mutex connectionsMutex;               // mutex to protect connections map
    std::map<int, ConnectionInfo> connections; // map to store connection info
    std::atomic<bool> shouldStop{false};       // atomic flag to stop the server

    void handleClient(int client_socket, const std::string &clientIp);
    void closeConnection(int client_socket);
    void logRequest(int client_socket, const std::string &message);
};

Server::Server(int port, const std::string &staticFolder, int threadCount, int maxRequests, int timeWindow, int cacheSizeMB, int maxAgeSeconds)
    : socket(port),
      router(staticFolder),
      pool(threadCount),
      epoll(),
      rateLimiter(maxRequests, std::chrono::seconds(timeWindow)),
      cache(cacheSizeMB, std::chrono::seconds(maxAgeSeconds))
{
    Logger::getInstance()->info("Server initialized with port: " + std::to_string(port) +
                                ", static folder: " + staticFolder +
                                ", thread count: " + std::to_string(threadCount) +
                                ", rate limit: " + std::to_string(maxRequests) +
                                " requests per " + std::to_string(timeWindow) + " seconds" +
                                ", cache size: " + std::to_string(cacheSizeMB) + "MB" +
                                ", cache max age: " + std::to_string(maxAgeSeconds) + " seconds");
    socket.bind();
    socket.listen();
}

void Server::start()
{
    Logger::getInstance()->info("Server starting up...");

    try
    {
        epoll.add(socket.getSocketFd(), EPOLLIN);
        Logger::getInstance()->info("Server is ready and waiting for connections...");

        struct epoll_event events[10];
        while (!shouldStop)
        {
            int nfds = epoll.wait(events, 10, 100); // 100ms timeout for more responsive shutdown

            if (nfds == -1)
            {
                if (errno == EINTR)
                    continue;
                Logger::getInstance()->error("Epoll wait failed: " + std::string(strerror(errno)));
                break;
            }

            for (int i = 0; i < nfds; ++i)
            {
                if (events[i].data.fd == socket.getSocketFd())
                {
                    std::string clientIp;
                    int client_socket = socket.acceptConnection(clientIp);
                    if (client_socket < 0)
                        continue;

                    {
                        std::lock_guard<std::mutex> lock(connectionsMutex);
                        connections.emplace(
                            client_socket,
                            ConnectionInfo{std::chrono::steady_clock::now(), clientIp, false, false, 0, 0}); // add connection info
                    }

                    try
                    {
                        epoll.add(client_socket, EPOLLIN | EPOLLET);
                    }
                    catch (const std::exception &e)
                    {
                        Logger::getInstance()->error("Failed to add client socket to epoll: " + std::string(e.what()));
                        closeConnection(client_socket);
                        continue;
                    }
                }
                else
                {
                    pool.enqueue([this, client_socket = events[i].data.fd]()
                                 {
                        std::string clientIp;
                        {
                            std::lock_guard<std::mutex> lock(connectionsMutex);
                            auto it = connections.find(client_socket);            // find client socket
                            if (it != connections.end()) clientIp = it->second.ip;// get client IP
                        }
                        handleClient(client_socket, clientIp); }); // handle client in thread pool
                }
            }
        }
    }
    catch (const std::exception &e)
    {
        Logger::getInstance()->error("Server error: " + std::string(e.what()));
    }

    Logger::getInstance()->info("Server is shutting down...");
}

void Server::stop()
{
    Logger::getInstance()->warning("Initiating server shutdown...");
    shouldStop = true;

    socket.closeSocket(); // stop accepting new connections

    pool.stop(); // stop worker threads

    // Close all existing connections
    std::vector<int> socketsToClose;
    {
        std::lock_guard<std::mutex> lock(connectionsMutex); // lock scope
        for (const auto &[client_socket, _] : connections)
        {
            socketsToClose.push_back(client_socket);
        }
    }

    for (int socket : socketsToClose)
    {
        closeConnection(socket);
    }

    Logger::getInstance()->info("All connections closed");
}

void Server::handleClient(int client_socket, const std::string &clientIp)
{
    std::vector<char> buffer(1024); // Initialize buffer for reading client data
    ssize_t valread;                // Variable to store the number of bytes read
    std::string request;            // String to accumulate the complete client request
    bool connectionClosed = false;  // Flag to track if the connection has been closed

    // Read data from the client in a loop
    while ((valread = read(client_socket, buffer.data(), buffer.size())) > 0)
    {
        // Check if the server should stop processing
        if (shouldStop)
        {
            closeConnection(client_socket);
            return;
        }

        // Append the read data to the request string
        request.append(buffer.data(), valread);

        // Update the bytes received for this connection
        std::lock_guard<std::mutex> lock(connectionsMutex);
        auto it = connections.find(client_socket);
        if (it != connections.end())
        {
            it->second.bytesReceived += valread;
        }
    }

    // Check if the connection has been closed by the client
    if (valread == 0 || (valread < 0 && errno != EAGAIN && errno != EWOULDBLOCK))
    {
        closeConnection(client_socket);
        connectionClosed = true;
    }

    // Process the request if the connection is still open and we have received data
    if (!connectionClosed && !request.empty())
    {
        std::string path = Http::getRequestPath(request); // Extract the request path
        bool isAsset = Http::isAssetRequest(path);        // Check if it's an asset request

        // Log non-asset requests
        if (!isAsset)
        {
            logRequest(client_socket, "Processing request: " + path);
        }

        // Apply rate limiting to the request
        std::string processedRequest = rateLimiter.process(request);

        // Check if the request was rate limited
        if (processedRequest == "HTTP/1.1 429 Too Many Requests\r\n"
                                "Content-Type: text/plain\r\n"
                                "Content-Length: 19\r\n"
                                "\r\n"
                                "Too Many Requests")
        {
            // Send the rate limit response to the client
            send(client_socket, processedRequest.c_str(), processedRequest.size(), 0);
        }
        else
        {
            // Create a compression middleware instance
            Compression compressionMiddleware;
            // Route the request with compression middleware
            router.route(path, client_socket, clientIp, &compressionMiddleware, &cache);
        }

        // Log completion of non-asset requests
        if (!isAsset)
        {
            logRequest(client_socket, "Request completed: " + path);
        }
    }
}

void Server::closeConnection(int client_socket)
{
    std::lock_guard<std::mutex> lock(connectionsMutex); // lock scope
    auto it = connections.find(client_socket);          // find client socket
    if (it != connections.end())
    {
        if (!it->second.isClosureLogged)
        {
            auto duration = std::chrono::steady_clock::now() - it->second.startTime;
            std::string durationStr = Socket::durationToString(duration);

            for (const auto &message : it->second.logBuffer) // log all buffered messages first
            {
                Logger::getInstance()->info(message, it->second.ip);
            }

            Logger::getInstance()->info(
                "Connection closed - Duration: " + durationStr +
                    ", Bytes received: " + std::to_string(it->second.bytesReceived) +
                    ", Bytes sent: " + std::to_string(it->second.bytesSent),
                it->second.ip); // always log the connection closure

            it->second.isClosureLogged = true;
        }
        connections.erase(it); // erase connection info
    }

    try
    {
        epoll.remove(client_socket); // remove client socket from epoll
    }
    catch (const std::exception &e)
    {
        Logger::getInstance()->warning("Failed to remove client socket from epoll: " + std::string(e.what()));
    }

    close(client_socket);
}

void Server::logRequest(int client_socket, const std::string &message)
{
    std::lock_guard<std::mutex> lock(connectionsMutex);
    auto it = connections.find(client_socket); // find client socket
    if (it != connections.end())
        it->second.logBuffer.push_back(message);
}

std::unique_ptr<Server> server; // instance of Server

std::atomic<bool> running(true); // flag to control the main loop

void signalHandler(int signum)
{
    running = false; // Set the running flag to false
}

int main(void)
{
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    try
    {
        Config config = Parser::parseConfig("pgs_conf.json"); // parse configuration file
        server = std::make_unique<Server>(config.port, config.staticFolder, config.threadCount,
                                          config.rateLimit.maxRequests, config.rateLimit.timeWindow,
                                          config.cache.sizeMB, config.cache.maxAgeSeconds); // create server instance

        std::thread serverThread([&]()
                                 { server->start(); }); // start server in a separate thread

        while (running)
        {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        std::thread shutdownThread([&]()
                                   { server->stop(); }); // initiate server shutdown in a separate thread

        if (shutdownThread.joinable())
        {
            shutdownThread.join();
        }
        else
        {
            Logger::getInstance()->error("Failed to join shutdown thread");
        }

        if (serverThread.joinable()) // join server thread
        {
            serverThread.join();
        }
        else
        {
            Logger::getInstance()->error("Failed to join server thread");
        }
    }
    catch (const std::exception &e)
    {
        Logger::getInstance()->error("Fatal error: " + std::string(e.what()));
        return EXIT_FAILURE;
    }

    Logger::getInstance()->info("Server shut down successfully");
    server.reset();            // explicitly destroy the server object
    Logger::destroyInstance(); // destroy the logger instance

    return EXIT_SUCCESS;
}