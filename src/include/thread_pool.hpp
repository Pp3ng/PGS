#ifndef PGS_THREAD_POOL_HPP
#define PGS_THREAD_POOL_HPP

#include "common.hpp"
#include "logger.hpp"

#define CACHE_LINE_SIZE 64 // cache line size in bytes

class ThreadPool
{
private:
    // performance optimization constants
    static constexpr size_t QUEUE_SIZE = 1024; // must be power of 2
    static constexpr size_t MAX_STEAL_ATTEMPTS = 3;
    static constexpr size_t SPIN_COUNT_MAX = 100;

    // lock-free queue implementation
    template <typename T>
    class LockFreeQueue
    {
    private:
        struct Entry
        {
            std::atomic<size_t> sequence;
            T data;
            explicit Entry() : sequence(0) {}
        };

        alignas(CACHE_LINE_SIZE) Entry *buffer;
        alignas(CACHE_LINE_SIZE) std::atomic<size_t> enqueuePos;
        alignas(CACHE_LINE_SIZE) std::atomic<size_t> dequeuePos;
        alignas(CACHE_LINE_SIZE) std::atomic<size_t> size_;
        const size_t mask;

    public:
        explicit LockFreeQueue(size_t capacity = QUEUE_SIZE);
        ~LockFreeQueue();
        bool push(T value);
        bool try_pop(T &value);
        size_t size() const;
        bool empty() const;

        LockFreeQueue(const LockFreeQueue &) = delete;
        LockFreeQueue &operator=(const LockFreeQueue &) = delete;
    };

    // thread local data structure with memory pool
    struct alignas(CACHE_LINE_SIZE) ThreadData
    {
        std::unique_ptr<LockFreeQueue<std::function<void()>>> local_queue;
        size_t steal_attempts{0};
        size_t id;
        std::atomic<size_t> tasks_processed{0};

        explicit ThreadData(size_t thread_id) : local_queue(std::make_unique<LockFreeQueue<std::function<void()>>>()),
                                                id(thread_id) {}
        ~ThreadData() = default;
    };

    // member variables
    std::vector<std::unique_ptr<ThreadData>> thread_data;
    std::vector<std::thread> workers;
    LockFreeQueue<std::function<void()>> global_queue;
    mutable std::shared_mutex taskMutex;
    std::condition_variable_any condition;
    std::atomic<bool> stop_flag{false};
    std::atomic<size_t> active_threads{0};

    bool steal_task(std::function<void()> &task, size_t self_id);
    void worker_thread(size_t id);
    [[nodiscard]] bool setThreadAffinity(pthread_t thread, size_t thread_id);

    struct ThreadLocalRandomData
    {
        std::random_device rd;
        std::mt19937 gen{rd()};
        std::uniform_int_distribution<size_t> dist{};
    };

    thread_local static ThreadLocalRandomData random_data;

    static_assert((QUEUE_SIZE & (QUEUE_SIZE - 1)) == 0, "QUEUE_SIZE must be power of 2");

public:
    explicit ThreadPool(size_t numThreads);
    ~ThreadPool();

    template <typename F, typename... Args>
    auto enqueue(F &&f, Args &&...args)
        -> std::future<typename std::invoke_result_t<F, Args...>>;

    void stop();

    ThreadPool(const ThreadPool &) = delete;
    ThreadPool &operator=(const ThreadPool &) = delete;
};

#include "thread_pool.inl"

#endif // PGS_THREAD_POOL_HPP