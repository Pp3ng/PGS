// rate_limiter.hpp
#ifndef PGS_RATE_LIMITER_HPP
#define PGS_RATE_LIMITER_HPP

#include "middleware.hpp"
#include <chrono>
#include <mutex>
#include <unordered_map>

// rate limiting middleware using token bucket algorithm
class RateLimiter : public Middleware
{
public:
    // constructor to initialize rate limiting parameters
    RateLimiter(size_t maxRequests, std::chrono::seconds timeWindow);

    // process incoming request with rate limiting
    [[nodiscard]] std::string process(const std::string &data) override;

private:
    const size_t maxTokens;  // maximum tokens in bucket
    const double refillRate; // tokens per second

    struct TokenBucket
    {
        double tokens = 0.0;                                  // current number of tokens in bucket
        std::chrono::steady_clock::time_point lastRefillTime; // last refill time for tokens
    };

    std::unordered_map<std::string, TokenBucket> clientBuckets; // client buckets for rate limiting
    std::mutex rateMutex;                                       // mutex for thread-safe access to client buckets

    // HTTP response for rate limit exceeded
    static constexpr const char *RATE_LIMIT_RESPONSE =
        "HTTP/1.1 429 Too Many Requests\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 19\r\n"
        "\r\n"
        "Too Many Requests";
};

#endif // PGS_RATE_LIMITER_HPP