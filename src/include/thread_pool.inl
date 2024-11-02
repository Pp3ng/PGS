#ifndef PGS_THREAD_POOL_INL
#define PGS_THREAD_POOL_INL

#ifdef PGS_THREAD_POOL_HPP

// lock-free queue implementations
template <typename T>
ThreadPool::LockFreeQueue<T>::LockFreeQueue(size_t capacity)
    : buffer(new Entry[capacity]),
      enqueuePos(0),
      dequeuePos(0),
      size_(0),
      mask(capacity - 1)
{
    for (size_t i = 0; i < capacity; ++i)
    {
        buffer[i].sequence.store(i, std::memory_order_relaxed);
    }
}

template <typename T>
ThreadPool::LockFreeQueue<T>::~LockFreeQueue()
{
    delete[] buffer;
}

template <typename T>
bool ThreadPool::LockFreeQueue<T>::push(T value)
{
    size_t pos = enqueuePos.load(std::memory_order_relaxed);

    for (;;)
    {
        Entry &entry = buffer[pos & mask];
        size_t seq = entry.sequence.load(std::memory_order_acquire);
        intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);

        if (diff == 0)
        {
            if (enqueuePos.compare_exchange_weak(pos, pos + 1,
                                                 std::memory_order_relaxed))
            {
                entry.data = std::move(value);
                entry.sequence.store(pos + 1, std::memory_order_release);
                size_.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
        }
        else if (diff < 0)
        {
            return false; // queue is full
        }
        else
        {
            pos = enqueuePos.load(std::memory_order_relaxed);
        }
    }
}

template <typename T>
bool ThreadPool::LockFreeQueue<T>::try_pop(T &value)
{
    size_t pos = dequeuePos.load(std::memory_order_relaxed);

    for (;;)
    {
        Entry &entry = buffer[pos & mask];
        size_t seq = entry.sequence.load(std::memory_order_acquire);
        intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);

        if (diff == 0)
        {
            if (dequeuePos.compare_exchange_weak(pos, pos + 1,
                                                 std::memory_order_relaxed))
            {
                value = std::move(entry.data);
                entry.sequence.store(pos + mask + 1, std::memory_order_release);
                size_.fetch_sub(1, std::memory_order_relaxed);
                return true;
            }
        }
        else if (diff < 0)
        {
            return false; // queue is empty
        }
        else
        {
            pos = dequeuePos.load(std::memory_order_relaxed);
        }
    }
}

template <typename T>
size_t ThreadPool::LockFreeQueue<T>::size() const
{
    return size_.load(std::memory_order_relaxed);
}

template <typename T>
bool ThreadPool::LockFreeQueue<T>::empty() const
{
    return size() == 0;
}

// thread pool enqueue implementation with memory pool support
template <typename F, typename... Args>
auto ThreadPool::enqueue(F &&f, Args &&...args)
    -> std::future<typename std::invoke_result_t<F, Args...>>
{
    using return_type = typename std::invoke_result_t<F, Args...>;

    // wrap the function to ensure it uses the thread's memory pool
    auto task = std::make_shared<std::packaged_task<return_type()>>(
        [f = std::forward<F>(f), ... args = std::forward<Args>(args)]() mutable
        {
            // task will execute in a thread with initialized memory pool
            return f(std::forward<Args>(args)...);
        });

    std::future<return_type> res = task->get_future();

    static thread_local size_t next_thread = 0;
    next_thread = (next_thread + 1) % thread_data.size();

    auto wrapped_task = [task]()
    { (*task)(); };

    if (!thread_data[next_thread]->local_queue->push(wrapped_task))
    {
        std::unique_lock<std::shared_mutex> lock(taskMutex);
        if (stop_flag)
        {
            throw std::runtime_error("cannot enqueue on stopped thread pool");
        }
        if (!global_queue.push(wrapped_task))
        {
            throw std::runtime_error("thread pool queue is full");
        }
    }

    condition.notify_one();
    return res;
}

#endif // PGS_THREAD_POOL_HPP
#endif // PGS_THREAD_POOL_INL