#ifndef PGS_CACHE_HPP
#define PGS_CACHE_HPP

#include "common.hpp"
#include "logger.hpp"

class Cache
{
private:
    // Cache entry structure for storing file content and metadata - O(1) access time
    struct CacheEntry
    {
        std::vector<char> data;                       // actual content of cached file
        std::string mimeType;                         // mime type of cached content
        time_t lastModified;                          // last modification time of file
        time_t cacheTime;                             // time when entry was added to cache
        std::list<std::string>::iterator lruIterator; // iterator pointing to key's position in lru list

        CacheEntry() : lastModified(0), cacheTime(0) {}

        // constructor with move semantics for data and mimetype
        CacheEntry(std::vector<char> &&d, std::string &&m, time_t lm,
                   std::list<std::string>::iterator it);

        // constructor with perfect forwarding for data and mimetype
        template <typename Vector>
        CacheEntry(Vector &&d, std::string &&m, time_t lm,
                   std::list<std::string>::iterator it);

        // disable copy and assignment operations
        CacheEntry(const CacheEntry &) = delete;
        CacheEntry &operator=(const CacheEntry &) = delete;
    };

    std::unordered_map<std::string, CacheEntry> cache; // main cache storage (key -> entry mapping)
    std::list<std::string> lruList;                    // LRU order tracking list (most recent -> least recent)
    mutable std::shared_mutex mutex;                   // mutex for thread-safe operations
    size_t maxSize;                                    // maximum size of cache in bytes
    size_t currentSize;                                // current size of cache in bytes
    std::chrono::seconds maxAge;                       // maximum age of cache entries

    // helper function to update LRU order - O(1) operation
    void updateLRU(const std::string &key);
    void cleanExpiredEntries();
    void batchUpdateLRU(const std::vector<std::string>& keys);

public:
    // constructor with overflow check - O(1)
    explicit Cache(size_t maxSizeMB, std::chrono::seconds maxAge);

    // retrieve an item from cache - O(1) average case
    template <typename Vector>
    bool get(const std::string &key, Vector &data, std::string &mimeType, time_t &lastModified);

    // add an item to cache - O(1) average case
    template <typename Vector>
    void set(std::string &&key, Vector &&data, std::string &&mimeType, time_t lastModified);

    void periodicCleanup();

    // clear all items from cache - O(1)
    void clear();

    // remove a specific item from cache - O(1) average case
    bool remove(const std::string &key);

    // check if an item exists in cache - O(1) average case
    bool exists(const std::string &key);

    // get current size of cache in bytes - O(1)
    size_t size() const;

    // get number of items in cache - O(1)
    size_t count() const;
};

#include "cache.inl"

#endif // PGS_CACHE_HPP