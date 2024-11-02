#include "memory_pool.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdalign.h>

// round up x to the nearest power of 2
static inline size_t round_up_to_power_of_2(size_t x)
{
    x--;
    x |= x >> 1;
    x |= x >> 2;
    x |= x >> 4;
    x |= x >> 8;
    x |= x >> 16;
#if UINTPTR_MAX > 0xFFFFFFFF
    x |= x >> 32; // support 64-bit systems
#endif
    return x + 1;
}

// calculate the level in the buddy system for a given size
static inline int get_level(size_t size)
{
    size_t normalized = round_up_to_power_of_2(size + sizeof(struct block_header));
    return __builtin_ctzl(normalized / MIN_BLOCK_SIZE);
}

// prefetch block for read/write
static inline void prefetch_block(void *addr, int rw)
{
    __builtin_prefetch(addr, rw, 3);
}

// thread local storage for memory pool and cache
THREAD_LOCAL struct thread_memory_pool *local_pool = NULL;
THREAD_LOCAL struct thread_cache local_cache = {0};

// try to get block from thread local cache
static void *get_from_cache(size_t size)
{
    struct thread_cache *cache = &local_cache;
    for (size_t i = 0; i < cache->count; i++)
    {
        struct block_header *block = PTR_CAST(struct block_header *, cache->blocks[i]);
        if (block->size >= size)
        {
            cache->blocks[i] = cache->blocks[--cache->count];
            return block;
        }
    }
    return NULL;
}

// add block to thread local cache
static void add_to_cache(struct thread_memory_pool *pool, void *block)
{
    struct thread_cache *cache = &local_cache;
    if (cache->count < MAX_CACHE_BLOCKS)
    {
        cache->blocks[cache->count++] = block;
        atomic_fetch_add(&pool->stats.cache_hits, 1);
    }
}

// merge buddy blocks to reduce fragmentation
static void merge_buddies(struct thread_memory_pool *pool, struct block_header *block)
{
    size_t size = MIN_BLOCK_SIZE << (block->state >> 1);
    struct block_header *buddy = PTR_CAST(struct block_header *, (uintptr_t)block ^ size);

    while ((uintptr_t)buddy >= (uintptr_t)pool->memory &&
           (uintptr_t)buddy < (uintptr_t)pool->memory + pool->total_size)
    {
        if (!(buddy->state & 1))
            break;

        block = (block < buddy) ? block : buddy;
        block->state += 2; // increment level
        size <<= 1;
        buddy = PTR_CAST(struct block_header *, (uintptr_t)block ^ size);
    }
}

// create a new thread memory pool with specified size and max levels
struct thread_memory_pool *create_thread_pool(size_t size, int max_levels)
{
    struct thread_memory_pool *pool = aligned_alloc(CACHE_LINE_SIZE,
                                                    sizeof(struct thread_memory_pool));
    if (!pool)
        return NULL;

    memset(pool, 0, sizeof(struct thread_memory_pool));

    // align total size to power of 2
    size = round_up_to_power_of_2(size);
    pool->total_size = size;

    // allocate aligned memory block
    pool->memory = aligned_alloc(ALIGNMENT, size);
    if (!pool->memory)
    {
        free(pool);
        return NULL;
    }

    // initialize first block
    struct block_header *first = PTR_CAST(struct block_header *, pool->memory);
    first->state = ((max_levels - 1) << 1) | 1; // set level and free flag
    first->size = size - sizeof(struct block_header);
    atomic_store(&pool->free_lists[max_levels - 1],
                 PTR_CAST(ATOMIC(struct block_header *), first));

    return pool;
}

// allocate memory from pool
void *pool_allocate(struct thread_memory_pool *pool, size_t size)
{
    // try thread local cache first
    void *cached = get_from_cache(size);
    if (cached)
    {
        atomic_fetch_add(&pool->stats.allocations, 1);
        return (char *)cached + sizeof(struct block_header);
    }

    int level = get_level(size);
    for (int i = level; i < DEFAULT_MAX_LEVELS; i++)
    {
        ATOMIC(struct block_header *)
        atomic_current = atomic_load(&pool->free_lists[i]);
        struct block_header *current = PTR_CAST(struct block_header *, atomic_current);

        while (current)
        {
            prefetch_block(current->next, 0);
            uint32_t expected_state = (i << 1) | 1; // level and free flag
            uint32_t new_state = (i << 1);          // clear free flag

            if (atomic_compare_exchange_strong(&current->state, &expected_state, new_state))
            {
                // split block if necessary
                while (i > level)
                {
                    i--;
                    size_t split_size = MIN_BLOCK_SIZE << i;
                    struct block_header *buddy = PTR_CAST(struct block_header *,
                                                          (char *)current + split_size);
                    buddy->state = (i << 1) | 1; // set level and free flag
                    buddy->size = split_size - sizeof(struct block_header);

                    atomic_store(&buddy->next, atomic_load(&pool->free_lists[i]));
                    atomic_store(&pool->free_lists[i],
                                 PTR_CAST(ATOMIC(struct block_header *), buddy));
                }

                atomic_fetch_add(&pool->stats.allocations, 1);
                atomic_fetch_add(&pool->stats.current_usage, size);
                size_t peak = atomic_load(&pool->stats.peak_usage);
                while (peak < pool->stats.current_usage)
                {
                    atomic_compare_exchange_weak(&pool->stats.peak_usage, &peak,
                                                 pool->stats.current_usage);
                }
                return (char *)current + sizeof(struct block_header);
            }
            atomic_current = atomic_load(&current->next);
            current = PTR_CAST(struct block_header *, atomic_current);
        }
    }
    return NULL;
}

// get thread local pool, creating if necessary
struct thread_memory_pool *get_thread_pool(void)
{
    if (!local_pool)
    {
        local_pool = create_thread_pool(1UL << 20, DEFAULT_MAX_LEVELS); // 1MB default
    }
    return local_pool;
}

// deallocate memory back to pool
void pool_deallocate(struct thread_memory_pool *pool, void *ptr)
{
    if (!ptr)
        return;

    struct block_header *header = PTR_CAST(struct block_header *,
                                           (char *)ptr - sizeof(struct block_header));

    // try to add to local cache first
    if (header->size <= (MIN_BLOCK_SIZE << (DEFAULT_MAX_LEVELS / 2)))
    {
        add_to_cache(pool, header);
        return;
    }

    uint32_t state = atomic_load(&header->state);
    atomic_store(&header->state, state | 1); // set free flag
    atomic_fetch_add(&pool->stats.deallocations, 1);
    atomic_fetch_sub(&pool->stats.current_usage, header->size);

    merge_buddies(pool, header);
}

// destroy thread memory pool and free all memory
void destroy_thread_pool(struct thread_memory_pool *pool)
{
    if (!pool)
        return;

    if (pool->memory)
    {
        free(pool->memory);
    }
    free(pool);

    if (local_pool == pool)
    {
        local_pool = NULL;
    }
}