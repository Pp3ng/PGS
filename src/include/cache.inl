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
    static thread_local std::vector<std::string> lru_update_batch;
    static thread_local std::chrono::steady_clock::time_point last_lru_update =
        std::chrono::steady_clock::now();

    std::shared_lock<std::shared_mutex> lock(mutex);

    auto it = cache.find(key);
    if (it == cache.end())
    {
        return false;
    }

    // copy the cached data and metadata
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

    // Batch LRU update
    const auto now = std::chrono::steady_clock::now();
    constexpr auto LRU_BATCH_INTERVAL = std::chrono::milliseconds(100);

    if (lru_update_batch.size() < 32 &&
        now - last_lru_update < LRU_BATCH_INTERVAL)
    {
        lru_update_batch.emplace_back(key);
        return true;
    }

    // Process batch updates
    lock.unlock();
    std::unique_lock<std::shared_mutex> writeLock(mutex);

    // Add current key to batch
    lru_update_batch.emplace_back(key);

    // Batch update LRU for all keys
    for (const auto &batchKey : lru_update_batch)
    {
        auto batchIt = cache.find(batchKey);
        if (batchIt != cache.end())
        {
            lruList.erase(batchIt->second.lruIterator);
            lruList.push_front(batchKey);
            batchIt->second.lruIterator = lruList.begin();
        }
    }

    lru_update_batch.clear();
    last_lru_update = now;

    return true;
}

// add an item to cache - o(1) average cass
template <typename Vector>
void Cache::set(std::string &&key, Vector &&data, std::string &&mimeType, time_t lastModified)
{
    if (data.size() > maxSize)
    {
        return; // don't cache if too large
    }

    std::unique_lock<std::shared_mutex> lock(mutex);

    // remove existing entry if present
    auto it = cache.find(key);
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
        // create key copy only once for LRU list
        const std::string keyCopy = key;

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