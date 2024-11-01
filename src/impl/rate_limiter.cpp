#include "rate_limiter.hpp"

RateLimiter::RateLimiter(size_t maxRequests, std::chrono::seconds timeWindow)
    : maxTokens(maxRequests), // set maximum number of tokens (requests)
      refillRate(static_cast<double>(maxRequests) / timeWindow.count())
{ // calculate refill rate based on time window
    // reserve space in unordered_map to reduce rehashing when new clients are added
    clientBuckets.reserve(1024);
}

// process a request for a given client identified by 'data'.
// returns original request data if request is allowed,
// or rate limit response if request exceeds allowed rate.
[[nodiscard]] std::string RateLimiter::process(const std::string &data)
{
    // get current time using steady_clock
    const auto now = std::chrono::steady_clock::now();

    // lock mutex to ensure thread-safe access to shared resources
    std::lock_guard<std::mutex> lock(rateMutex);

    // retrieve bucket for current client identified by 'data'
    auto &bucket = clientBuckets[data];

    // check if this is first request from client
    if (bucket.lastRefillTime == std::chrono::steady_clock::time_point())
    {
        // initialize bucket with max tokens and set last refill time to now
        bucket.tokens = maxTokens;
        bucket.lastRefillTime = now;
    }
    else
    {
        // calculate time elapsed since last refill
        const auto timeElapsed = std::chrono::duration_cast<std::chrono::seconds>(
                                     now - bucket.lastRefillTime)
                                     .count();

        // if time has elapsed, refill tokens based on elapsed time
        if (timeElapsed > 0)
        {
            // optimize token calculation by adding new tokens, ensuring it does not exceed maxTokens
            bucket.tokens = std::min(maxTokens,
                                     static_cast<size_t>(bucket.tokens + timeElapsed * refillRate));
            // update last refill time to current time
            bucket.lastRefillTime = now;
        }
    }

    // check if there are enough tokens to process request
    if (bucket.tokens < 1)
    {
        // if not enough tokens, return rate limit response
        return RATE_LIMIT_RESPONSE; // could also return remaining tokens if needed
    }

    // deduct a token for current request
    bucket.tokens -= 1;

    // return original request data if allowed
    return data;
}