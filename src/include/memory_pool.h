#ifndef MEMORY_POOL_H
#define MEMORY_POOL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
#include <atomic>
#define ATOMIC(T) std::atomic<T>
#define THREAD_LOCAL thread_local
extern "C"
{
#else
#include <stdatomic.h>
#define ATOMIC(T) _Atomic T
#define THREAD_LOCAL _Thread_local
#endif

// helper macros for type conversion
#ifdef __cplusplus
#define PTR_CAST(T, x) reinterpret_cast<T>(x)
#else
#define PTR_CAST(T, x) (T)(x)
#endif

// memory pool constants (i dont know if this configuration is correct)
#define DEFAULT_CACHE_LINE_SIZE 128 // default cache line size
#define MIN_BLOCK_SIZE 256          // minimum block size
#define DEFAULT_MAX_LEVELS 18       // maximum buddy system levels
#define ALIGNMENT 128               // memory alignment requirement
#define MAX_CACHE_BLOCKS 64         // maximum blocks in thread local cache

// cache line size for architecture
#ifndef CACHE_LINE_SIZE
#define CACHE_LINE_SIZE DEFAULT_CACHE_LINE_SIZE
#endif

// atomic operations helpers
#define ATOMIC_LOAD(ptr) atomic_load(ptr)
#define ATOMIC_STORE(ptr, val) atomic_store(ptr, val)
#define ATOMIC_EXCHANGE(ptr, val) atomic_exchange(ptr, val)

    // thread local cache structure
    struct thread_cache
    {
        void *blocks[MAX_CACHE_BLOCKS];
        size_t count;
        uint8_t padding[CACHE_LINE_SIZE];
    } __attribute__((aligned(CACHE_LINE_SIZE)));

    // block header with cache line padding
    struct block_header
    {
        ATOMIC(uint32_t)
        state; // combines level and is_free flags
        ATOMIC(struct block_header *)
        next;
        size_t size; // actual block size
        uint8_t padding[CACHE_LINE_SIZE];
    } __attribute__((aligned(CACHE_LINE_SIZE)));

    // pool usage statistics
    struct pool_stats
    {
        ATOMIC(size_t)
        allocations; // total allocations
        ATOMIC(size_t)
        deallocations; // total deallocations
        ATOMIC(size_t)
        current_usage; // current memory usage
        ATOMIC(size_t)
        peak_usage; // peak memory usage
        ATOMIC(size_t)
        cache_hits; // local cache hits
        ATOMIC(size_t)
        cache_misses; // local cache misses
    };

    // thread memory pool structure
    struct thread_memory_pool
    {
        void *memory;               // pool memory block
        size_t total_size;          // total pool size
        struct thread_cache *cache; // thread local cache
        ATOMIC(struct block_header *)
        free_lists[DEFAULT_MAX_LEVELS]; // free block lists
        struct pool_stats stats;        // pool statistics
        char padding[CACHE_LINE_SIZE];  // prevent false sharing
    } __attribute__((aligned(CACHE_LINE_SIZE)));

    // thread-local pool instance
    extern THREAD_LOCAL struct thread_memory_pool *local_pool;

    // memory pool operations
    struct thread_memory_pool *get_thread_pool(void);
    struct thread_memory_pool *create_thread_pool(size_t size, int max_levels);
    void *pool_allocate(struct thread_memory_pool *pool, size_t size);
    void pool_deallocate(struct thread_memory_pool *pool, void *ptr);
    void destroy_thread_pool(struct thread_memory_pool *pool);

#ifdef __cplusplus
}
#endif

#endif // MEMORY_POOL_H