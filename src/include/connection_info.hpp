#ifndef PGS_CONNECTION_INFO_HPP
#define PGS_CONNECTION_INFO_HPP

#include "common.hpp"

// Connection information structure
struct ConnectionInfo
{
  std::chrono::steady_clock::time_point startTime; // connection start time
  std::string ip;                                  // client IP address
  bool isLogged;                                   // flag to track if connection is logged
  bool isClosureLogged;                            // flag to track if connection closure is logged
  uint64_t bytesReceived;                          // bytes received from client
  uint64_t bytesSent;                              // bytes sent to client

  ConnectionInfo(const std::chrono::steady_clock::time_point &time,
                 const std::string &ipAddr, bool logged = false,
                 bool closureLogged = false, uint64_t received = 0,
                 uint64_t sent = 0)
      : startTime(time), ip(ipAddr), isLogged(logged),
        isClosureLogged(closureLogged), bytesReceived(received),
        bytesSent(sent) {}
};

#endif // PGS_CONNECTION_INFO_HPP