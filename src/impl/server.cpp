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
    startCacheCleanup();
    try
    {
        // configure epoll
        static constexpr size_t MAX_EVENTS = 4096;
        static constexpr size_t BATCH_SIZE = 64;
        struct epoll_event events[MAX_EVENTS];

        // timeout tracking
        std::vector<std::chrono::steady_clock::time_point> lastActivityTimes(1024);
        static constexpr auto IDLE_TIMEOUT = std::chrono::seconds(20);
        static constexpr auto TIMEOUT_CHECK_INTERVAL = std::chrono::seconds(5);
        auto lastTimeoutCheck = std::chrono::steady_clock::now();

        // per batch connection handling
        std::vector<int> newConnections;
        std::vector<std::string> newConnectionIps;
        newConnections.reserve(BATCH_SIZE);
        newConnectionIps.reserve(BATCH_SIZE);

        if (!epoll.add(socket.getSocketFd(), EPOLLIN | EPOLLET))
        {
            Logger::getInstance()->error("Failed to add server socket to epoll");
            return;
        }

        while (!shouldStop)
        {

            int nfds = epoll.wait(events, MAX_EVENTS, 50);
            auto now = std::chrono::steady_clock::now();

            // banlance between performance and accuracy(when low load check more frequently)
            if ((static_cast<size_t>(nfds) < MAX_EVENTS / 2 &&
                 now - lastTimeoutCheck > TIMEOUT_CHECK_INTERVAL) ||
                now - lastTimeoutCheck > std::chrono::seconds(10))
            {
                std::vector<int> timeoutSockets;
                timeoutSockets.reserve(32);

                {
                    std::lock_guard<std::mutex> lock(connectionsMutex);
                    auto it = connections.begin();
                    while (it != connections.end())
                    {
                        if (now - it->second.startTime > IDLE_TIMEOUT)
                        {
                            timeoutSockets.push_back(it->first);
                            it = connections.erase(it);
                        }
                        else
                        {
                            ++it;
                        }
                    }
                }

                if (!timeoutSockets.empty())
                {
                    epoll.batch_operation(timeoutSockets.begin(), timeoutSockets.end(),
                                          EPOLL_CTL_DEL);

                    for (int fd : timeoutSockets)
                    {
                        close(fd);
                    }
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

            newConnections.clear();
            newConnectionIps.clear();

            for (int i = 0; i < nfds; ++i)
            {
                if (events[i].data.fd == socket.getSocketFd())
                {
                    // accept more when load is low
                    size_t maxAccepts = static_cast<size_t>(nfds) < MAX_EVENTS / 2 ? BATCH_SIZE : BATCH_SIZE / 2;

                    for (size_t j = 0; j < maxAccepts; ++j)
                    {
                        std::string clientIp;
                        int client_socket = socket.acceptConnection(clientIp);

                        if (client_socket < 0)
                        {
                            if (errno != EAGAIN && errno != EWOULDBLOCK)
                            {
                                Logger::getInstance()->error("Accept error: " +
                                                             std::string(strerror(errno)));
                            }
                            break;
                        }

                        newConnections.push_back(client_socket);
                        newConnectionIps.push_back(clientIp);
                    }

                    if (!newConnections.empty())
                    {
                        size_t added = epoll.batch_operation(
                            newConnections.begin(),
                            newConnections.end(),
                            EPOLL_CTL_ADD,
                            EPOLLIN | EPOLLET);

                        {
                            std::lock_guard<std::mutex> lock(connectionsMutex);
                            for (size_t j = 0; j < added; ++j)
                            {
                                connections.emplace(newConnections[j],
                                                    ConnectionInfo{now, newConnectionIps[j]});

                                if (static_cast<size_t>(newConnections[j]) < lastActivityTimes.size())
                                {
                                    lastActivityTimes[newConnections[j]] = now;
                                }
                                else
                                {
                                    lastActivityTimes.resize(newConnections[j] + 1024, now);
                                }
                            }
                        }

                        if (added != newConnections.size())
                        {
                            for (size_t j = added; j < newConnections.size(); ++j)
                            {
                                close(newConnections[j]);
                            }
                        }
                    }
                }
                else
                {
                    int client_socket = events[i].data.fd;
                    std::string clientIp;

                    {
                        std::lock_guard<std::mutex> lock(connectionsMutex);
                        auto it = connections.find(client_socket);
                        if (it != connections.end())
                        {
                            clientIp = it->second.ip;
                            if (static_cast<size_t>(client_socket) < lastActivityTimes.size())
                            {
                                lastActivityTimes[client_socket] = now;
                            }
                        }
                    }

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

    // stop accepting new connections first
    socket.closeSocket();

    // stop thread pool to prevent new tasks
    pool.stop();

    // join cache cleanup thread
    if (cacheCleanupThread.joinable())
    {
        cacheCleanupThread.join();
    }

    // collect all socket file descriptors
    std::vector<int> socketsToClose;
    {
        std::lock_guard<std::mutex> lock(connectionsMutex);
        socketsToClose.reserve(connections.size());
        for (const auto &[client_socket, _] : connections)
        {
            socketsToClose.push_back(client_socket);
        }
    }

    if (!socketsToClose.empty())
    {
        // batch remove all sockets from epoll first
        epoll.batch_operation(socketsToClose.begin(), socketsToClose.end(),
                              EPOLL_CTL_DEL);

        // use parallel processing for large number of connections
        static constexpr size_t PARALLEL_THRESHOLD = 32;
        if (socketsToClose.size() > PARALLEL_THRESHOLD)
        {
            // limit max threads to avoid resource exhaustion
            static constexpr size_t MAX_THREADS = 4;
            size_t num_threads = std::min(
                socketsToClose.size() / PARALLEL_THRESHOLD,
                MAX_THREADS);

            // prepare async tasks
            std::vector<std::future<void>> futures;
            futures.reserve(num_threads);

            // calculate chunk size for each thread
            size_t chunk_size = socketsToClose.size() / num_threads;
            for (size_t i = 0; i < num_threads; ++i)
            {
                size_t start = i * chunk_size;
                size_t end = (i == num_threads - 1) ? socketsToClose.size() : (i + 1) * chunk_size;

                // launch async task for each chunk
                futures.push_back(std::async(std::launch::async,
                                             [this, &socketsToClose, start, end]()
                                             {
                                                 // collect connection info in batch
                                                 std::vector<ConnectionInfo> infos;
                                                 infos.reserve(end - start);

                                                 {
                                                     std::lock_guard<std::mutex> lock(connectionsMutex);
                                                     for (size_t j = start; j < end; ++j)
                                                     {
                                                         auto it = connections.find(socketsToClose[j]);
                                                         if (it != connections.end())
                                                         {
                                                             infos.push_back(it->second);
                                                         }
                                                     }
                                                 }

                                                 // close sockets in batch
                                                 for (size_t j = start; j < end; ++j)
                                                 {
                                                     close(socketsToClose[j]);
                                                 }

                                                 // log connection info in batch
                                                 for (const auto &info : infos)
                                                 {
                                                     auto duration = std::chrono::steady_clock::now() -
                                                                     info.startTime;
                                                     Logger::getInstance()->info(
                                                         "Connection closed - Duration: " +
                                                             Socket::durationToString(duration) +
                                                             ", Bytes received: " +
                                                             std::to_string(info.bytesReceived) +
                                                             ", Bytes sent: " +
                                                             std::to_string(info.bytesSent),
                                                         info.ip);
                                                 }
                                             }));
            }

            // wait for all tasks to complete
            for (auto &future : futures)
            {
                future.wait();
            }
        }
        else
        {
            // sequential processing for small number of connections
            for (int sock : socketsToClose)
            {
                close(sock);
            }
        }

        // clear all connection info
        {
            std::lock_guard<std::mutex> lock(connectionsMutex);
            connections.clear();
        }
    }

    Logger::getInstance()->info("All connections closed");
}

void Server::handleClient(int client_socket, const std::string &clientIp)
{
    static constexpr size_t BUFFER_SIZE = 16384;
    // use thread local to avoid repeated allocation
    static thread_local std::vector<char> buffer(BUFFER_SIZE);
    static thread_local std::string request;
    request.clear();

    ssize_t valread;                // bytes read in each operation
    bool connectionClosed = false;  // track connection state
    ssize_t totalBytesReceived = 0; // accumulate bytes for batch update

    // read data from client in chunks using non-blocking recv
    while ((valread = recv(client_socket, buffer.data(), buffer.size(), MSG_DONTWAIT)) > 0)
    {
        // check if server should stop during data reading
        if (shouldStop)
        {
            closeConnection(client_socket);
            return;
        }

        // append received data to request string
        request.append(buffer.data(), valread);
        totalBytesReceived += valread;
    }

    // update connection statistics in batch to minimize lock time
    if (totalBytesReceived > 0)
    {
        std::lock_guard<std::mutex> lock(connectionsMutex);
        auto it = connections.find(client_socket);
        if (it != connections.end())
        {
            it->second.bytesReceived += totalBytesReceived;
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
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
        {
            closeConnection(client_socket);
            connectionClosed = true;
        }
    }

    // process request if connection is still open and we have data
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

        // apply rate limiting to the request using move semantics
        std::string processedRequest = rateLimiter.process(std::move(request));

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
            // reuse compression middleware instance per thread
            static thread_local Compression compressionMiddleware;
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
    // copy connection info and check if need logging
    std::string logInfo;
    {
        std::lock_guard<std::mutex> lock(connectionsMutex);
        auto it = connections.find(client_socket);
        if (it != connections.end())
        {
            if (!it->second.isClosureLogged)
            {
                // prepare log message inside lock but log outside
                auto duration = std::chrono::steady_clock::now() - it->second.startTime;
                std::string durationStr = Socket::durationToString(duration);
                logInfo = "Connection closed - Duration: " + durationStr +
                          ", Bytes received: " + std::to_string(it->second.bytesReceived) +
                          ", Bytes sent: " + std::to_string(it->second.bytesSent);
            }
            connections.erase(it);
        }
    }

    // log outside the lock if needed
    if (!logInfo.empty())
    {
        Logger::getInstance()->info(logInfo);
    }

    // batch remove from epoll and close socket
    epoll.batch_operation(&client_socket, &client_socket + 1, EPOLL_CTL_DEL);
    close(client_socket);
}

void Server::startCacheCleanup()
{
    cacheCleanupThread = std::thread([this]()
                                     {
        pthread_setname_np(pthread_self(), "cache-cleanup");
        
        if (nice(10) == -1) {
            Logger::getInstance()->warning("Failed to set cache cleanup thread priority");
        }

        std::atomic<bool> cleanupRunning{true};
        
        sigset_t set;
        sigemptyset(&set);
        sigaddset(&set, SIGINT);
        sigaddset(&set, SIGTERM);
        pthread_sigmask(SIG_BLOCK, &set, nullptr);

        while (!shouldStop) {
            cache.periodicCleanup();
            
            for (int i = 0; i < 60 && !shouldStop; ++i) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
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