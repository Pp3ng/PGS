#include <algorithm>          // applying transformations
#include <arpa/inet.h>        // inet_ntoa - for converting IP addresses
#include <array>              // fixed-size arrays
#include <atomic>             // atomic operations
#include <chrono>             // measuring time
#include <condition_variable> // blocking thread synchronization
#include <csignal>            // signal handling
#include <cstring>            // strlen() - for string manipulation
#include <ctime>              // handling timestamps
#include <deque>              // double-ended queue
#include <fcntl.h>            // file control options
#include <filesystem>         // filesystem operations
#include <fstream>            // file reading operations
#include <functional>         // function objects
#include <future>             // asynchronous tasks
#include <iostream>           // std::cout, std::cerr - for console output
#include <list>               // doubly linked list for cache implementation
#include <map>                // ordered associative container (Red-Black Tree)
#include <memory>             // smart pointers
#include <memory_resource>    // memory resource management
#include <mutex>              // thread synchronization
#include <netinet/in.h>       // sockaddr_in - structure for IPv4 addresses
#include <netinet/tcp.h>      // TCP_KEEPIDLE, TCP_KEEPINTVL, TCP_KEEPCNT - TCP connection keepalive options
#include <new>                // memory alignment
#include <nlohmann/json.hpp>  // JSON parsing
#include <optional>           // optional type
#include <pthread.h>          // POSIX threads
#include <queue>              // queue data structure
#include <sched.h>            // CPU affinity
#include <set>                // ordered unique elements (Red-Black Tree)
#include <shared_mutex>       // shared mutexes
#include <sstream>            // string stream manipulations
#include <stdexcept>          // standard exceptions like std::runtime_error
#include <string_view>        // efficient string handling without ownership
#include <sys/epoll.h>        // epoll - for scalable I/O event notification
#include <sys/mman.h>         // mmap - for memory-mapped file access
#include <sys/sendfile.h>     // sendfile - for efficient file sending
#include <sys/socket.h>       // socket(), bind(), listen(), accept() - for socket operations
#include <sys/stat.h>         // fstat - to get file status
#include <sys/uio.h>          // writev - to write to multiple buffers
#include <thread>             // multithreading support
#include <unistd.h>           // close() function - to close file descriptors
#include <unordered_map>      // unordered associative container (Hash Table)
#include <unordered_set>      // unordered unique elements (Hash Table)
#include <vector>             // dynamic array
#include <zlib.h>             // zlib compression

namespace fs = std::filesystem; // Alias for filesystem namespace
using json = nlohmann::json;    // Alias for JSON namespace

struct Config
{
    int port;                 // prot for server
    std::string staticFolder; // path for static files
    int threadCount;          // thread count of worker threads
    struct
    {
        int maxRequests; // maximum number of requests allowed within time window
        int timeWindow;  // duration of time window for rate limiting
    } rateLimit;
    struct
    {
        size_t sizeMB;     // maximum size of cache in MB
        int maxAgeSeconds; // maximum age of cache entries in seconds
    } cache;
};

// Connection information structure
struct ConnectionInfo
{
    std::chrono::steady_clock::time_point startTime; // connection start time
    std::string ip;                                  // client IP address
    bool isLogged;                                   // flag to track if connection is logged
    bool isClosureLogged;                            // flag to track if connection closure is logged
    uint64_t bytesReceived;                          // bytes received from client
    uint64_t bytesSent;                              // bytes sent to client
    std::vector<std::string> logBuffer;              // buffer for storing logs

    ConnectionInfo(const std::chrono::steady_clock::time_point &time,
                   const std::string &ipAddr, bool logged = false,
                   bool closureLogged = false, uint64_t received = 0,
                   uint64_t sent = 0)
        : startTime(time), ip(ipAddr), isLogged(logged),
          isClosureLogged(closureLogged), bytesReceived(received),
          bytesSent(sent) {}
};

class Logger
{
private:
    // Define log levels using enum class for type safety and better semantics
    enum class LogLevel
    {
        INFO,
        WARNING,
        ERROR,
        SUCCESS
    };

    // ANSI escape codes for colors
    static constexpr std::array<const char *, 9> COLORS = {
        "\033[0m",  // RESET
        "\033[30m", // BLACK
        "\033[31m", // RED
        "\033[32m", // GREEN
        "\033[33m", // YELLOW
        "\033[34m", // BLUE
        "\033[35m", // MAGENTA
        "\033[36m", // CYAN
        "\033[37m", // WHITE
    };

    static constexpr const char *BOLD = "\033[1m";

    // Special symbols
    static constexpr const char *CHECK_MARK = "✅";
    static constexpr const char *CROSS_MARK = "❌";
    static constexpr const char *INFO_MARK = "🔵";
    static constexpr const char *WARN_MARK = "⚠️";

    // Constant string views for log levels
    static constexpr std::string_view LOG_LEVELS[] = {
        "INFO",
        "WARNING",
        "ERROR",
        "SUCCESS"};

    // Helper functions for formatting with colors
    [[nodiscard]]
    static std::string formatSuccess(const std::string &msg)
    {
        return std::string(COLORS[3]) + CHECK_MARK + " " + msg + COLORS[0];
    }

    [[nodiscard]]
    static std::string formatError(const std::string &msg)
    {
        return std::string(COLORS[2]) + CROSS_MARK + " " + msg + COLORS[0];
    }

    [[nodiscard]]
    static std::string formatInfo(const std::string &msg)
    {
        return std::string(COLORS[5]) + INFO_MARK + " " + msg + COLORS[0];
    }

    [[nodiscard]]
    static std::string formatWarning(const std::string &msg)
    {
        return std::string(COLORS[4]) + WARN_MARK + " " + msg + COLORS[0];
    }

    [[nodiscard]]
    static std::string formatStep(int num, const std::string &msg)
    {
        return std::string(COLORS[6]) + "[Step " + std::to_string(num) + "] " + msg + COLORS[0];
    }

    // Enhanced log message structure
    struct LogMessage
    {
        std::string message;
        LogLevel level;
        std::string ip;
        std::chrono::system_clock::time_point timestamp;

        LogMessage(std::string msg, LogLevel lvl, std::string clientIp)
            : message(std::move(msg)), level(lvl), ip(std::move(clientIp)), timestamp(std::chrono::system_clock::now()) {}
    };

    // Memory management and synchronization members
    std::pmr::monotonic_buffer_resource pool{64 * 1024};
    std::pmr::deque<LogMessage> messageQueue{&pool};

    mutable std::shared_mutex mutex;
    std::ofstream logFile;
    std::condition_variable_any queueCV;
    std::jthread loggerThread;
    std::atomic<bool> running{true};

    std::optional<std::chrono::steady_clock::time_point> lastEventWaitLog;
    bool isWaitingForEvents{false};

    static constexpr auto EVENT_WAIT_LOG_INTERVAL = std::chrono::seconds(5);

    static inline std::unique_ptr<Logger> instance;
    static inline std::once_flag initFlag;

    Logger()
    {
        logFile.open("pgs.log", std::ios::app);
        loggerThread = std::jthread([this](std::stop_token st)
                                    { processLogs(st); });
        writeLogMessage({{"Logger initialized"}, LogLevel::SUCCESS, "-"});
    }

    void processLogs(std::stop_token st)
    {
        while (!st.stop_requested())
        {
            std::vector<LogMessage> messages;
            messages.reserve(100);

            {
                std::shared_lock lock(mutex);
                auto pred = [this]
                {
                    return !messageQueue.empty() || !running;
                };

                if (queueCV.wait_for(lock, std::chrono::seconds(1), pred))
                {
                    while (!messageQueue.empty() && messages.size() < 100)
                    {
                        messages.push_back(std::move(messageQueue.front()));
                        messageQueue.pop_front();
                    }
                }
            }

            if (!messages.empty())
            {
                std::unique_lock lock(mutex);
                for (const auto &msg : messages)
                {
                    writeLogMessage(msg);
                }
                logFile.flush();
            }
        }
    }

    void writeLogMessage(const LogMessage &msg)
    {
        // Format message for file (without colors)
        std::string fileMessage = formatLogMessage(msg);
        logFile << fileMessage << std::endl;

        // Format message for console (with colors)
        std::string consoleMessage;
        switch (msg.level)
        {
        case LogLevel::ERROR:
            consoleMessage = formatError(fileMessage);
            break;
        case LogLevel::WARNING:
            consoleMessage = formatWarning(fileMessage);
            break;
        case LogLevel::SUCCESS:
            consoleMessage = formatSuccess(fileMessage);
            break;
        default:
            consoleMessage = formatInfo(fileMessage);
            break;
        }
        std::cout << consoleMessage << std::endl;
    }

    [[nodiscard]]
    std::string formatLogMessage(const LogMessage &msg)
    {
        auto time = std::chrono::system_clock::to_time_t(msg.timestamp);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      msg.timestamp.time_since_epoch()) %
                  1000;

        char timestamp[32];
        std::strftime(timestamp, sizeof(timestamp), "[%Y-%m-%d %H:%M:%S]",
                      std::localtime(&time));

        std::ostringstream oss;
        oss << timestamp << "."
            << std::setfill('0') << std::setw(3) << ms.count()
            << " [" << LOG_LEVELS[static_cast<int>(msg.level)] << "] "
            << "[" << msg.ip << "] "
            << msg.message;

        return oss.str();
    }

public:
    static Logger *getInstance()
    {
        std::call_once(initFlag, []()
                       { instance = std::unique_ptr<Logger>(new Logger()); });
        return instance.get();
    }

    void log(std::string_view message, LogLevel level = LogLevel::INFO,
             std::string_view ip = "-")
    {
        if (message == "Waiting for events...")
        {
            std::shared_lock lock(mutex);
            auto now = std::chrono::steady_clock::now();

            if (!isWaitingForEvents ||
                !lastEventWaitLog ||
                (now - *lastEventWaitLog) >= EVENT_WAIT_LOG_INTERVAL)
            {
                isWaitingForEvents = true;
                lastEventWaitLog = now;
            }
            else
            {
                return;
            }
        }
        else
        {
            isWaitingForEvents = false;
        }

        {
            std::unique_lock lock(mutex);
            messageQueue.emplace_back(std::string(message), level, std::string(ip));
        }
        queueCV.notify_one();
    }

    void error(std::string_view message, std::string_view ip = "-")
    {
        log(message, LogLevel::ERROR, ip);
    }

    void warning(std::string_view message, std::string_view ip = "-")
    {
        log(message, LogLevel::WARNING, ip);
    }

    void success(std::string_view message, std::string_view ip = "-")
    {
        log(message, LogLevel::SUCCESS, ip);
    }

    void info(std::string_view message, std::string_view ip = "-")
    {
        log(message, LogLevel::INFO, ip);
    }

    // Helper method for step logging
    void step(int num, std::string_view message, std::string_view ip = "-")
    {
        log(formatStep(num, std::string(message)), LogLevel::INFO, ip);
    }

    static void destroyInstance()
    {
        instance.reset();
    }

    ~Logger()
    {
        running = false;
        queueCV.notify_all();
        if (logFile.is_open())
        {
            logFile.close();
        }
    }

    Logger(const Logger &) = delete;
    Logger &operator=(const Logger &) = delete;
    Logger(Logger &&) = delete;
    Logger &operator=(Logger &&) = delete;
};

class Cache
{
private:
    // Cache entry structure for storing file content and metadata - O(1) access
    // time
    struct CacheEntry
    {
        std::vector<char> data; // actual content of cached file
        std::string mimeType;   // MIME type of cached content
        time_t lastModified;    // last modification time of file
        std::list<std::string>::iterator
            lruIterator; // iterator pointing to key's position in LRU list

        CacheEntry() : lastModified(0) {}

        // constructor with move semantics for data and mimeType - O(1)
        CacheEntry(std::vector<char> &&d, std::string &&m, time_t lm,
                   std::list<std::string>::iterator it)
            : data(std::move(d)), mimeType(std::move(m)), lastModified(lm), lruIterator(it) {}

        // constructor with perfect forwarding for data and mimeType - O(1)
        template <typename Vector>
        CacheEntry(Vector &&d, std::string &&m, time_t lm,
                   std::list<std::string>::iterator it)
            : data(std::make_move_iterator(d.begin()),
                   std::make_move_iterator(d.end())),
              mimeType(std::move(m)), lastModified(lm), lruIterator(it) {}

        // disable copy and assignment operations
        CacheEntry(const CacheEntry &) = delete;
        CacheEntry &operator=(const CacheEntry &) = delete;
    };

    std::unordered_map<std::string, CacheEntry>
        cache; // main cache storage (key -> entry mapping)
    std::list<std::string>
        lruList;                     // LRU order tracking list (most recent -> least recent)
    mutable std::shared_mutex mutex; // mutex for thread-safe operations
    size_t maxSize;                  // maximum size of cache in bytes
    size_t currentSize;              // current size of cache in bytes
    std::chrono::seconds maxAge;     // maximum age of cache entries

    // helper function to update LRU order - O(1) operation
    void updateLRU(const std::string &key)
    {
        lruList.erase(cache[key].lruIterator);    // remove from current position
        lruList.push_front(key);                  // add to front (most recently used)
        cache[key].lruIterator = lruList.begin(); // update iterator
    }

public:
    // constructor with overflow check - O(1)
    explicit Cache(size_t maxSizeMB, std::chrono::seconds maxAge)
        : maxSize(static_cast<size_t>(maxSizeMB) * 1024 * 1024), currentSize(0),
          maxAge(maxAge)
    {
        // check for cache size overflow
        if (maxSize / (1024 * 1024) !=
            maxSizeMB) // calculate: 1024*1024=1048576(1MB)
        {
            throw std::overflow_error("Cache size overflow");
        }
    }

    // retrieve an item from cache - O(1) average case
    template <typename Vector>
    bool get(const std::string &key, Vector &data, std::string &mimeType, time_t &lastModified)
    {
        std::shared_lock<std::shared_mutex> lock(mutex);
        auto it = cache.find(key);
        if (it != cache.end())
        {
            data = Vector(std::make_move_iterator(it->second.data.begin()),
                          std::make_move_iterator(it->second.data.end()));
            mimeType = std::move(it->second.mimeType);
            lastModified = it->second.lastModified;

            lock.unlock();
            std::unique_lock<std::shared_mutex> uniqueLock(mutex);
            updateLRU(key);
            return true;
        }
        return false;
    }

    // add an item to cache - O(1) average case
    template <typename Vector>
    void set(std::string &&key, Vector &&data, std::string &&mimeType, time_t lastModified)
    {
        std::unique_lock<std::shared_mutex> lock(mutex);

        // if entry already exists, remove it first
        auto it = cache.find(key);
        if (it != cache.end())
        {
            currentSize -= it->second.data.size();
            lruList.erase(it->second.lruIterator);
            cache.erase(it);
        }

        // if new entry is too large, don't cache it
        if (data.size() > maxSize)
        {
            return;
        }

        // remove least recently used entries until we have enough space
        while (!lruList.empty() && currentSize + data.size() > maxSize)
        {
            const std::string &lruKey = lruList.back();
            currentSize -= cache[lruKey].data.size();
            cache.erase(lruKey);
            lruList.pop_back();
        }

        // add new entry to front of LRU list
        lruList.push_front(key);

        try
        {
            // emplace new entry into cache
            cache.emplace(
                std::piecewise_construct,
                std::forward_as_tuple(std::move(key)),
                std::forward_as_tuple(
                    std::forward<Vector>(data),
                    std::move(mimeType),
                    lastModified,
                    lruList.begin()));

            currentSize += data.size();
        }
        catch (const std::exception &e)
        {
            lruList.pop_front(); // rollback on failure
            Logger::getInstance()->error("Cache allocation failed: " +
                                         std::string(e.what()));
        }
    }

    // clear all items from cache - O(1)
    void clear()
    {
        std::unique_lock<std::shared_mutex> lock(mutex);
        cache.clear();
        lruList.clear();
        currentSize = 0;
    }

    // remove a specific item from cache - O(1) average case
    bool remove(const std::string &key)
    {
        std::unique_lock<std::shared_mutex> lock(mutex);
        auto it = cache.find(key);
        if (it != cache.end())
        {
            currentSize -= it->second.data.size();
            lruList.erase(it->second.lruIterator);
            cache.erase(it);
            return true;
        }
        return false;
    }

    // check if an item exists in cache - O(1) average case
    bool exists(const std::string &key)
    {
        std::shared_lock<std::shared_mutex> lock(mutex);
        return cache.find(key) != cache.end();
    }

    // get current size of cache in bytes - O(1)
    size_t size() const
    {
        std::shared_lock<std::shared_mutex> lock(mutex);
        return currentSize;
    }

    // get number of items in cache - O(1)
    size_t count() const
    {
        std::shared_lock<std::shared_mutex> lock(mutex);
        return cache.size();
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
    RateLimiter(size_t maxRequests, std::chrono::seconds timeWindow)
        : maxRequests(maxRequests), timeWindow(timeWindow) {}

    std::string
    process(const std::string &data) override // override process method
    {
        std::lock_guard<std::mutex> lock(
            rateMutex); // lock mutex to protect clientRequests map

        auto now = std::chrono::steady_clock::now(); // get current timestamp

        auto &timestamps =
            clientRequests[data]; // retrieve reference to list of timestamps

        while (!timestamps.empty() &&
               now - timestamps.front() > timeWindow) // remove expired timestamps
        {
            timestamps.pop_front(); // remove oldest timestamp
        }

        // check if number of requests in time window exceeds allowed limit
        if (timestamps.size() >= maxRequests)
        {
            // if exceeded, return a 429 Too Many Requests response
            return "HTTP/1.1 429 Too Many Requests\r\n"
                   "Content-Type: text/plain\r\n"
                   "Content-Length: 19\r\n"
                   "\r\n"
                   "Too Many Requests";
        }

        timestamps.push_back(now); // add current timestamp to list

        return data; // return original data if rate limit is not exceeded
    }

private:
    size_t maxRequests;              // maximum number of requests allowed within time window
    std::chrono::seconds timeWindow; // duration of time window for rate limiting
    // map to store timestamps of requests for each client, identified by their
    // data
    std::unordered_map<std::string,
                       std::deque<std::chrono::steady_clock::time_point>>
        clientRequests;
    std::mutex rateMutex; // mutex to protect access to clientRequests
};

class Compression : public Middleware
{
public:
    [[nodiscard]]
    static bool shouldCompress(const std::string &mimeType,
                               size_t contentLength)
    {
        // define non-compressible mime types using unordered_set for efficient
        // look-up
        static const std::unordered_set<std::string> nonCompressibleTypes = {
            "image/png",
            "image/gif",
            "image/svg+xml",
            "image/x-icon",
            "image/webp",
            "audio/mpeg",
            "video/mp4",
            "video/webm",
            "application/zip",
            "font/woff",
            "font/woff2",
            "font/ttf",
            "application/vnd.ms-fontobject"};

        // define compressible mime types using unordered_set for efficient look-up
        static const std::unordered_set<std::string> compressibleTypes = {
            "text/",
            "application/javascript",
            "application/json",
            "application/xml",
            "application/x-yaml",
            "application/x-www-form-urlencoded"};

        // Return false if it's a non-compressible type or content length is less
        // than 1KB
        if (nonCompressibleTypes.count(mimeType) > 0 || contentLength < 1024)
        {
            return false;
        }

        // Check if MIME type is compressible
        return std::any_of(compressibleTypes.begin(), compressibleTypes.end(),
                           [&](const std::string &type)
                           {
                               return mimeType.find(type) ==
                                      0; // Check for prefix match
                           });
    }

    [[nodiscard]]
    std::string process(const std::string &data) override
    {
        std::string compressed = compressData(data);
        if (!compressed.empty()) // check if compression was successful
        {
            Logger::getInstance()->info(
                "Compressed data: " + std::to_string(data.size()) + " -> " +
                std::to_string(compressed.size()));
            return compressed;
        }
        return data; // Ensure a string is returned in all cases
    }

private:
    [[nodiscard]]
    std::string compressData(const std::string &data)
    {
        z_stream zs;                // create a z_stream object for compression
        memset(&zs, 0, sizeof(zs)); // zero-initialize z_stream structure

        // Initialize the zlib compression
        if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, // set compression level
                         Z_DEFLATED,                 // use deflate compression method
                         15 | 16,                    // 15 | 16 for gzip encoding
                         8,                          // set window size
                         Z_DEFAULT_STRATEGY) !=
            Z_OK) // use default compression strategy
        {
            throw std::runtime_error("Failed to initialize zlib");
        }

        // set input data for compression
        zs.next_in = reinterpret_cast<Bytef *>(
            const_cast<char *>(data.data())); // input data
        zs.avail_in = data.size();
        // size of input data

        int ret;                         // variable to hold return status of compression
        const size_t bufferSize = 32768; // define size of output buffer (32KB)
        std::string compressed;          // string to hold final compressed data
        char outbuffer[bufferSize];      // buffer for compressed output

        // compress data in a loop until all data is processed
        do
        {
            zs.next_out = reinterpret_cast<Bytef *>(
                outbuffer);            // output buffer for compressed data
            zs.avail_out = bufferSize; // size of output buffer

            ret = deflate(&zs, Z_FINISH); // perform compression operation

            if (compressed.size() <
                zs.total_out) // append compressed data to output string
            {
                compressed.append(outbuffer, zs.total_out - compressed.size());
            }
        } while (ret == Z_OK); // continue until no more data to compress

        // check if compression completed successfully
        if (ret != Z_STREAM_END)
        {
            deflateEnd(&zs); // clean up z_stream object
            throw std::runtime_error("Failed to compress data");
        }

        deflateEnd(&zs);   // clean up and free resources allocated by zlib
        return compressed; // return compressed data
    }
};

class ThreadPool
{
public:
    // constructor: Initialize thread pool with specified number of threads
    explicit ThreadPool(size_t numThreads) { start(numThreads); }

    // destructor: Ensure proper cleanup of all threads
    ~ThreadPool() { stop(); }

    // Disable copy operations to prevent resource duplication
    ThreadPool(const ThreadPool &) = delete;
    ThreadPool &operator=(const ThreadPool &) = delete;

    // Template method to submit tasks to the thread pool
    // F: Function type
    // Args: Function arguments pack
    // Returns: std::future containing the result of the task
    template <typename F, typename... Args>
    auto enqueue(F &&f, Args &&...args)
        -> std::future<typename std::invoke_result_t<F, Args...>>
    {
        using return_type = typename std::invoke_result_t<F, Args...>;

        // Create a packaged task that will store the function and its arguments
        auto task = std::make_shared<std::packaged_task<return_type()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...));

        // Get future from the task before potentially moving the task
        std::future<return_type> res = task->get_future();
        {
            // Need exclusive lock for writing to task queue
            std::unique_lock<std::shared_mutex> lock(taskMutex);
            if (stop_flag)
            {
                throw std::runtime_error("Cannot enqueue on stopped ThreadPool");
            }
            tasks.emplace([task]()
                          { (*task)(); });
        }
        condition.notify_one();
        return res;
    }

    // Gracefully stop the thread pool
    void stop()
    {
        {
            std::unique_lock<std::shared_mutex> lock(taskMutex);
            stop_flag = true;
        }
        condition.notify_all();

        for (std::thread &worker : workers)
        {
            if (worker.joinable())
            {
                worker.join();
            }
        }

        workers.clear();
        std::queue<std::function<void()>> empty;
        std::swap(tasks, empty);
    }

private:
    std::vector<std::thread> workers;        // Container for worker threads
    std::queue<std::function<void()>> tasks; // Task queue
    mutable std::shared_mutex
        taskMutex; // Read-write lock for protecting task queue
    std::condition_variable_any
        condition;                         // Condition variable that works with shared_mutex
    std::atomic<bool> stop_flag{false};    // Flag to signal thread pool shutdown
    std::atomic<size_t> active_threads{0}; // Counter for currently active threads

    // Set CPU affinity for a thread
    bool setThreadAffinity(pthread_t thread, size_t thread_id)
    {
        const int num_cores = std::thread::hardware_concurrency();
        if (num_cores == 0)
            return false;

        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);

        const int cores_per_thread = std::max(1, num_cores / 4);
        const int base_core = (thread_id * cores_per_thread) % num_cores;

        for (int i = 0; i < cores_per_thread; ++i)
        {
            CPU_SET((base_core + i) % num_cores, &cpuset);
        }

        int rc = pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset);
        if (rc != 0)
        {
            Logger::getInstance()->warning("Thread affinity setting failed: " +
                                           std::string(strerror(rc)));
            return false;
        }
        return true;
    }

    // Initialize and start the thread pool
    void start(size_t numThreads)
    {
        const int num_cores = std::thread::hardware_concurrency();
        workers.reserve(numThreads);

        for (size_t i = 0; i < numThreads; ++i)
        {
            workers.emplace_back([this, i]
                                 {
        pthread_setname_np(pthread_self(),
                           ("worker-" + std::to_string(i)).c_str());
        setThreadAffinity(pthread_self(), i);

        while (true) {
          std::function<void()> task;
          {
            // Use shared lock initially for reading
            std::shared_lock<std::shared_mutex> lock(taskMutex);

            auto waitResult = condition.wait_for(
                lock, std::chrono::milliseconds(100),
                [this] { return stop_flag || !tasks.empty(); });

            if (stop_flag && tasks.empty()) {
              return;
            }

            if (!tasks.empty()) {
              // Upgrade to unique lock for task extraction
              lock.unlock();
              std::unique_lock<std::shared_mutex> uniqueLock(taskMutex);
              if (!tasks.empty()) { // Check again after acquiring unique lock
                task = std::move(tasks.front());
                tasks.pop();
              }
            } else if (!waitResult) {
              continue;
            }
          }

          if (task) {
            active_threads++;
            try {
              task();
            } catch (const std::exception &e) {
              Logger::getInstance()->error("Thread pool task exception: " +
                                           std::string(e.what()));
            } catch (...) {
              Logger::getInstance()->error(
                  "Unknown exception in thread pool task");
            }
            active_threads--;
          }
        } });
        }

        Logger::getInstance()->success(
            "Thread pool initialized with " + std::to_string(numThreads) +
            " threads" +
            (num_cores > 0 ? " across " + std::to_string(num_cores) + " CPU cores"
                           : " (CPU core count unknown)"));
    }
};

class Socket
{
public:
    explicit Socket(int port);
    ~Socket();
    void bind();
    void listen();
    void closeSocket();
    int acceptConnection(std::string &clientIp);
    int getSocketFd() const;

    static std::string
    durationToString(const std::chrono::steady_clock::duration &duration)
    {
        auto seconds =
            std::chrono::duration_cast<std::chrono::seconds>(duration).count();
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
    // Create a dual-stack socket that supports both IPv6 and IPv4
    if ((server_fd = socket(AF_INET6, SOCK_STREAM, 0)) == -1)
    {
        Logger::getInstance()->error("Socket creation failed: " +
                                     std::string(strerror(errno)));
        throw std::runtime_error("Socket creation failed!");
    }

    try
    {
        // Lambda for handling setsockopt calls
        auto setSocketOption = [this](int level, int optname, const void *optval,
                                      socklen_t optlen, const char *errorMsg)
        {
            if (setsockopt(server_fd, level, optname, optval, optlen) < 0)
            {
                throw std::runtime_error(std::string(errorMsg) + ": " +
                                         std::string(strerror(errno)));
            }
        };

        // Allow IPv4 connections on IPv6 socket (disable IPV6_V6ONLY)
        int no = 0;
        setSocketOption(IPPROTO_IPV6, IPV6_V6ONLY, &no, sizeof(no),
                        "Failed to set IPV6_V6ONLY");

        // Enable address and port reuse
        int opt = 1;
        setSocketOption(SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt),
                        "Failed to set SO_REUSEADDR | SO_REUSEPORT");

        // Set send/receive buffer sizes for better performance
        int bufSize =
            1024 * 1024; // 1MB buffer (i don't know if this is a good size and
                         // should i make it as a configurable parameter ?)
        setSocketOption(SOL_SOCKET, SO_RCVBUF, &bufSize, sizeof(bufSize),
                        "Failed to set SO_RCVBUF");
        setSocketOption(SOL_SOCKET, SO_SNDBUF, &bufSize, sizeof(bufSize),
                        "Failed to set SO_SNDBUF");

        // Set non-blocking mode for epoll
        int flags = fcntl(server_fd, F_GETFL, 0);
        if (flags == -1)
        {
            throw std::runtime_error("Failed to get socket flags: " +
                                     std::string(strerror(errno)));
        }
        if (fcntl(server_fd, F_SETFL, flags | O_NONBLOCK) < 0)
        {
            throw std::runtime_error("Failed to set non-blocking mode: " +
                                     std::string(strerror(errno)));
        }

        // Note: We don't set the following here because they are managed elsewhere:
        // - TCP_CORK: Managed by SocketOptionGuard in Http::sendResponse
        // - SO_KEEPALIVE and related: Set in Http::setupSocketOptions for each
        // client
        // - TCP_NODELAY: Not used as we're using TCP_CORK for static file serving

        Logger::getInstance()->success(
            "Socket created and configured successfully");
    }
    catch (const std::exception &e)
    {
        Logger::getInstance()->error("Socket configuration failed: " +
                                     std::string(e.what()));
        close(server_fd);
        throw;
    }
}

Socket::~Socket()
{
    Logger::getInstance()->info("Closing server socket");
    close(server_fd);
}

void Socket::bind()
{
    struct sockaddr_in6 address;
    memset(&address, 0, sizeof(address));
    address.sin6_family = AF_INET6;  // Set address family to IPv6
    address.sin6_addr = in6addr_any; // Bind to all available interfaces
    address.sin6_port = htons(port); // Set port

    if (::bind(server_fd, reinterpret_cast<struct sockaddr *>(&address),
               sizeof(address)) < 0) // Bind socket to address and port
    {
        std::string errorMsg = "Bind failed: " + std::string(strerror(errno));
        Logger::getInstance()->error(errorMsg);
        throw std::runtime_error(errorMsg);
    }

    Logger::getInstance()->success("Socket successfully bound to port " +
                                   std::to_string(port));
}

void Socket::listen()
{
    ::listen(
        server_fd,
        42); // 42 is the ultimate answer to life, the universe, and everything
}
void Socket::closeSocket()
{
    if (server_fd != -1) // Check if socket is valid
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
    int new_socket =
        accept(server_fd, reinterpret_cast<struct sockaddr *>(&address),
               &addrlen); // Accept a new connection

    if (new_socket >= 0)
    {
        char ipstr[INET6_ADDRSTRLEN];
        if (address.sin6_family == AF_INET6)
        {
            inet_ntop(AF_INET6, &address.sin6_addr, ipstr,
                      sizeof(ipstr)); // Convert IPv6 address to string
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
                             Middleware *middleware = nullptr,
                             Cache *cache = nullptr);
    static bool isAssetRequest(const std::string &path);

private:
    // Constants for optimized I/O
    static constexpr size_t BUFFER_SIZE = 65536;      // 64KB buffer size
    static constexpr size_t ALIGNMENT = 512;          // memory alignment boundary
    static constexpr size_t SENDFILE_CHUNK = 1048576; // 1MB sendfile chunk size
    static constexpr int MAX_IOV = IOV_MAX;           // maximum iovec array size

    // Socket option settings
    struct SocketSettings
    {
        static constexpr int keepAlive = 1;
        static constexpr int keepIdle = 60;
        static constexpr int keepInterval = 10;
        static constexpr int keepCount = 3;
    };

    // RAII wrappers
    class SocketOptionGuard
    {
        int &cork;
        int client_socket;

    public:
        SocketOptionGuard(int &c, int cs) : cork(c), client_socket(cs) {}
        ~SocketOptionGuard()
        {
            cork = 0;
            setsockopt(client_socket, IPPROTO_TCP, TCP_CORK, &cork, sizeof(cork));
        }
    };

    class FileGuard
    {
        int fd;

    public:
        FileGuard() : fd(-1) {}
        explicit FileGuard(int f) : fd(f) {}
        ~FileGuard()
        {
            if (fd != -1)
                close(fd);
        }
        int get() const { return fd; }
        void reset(int f = -1)
        {
            if (fd != -1)
                close(fd);
            fd = f;
        }
    };

    class MMapGuard
    {
        void *addr;
        size_t length;

    public:
        MMapGuard(void *a, size_t l) : addr(a), length(l) {}
        ~MMapGuard()
        {
            if (addr != MAP_FAILED)
                munmap(addr, length);
        }
    };

    // Helper functions declarations
    static bool setupSocketOptions(int client_socket, int cork,
                                   const std::string &clientIp);
    static bool handleFileContent(FileGuard &fileGuard,
                                  const std::string &filePath,
                                  std::pmr::vector<char> &fileContent,
                                  size_t &fileSize, time_t &lastModified,
                                  const std::string &clientIp);
    static bool compressContent(Middleware *middleware,
                                const std::string &mimeType, size_t fileSize,
                                std::pmr::vector<char> &fileContent,
                                std::pmr::string &compressedContent,
                                bool cacheHit, const FileGuard &fileGuard,
                                std::pmr::monotonic_buffer_resource &pool);
    static std::string generateHeaders(int statusCode,
                                       const std::string &mimeType,
                                       size_t fileSize, time_t lastModified,
                                       bool isCompressed);
    static size_t sendWithWritev(int client_socket, const std::string &headerStr,
                                 const std::pmr::string &compressedContent,
                                 const std::pmr::vector<char> &fileContent,
                                 bool isCompressed, bool cacheHit,
                                 const std::string &clientIp);
    static size_t sendLargeFile(int client_socket, const FileGuard &fileGuard,
                                size_t fileSize, const std::string &clientIp);
    static void updateCache(Cache *cache, const std::string &filePath,
                            const std::string &mimeType, time_t lastModified,
                            const FileGuard &fileGuard, size_t fileSize,
                            std::pmr::monotonic_buffer_resource &pool);
};
bool Http::isAssetRequest(const std::string &path)
{
    // cache string length to avoid multiple calls
    const size_t pathLen = path.length();
    if (pathLen < 4)
        return false; // Minimum length for an asset (.css)

    // convert last 10 chars (max extension length) to lowercase once
    // this avoids converting the entire path
    alignas(8) char lastChars[10]; // Aligned for better memory access
    const size_t checkLen = std::min(pathLen, size_t(10));
    std::transform(path.end() - checkLen, path.end(), lastChars, ::tolower);

    std::string_view lastView(lastChars, checkLen);

    // Most common image types and web assets first (ordered by frequency)
    static constexpr std::array<std::string_view, 16> commonExts = {
        ".jpg", ".png", ".gif", // most frequent image types
        ".jpeg", ".webp",       // less frequent image types
        ".css", ".js",          // essential web assets
        ".ico", ".svg",         // common icons
        ".woff2", ".woff",      // modern fonts first
        ".ttf",                 // legacy font
        ".mp4", ".webm",        // video files
        ".json", ".xml"         // data files
    };

    // fast path: check most common extensions first using simd-friendly loop
    const auto commonExtsSize = commonExts.size();
    for (size_t i = 0; i < commonExtsSize; ++i)
    {
        const auto &ext = commonExts[i];
        if (checkLen >= ext.length() &&
            lastView.compare(checkLen - ext.length(), ext.length(), ext) == 0)
        {
            return true;
        }
        // early exit after checking most common types
        if (i == 7 && pathLen > 6)
            break; // skip less common types for longer paths
    }

    // less common extensions - now includes previously separate ones
    static constexpr std::array<std::string_view, 5> rareExts = {
        ".eot", ".map", ".pdf", ".mp3", ".wav"};

    // check rare extensions only for specific path patterns
    if (pathLen > 8)
    { // only check for longer paths
        size_t dotPos = path.find_last_of('.');
        if (dotPos != std::string::npos)
        {
            std::string_view ext(lastChars + (dotPos - (pathLen - checkLen)),
                                 checkLen - (dotPos - (pathLen - checkLen)));
            if (std::any_of(rareExts.begin(), rareExts.end(),
                            [&](const auto &rareExt)
                            { return ext == rareExt; }))
            {
                return true;
            }
        }
    }

    // directory check optimization using string_view and branch prediction
    std::string_view pathView(path);

    // hot path: check most common asset directories first
    static constexpr std::array<std::string_view, 6> hotDirs = {
        "/img/", "/images/",   // image directories
        "/css/", "/js/",       // essential asset directories
        "/assets/", "/static/" // common resource directories
    };

    // use likely/unlikely for better branch prediction
    if (std::any_of(hotDirs.begin(), hotDirs.end(),
                    [&](const auto &dir)
                    { return pathView.starts_with(dir); }))
        [[likely]]
    {
        return true;
    }

    // cold path: less common directories
    static constexpr std::array<std::string_view, 3> coldDirs = {
        "/fonts/", "/media/", "/photos/"};

    // check cold directories only if hot directories didn't match
    if (pathLen > 7) [[unlikely]]
    {
        if (std::any_of(coldDirs.begin(), coldDirs.end(), [&](const auto &dir)
                        { return pathView.find(dir) != std::string_view::npos; }))
        {
            return true;
        }
    }

    return false;
}

[[nodiscard]]
std::string Http::getRequestPath(const std::string &request)
{
    size_t pos1 = request.find("GET ");
    size_t pos2 = request.find(" HTTP/");
    if (pos1 == std::string::npos ||
        pos2 == std::string::npos) // Check if request is valid
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
    // Create a memory resource for this request
    std::pmr::monotonic_buffer_resource pool(64 * 1024); // 64KB initial size

    // Performance metrics
    auto startTime = std::chrono::steady_clock::now();
    size_t totalBytesSent = 0;

    // Socket options
    int cork = 1;
    SocketOptionGuard sockGuard(cork, client_socket);

    // Set socket options including TCP_CORK
    if (!setupSocketOptions(client_socket, cork, clientIp))
    {
        return;
    }

    // File content and cache handling
    std::pmr::vector<char> fileContent{&pool};
    size_t fileSize;
    time_t lastModified;
    bool cacheHit = false;

    // Try to get content from cache
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

    // Handle file if not in cache
    FileGuard fileGuard;
    if (!cacheHit)
    {
        if (!handleFileContent(fileGuard, filePath, fileContent, fileSize,
                               lastModified, clientIp))
        {
            return;
        }
    }

    // Compression handling
    bool isCompressed = false;
    std::pmr::string compressedContent{&pool};

    if (middleware && Compression::shouldCompress(mimeType, fileSize) &&
        mimeType.find("image/") == std::string::npos)
    {
        isCompressed =
            compressContent(middleware, mimeType, fileSize, fileContent,
                            compressedContent, cacheHit, fileGuard, pool);
        if (isCompressed)
        {
            fileSize = compressedContent.size();
        }
    }

    // Generate response headers
    std::string headerStr = generateHeaders(statusCode, mimeType, fileSize,
                                            lastModified, isCompressed);

    // Send headers and content using writev
    totalBytesSent +=
        sendWithWritev(client_socket, headerStr, compressedContent, fileContent,
                       isCompressed, cacheHit, clientIp);

    // Handle large file transfer using sendfile or mmap
    if (!isCompressed && !cacheHit && fileGuard.get() != -1)
    {
        totalBytesSent +=
            sendLargeFile(client_socket, fileGuard, fileSize, clientIp);

        if (cache && statusCode == 200)
        {
            // Update cache if need
            updateCache(cache, std::string(filePath), std::string(mimeType),
                        lastModified, fileGuard, fileSize, pool);
        }
    }

    // Record performance metrics
    auto endTime = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
        endTime - startTime);

    if (isIndex)
    {
        Logger::getInstance()->info(
            "Response sent: status=" + std::to_string(statusCode) +
                ", path=" + filePath + ", size=" + std::to_string(fileSize) +
                ", type=" + mimeType + ", cache=" + (cacheHit ? "HIT" : "MISS") +
                ", time=" + std::to_string(duration.count()) + "µs" +
                ", bytes=" + std::to_string(totalBytesSent),
            clientIp);
    }
}
bool Http::setupSocketOptions(int client_socket, int cork,
                              const std::string &clientIp)
{
    auto setSocketOption = [&](int level, int optname, const void *optval,
                               socklen_t optlen)
    {
        if (setsockopt(client_socket, level, optname, optval, optlen) < 0)
        {
            Logger::getInstance()->error("Failed to set socket option: " +
                                             std::string(strerror(errno)),
                                         clientIp);
            close(client_socket);
            return false;
        }
        return true;
    };

    return setSocketOption(SOL_SOCKET, SO_KEEPALIVE, &SocketSettings::keepAlive,
                           sizeof(SocketSettings::keepAlive)) &&
           setSocketOption(IPPROTO_TCP, TCP_KEEPIDLE, &SocketSettings::keepIdle,
                           sizeof(SocketSettings::keepIdle)) &&
           setSocketOption(IPPROTO_TCP, TCP_KEEPINTVL,
                           &SocketSettings::keepInterval,
                           sizeof(SocketSettings::keepInterval)) &&
           setSocketOption(IPPROTO_TCP, TCP_KEEPCNT, &SocketSettings::keepCount,
                           sizeof(SocketSettings::keepCount)) &&
           setSocketOption(IPPROTO_TCP, TCP_CORK, &cork, sizeof(cork));
}

bool Http::handleFileContent(FileGuard &fileGuard, const std::string &filePath,
                             std::pmr::vector<char> &fileContent,
                             size_t &fileSize, time_t &lastModified,
                             const std::string &clientIp)
{
    // Open file with O_DIRECT for large files to bypass system cache
    int flags = O_RDONLY;
    struct stat statBuf;
    if (stat(filePath.c_str(), &statBuf) == 0 &&
        statBuf.st_size > 10 * 1024 * 1024) // 10MB threshold
    {
        flags |= O_DIRECT;
    }

    int fd = open(filePath.c_str(), flags);
    if (fd == -1 &&
        errno == EINVAL) // O_DIRECT not supported, fallback to normal open
    {
        fd = open(filePath.c_str(), O_RDONLY);
    }

    if (fd == -1)
    {
        Logger::getInstance()->error(
            "Failed to open file: errno=" + std::to_string(errno), clientIp);
        return false;
    }
    fileGuard.reset(fd);

    // Advise kernel about access pattern for optimal I/O performance
    posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);

    struct stat file_stat;
    if (fstat(fd, &file_stat) == -1)
    {
        Logger::getInstance()->error("Fstat failed: errno=" + std::to_string(errno),
                                     clientIp);
        return false;
    }

    fileSize = file_stat.st_size;
    lastModified = file_stat.st_mtime;
    return true;
}

bool Http::compressContent(Middleware *middleware, const std::string &mimeType,
                           size_t fileSize, std::pmr::vector<char> &fileContent,
                           std::pmr::string &compressedContent, bool cacheHit,
                           const FileGuard &fileGuard,
                           std::pmr::monotonic_buffer_resource &pool)
{
    if (cacheHit)
    {
        compressedContent = middleware->process(
            std::string(fileContent.begin(), fileContent.end()));
    }
    else
    {
        // Use aligned buffer with RAII
        struct AlignedBuffer
        {
            void *ptr;
            AlignedBuffer(size_t size, size_t alignment) : ptr(nullptr)
            {
                if (posix_memalign(&ptr, alignment, size) != 0)
                {
                    throw std::bad_alloc();
                }
            }
            ~AlignedBuffer() { free(ptr); }
            void *get() { return ptr; }
        } alignedBuffer(BUFFER_SIZE, ALIGNMENT);

        std::pmr::vector<char> buffer{&pool};
        buffer.reserve(fileSize);

        size_t totalRead = 0;
        while (totalRead < fileSize)
        {
            ssize_t bytesRead = read(fileGuard.get(), alignedBuffer.get(),
                                     std::min(BUFFER_SIZE, fileSize - totalRead));
            if (bytesRead <= 0)
                break;
            buffer.insert(buffer.end(), static_cast<char *>(alignedBuffer.get()),
                          static_cast<char *>(alignedBuffer.get()) + bytesRead);
            totalRead += bytesRead;
        }

        compressedContent =
            middleware->process(std::string(buffer.begin(), buffer.end()));
    }
    return true;
}
std::string Http::generateHeaders(int statusCode, const std::string &mimeType,
                                  size_t fileSize, time_t lastModified,
                                  bool isCompressed)
{
    // Pre-allocate header string capacity
    std::string headerStr;
    headerStr.reserve(1024); // Reserve reasonable space for headers

    // Generate time strings
    char timeBuffer[128], lastModifiedBuffer[128];
    time_t now = time(nullptr);
    struct tm tmBuf;
    const struct tm *tm_info = gmtime_r(&now, &tmBuf); // Thread-safe version
    strftime(timeBuffer, sizeof(timeBuffer), "%a, %d %b %Y %H:%M:%S GMT",
             tm_info);

    tm_info = gmtime_r(&lastModified, &tmBuf);
    strftime(lastModifiedBuffer, sizeof(lastModifiedBuffer),
             "%a, %d %b %Y %H:%M:%S GMT", tm_info);

    // Status message lookup using array for better performance
    static const char *const STATUS_MESSAGES[] = {
        "OK",                    // 200
        nullptr,                 // 201-299
        nullptr,                 // 300-303
        "Not Modified",          // 304
        nullptr,                 // 305-399
        "Bad Request",           // 400
        "Unauthorized",          // 401
        nullptr,                 // 402
        "Forbidden",             // 403
        "Not Found",             // 404
        nullptr,                 // 405-499
        "Internal Server Error", // 500
        nullptr,                 // 501-502
        "Service Unavailable",   // 503
    };

    const char *statusMessage;
    if (statusCode >= 200 &&
        statusCode <
            static_cast<int>(sizeof(STATUS_MESSAGES) / sizeof(char *)) + 200)
    {
        statusMessage = STATUS_MESSAGES[statusCode - 200];
        if (!statusMessage)
            statusMessage = "Unknown Status";
    }
    else
    {
        statusMessage = "Unknown Status";
    }

    // Assemble response headers using string
    headerStr = "HTTP/1.1 " + std::to_string(statusCode) + " " + statusMessage +
                "\r\n"
                "Server: RobustHTTP/1.0\r\n"
                "Date: " +
                std::string(timeBuffer) +
                "\r\n"
                "Content-Type: " +
                mimeType +
                "\r\n"
                "Content-Length: " +
                std::to_string(fileSize) +
                "\r\n"
                "Last-Modified: " +
                std::string(lastModifiedBuffer) +
                "\r\n"
                "Connection: keep-alive\r\n"
                "Keep-Alive: timeout=60, max=1000\r\n"
                "Accept-Ranges: bytes\r\n"
                "Cache-Control: public, max-age=31536000\r\n"
                "X-Content-Type-Options: nosniff\r\n"
                "X-Frame-Options: SAMEORIGIN\r\n"
                "X-XSS-Protection: 1; mode=block\r\n";

    if (isCompressed)
    {
        headerStr += "Content-Encoding: gzip\r\n"
                     "Vary: Accept-Encoding\r\n";
    }
    headerStr += "\r\n";

    return headerStr;
}

size_t Http::sendWithWritev(int client_socket, const std::string &headerStr,
                            const std::pmr::string &compressedContent,
                            const std::pmr::vector<char> &fileContent,
                            bool isCompressed, bool cacheHit,
                            const std::string &clientIp)
{
    // Optimize writev using maximum allowed iovec structures
    std::array<struct iovec, MAX_IOV> iov;
    int iovcnt = 0;

    // Add header to iovec
    iov[iovcnt].iov_base = const_cast<char *>(headerStr.c_str());
    iov[iovcnt].iov_len = headerStr.size();
    iovcnt++;

    // Add content to iovec if compressed or cached
    if (isCompressed)
    {
        iov[iovcnt].iov_base = const_cast<char *>(compressedContent.c_str());
        iov[iovcnt].iov_len = compressedContent.size();
        iovcnt++;
    }
    else if (cacheHit)
    {
        iov[iovcnt].iov_base = const_cast<char *>(fileContent.data());
        iov[iovcnt].iov_len = fileContent.size();
        iovcnt++;
    }

    // Send headers and content using writev with retry logic
    size_t totalSent = 0;
    const size_t totalSize =
        headerStr.size() + (isCompressed ? compressedContent.size()
                                         : (cacheHit ? fileContent.size() : 0));

    while (totalSent < totalSize)
    {
        ssize_t sent = writev(client_socket, iov.data(), iovcnt);
        if (sent <= 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                std::this_thread::sleep_for(
                    std::chrono::microseconds(1000)); // 1ms sleep
                continue;                             // retry
            }
            Logger::getInstance()->error(
                "Failed to send response: errno=" + std::to_string(errno), clientIp);
            return totalSent;
        }
        totalSent += sent;

        // Update iovec structures with zero-copy approach
        while (sent > 0 && iovcnt > 0)
        {
            if (static_cast<size_t>(sent) >= iov[0].iov_len)
            {
                sent -= iov[0].iov_len;
                iovcnt--;
                std::copy(iov.begin() + 1, iov.begin() + 1 + iovcnt, iov.begin());
            }
            else
            {
                iov[0].iov_base = static_cast<char *>(iov[0].iov_base) + sent;
                iov[0].iov_len -= sent;
                break;
            }
        }
    }
    return totalSent;
}
size_t Http::sendLargeFile(int client_socket, const FileGuard &fileGuard,
                           size_t fileSize, const std::string &clientIp)
{
    size_t totalSent = 0;
    off_t offset = 0;
    bool useMmap = false;

    // Try sendfile with optimal chunk size and retry logic
    while (offset < static_cast<off_t>(fileSize))
    {
        size_t chunk = std::min(SENDFILE_CHUNK, fileSize - offset);
        ssize_t sent = sendfile(client_socket, fileGuard.get(), &offset, chunk);

        if (sent == -1)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                std::this_thread::sleep_for(
                    std::chrono::microseconds(1000)); // 1ms sleep
                continue;                             // retry
            }
            else if (errno == EINVAL || errno == ENOSYS)
            {
                useMmap = true;
                break;
            }
            Logger::getInstance()->error(
                "Failed to send file: errno=" + std::to_string(errno), clientIp);
            return totalSent;
        }
        else if (sent == 0)
        {
            break;
        }
        totalSent += sent;
    }

    // Fall back to mmap for large files with huge pages support
    if (useMmap)
    {
        int flags = MAP_PRIVATE;
        if (fileSize >= 2 * 1024 * 1024) // 2MB threshold for huge pages
        {
            flags |= MAP_HUGETLB;
        }

        void *mmapAddr =
            mmap(nullptr, fileSize, PROT_READ, flags, fileGuard.get(), 0);
        if (mmapAddr == MAP_FAILED && (flags & MAP_HUGETLB))
        {
            flags &= ~MAP_HUGETLB;
            mmapAddr = mmap(nullptr, fileSize, PROT_READ, flags, fileGuard.get(), 0);
        }

        if (mmapAddr == MAP_FAILED)
        {
            Logger::getInstance()->error(
                "Mmap failed: errno=" + std::to_string(errno), clientIp);
            return totalSent;
        }

        // RAII for mmap
        MMapGuard mmapGuard(mmapAddr, fileSize);
        madvise(mmapAddr, fileSize, MADV_SEQUENTIAL);

        const char *fileContent = static_cast<const char *>(mmapAddr);
        size_t remainingBytes = fileSize;
        size_t bytesSent = 0;

        while (remainingBytes > 0)
        {
            size_t chunk = std::min(BUFFER_SIZE, remainingBytes);
            ssize_t sent =
                send(client_socket, fileContent + bytesSent, chunk, MSG_NOSIGNAL);
            if (sent == -1)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                {
                    std::this_thread::sleep_for(
                        std::chrono::microseconds(1000)); // 1ms sleep
                    continue;                             // retry
                }
                Logger::getInstance()->error("Failed to send mmap data: errno=" +
                                                 std::to_string(errno),
                                             clientIp);
                return totalSent + bytesSent;
            }
            bytesSent += sent;
            remainingBytes -= sent;
        }
        totalSent += bytesSent;
    }

    return totalSent;
}

void Http::updateCache(Cache *cache, const std::string &filePath,
                       const std::string &mimeType, time_t lastModified,
                       const FileGuard &fileGuard, size_t fileSize,
                       std::pmr::monotonic_buffer_resource &pool)
{
    std::pmr::vector<char> content{&pool};
    content.reserve(fileSize);

    if (lseek(fileGuard.get(), 0, SEEK_SET) != -1)
    {
        // Use aligned buffer for optimal read performance
        std::unique_ptr<char[]> alignedBuffer(
            new (std::align_val_t{ALIGNMENT}) char[BUFFER_SIZE]);
        size_t totalRead = 0;

        while (totalRead < fileSize)
        {
            ssize_t bytesRead = read(fileGuard.get(), alignedBuffer.get(),
                                     std::min(BUFFER_SIZE, fileSize - totalRead));
            if (bytesRead <= 0)
                break;

            content.insert(content.end(), alignedBuffer.get(),
                           static_cast<char *>(alignedBuffer.get()) + bytesRead);
            totalRead += bytesRead;
        }

        // Only update cache if we read the entire file
        if (totalRead == fileSize)
        {
            // temporarily store the content in a string for move semantics
            std::string keyTemp(filePath);
            std::string mimeTemp(mimeType);
            cache->set(std::move(keyTemp), std::move(content),
                       std::move(mimeTemp), lastModified);

            Logger::getInstance()->info((
                "set cache: " + filePath +
                " cache size: " + std::to_string(cache->size()) +
                ", cache count: " + std::to_string(cache->count())));
        }
    }
}

class Router
{
public:
    explicit Router(const std::string &staticFolder)
        : staticFolder(staticFolder)
    {
        Logger::getInstance()->success("Router initialized with static folder: " +
                                       staticFolder);
    }
    void route(const std::string &path, int client_socket,
               const std::string &clientIp, Middleware *middleware, Cache *cache);

private:
    std::string staticFolder; // path to static files
    std::string
    getMimeType(const std::string &path); // get MIME type based on file extension
};

[[nodiscard]]
std::string Router::getMimeType(const std::string &path)
{
    // cache frequently used MIME types for quick access
    static const std::string TEXT_HTML = "text/html";
    static const std::string TEXT_PLAIN = "text/plain";
    static const std::string IMAGE_JPEG = "image/jpeg";

    // define sets of extensions that map to the same MIME type
    static const std::set<std::string_view> htmlExts = {".html", ".htm"};
    static const std::set<std::string_view> jpegExts = {".jpg", ".jpeg"};

    // define valid extensions set for quick validation
    static const std::set<std::string_view> validExts = {
        ".html", ".htm", ".css", ".js", ".json", ".png", ".jpg", ".jpeg",
        ".gif", ".svg", ".ico", ".txt", ".pdf", ".xml", ".zip", ".woff",
        ".woff2", ".ttf", ".eot", ".mp3", ".mp4", ".webm", ".webp"};

    // define MIME type mappings for validated extensions
    static const std::unordered_map<std::string_view, std::string_view>
        mimeTypes = {{".css", "text/css"},
                     {".js", "application/javascript"},
                     {".json", "application/json"},
                     {".png", "image/png"},
                     {".gif", "image/gif"},
                     {".svg", "image/svg+xml"},
                     {".ico", "image/x-icon"},
                     {".txt", "text/plain"},
                     {".pdf", "application/pdf"},
                     {".xml", "application/xml"},
                     {".zip", "application/zip"},
                     {".woff", "font/woff"},
                     {".woff2", "font/woff2"},
                     {".ttf", "font/ttf"},
                     {".eot", "application/vnd.ms-fontobject"},
                     {".mp3", "audio/mpeg"},
                     {".mp4", "video/mp4"},
                     {".webm", "video/webm"},
                     {".webp", "image/webp"}};

    // find last dot in path
    size_t dotPos = path.find_last_of('.');
    if (dotPos == std::string::npos)
    {
        return TEXT_PLAIN;
    }

    // extract extension and convert to lowercase using string_view
    // this avoids string allocation for the extension
    char lastChars[10]; // max extension length
    const size_t extLen = std::min(path.length() - dotPos, size_t(10));
    std::transform(path.begin() + dotPos, path.begin() + dotPos + extLen,
                   lastChars, ::tolower);
    std::string_view ext(lastChars, extLen);

    // quick validation check
    if (!validExts.contains(ext))
    {
        return TEXT_PLAIN;
    }

    // fast path for most common types
    if (htmlExts.contains(ext))
    {
        return TEXT_HTML;
    }
    if (jpegExts.contains(ext))
    {
        return IMAGE_JPEG;
    }

    // look up MIME type for other extensions
    auto it = mimeTypes.find(ext);
    return it != mimeTypes.end() ? std::string(it->second) : TEXT_PLAIN;
}

void Router::route(const std::string &path, int client_socket,
                   const std::string &clientIp, Middleware *middleware,
                   Cache *cache)
{
    // pre-allocate string capacity to avoid reallocation
    // +11 accounts for potential "/index.html" addition
    std::string filePath;
    filePath.reserve(staticFolder.size() + path.size() + 11);
    filePath = staticFolder;
    filePath += path;

    // normalize path to lowercase for case-insensitive comparison
    // using a separate string to preserve original path for logging
    std::string normalizedPath = path;
    std::transform(normalizedPath.begin(), normalizedPath.end(),
                   normalizedPath.begin(), ::tolower);

    // use string_view for efficient string operations
    // this avoids string copies during comparisons
    std::string_view pathView(normalizedPath);

    // cache filesystem check results to avoid redundant calls
    bool isDir = fs::is_directory(filePath);
    bool isIndex = (pathView == "/index.html" || pathView == "/" || isDir);
    bool isAsset = Http::isAssetRequest(normalizedPath);

    // append index.html for directory paths
    if (isDir)
    {
        filePath += "/index.html";
    }

    // log non-asset index requests
    if (!isAsset && isIndex)
    {
        Logger::getInstance()->info("Processing request: " + path, clientIp);
    }

    // cache for 404.html content
    // using static variables and std::once_flag for thread-safe initialization
    static std::string cached404Content;
    static std::once_flag flag404;

    // check if file exists and handle 404 errors
    if (!fs::exists(filePath) || (isDir && !fs::exists(filePath)))
    {
        // log warning for non-asset requests
        if (!isAsset)
        {
            Logger::getInstance()->warning("File not found: " + filePath, clientIp);
        }

        static bool has404File = false;
        // initialize 404.html content only once
        // this is thread-safe due to std::call_once
        std::call_once(flag404, [&]()
                       {
      std::ifstream file("404.html", std::ios::binary);
      if (file) {
        has404File = true;
        // efficient file reading using iterators
        cached404Content.assign(std::istreambuf_iterator<char>(file),
                                std::istreambuf_iterator<char>());
      } });

        // send simple 404 response if custom 404.html doesn't exist
        if (!has404File)
        {
            // static response for 404 errors
            // this avoids creating the string repeatedly
            static const char *SIMPLE_404_RESPONSE = "HTTP/1.1 404 Not Found\r\n"
                                                     "Content-Type: text/plain\r\n"
                                                     "Content-Length: 9\r\n"
                                                     "\r\n"
                                                     "Not Found";
            // use MSG_NOSIGNAL to prevent SIGPIPE
            send(client_socket, SIMPLE_404_RESPONSE, strlen(SIMPLE_404_RESPONSE),
                 MSG_NOSIGNAL);
            return;
        }

        // static response header for 404 errors
        // this is shared across all 404 responses
        static const std::string RESPONSE_404_HEADER = "HTTP/1.1 404 Not Found\r\n"
                                                       "Content-Type: text/html\r\n"
                                                       "Connection: close\r\n"
                                                       "Content-Length: ";

        // use writev for efficient response sending
        // this minimizes the number of system calls
        struct iovec iov[4]; // array of io vectors for writev
        std::string contentLength = std::to_string(cached404Content.size());
        static const char *CRLF = "\r\n\r\n";

        // set up io vectors for response components
        iov[0].iov_base = const_cast<char *>(RESPONSE_404_HEADER.data());
        iov[0].iov_len = RESPONSE_404_HEADER.size();
        iov[1].iov_base = const_cast<char *>(contentLength.data());
        iov[1].iov_len = contentLength.size();
        iov[2].iov_base = const_cast<char *>(CRLF);
        iov[2].iov_len = 4; // Length of "\r\n\r\n"
        iov[3].iov_base = const_cast<char *>(cached404Content.data());
        iov[3].iov_len = cached404Content.size();

        // send all components in a single system call
        if (writev(client_socket, iov, 4) == -1)
        {
            Logger::getInstance()->error("Failed to send 404 response: " +
                                             std::string(strerror(errno)),
                                         clientIp);
        }
        return;
    }

    // get mime type and send response
    // note: getmimetype is assumed to be case-insensitive
    std::string mimeType = getMimeType(filePath);

    // send the response using the optimized http::sendresponse method
    // the !isasset && isindex parameter determines whether to log the response
    Http::sendResponse(client_socket, filePath, mimeType, 200, clientIp,
                       !isAsset && isIndex, middleware, cache);
}

class Parser
{
public:
    static Config parseConfig(const std::string &configFilePath);
};

Config Parser::parseConfig(const std::string &configFilePath)
{
    // log start of configuration reading process
    Logger::getInstance()->info("Reading configuration from: " + configFilePath);

    // open  configuration file
    std::ifstream file(configFilePath);
    if (!file.is_open())
    {
        Logger::getInstance()->error("Could not open config file: " +
                                     configFilePath);
        throw std::runtime_error("Could not open config file!");
    }

    // parse JSON configuration
    json configJson;
    try
    {
        file >> configJson;
    }
    catch (const json::parse_error &e)
    {
        Logger::getInstance()->error("JSON parsing error: " +
                                     std::string(e.what()));
        throw std::runtime_error("Failed to parse configuration file");
    }

    // check if all required fields exist and are not null
    if (!configJson.contains("port") || configJson["port"].is_null() ||
        !configJson.contains("static_folder") ||
        configJson["static_folder"].is_null() ||
        !configJson.contains("thread_count") ||
        configJson["thread_count"].is_null() ||
        !configJson.contains("rate_limit") ||
        configJson["rate_limit"].is_null() ||
        !configJson["rate_limit"].contains("max_requests") ||
        configJson["rate_limit"]["max_requests"].is_null() ||
        !configJson["rate_limit"].contains("time_window") ||
        configJson["rate_limit"]["time_window"].is_null() ||
        !configJson.contains("cache") || configJson["cache"].is_null() ||
        !configJson["cache"].contains("size_mb") ||
        configJson["cache"]["size_mb"].is_null() ||
        !configJson["cache"].contains("max_age_seconds") ||
        configJson["cache"]["max_age_seconds"].is_null())
    {
        Logger::getInstance()->error("Configuration file is missing required "
                                     "fields or contains null values");
        throw std::runtime_error("Incomplete configuration file");
    }

    // create a Config object and populate it with values from JSON
    Config config;
    config.port = configJson["port"];
    config.staticFolder = configJson["static_folder"];
    config.threadCount = configJson["thread_count"];
    config.rateLimit.maxRequests = configJson["rate_limit"]["max_requests"];
    config.rateLimit.timeWindow = configJson["rate_limit"]["time_window"];
    config.cache.sizeMB = configJson["cache"]["size_mb"].get<size_t>();
    config.cache.maxAgeSeconds =
        configJson["cache"]["max_age_seconds"].get<int>();

    // validate port number
    if (config.port <= 0 || config.port > 65535)
    {
        Logger::getInstance()->error("Invalid port number: " +
                                     std::to_string(config.port));
        throw std::runtime_error("Invalid port number");
    }

    // validate static folder path
    if (!fs::exists(config.staticFolder))
    {
        Logger::getInstance()->error("Static folder does not exist: " +
                                     config.staticFolder);
        throw std::runtime_error("Invalid static folder path");
    }

    // validate thread count
    if (config.threadCount <= 0 || config.threadCount > 1000)
    {
        Logger::getInstance()->error("Invalid thread count: " +
                                     std::to_string(config.threadCount));
        throw std::runtime_error("Invalid thread count");
    }

    // validate maximum requests for rate limiting
    if (config.rateLimit.maxRequests <= 0)
    {
        Logger::getInstance()->error("Invalid max requests for rate limiting: " +
                                     std::to_string(config.rateLimit.maxRequests));
        throw std::runtime_error("Invalid max requests for rate limiting");
    }

    // validate time window for rate limiting
    if (config.rateLimit.timeWindow <= 0)
    {
        Logger::getInstance()->error("Invalid time window for rate limiting: " +
                                     std::to_string(config.rateLimit.timeWindow));
        throw std::runtime_error("Invalid time window for rate limiting");
    }

    // validate cache size
    if (config.cache.sizeMB == 0 ||
        config.cache.sizeMB >
            std::numeric_limits<size_t>::max() / (1024 * 1024))
    {
        Logger::getInstance()->error(
            "Invalid cache size: " + std::to_string(config.cache.sizeMB) + " MB");
        throw std::runtime_error("Invalid cache size");
    }

    // validate cache max age
    if (config.cache.maxAgeSeconds <= 0)
    {
        Logger::getInstance()->error(
            "Invalid cache max age: " + std::to_string(config.cache.maxAgeSeconds) +
            " seconds");
        throw std::runtime_error("Invalid cache max age");
    }

    // log successful configuration loading
    Logger::getInstance()->success("Configuration loaded successfully");

    // return populated and validated Config object
    return config;
}

class EpollWrapper
{
public:
    // constructor with minimal but essential error handling
    inline EpollWrapper() : epoll_fd(epoll_create1(EPOLL_CLOEXEC))
    {
        if (epoll_fd == -1)
        {
            throw std::system_error(errno, std::system_category(),
                                    "epoll_create1 failed");
        }
    }

    inline ~EpollWrapper() noexcept
    {
        if (epoll_fd != -1)
            close(epoll_fd);
    }

    // delete copy operations
    EpollWrapper(const EpollWrapper &) = delete;
    EpollWrapper &operator=(const EpollWrapper &) = delete;

    /*
    decide to use inline functions for performance
    but who knows, the compiler might ignore it
    */

    // optimized move operations
    inline EpollWrapper(EpollWrapper &&other) noexcept
        : epoll_fd(other.epoll_fd)
    {
        other.epoll_fd = -1;
    }

    inline EpollWrapper &
    operator=(EpollWrapper &&other) noexcept // move assignment operator
    {
        if (this != &other)
        {
            if (epoll_fd != -1)
                close(epoll_fd);
            epoll_fd = std::exchange(other.epoll_fd, -1);
        }
        return *this;
    }

    [[nodiscard]] inline int get() const noexcept // get epoll file descriptor
    {
        return epoll_fd;
    }

    [[nodiscard]] inline int wait(struct epoll_event *events, int maxEvents,
                                  int timeout) noexcept
    {
        return epoll_wait(epoll_fd, events, maxEvents, timeout);
    }

    inline bool add(int fd, uint32_t events) noexcept // add fd to epoll set with
                                                      // optimized error handling
    {

        struct epoll_event ev;
        ev.events = events;
        ev.data.fd = fd;
        return epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) == 0;
    }

    inline bool modify(int fd,
                       uint32_t events) noexcept // modify fd in epoll set with
                                                 // optimized error handling
    {
        struct epoll_event ev;
        ev.events = events;
        ev.data.fd = fd;
        return epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev) == 0;
    }

    inline bool remove(
        int fd) noexcept // remove fd from epoll set with optimized error handling
    {

        return epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr) == 0;
    }

    template <typename Iterator>
    inline size_t batch_operation(Iterator begin, Iterator end, int op,
                                  uint32_t events = 0) noexcept
    {
        size_t success_count = 0;
        struct epoll_event ev;
        ev.events = events;

        for (Iterator it = begin; it != end; ++it)
        {
            const int fd = *it; // get file descriptor
            ev.data.fd = fd;
            if (epoll_ctl(epoll_fd, op, fd, (op == EPOLL_CTL_DEL) ? nullptr : &ev) ==
                0)
            {
                ++success_count;
            }
        }
        return success_count;
    }

    [[nodiscard]] inline bool
    is_monitored(int fd) const noexcept // check if fd is monitored by epoll
    {
        struct epoll_event ev;
        return epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev) != -1 || errno != ENOENT;
    }

private:
    int epoll_fd; // epoll file descriptor

    // Static assertions for compile-time checks
    static_assert(
        sizeof(int) >= sizeof(void *) ||
            sizeof(int64_t) >=
                sizeof(void *), // check if int or int64_t can store a pointer
        "Platform must support storing pointers in epoll_data");
};

class Server
{
public:
    Server(int port, const std::string &staticFolder, int threadCount,
           int maxRequests, int timeWindow, int cacheSizeMB, int maxAgeSeconds);
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
    std::atomic<bool> shouldStop{false};       // atomic flag to stop server

    void handleClient(int client_socket, const std::string &clientIp);
    void closeConnection(int client_socket);
    void logRequest(int client_socket, const std::string &message);
};

Server::Server(int port, const std::string &staticFolder, int threadCount,
               int maxRequests, int timeWindow, int cacheSizeMB,
               int maxAgeSeconds)
    : socket(port), router(staticFolder), pool(threadCount), epoll(),
      rateLimiter(maxRequests, std::chrono::seconds(timeWindow)),
      cache(cacheSizeMB, std::chrono::seconds(maxAgeSeconds))
{
    Logger::getInstance()->step(
        1, "Initializing server components...");
    std::ostringstream oss;
    oss << "Creating dual-stack server on port: " << port
        << "\n   static folder: " << staticFolder
        << "\n   thread count: " << threadCount << ", rate limit: " << maxRequests
        << " requests per " << timeWindow << " seconds"
        << "\n   cache size: " << cacheSizeMB << "MB"
        << ", cache max age: " << maxAgeSeconds << " seconds";

    Logger::getInstance()->info(oss.str());
    Logger::getInstance()->step(2, "Binding socket...");
    socket.bind();
    Logger::getInstance()->step(3, "Listening on socket...");
    socket.listen();
}

void Server::start()
{

    try
    {
        // pre-allocate events array with optimal size
        static constexpr size_t MAX_EVENTS =
            1024; // again i don't know should i make it as a config or not
        struct epoll_event events[MAX_EVENTS];

        // Efficient timeout tracking with pre-allocated vector
        std::vector<std::chrono::steady_clock::time_point> lastActivityTimes(1024);
        static constexpr auto IDLE_TIMEOUT = std::chrono::seconds(30);
        auto lastTimeoutCheck = std::chrono::steady_clock::now();

        // add server socket to epoll
        epoll.add(socket.getSocketFd(), EPOLLIN);

        while (!shouldStop)
        {
            // use shorter timeout for better responsiveness
            int nfds = epoll.wait(events, MAX_EVENTS,
                                  100); //(another parameter i don't know should i
                                        // make it as a config or not)

            // Only check timeouts when system load is not high
            auto now = std::chrono::steady_clock::now();
            if (static_cast<size_t>(nfds) < MAX_EVENTS &&
                now - lastTimeoutCheck > std::chrono::seconds(30))
            {
                std::vector<int> timeoutSockets;
                timeoutSockets.reserve(32);

                {
                    std::lock_guard<std::mutex> lock(connectionsMutex);
                    for (const auto &[fd, info] : connections)
                    {
                        if (static_cast<size_t>(fd) < lastActivityTimes.size() &&
                            now - lastActivityTimes[fd] > IDLE_TIMEOUT)
                        {
                            timeoutSockets.push_back(fd);
                        }
                    }
                }

                for (int fd : timeoutSockets)
                {
                    closeConnection(fd);
                }

                lastTimeoutCheck = now;
            }

            if (nfds == -1)
            {
                if (errno == EINTR)
                    continue;
                Logger::getInstance()->error("Epoll wait failed: " +
                                             std::string(strerror(errno)));
                break;
            }

            for (int i = 0; i < nfds; ++i)
            {
                if (events[i].data.fd == socket.getSocketFd())
                {
                    // handle new connection
                    std::string clientIp;
                    int client_socket = socket.acceptConnection(clientIp);
                    if (client_socket < 0)
                        continue;

                    // add connection info under lock
                    {
                        std::lock_guard<std::mutex> lock(connectionsMutex);
                        connections.emplace(client_socket,
                                            ConnectionInfo{now, // reuse timestamp
                                                           clientIp, false, false, 0, 0});

                        if (static_cast<size_t>(client_socket) < lastActivityTimes.size())
                        {
                            lastActivityTimes[client_socket] = now;
                        }
                        else
                        {
                            lastActivityTimes.resize(client_socket + 1024, now);
                        }
                    }

                    try
                    {
                        // add to epoll with edge-triggered mode
                        epoll.add(client_socket, EPOLLIN | EPOLLET);
                    }
                    catch (const std::exception &e)
                    {
                        Logger::getInstance()->error(
                            "Failed to add client socket to epoll: " +
                            std::string(e.what()));
                        closeConnection(client_socket);
                        continue;
                    }
                }
                else
                {
                    // handle existing connection
                    int client_socket = events[i].data.fd;
                    std::string clientIp;

                    // get client IP under lock
                    {
                        std::lock_guard<std::mutex> lock(connectionsMutex);
                        auto it = connections.find(client_socket);
                        if (it != connections.end())
                        {
                            clientIp = it->second.ip;
                            if (static_cast<size_t>(client_socket) <
                                lastActivityTimes.size())
                            {
                                lastActivityTimes[client_socket] = now;
                            }
                        }
                    }

                    // enqueue client handling task
                    if (!clientIp.empty())
                    {
                        pool.enqueue([this, client_socket, clientIp]
                                     { handleClient(client_socket, clientIp); });
                    }
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

    for (int sock : socketsToClose)
    {
        closeConnection(sock);
    }

    Logger::getInstance()->info("All connections closed");
}

void Server::handleClient(int client_socket, const std::string &clientIp)
{
    std::vector<char> buffer(1024); // initialize buffer for reading client data
    ssize_t valread;                // variable to store number of bytes read
    std::string request;            // string to accumulate complete client request
    bool connectionClosed = false;  // flag to track if connection has been closed

    // read data from client in a loop
    while ((valread = read(client_socket, buffer.data(), buffer.size())) > 0)
    {
        // check if server should stop and while reading data
        if (shouldStop)
        {
            closeConnection(client_socket);
            return;
        }

        // append read data to request string
        request.append(buffer.data(), valread);

        // update bytes received for this connection
        std::lock_guard<std::mutex> lock(connectionsMutex);
        auto it = connections.find(client_socket);
        if (it != connections.end())
        {
            it->second.bytesReceived += valread;
        }
    }

    // check if connection has been closed by client
    if (valread == 0 ||
        (valread < 0 && errno != EAGAIN && errno != EWOULDBLOCK))
    {
        closeConnection(client_socket);
        connectionClosed = true;
    }

    // process request if connection is still open and we have received data
    if (!connectionClosed && !request.empty())
    {
        std::string path = Http::getRequestPath(request); // extract request path
        bool isAsset = Http::isAssetRequest(path);        // check if it's an asset request

        // log non-asset requests
        if (!isAsset)
        {
            logRequest(client_socket, "Processing request: " + path);
        }

        // apply rate limiting to request
        std::string processedRequest = rateLimiter.process(request);

        // check if request was rate limited
        if (processedRequest == "HTTP/1.1 429 Too Many Requests\r\n"
                                "Content-Type: text/plain\r\n"
                                "Content-Length: 19\r\n"
                                "\r\n"
                                "Too Many Requests")
        {
            // send rate limit response to client
            send(client_socket, processedRequest.c_str(), processedRequest.size(), 0);
        }
        else
        {
            // create a compression middleware instance
            Compression compressionMiddleware;
            // route request with compression middleware
            router.route(path, client_socket, clientIp, &compressionMiddleware,
                         &cache);
        }

        // log completion of non-asset requests
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

            for (const auto &message :
                 it->second.logBuffer) // log all buffered messages first
            {
                Logger::getInstance()->info(message, it->second.ip);
            }

            Logger::getInstance()->info(
                "Connection closed - Duration: " + durationStr +
                    ", Bytes received: " + std::to_string(it->second.bytesReceived) +
                    ", Bytes sent: " + std::to_string(it->second.bytesSent),
                it->second.ip); // always log connection closure

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
        Logger::getInstance()->warning(
            "Failed to remove client socket from epoll: " + std::string(e.what()));
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

std::atomic<bool> running(true); // flag to control main loop

void signalHandler(int signum)
{
    running = false; // set running flag to false
}

auto main(void) -> int
{
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    try
    {
        Config config =
            Parser::parseConfig("pgs_conf.json"); // parse configuration file
        server = std::make_unique<Server>(
            config.port, config.staticFolder, config.threadCount,
            config.rateLimit.maxRequests, config.rateLimit.timeWindow,
            config.cache.sizeMB,
            config.cache.maxAgeSeconds); // create server instance

        std::thread serverThread(
            [&]()
            { server->start(); }); // start server in a separate thread

        while (running)
        {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        std::thread shutdownThread([&]()
                                   { server->stop(); }); // initiate server shutdown in a separate thread

        if (shutdownThread.joinable())
            shutdownThread.join();
        else
            Logger::getInstance()->error("Failed to join shutdown thread");

        if (serverThread.joinable()) // join server thread
            serverThread.join();
        else
            Logger::getInstance()->error("Failed to join server thread");
    }
    catch (const std::exception &e)
    {
        Logger::getInstance()->error("Fatal error: " + std::string(e.what()));
        return EXIT_FAILURE;
    }

    Logger::getInstance()->info("Server shut down successfully");
    server.reset();            // explicitly destroy server object
    Logger::destroyInstance(); // destroy logger instance

    return EXIT_SUCCESS;
}