// Template implementations
#ifndef PGS_CACHE_INL
#define PGS_CACHE_INL

#ifdef PGS_CACHE_HPP

template <typename Vector>
Cache::CacheEntry::CacheEntry(Vector &&d, std::string &&m, time_t lm,
                              std::list<std::string>::iterator it)
    : data(std::make_move_iterator(d.begin()),
           std::make_move_iterator(d.end())),
      mimeType(std::move(m)),
      lastModified(lm),
      cacheTime(std::chrono::system_clock::to_time_t(
          std::chrono::system_clock::now())), // set cache time to current time
      lruIterator(it)
{
}

// retrieve an item from cache - O(1) average case
template <typename Vector>
bool Cache::get(const std::string &key, Vector &data, std::string &mimeType, time_t &lastModified)
{
    std::shared_lock<std::shared_mutex> readLock(mutex);

    auto it = cache.find(key);
    if (it == cache.end())
    {
        return false;
    }

    // copy the cached data and metadata first
    try
    {
        data.reserve(it->second.data.size());
        data.assign(it->second.data.begin(), it->second.data.end());
        mimeType = it->second.mimeType;
        lastModified = it->second.lastModified;
    }
    catch (const std::bad_alloc &e)
    {
        Logger::getInstance()->error("Failed to copy cached data: " + std::string(e.what()));
        return false;
    }

    // update LRU list under exclusive lock
    readLock.unlock();
    std::unique_lock<std::shared_mutex> writeLock(mutex);

    // re-check if entry still exists after lock switch
    it = cache.find(key);
    if (it != cache.end())
    {
        // move entry to front of LRU list
        lruList.erase(it->second.lruIterator);
        lruList.push_front(key);
        it->second.lruIterator = lruList.begin();
        return true;
    }

    return false;
}

// add an item to cache - o(1) average case
template <typename Vector>
void Cache::set(std::string &&key, Vector &&data, std::string &&mimeType, time_t lastModified)
{
    if (data.size() > maxSize)
    {
        return; // don't cache if too large
    }

    std::unique_lock<std::shared_mutex> lock(mutex);

    // store key copy for lookup
    const std::string keyCopy = key;

    // remove existing entry if present
    auto it = cache.find(keyCopy);
    if (it != cache.end())
    {
        currentSize -= it->second.data.size();
        lruList.erase(it->second.lruIterator);
        cache.erase(it);
    }

    // ensure space available
    const size_t requiredSize = data.size();
    while (!lruList.empty() && currentSize + requiredSize > maxSize)
    {
        const std::string &lruKey = lruList.back();
        auto lruIt = cache.find(lruKey);
        if (lruIt != cache.end())
        {
            currentSize -= lruIt->second.data.size();
            cache.erase(lruIt);
        }
        lruList.pop_back();
    }

    // add new entry
    try
    {
        // add to lru first
        lruList.push_front(keyCopy);
        auto lruIt = lruList.begin();

        // then add to cache
        // cachetime will be set automatically in the cacheentry constructor
        cache.emplace(
            std::piecewise_construct,
            std::forward_as_tuple(std::move(key)),
            std::forward_as_tuple(
                std::forward<Vector>(data),
                std::move(mimeType),
                lastModified,
                lruIt));

        currentSize += requiredSize;
    }
    catch (const std::exception &e)
    {
        // rollback on failure
        if (!lruList.empty())
        {
            lruList.pop_front();
        }
        Logger::getInstance()->error("cache allocation failed: " + std::string(e.what()));
    }
}

#endif // PGS_CACHE_HPP
#endif // PGS_CACHE_INL