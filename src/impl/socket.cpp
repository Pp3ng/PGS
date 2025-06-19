#include "socket.hpp"

Socket::Socket(int port) : port(port)
{
    // create a dual-stack socket that supports both ipv6 and ipv4
    if ((server_fd = socket(AF_INET6, SOCK_STREAM, 0)) == -1)
    {
        Logger::getInstance()->error("Socket creation failed: " +
                                     std::string(strerror(errno)));
        throw std::runtime_error("Socket creation failed!");
    }

    try
    {
        // lambda for handling setsockopt calls
        auto setSocketOption = [this](int level, int optname, const void *optval,
                                      socklen_t optlen, const char *errorMsg)
        {
            if (setsockopt(server_fd, level, optname, optval, optlen) < 0)
            {
                throw std::runtime_error(std::string(errorMsg) + ": " +
                                         std::string(strerror(errno)));
            }
        };

        // allow ipv4 connections on ipv6 socket
        int no = 0;
        setSocketOption(IPPROTO_IPV6, IPV6_V6ONLY, &no, sizeof(no),
                        "Failed to set IPV6_V6ONLY");

        // enable address and port reuse
        int opt = 1;
        setSocketOption(SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt),
                        "Failed to set SO_REUSEADDR");
        setSocketOption(SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt),
                        "Failed to set SO_REUSEPORT");

        // enable tcp quickack for lower latency
        setSocketOption(IPPROTO_TCP, TCP_QUICKACK, &opt, sizeof(opt),
                        "Failed to set TCP_QUICKACK");

        // enable busy polling
        int busy_poll_usec = 10; // 10 microseconds
        setSocketOption(SOL_SOCKET, SO_BUSY_POLL, &busy_poll_usec, sizeof(busy_poll_usec),
                        "Failed to set SO_BUSY_POLL");

        int bufSize = 4 * 1024 * 1024; // 4mb buffer size
        setSocketOption(SOL_SOCKET, SO_RCVBUF, &bufSize, sizeof(bufSize),
                        "Failed to set SO_RCVBUF");
        setSocketOption(SOL_SOCKET, SO_SNDBUF, &bufSize, sizeof(bufSize),
                        "Failed to set SO_SNDBUF");

        // optimize tcp settings for high performance
        int maxSeg = 1448; // optimal mss for ipv6
        setSocketOption(IPPROTO_TCP, TCP_MAXSEG, &maxSeg, sizeof(maxSeg),
                        "Failed to set TCP_MAXSEG");

        // enable tcp fastopen for faster subsequent connections
        int qlen = 5;
        setSocketOption(SOL_TCP, TCP_FASTOPEN, &qlen, sizeof(qlen),
                        "Failed to set TCP_FASTOPEN");

        // set non-blocking mode for epoll
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

        Logger::getInstance()->success("Socket created and configured successfully");
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
    address.sin6_family = AF_INET6;
    address.sin6_addr = in6addr_any;
    address.sin6_port = htons(port);

    if (::bind(server_fd, reinterpret_cast<struct sockaddr *>(&address),
               sizeof(address)) < 0)
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
    ::listen(server_fd, 42); // 42 is the ulrimate answer to life, universe and everything 😉
}

void Socket::closeSocket()
{
    if (server_fd != -1)
    {
        close(server_fd);
        server_fd = -1;
    }
}

int Socket::acceptConnection(std::string &clientIp)
{
    struct sockaddr_in6 address;
    socklen_t addrlen = sizeof(address);

    // use accept4 to set nonblock flag directly, avoiding extra fcntl calls
    int new_socket = accept4(server_fd,
                             reinterpret_cast<struct sockaddr *>(&address),
                             &addrlen,
                             SOCK_NONBLOCK | SOCK_CLOEXEC);

    if (new_socket >= 0)
    {
        // convert ip address to string format
        if (address.sin6_family == AF_INET6)
        {
            static thread_local char ipstr[INET6_ADDRSTRLEN];
            inet_ntop(AF_INET6, &address.sin6_addr, ipstr, sizeof(ipstr));
            clientIp = ipstr;
        }
        else
        {
            clientIp = "unknown";
        }
        return new_socket;
    }
    else if (errno != EWOULDBLOCK && errno != EAGAIN) // no pending connections
    {
        // log error only for real failures, not for no-connection case
        clientIp = "-";
        Logger::getInstance()->error("Failed to accept connection: " +
                                     std::string(strerror(errno)));
    }
    return -1;
}

int Socket::getSocketFd() const
{
    return server_fd;
}

std::string Socket::durationToString(const std::chrono::steady_clock::duration &duration)
{
    auto seconds = std::chrono::duration_cast<std::chrono::seconds>(duration).count();
    auto minutes = seconds / 60;
    seconds %= 60;
    return std::to_string(minutes) + "m " + std::to_string(seconds) + "s";
}