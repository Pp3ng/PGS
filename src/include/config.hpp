#ifndef PGS_CONFIG_HPP
#define PGS_CONFIG_HPP

#include "common.hpp"

struct Config
{
    int port;                 // prot for server
    std::string staticFolder; // path for static files
    int threadCount;          // thread count of worker threads
    struct
    {
        int maxRequests; // maximum number of requests allowed within time window
        int timeWindow;  // duration of time window for rate limiting
    } rateLimit;
    struct
    {
        size_t sizeMB;     // maximum size of cache in MB
        int maxAgeSeconds; // maximum age of cache entries in seconds
    } cache;
};

#endif // PGS_CONFIG_HPP