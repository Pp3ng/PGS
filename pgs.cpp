#include <iostream>           // std::cout, std::cerr
#include <cstring>            // strlen()
#include <sys/socket.h>       // socket(), bind(), listen(), accept()
#include <arpa/inet.h>        // inet_ntoa
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
#include <chrono>             // For time measurement
#include <csignal>            // Signal handling
#include <atomic>             // Atomic bool flag
#include "include/terminal_utils.h"

namespace fs = std::filesystem; // Alias for filesystem namespace
using json = nlohmann::json;    // Alias for JSON namespace

struct Config
{
    int port;                 // prot for server
    std::string staticFolder; // path for static files
    int threadCount;          // thread count of worker threads
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
    std::mutex logMutex;   // Mutex to protect log file access
    std::ofstream logFile; // Log file stream
    static Logger *instance;
    bool isWaitingForEvents;                                                 // New flag to track event waiting state
    std::chrono::steady_clock::time_point lastEventWaitLog;                  // Track last event wait log time
    static constexpr auto EVENT_WAIT_LOG_INTERVAL = std::chrono::seconds(5); // Log interval for waiting events

    Logger() : isWaitingForEvents(false)
    {
        logFile.open("pgs.log", std::ios::app); // Open log file in append mode
        info("Logger initialized");
    }

    [[nodiscard]]
    std::string getTimestamp()
    {
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      now.time_since_epoch()) %
                  1000;

        char timestamp[32];
        std::strftime(timestamp, sizeof(timestamp), "[%Y-%m-%d %H:%M:%S]", std::localtime(&time)); // format timestamp

        std::string result = timestamp;
        result += "." + std::to_string(ms.count());
        return result;
    }

public:
    static Logger *getInstance() // Singleton pattern
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

    ~Logger()
    {
        if (logFile.is_open())
        {
            logFile.close();
        }
    }

    void log(const std::string &message, const std::string &level = "INFO", const std::string &ip = "-")
    {
        // Skip duplicate "Waiting for events" messages
        if (message == "Waiting for events...")
        {
            std::lock_guard<std::mutex> lock(logMutex);
            auto now = std::chrono::steady_clock::now();

            if (!isWaitingForEvents ||
                (now - lastEventWaitLog) >= EVENT_WAIT_LOG_INTERVAL) // Check if interval has passed
            {
                isWaitingForEvents = true;
                lastEventWaitLog = now;
            }
            else
            {
                return; // Skip logging if we're already waiting and interval hasn't passed
            }
        }
        else
        {
            isWaitingForEvents = false; // Reset the waiting flag
        }

        std::lock_guard<std::mutex> lock(logMutex);
        std::string logMessage = getTimestamp() + " [" + level + "] [" + ip + "] " + message;

        logFile << logMessage << std::endl; // Write log message to file
        logFile.flush();                    // Flush the log file stream inmediately

        if (level == "ERROR")
        {
            std::cout << TerminalUtils::error(logMessage) << std::endl;
        }
        else if (level == "WARNING")
        {
            std::cout << TerminalUtils::warning(logMessage) << std::endl;
        }
        else if (level == "SUCCESS")
        {
            std::cout << TerminalUtils::success(logMessage) << std::endl;
        }
        else
        {
            std::cout << TerminalUtils::info(logMessage) << std::endl;
        }
    }

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

Logger *Logger::instance = nullptr; // Initialize the static singleton instance

class ThreadPool
{
public:
    ThreadPool(size_t numThreads);
    ~ThreadPool();
    void enqueue(std::function<void()> task);
    void stop();

private:
    std::vector<std::thread> workers;        // Worker threads
    std::queue<std::function<void()>> tasks; // Task queue
    std::mutex queueMutex;                   // Mutex to protect task queue
    std::condition_variable condition;       // Condition variable for task queue
    std::atomic<bool> stop_flag{false};      // Flag to stop the worker threads

    void workerThread();
};

ThreadPool::ThreadPool(size_t numThreads)
{
    for (size_t i = 0; i < numThreads; ++i)
    {
        workers.emplace_back(&ThreadPool::workerThread, this); // Create worker threads
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
                             const std::string &clientIp, bool isIndex = false);
    static bool isAssetRequest(const std::string &path)
    {
        static const std::vector<std::string> assetExtensions = {
            ".css", ".js", ".png", ".jpg", ".jpeg", ".gif", ".ico",
            ".svg", ".woff", ".woff2", ".ttf", ".eot", ".map",
            ".webp", ".pdf", ".mp4", ".webm", ".mp3", ".wav",
            ".json", ".xml"};

        static const std::vector<std::string> assetDirs = {
            "/assets/", "/static/", "/images/", "/img/",
            "/css/", "/js/", "/fonts/", "/media/"};

        // Check if the path ends with an asset extension
        for (const auto &ext : assetExtensions)
        {
            if (path.length() > ext.length() &&
                path.substr(path.length() - ext.length()) == ext)
            {
                return true;
            }
        }

        for (const auto &dir : assetDirs)
        {
            if (path.find(dir) != std::string::npos)
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
    if (pos1 == std::string::npos || pos2 == std::string::npos)
    {
        return "/";
    }
    return request.substr(pos1 + 4, pos2 - (pos1 + 4));
}

void Http::sendResponse(int client_socket, const std::string &content,
                        const std::string &mimeType, int statusCode,
                        const std::string &clientIp, bool isIndex)
{
    std::ostringstream headers;
    headers << "HTTP/1.1 " << statusCode << " OK\r\n"
            << "Content-Type: " << mimeType << "\r\n"
            << "Content-Length: " << content.size() << "\r\n"
            << "\r\n";

    std::string headerStr = headers.str();
    ssize_t headersSent = send(client_socket, headerStr.c_str(), headerStr.size(), 0); // Send the headers
    ssize_t contentSent = send(client_socket, content.c_str(), content.size(), 0);     // Send the content

    size_t totalSent = (headersSent > 0 ? headersSent : 0) +
                       (contentSent > 0 ? contentSent : 0); // Calculate total bytes sent

    if (isIndex)
    {
        Logger::getInstance()->info(
            "Response sent: status=" + std::to_string(statusCode) +
                ", size=" + std::to_string(totalSent) +
                ", type=" + mimeType,
            clientIp); // Log the response details
    }
}

class Router
{
public:
    Router(const std::string &staticFolder) : staticFolder(staticFolder)
    {
        Logger::getInstance()->info("Router initialized with static folder: " + staticFolder);
    }
    void route(const std::string &path, int client_socket, const std::string &clientIp);
    std::string getStaticFolder() const { return staticFolder; }

private:
    std::string staticFolder;                                               // path to static files
    std::string getMimeType(const std::string &path);                       // get MIME type based on file extension
    std::string readFileContent(const std::string &filePath);               // read file content
    bool readBinaryFile(const std::string &filePath, std::string &content); // read binary file content
};

[[nodiscard]]
std::string Router::getMimeType(const std::string &path) // calculate MIME type based on file extension
{
    if (path.rfind(".html") == path.length() - 5)
        return "text/html";
    if (path.rfind(".css") == path.length() - 4)
        return "text/css";
    if (path.rfind(".js") == path.length() - 3)
        return "application/javascript";
    if (path.rfind(".png") == path.length() - 4)
        return "image/png";
    if (path.rfind(".jpg") == path.length() - 4 || path.rfind(".jpeg") == path.length() - 5)
        return "image/jpeg";
    if (path.rfind(".gif") == path.length() - 4)
        return "image/gif";
    return "text/plain";
}

bool Router::readBinaryFile(const std::string &filePath, std::string &content)
{
    std::ifstream file(filePath, std::ios::binary | std::ios::ate); // Open file in binary mode and seek to the end
    if (!file.is_open())
    {
        return false;
    }

    std::streamsize size = file.tellg();
    if (size <= 0)
    {
        return false;
    }

    file.seekg(0, std::ios::beg);
    std::vector<char> buffer(size);

    if (!file.read(buffer.data(), size))
    {
        return false;
    }

    content.assign(buffer.data(), size);
    return true;
}

std::string Router::readFileContent(const std::string &filePath)
{
    std::string mimeType = getMimeType(filePath);
    bool isBinary = (mimeType.find("image/") != std::string::npos ||
                     mimeType.find("application/") != std::string::npos);

    if (isBinary)
    {
        std::string content;
        if (!readBinaryFile(filePath, content))
        {
            Logger::getInstance()->error("Failed to read binary file: " + filePath);
            return "";
        }
        return content;
    }

    // Text file handling
    std::ifstream file(filePath);
    if (!file.is_open())
    {
        return "";
    }

    try
    {
        std::stringstream buffer; // read file content into a string stream
        buffer << file.rdbuf();
        return buffer.str();
    }
    catch (const std::exception &e)
    {
        Logger::getInstance()->error("Failed to read file: " + filePath + " - " + e.what());
        return "";
    }
}

void Router::route(const std::string &path, int client_socket, const std::string &clientIp)
{
    std::string filePath = staticFolder + path;                                          // get full file path
    bool isIndex = (path == "/index.html" || path == "/" || fs::is_directory(filePath)); // check if it's an index request
    bool isAsset = Http::isAssetRequest(path);                                           // check if it's an asset request

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
        const char *response =
            "HTTP/1.1 404 Not Found\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: 9\r\n"
            "\r\n"
            "Not Found";
        send(client_socket, response, strlen(response), 0);
        return;
    }

    std::string content = readFileContent(filePath);
    if (content.empty())
    {
        if (!isAsset)
        {
            Logger::getInstance()->error("Failed to read file: " + filePath, clientIp);
        }
        const char *response =
            "HTTP/1.1 500 Internal Server Error\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: 21\r\n"
            "\r\n"
            "Internal Server Error";
        send(client_socket, response, strlen(response), 0); // send error response
        return;
    }

    std::string mimeType = getMimeType(filePath);
    Http::sendResponse(client_socket, content, mimeType, 200, clientIp, !isAsset && isIndex); // send response
}

class Parser
{
public:
    static Config parseConfig(const std::string &configFilePath);
};

Config Parser::parseConfig(const std::string &configFilePath)
{
    Logger::getInstance()->info("Reading configuration from: " + configFilePath);
    std::ifstream file(configFilePath);
    if (!file.is_open())
    {
        Logger::getInstance()->error("Could not open config file: " + configFilePath);
        throw std::runtime_error("Could not open config file!");
    }

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

    // Check if required fields exist and are not null
    if (!configJson.contains("port") || configJson["port"].is_null() ||
        !configJson.contains("static_folder") || configJson["static_folder"].is_null() ||
        !configJson.contains("thread_count") || configJson["thread_count"].is_null())
    {
        Logger::getInstance()->error("Configuration file is missing required fields or contains null values");
        throw std::runtime_error("Incomplete configuration file");
    }

    Config config;
    config.port = configJson["port"];
    config.staticFolder = configJson["static_folder"];
    config.threadCount = configJson["thread_count"];

    // Add robust configuration validation
    if (config.port <= 0 || config.port > 65535)
    {
        Logger::getInstance()->error("Invalid port number: " + std::to_string(config.port));
        throw std::runtime_error("Invalid port number");
    }
    if (!fs::exists(config.staticFolder))
    {
        Logger::getInstance()->error("Static folder does not exist: " + config.staticFolder);
        throw std::runtime_error("Invalid static folder path");
    }
    if (config.threadCount <= 0 || config.threadCount > 1000)
    {
        Logger::getInstance()->error("Invalid thread count: " + std::to_string(config.threadCount));
        throw std::runtime_error("Invalid thread count");
    }

    Logger::getInstance()->success("Configuration loaded successfully");
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
        struct epoll_event ev;
        ev.events = events;
        ev.data.fd = fd;
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) == -1) // add fd to epoll
        {
            throw std::runtime_error("Failed to add fd to epoll: " + std::string(strerror(errno)));
        }
    }

    void modify(int fd, uint32_t events)
    {
        struct epoll_event ev;
        ev.events = events;
        ev.data.fd = fd;
        if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev) == -1) // modify fd in epoll
        {
            throw std::runtime_error("Failed to modify fd in epoll: " + std::string(strerror(errno)));
        }
    }

    void remove(int fd)
    {
        if (epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr) == -1)
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
    Server(int port, const std::string &staticFolder, int threadCount);
    void start();
    void stop();

private:
    Socket socket;                             // server socket
    Router router;                             // server router instance
    ThreadPool pool;                           // server thread pool
    EpollWrapper epoll;                        // server epoll instance
    std::mutex connectionsMutex;               // mutex to protect connections map
    std::map<int, ConnectionInfo> connections; // map to store connection info
    std::atomic<bool> shouldStop{false};       // atomic flag to stop the server

    void handleClient(int client_socket, const std::string &clientIp);
    void closeConnection(int client_socket);
    void logRequest(int client_socket, const std::string &message);
};

Server::Server(int port, const std::string &staticFolder, int threadCount)
    : socket(port), router(staticFolder), pool(threadCount)
{
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
    std::vector<char> buffer(1024); // initialize buffer
    ssize_t valread;                // initialize valread
    std::string request;            // initialize request string
    bool connectionClosed = false;  // initialize connectionClosed flag

    while ((valread = read(client_socket, buffer.data(), buffer.size())) > 0)
    {
        if (shouldStop)
        {
            closeConnection(client_socket);
            return;
        }

        request.append(buffer.data(), valread);

        std::lock_guard<std::mutex> lock(connectionsMutex);
        auto it = connections.find(client_socket);
        if (it != connections.end())
        {
            it->second.bytesReceived += valread;
        }
    }

    if (valread == 0 || (valread < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) // check if connection is closed
    {
        closeConnection(client_socket);
        connectionClosed = true;
    }

    if (!connectionClosed && !request.empty()) // check if request is not empty
    {
        std::string path = Http::getRequestPath(request); // get request path
        bool isAsset = Http::isAssetRequest(path);        // check if it's an asset request

        if (!isAsset)
        {
            logRequest(client_socket, "Processing request: " + path);
        }

        router.route(path, client_socket, clientIp); // route request

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
        Config config = Parser::parseConfig("pgs_conf.json");                                    // parse configuration file
        server = std::make_unique<Server>(config.port, config.staticFolder, config.threadCount); // create server instance

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