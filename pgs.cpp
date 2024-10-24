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
#include "include/terminal_utils.h"

namespace fs = std::filesystem; // Define alias for filesystem
using json = nlohmann::json;    // Define alias for JSON

// Define structure for static file directory and port number
struct Config
{
    int port;                 // Port for server
    std::string staticFolder; // Directory for static files
    int threadCount;          // Number of threads in thread pool
};
class Logger
{
private:
    std::mutex logMutex;
    std::ofstream logFile;
    static Logger *instance;

    Logger()
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
    static Logger *getInstance()
    {
        if (instance == nullptr)
        {
            instance = new Logger();
        }
        return instance;
    }

    ~Logger()
    {
        if (logFile.is_open())
        {
            logFile.close();
        }
    }

    void log(const std::string &message, const std::string &level = "INFO", const std::string &ip = "-") // Add ip parameter
    {
        std::lock_guard<std::mutex> lock(logMutex);

        std::string logMessage = getTimestamp() + " [" + level + "] [" + ip + "] " + message; // Add ip to log message

        // Write to file
        logFile << logMessage << std::endl;
        logFile.flush(); // Ensure immediate write to disk

        // Print to console with color
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

Logger *Logger::instance = nullptr;

class ThreadPool
{
public:
    ThreadPool(size_t numThreads);
    ~ThreadPool();
    void enqueue(std::function<void()> task);

private:
    std::vector<std::thread> workers;        // worker threads
    std::queue<std::function<void()>> tasks; // task queue
    std::mutex queueMutex;                   // mutex
    std::condition_variable condition;       // condition variable
    bool stop;                               // stop flag

    void workerThread();
};

ThreadPool::ThreadPool(size_t numThreads) : stop(false)
{
    std::string boldNumThreads = "\033[1m" + std::to_string(numThreads) + " threads" + "\033[0m"; // bold text
    Logger::getInstance()->info("Initializing thread pool with " + boldNumThreads);
    for (size_t i = 0; i < numThreads; ++i)
    {
        workers.emplace_back(&ThreadPool::workerThread, this);
    }
}

ThreadPool::~ThreadPool()
{
    Logger::getInstance()->info("Shutting down thread pool");
    {
        std::unique_lock<std::mutex> lock(queueMutex); // lock the queue
        stop = true;
    }
    condition.notify_all();
    for (std::thread &worker : workers)
    {
        worker.join();
    }
}

void ThreadPool::enqueue(std::function<void()> task)
{
    {
        std::unique_lock<std::mutex> lock(queueMutex);
        tasks.push(std::move(task));
    }
    condition.notify_one();
}

void ThreadPool::workerThread()
{
    while (true)
    {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            condition.wait(lock, [this]
                           { return stop || !tasks.empty(); }); // wait until there is a task
            if (stop && tasks.empty())
                return;
            task = std::move(tasks.front());
            tasks.pop();
        }
        task();
    }
}

class Socket
{
public:
    Socket(int port);
    ~Socket();
    void bind();
    void listen();
    int acceptConnection(std::string &clientIp);
    int getSocketFd() const;

private:
    int server_fd;
    int port;
};

Socket::Socket(int port) : port(port)
{
    Logger::getInstance()->info("Creating socket on port: " + std::to_string(port));
    server_fd = socket(AF_INET, SOCK_STREAM, 0); // Create a socket
    if (server_fd == 0)
    {
        Logger::getInstance()->error("Socket creation failed!");
        throw std::runtime_error("Socket creation failed!");
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
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt));

    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    if (::bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0)
    {
        Logger::getInstance()->error("Bind failed!");
        throw std::runtime_error("Bind failed!");
    }
    Logger::getInstance()->success("Socket successfully bound to port " + std::to_string(port));
}

void Socket::listen()
{
    ::listen(server_fd, 42); // 42 is the Ultimate Answer to the Ultimate Question of Life, the Universe, and Everything
    Logger::getInstance()->info("Server listening on port " + std::to_string(port));
}

[[nodiscard]]
int Socket::acceptConnection(std::string &clientIp)
{
    struct sockaddr_in address;
    socklen_t addrlen = sizeof(address);
    int new_socket = accept(server_fd, (struct sockaddr *)&address, &addrlen); // Accept connection

    if (new_socket >= 0)
    {
        clientIp = inet_ntoa(address.sin_addr); // Get client IP address
        Logger::getInstance()->success("New connection accepted from " + clientIp);

        // Set the new socket to non-blocking mode
        int flags = fcntl(new_socket, F_GETFL, 0); // Get the socket flags
        if (flags == -1)
        {
            Logger::getInstance()->error("Failed to get socket flags");
            close(new_socket);
            return -1;
        }
        if (fcntl(new_socket, F_SETFL, flags | O_NONBLOCK) == -1) // Set the socket to non-blocking mode
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
                             const std::string &mimeType, int statusCode, const std::string &clientIp);
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
    return request.substr(pos1 + 4, pos2 - (pos1 + 4)); // Extract the path from the request
}

void Http::sendResponse(int client_socket, const std::string &content,
                        const std::string &mimeType, int statusCode, const std::string &clientIp)
{
    std::ostringstream response;
    response << "HTTP/1.1 " << statusCode << " OK\r\n"
             << "Content-Type: " << mimeType << "\r\n"
             << "Content-Length: " << content.size() << "\r\n"
             << "\r\n"
             << content;
    send(client_socket, response.str().c_str(), response.str().size(), 0);

    Logger::getInstance()->info(
        "Response sent: status=" + std::to_string(statusCode) +
            ", size=" + std::to_string(content.size()) +
            ", type=" + mimeType,
        clientIp);
}

class Router
{
public:
    Router(const std::string &staticFolder) : staticFolder(staticFolder)
    {
        Logger::getInstance()->info("Router initialized with static folder: " + staticFolder);
    }
    void route(const std::string &path, int client_socket, const std::string &clientIp);

private:
    std::string staticFolder;
    std::string getMimeType(const std::string &path);
    std::string readFileContent(const std::string &filePath);
};

// Calculate MIME type based on file extension
[[nodiscard]]
std::string Router::getMimeType(const std::string &path)
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

[[nodiscard]]
std::string Router::readFileContent(const std::string &filePath)
{
    std::ifstream file(filePath);
    if (!file.is_open())
    {
        Logger::getInstance()->warning("Failed to open file: " + filePath);
        return "";
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

void Router::route(const std::string &path, int client_socket, const std::string &clientIp)
{
    std::string filePath = staticFolder + path;

    if (path == "/index.html")
    {
        Logger::getInstance()->info("Routing request: " + path, clientIp);
    }

    // If the request path is a directory, return index.html by default
    if (fs::is_directory(filePath))
    {
        filePath += "/index.html";
        if (path == "/index.html")
        {
            Logger::getInstance()->info("Directory requested, serving index.html", clientIp);
        }
    }

    // Check if the file exists
    if (!fs::exists(filePath) || fs::is_directory(filePath))
    {
        Logger::getInstance()->warning("File not found: " + filePath, clientIp);
        const char *response =
            "HTTP/1.1 404 Not Found\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: 9\r\n"
            "\r\n"
            "Not Found";
        send(client_socket, response, strlen(response), 0);
    }
    else
    {
        [[likely]]
        std::string content = readFileContent(filePath);
        std::string mimeType = getMimeType(filePath);
        Http::sendResponse(client_socket, content, mimeType, 200, clientIp);
    }

    // Close the client socket and log
    close(client_socket);
    if (path == "/index.html")
    {
        Logger::getInstance()->info("Connection closed", clientIp);
    }
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
    file >> configJson;

    Config config;
    config.port = configJson["port"];
    config.staticFolder = configJson["static_folder"];
    config.threadCount = configJson["thread_count"];

    Logger::getInstance()->success("Configuration loaded successfully");
    return config;
}

class Server
{
public:
    Server(int port, const std::string &staticFolder, int threadCount);
    void start();

private:
    Socket socket;   // Socket instance
    Router router;   // Router instance
    ThreadPool pool; // Thread pool instance
    void handleClient(int client_socket, const std::string &clientIp);
};

Server::Server(int port, const std::string &staticFolder, int threadCount)
    : socket(port), router(staticFolder), pool(threadCount)
{
    socket.bind();
    socket.listen();
}

void Server::handleClient(int client_socket, const std::string &clientIp)
{
    char buffer[1024] = {0};
    ssize_t valread = read(client_socket, buffer, 1024); // Read from the client socket
    if (valread < 0)
    {
        if (errno == EWOULDBLOCK || errno == EAGAIN) // Check if the read would block
        {
            Logger::getInstance()->warning("Non-blocking read would block", clientIp);
            return;
        }
        Logger::getInstance()->error("Failed to read from socket", clientIp);
        return;
    }
    std::string request(buffer);
    std::string path = Http::getRequestPath(request); // Extract the path from the request

    Logger::getInstance()->info("Received request for path: " + path, clientIp);

    router.route(path, client_socket, clientIp); // Route the request
}

void Server::start()
{
    Logger::getInstance()->info("Server starting up...");

    int epoll_fd = epoll_create1(0); // Create epoll instance
    if (epoll_fd == -1)
    {
        Logger::getInstance()->error("Failed to create epoll instance");
        perror("epoll_create1");
        exit(EXIT_FAILURE);
    }

    struct epoll_event ev, events[10]; // Define epoll event structure
    ev.events = EPOLLIN;
    ev.data.fd = socket.getSocketFd();
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, socket.getSocketFd(), &ev) == -1)
    {
        Logger::getInstance()->error("Failed to add listening socket to epoll");
        perror("epoll_ctl: listen_sock");
        close(epoll_fd);
        exit(EXIT_FAILURE);
    }

    while (true)
    {
        Logger::getInstance()->info("Waiting for events...");
        int nfds = epoll_wait(epoll_fd, events, 10, -1); // Wait for events
        if (nfds == -1)
        {
            Logger::getInstance()->error("Epoll wait failed");
            perror("epoll_wait");
            close(epoll_fd);
            exit(EXIT_FAILURE);
        }

        for (int i = 0; i < nfds; ++i)
        {
            if (events[i].data.fd == socket.getSocketFd()) // Check if the event is for the listening socket
            {
                std::string clientIp;
                int client_socket = socket.acceptConnection(clientIp); // Accept connection
                if (client_socket < 0)
                {
                    Logger::getInstance()->error("Failed to accept connection");
                    continue;
                }

                ev.events = EPOLLIN | EPOLLET; // Use edge-triggered mode
                ev.data.fd = client_socket;    // Add the client socket to epoll
                if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_socket, &ev) == -1)
                {
                    Logger::getInstance()->error("Failed to add client socket to epoll");
                    perror("epoll_ctl: client_socket");
                    close(client_socket); // Close the client socket on error
                    continue;
                }
            }
            else
            {
                pool.enqueue([this, client_socket = events[i].data.fd]() // Enqueue the client socket to the thread pool
                             {
                                 std::string clientIp;
                                 handleClient(client_socket, clientIp); // Handle client request
                                 close(client_socket);                  // Ensure the client socket is closed after handling
                             });
            }
        }
    }
}

int main()
{
    try
    {
        Config config = Parser::parseConfig("pgs_conf.json");                // Read configuration
        Server server(config.port, config.staticFolder, config.threadCount); // Initialize server
        server.start();                                                      // Start server
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
