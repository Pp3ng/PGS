#ifndef PGS_SERVER_HPP
#define PGS_SERVER_HPP

#include "common.hpp"
#include "socket.hpp"
#include "router.hpp"
#include "thread_pool.hpp"
#include "epoll_wrapper.hpp"
#include "rate_limiter.hpp"
#include "cache.hpp"
#include "logger.hpp"
#include "connection_info.hpp"

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

    std::thread cacheCleanupThread;                                         // thread for cache cleanup
    static constexpr auto CACHE_CLEANUP_INTERVAL = std::chrono::minutes(5); // 5 minutes

    void handleClient(int client_socket, const std::string &clientIp);
    void closeConnection(int client_socket);
    void startCacheCleanup();
};

#endif // PGS_SERVER_HPP