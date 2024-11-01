#ifndef PGS_SOCKET_HPP
#define PGS_SOCKET_HPP

#include "common.hpp"
#include "logger.hpp"

class Socket
{
public:
    explicit Socket(int port);
    ~Socket();

    void bind();
    void listen();
    void closeSocket();
    [[nodiscard]] int acceptConnection(std::string &clientIp);
    [[nodiscard]] int getSocketFd() const;

    static std::string durationToString(const std::chrono::steady_clock::duration &duration);

private:
    int server_fd; // Server socket file descriptor
    int port;      // Port number
};

#endif // PGS_SOCKET_HPP