#include "cache.hpp"

// constructor with move semantics for data and mimeType - O(1)
Cache::CacheEntry::CacheEntry(std::vector<char> &&d, std::string &&m, time_t lm,
                              std::list<std::string>::iterator it)
    : data(std::move(d)), mimeType(std::move(m)), lastModified(lm), lruIterator(it) {}

// constructor with overflow check - O(1)
Cache::Cache(size_t maxSizeMB, std::chrono::seconds maxAge)
    : maxSize(static_cast<size_t>(maxSizeMB) * 1024 * 1024), currentSize(0),
      maxAge(maxAge)
{
    // check for cache size overflow
    if (maxSize / (1024 * 1024) != maxSizeMB)
    { // calculate: 1024*1024=1048576(1MB)
        throw std::overflow_error("Cache size overflow");
    }
}

// helper function to update LRU order - O(1) operation
void Cache::updateLRU(const std::string &key)
{
    lruList.erase(cache[key].lruIterator);    // remove from current position
    lruList.push_front(key);                  // add to front (most recently used)
    cache[key].lruIterator = lruList.begin(); // update iterator
}

void Cache::cleanExpiredEntries()
{
    std::unique_lock<std::shared_mutex> lock(mutex);
    auto now = std::chrono::system_clock::now();

    std::vector<std::string> expiredKeys;
    for (auto it = cache.begin(); it != cache.end(); ++it)
    {
        auto entryAge = now - std::chrono::system_clock::from_time_t(it->second.lastModified);
        if (entryAge > maxAge)
        {
            currentSize -= it->second.data.size();
            lruList.erase(it->second.lruIterator);
            expiredKeys.push_back(it->first);
            Logger::getInstance()->info("Cache entry expired: " + it->first +
                                        ", age: " + std::to_string(std::chrono::duration_cast<std::chrono::seconds>(entryAge).count()) +
                                        "s, max age: " + std::to_string(maxAge.count()) + "s");
        }
    }

    for (const auto &key : expiredKeys)
    {
        cache.erase(key);
    }

    if (!expiredKeys.empty())
    {
        Logger::getInstance()->info("Cleaned " + std::to_string(expiredKeys.size()) +
                                    " expired cache entries, current cache size: " +
                                    std::to_string(currentSize / (1024 * 1024)) + "MB");
    }
}

void Cache::periodicCleanup()
{
    auto start = std::chrono::steady_clock::now();
    cleanExpiredEntries();
    auto duration = std::chrono::steady_clock::now() - start;

    if (std::chrono::duration_cast<std::chrono::milliseconds>(duration).count() > 100) // 100ms
    {
        Logger::getInstance()->warning("Cache cleanup took " +
                                       std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(duration).count()) +
                                       "ms");
    }
}

// clear all items from cache - O(1)
void Cache::clear()
{
    std::unique_lock<std::shared_mutex> lock(mutex);
    cache.clear();
    lruList.clear();
    currentSize = 0;
}

// remove a specific item from cache - O(1) average case
bool Cache::remove(const std::string &key)
{
    std::unique_lock<std::shared_mutex> lock(mutex);
    auto it = cache.find(key);
    if (it != cache.end())
    {
        currentSize -= it->second.data.size();
        lruList.erase(it->second.lruIterator);
        cache.erase(it);
        return true;
    }
    return false;
}

// check if an item exists in cache - O(1) average case
bool Cache::exists(const std::string &key)
{
    std::shared_lock<std::shared_mutex> lock(mutex);
    return cache.find(key) != cache.end();
}

// get current size of cache in bytes - O(1)
size_t Cache::size() const
{
    std::shared_lock<std::shared_mutex> lock(mutex);
    return currentSize;
}

// get number of items in cache - O(1)
size_t Cache::count() const
{
    std::shared_lock<std::shared_mutex> lock(mutex);
    return cache.size();
}