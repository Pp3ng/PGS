#ifndef PGS_COMMON_HPP
#define PGS_COMMON_HPP

// Standard C++ headers
#include <algorithm>          // efficient algorithms
#include <array>              // fixed-size arrays
#include <atomic>             // atomic operations
#include <chrono>             // measuring time
#include <condition_variable> // blocking thread synchronization
#include <deque>              // double-ended queue
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
#include <new>                // memory alignment
#include <optional>           // optional type
#include <queue>              // queue data structure
#include <random>             // random number generation
#include <set>                // ordered unique elements (Red-Black Tree)
#include <shared_mutex>       // shared mutexes
#include <sstream>            // string stream manipulations
#include <stdexcept>          // standard exceptions like std::runtime_error
#include <string_view>        // efficient string handling without ownership
#include <thread>             // multithreading support
#include <unordered_map>      // unordered associative container (Hash Table)
#include <unordered_set>      // unordered unique elements (Hash Table)
#include <vector>             // dynamic array

// System headers
#include <arpa/inet.h>    // inet_ntoa - for converting IP addresses
#include <csignal>        // signal handling
#include <cstring>        // strlen() - for string manipulation
#include <ctime>          // handling timestamps
#include <fcntl.h>        // file control options
#include <netinet/in.h>   // sockaddr_in - structure for IPv4 addresses
#include <netinet/tcp.h>  // TCP_KEEPIDLE, TCP_KEEPINTVL, TCP_KEEPCNT
#include <pthread.h>      // POSIX threads
#include <sched.h>        // CPU affinity
#include <sys/epoll.h>    // epoll - for scalable I/O event notification
#include <sys/mman.h>     // mmap - for memory-mapped file access
#include <sys/sendfile.h> // sendfile - for efficient file sending
#include <sys/socket.h>   // socket(), bind(), listen(), accept() - for socket operations
#include <sys/stat.h>     // fstat - to get file status
#include <sys/uio.h>      // writev - to write to multiple buffers
#include <unistd.h>       // close() function - to close file descriptors

// Third-party headers
#include <nlohmann/json.hpp> // JSON parsing
#include <zlib.h>            // zlib compression

namespace fs = std::filesystem; // Alias for filesystem namespace
using json = nlohmann::json;    // Alias for JSON namespace

#endif // PGS_COMMON_HPP