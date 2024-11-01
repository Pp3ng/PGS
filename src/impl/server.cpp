#include "server.hpp"
#include "compression.hpp"

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
        if (!epoll.add(socket.getSocketFd(), EPOLLIN))
        {
            Logger::getInstance()->error("Failed to add server socket to epoll");
            return;
        }

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
                        if (!epoll.add(client_socket, EPOLLIN | EPOLLET))
                        {
                            Logger::getInstance()->error("Failed to add client socket to epoll");
                            closeConnection(client_socket);
                            continue;
                        }
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

    // stop accepting new connections
    socket.closeSocket();

    // stop thread pool
    pool.stop();

    // close all existing connections
    std::vector<int> socketsToClose;
    socketsToClose.reserve(connections.size()); // pre-allocate

    {
        std::lock_guard<std::mutex> lock(connectionsMutex);
        for (const auto &[client_socket, _] : connections)
        {
            socketsToClose.push_back(client_socket);
        }
    }

    // close connections in parallel for large number of connections
    static constexpr size_t PARALLEL_THRESHOLD = 1000;
    if (socketsToClose.size() > PARALLEL_THRESHOLD)
    {
        const size_t num_threads = std::thread::hardware_concurrency();
        std::vector<std::thread> threads;
        threads.reserve(num_threads);

        const size_t chunk_size = socketsToClose.size() / num_threads;
        for (size_t i = 0; i < num_threads; ++i)
        {
            size_t startPos = i * chunk_size;
            size_t end = (i == num_threads - 1) ? socketsToClose.size() : (i + 1) * chunk_size;

            threads.emplace_back([this, &socketsToClose, startPos, end]()
                                 {
                for (size_t j = startPos; j < end; ++j) {
                    closeConnection(socketsToClose[j]);
                } });
        }

        for (auto &thread : threads)
        {
            thread.join();
        }
    }
    else
    {
        // sequential processing for small number of connections
        for (int sock : socketsToClose)
        {
            closeConnection(sock);
        }
    }

    Logger::getInstance()->info("All connections closed");
}

void Server::handleClient(int client_socket, const std::string &clientIp)
{
    // 1024 bytes is a good balance between memory usage and performance
    std::vector<char> buffer(1024);

    ssize_t valread;               // number of bytes read in each operation
    std::string request;           // accumulates the complete client request
    bool connectionClosed = false; // tracks if the connection has been closed

    // read data from client in chunks using non-blocking recv
    while ((valread = recv(client_socket, buffer.data(), buffer.size(), MSG_DONTWAIT)) > 0)
    {
        // check if server should stop during data reading
        if (shouldStop)
        {
            closeConnection(client_socket);
            return;
        }

        // append the received data to the request string
        // string will grow automatically as needed
        request.append(buffer.data(), valread);

        // update connection statistics under lock
        // using minimal lock scope for better performance
        std::lock_guard<std::mutex> lock(connectionsMutex);
        auto it = connections.find(client_socket);
        if (it != connections.end())
        {
            it->second.bytesReceived += valread;
        }
    }

    // handle connection closure and errors
    if (valread == 0)
    {
        // connection closed by client (normal closure)
        closeConnection(client_socket);
        connectionClosed = true;
    }
    else if (valread < 0)
    {
        // check for expected non-blocking errors
        // EAGAIN/EWOULDBLOCK: No data available
        // EINTR: Interrupted by signal
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
        {
            closeConnection(client_socket);
            connectionClosed = true;
        }
    }

    // process the request if connection is still open and we have data
    if (!connectionClosed && !request.empty())
    {
        // extract request path and check if it's an asset request
        std::string path = Http::getRequestPath(request);
        bool isAsset = Http::isAssetRequest(path);

        // log only non-asset requests to reduce log volume
        if (!isAsset)
        {
            Logger::getInstance()->info("Processing request: " + path, clientIp);
        }

        // apply rate limiting to the request
        std::string processedRequest = rateLimiter.process(request);

        // check if request was rate limited
        if (processedRequest == "HTTP/1.1 429 Too Many Requests\r\n"
                                "Content-Type: text/plain\r\n"
                                "Content-Length: 19\r\n"
                                "\r\n"
                                "Too Many Requests")
        {
            // send rate limit response using MSG_NOSIGNAL to prevent SIGPIPE
            send(client_socket, processedRequest.c_str(),
                 processedRequest.size(), MSG_NOSIGNAL);
        }
        else
        {
            // process the request through compression middleware and router
            Compression compressionMiddleware;
            router.route(path, client_socket, clientIp,
                         &compressionMiddleware, &cache);
        }

        // log completion for non-asset requests
        if (!isAsset)
        {
            Logger::getInstance()->info("Request completed: " + path, clientIp);
        }
    }
}

void Server::closeConnection(int client_socket)
{
    std::lock_guard<std::mutex> lock(connectionsMutex);
    auto it = connections.find(client_socket);
    if (it != connections.end())
    {
        if (!it->second.isClosureLogged)
        {
            auto duration = std::chrono::steady_clock::now() - it->second.startTime;
            std::string durationStr = Socket::durationToString(duration);

            Logger::getInstance()->info(
                "Connection closed - Duration: " + durationStr +
                    ", Bytes received: " + std::to_string(it->second.bytesReceived) +
                    ", Bytes sent: " + std::to_string(it->second.bytesSent),
                it->second.ip);

            it->second.isClosureLogged = true;
        }
        connections.erase(it);
    }

    if (!epoll.remove(client_socket))
    {
        Logger::getInstance()->error("Failed to remove client socket from epoll");
    }

    close(client_socket);
}

void Server::startCacheCleanup()
{
    cacheCleanupThread = std::thread([this]()
                                     {
        pthread_setname_np(pthread_self(), "cache-cleanup");
        
        // set higher priority for cache cleanup thread
        if (nice(10) == -1) {
            Logger::getInstance()->warning("Failed to set cache cleanup thread priority");
        }

        std::mutex mtx;
        std::condition_variable cv;

        while (!shouldStop) {
            cache.periodicCleanup();
            
            {
                std::unique_lock<std::mutex> lock(mtx);
                cv.wait_for(lock, CACHE_CLEANUP_INTERVAL, [this]() { 
                    return shouldStop.load(); 
                });
            }
        } });

    // set to the last CPU core
    int numCores = std::thread::hardware_concurrency();
    if (numCores > 0)
    {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(numCores - 1, &cpuset);
        int rc = pthread_setaffinity_np(cacheCleanupThread.native_handle(),
                                        sizeof(cpu_set_t), &cpuset);
        if (rc != 0)
        {
            Logger::getInstance()->warning("Failed to set cache cleanup thread affinity");
        }
    }
}