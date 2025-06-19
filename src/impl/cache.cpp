#include "cache.hpp"

// constructor with move semantics
Cache::CacheEntry::CacheEntry(std::vector<char> &&d, std::string &&m, time_t lm,
                              std::list<std::string>::iterator it)
    : data(std::move(d)),
      mimeType(std::move(m)),
      lastModified(lm),
      cacheTime(std::chrono::system_clock::to_time_t(
          std::chrono::system_clock::now())), // set cache time to current time
      lruIterator(it)
{
}

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

// batch update LRU for performance optimization
void Cache::batchUpdateLRU(const std::vector<std::string>& keys)
{
    std::unique_lock<std::shared_mutex> lock(mutex);
    
    for (const auto& key : keys)
    {
        auto it = cache.find(key);
        if (it != cache.end())
        {
            lruList.erase(it->second.lruIterator);
            lruList.push_front(key);
            it->second.lruIterator = lruList.begin();
        }
    }
}

void Cache::cleanExpiredEntries()
{
    std::unique_lock<std::shared_mutex> lock(mutex);

    time_t now = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now());

    std::vector<std::string> expiredKeys;
    for (auto it = cache.begin(); it != cache.end(); ++it)
    {
        // calculate age based on cache time instead of last modified time
        time_t age = now - it->second.cacheTime;

        if (age > maxAge.count())
        {
            expiredKeys.push_back(it->first);
            currentSize -= it->second.data.size();
            lruList.erase(it->second.lruIterator);

            Logger::getInstance()->info(
                "Cache entry expired: " + it->first +
                ", age: " + std::to_string(age) +
                "s, max age: " + std::to_string(maxAge.count()) + "s");
        }
    }
    // remove expired entries
    for (const auto &key : expiredKeys)
    {
        cache.erase(key);
    }

    if (!expiredKeys.empty())
    {
        Logger::getInstance()->info(
            "Cleaned " + std::to_string(expiredKeys.size()) +
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