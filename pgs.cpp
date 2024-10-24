#include <iostream>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <thread>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <nlohmann/json.hpp> // Using nlohmann/json library to parse JSON

namespace fs = std::filesystem;
using json = nlohmann::json;

// Define structure for static file directory and port number
struct Config
{
    int port;
    std::string staticFolder;
};

class ThreadPool
{
public:
    ThreadPool(size_t numThreads);
    ~ThreadPool();
    void enqueue(std::function<void()> task);

private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex queueMutex;
    std::condition_variable condition;
    bool stop;

    void workerThread();
};

ThreadPool::ThreadPool(size_t numThreads) : stop(false)
{
    for (size_t i = 0; i < numThreads; ++i)
    {
        workers.emplace_back(&ThreadPool::workerThread, this);
    }
}

ThreadPool::~ThreadPool()
{
    {
        std::unique_lock<std::mutex> lock(queueMutex);
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
                           { return stop || !tasks.empty(); });
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
    int acceptConnection();
    int getSocketFd() const;

private:
    int server_fd;
    int port;
};

Socket::Socket(int port) : port(port)
{
    std::cout << "Creating socket on port: " << port << std::endl; // Print port
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == 0)
    {
        throw std::runtime_error("Socket creation failed!");
    }
}

Socket::~Socket()
{
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
        throw std::runtime_error("Bind failed!");
    }
}

void Socket::listen()
{
    ::listen(server_fd, 3);
}

int Socket::acceptConnection()
{
    struct sockaddr_in address;
    socklen_t addrlen = sizeof(address);
    return accept(server_fd, (struct sockaddr *)&address, &addrlen);
}

int Socket::getSocketFd() const
{
    return server_fd;
}

class Http
{
public:
    static std::string getRequestPath(const std::string &request);
    static void sendResponse(int client_socket, const std::string &content, const std::string &mimeType, int statusCode);
};

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

void Http::sendResponse(int client_socket, const std::string &content, const std::string &mimeType, int statusCode)
{
    std::ostringstream response;
    response << "HTTP/1.1 " << statusCode << " OK\r\n"
             << "Content-Type: " << mimeType << "\r\n"
             << "Content-Length: " << content.size() << "\r\n"
             << "\r\n"
             << content;
    send(client_socket, response.str().c_str(), response.str().size(), 0);
}

class Router
{
public:
    Router(const std::string &staticFolder) : staticFolder(staticFolder) {}
    void route(const std::string &path, int client_socket);

private:
    std::string staticFolder;
    std::string getMimeType(const std::string &path);
    std::string readFileContent(const std::string &filePath);
};

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

std::string Router::readFileContent(const std::string &filePath)
{
    std::ifstream file(filePath);
    if (!file.is_open())
    {
        return "";
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

void Router::route(const std::string &path, int client_socket)
{
    std::string filePath = staticFolder + path;

    // If the request path is a directory, return index.html by default
    if (fs::is_directory(filePath))
    {
        filePath += "/index.html";
    }

    // Check if the file exists
    if (!fs::exists(filePath) || fs::is_directory(filePath))
    {
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
        std::string content = readFileContent(filePath);
        std::string mimeType = getMimeType(filePath);
        Http::sendResponse(client_socket, content, mimeType, 200);
    }

    close(client_socket);
}

class Parser
{
public:
    static Config parseConfig(const std::string &configFilePath);
};

Config Parser::parseConfig(const std::string &configFilePath)
{
    std::ifstream file(configFilePath);
    if (!file.is_open())
    {
        throw std::runtime_error("Could not open config file!");
    }

    json configJson;
    file >> configJson;

    Config config;
    config.port = configJson["port"];
    config.staticFolder = configJson["static_folder"];
    return config;
}

class Server
{
public:
    Server(int port, const std::string &staticFolder);
    void start();

private:
    Socket socket;
    Router router;
    ThreadPool pool;
    void handleClient(int client_socket);
};

Server::Server(int port, const std::string &staticFolder)
    : socket(port), router(staticFolder), pool(4)
{
    socket.bind();
    socket.listen();
}

void Server::handleClient(int client_socket)
{
    char buffer[1024] = {0};
    read(client_socket, buffer, 1024);

    std::string request(buffer);
    std::string path = Http::getRequestPath(request); // Get request path

    router.route(path, client_socket); // Handle request based on path
}

void Server::start()
{
    std::cout << "Server is listening on port " << socket.getSocketFd() << "..." << std::endl;

    int epoll_fd = epoll_create1(0);
    if (epoll_fd == -1)
    {
        perror("epoll_create1");
        exit(EXIT_FAILURE);
    }

    struct epoll_event ev, events[10];
    ev.events = EPOLLIN;
    ev.data.fd = socket.getSocketFd();
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, socket.getSocketFd(), &ev) == -1)
    {
        perror("epoll_ctl: listen_sock");
        exit(EXIT_FAILURE);
    }

    while (true)
    {
        std::cout << "Waiting for events..." << std::endl;
        int nfds = epoll_wait(epoll_fd, events, 10, -1);
        if (nfds == -1)
        {
            perror("epoll_wait");
            exit(EXIT_FAILURE);
        }

        for (int i = 0; i < nfds; ++i)
        {
            if (events[i].data.fd == socket.getSocketFd())
            {
                std::cout << "New connection..." << std::endl;
                int new_socket = socket.acceptConnection();
                if (new_socket == -1)
                {
                    perror("accept");
                    continue;
                }
                ev.events = EPOLLIN;
                ev.data.fd = new_socket;
                if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, new_socket, &ev) == -1)
                {
                    perror("epoll_ctl: new_socket");
                    close(new_socket);
                    continue;
                }
            }
            else
            {
                std::cout << "Handling client..." << std::endl;
                pool.enqueue([this, events, i]()
                             { this->handleClient(events[i].data.fd); });
            }
        }
    }
}

int main()
{
    try
    {
        Config config = Parser::parseConfig("config.json"); // Read configuration
        Server server(config.port, config.staticFolder);    // Initialize server
        server.start();                                     // Start server
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}