#ifndef MEMORY_POOL_H
#define MEMORY_POOL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
#define THREAD_LOCAL thread_local
extern "C"
{
#else
#define THREAD_LOCAL _Thread_local
#endif

// helper macros for type conversion
#ifdef __cplusplus
#define PTR_CAST(T, x) reinterpret_cast<T>(x)
#else
#define PTR_CAST(T, x) (T)(x)
#endif

// memory pool constants
#define CACHE_LINE_SIZE 64    // default cache line size
#define MIN_BLOCK_SIZE 32     // minimum block size
#define DEFAULT_MAX_LEVELS 16 // maximum buddy system levels
#define ALIGNMENT 16          // memory alignment requirement
#define MAX_CACHE_BLOCKS 64   // maximum blocks in thread local cache

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
        uint32_t magic; // magic number to detect corruption
        uint32_t state; // level and free flag
        struct block_header *next;
        size_t size;          // block size
        uint32_t guard_begin; // guard pattern
        uint8_t padding[CACHE_LINE_SIZE - sizeof(uint32_t)];
    } __attribute__((aligned(CACHE_LINE_SIZE)));

    // footer for additional corruption detection
    struct block_footer
    {
        uint32_t guard_end; // guard pattern
    };

    // pool usage statistics
    struct pool_stats
    {
        size_t allocations;   // total allocations
        size_t deallocations; // total deallocations
        size_t current_usage; // current memory usage
        size_t peak_usage;    // peak memory usage
        size_t frees;         // total free operations
        size_t cache_hits;    // local cache hits
        size_t cache_misses;  // local cache misses
    };

    // thread memory pool structure
    struct thread_memory_pool
    {
        void *memory;                                        // pool memory block
        size_t total_size;                                   // total pool size
        struct thread_cache *cache;                          // thread local cache
        struct block_header *free_lists[DEFAULT_MAX_LEVELS]; // free block lists
        struct pool_stats stats;                             // pool statistics
        char padding[CACHE_LINE_SIZE];                       // prevent false sharing
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