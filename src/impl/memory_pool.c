#include "memory_pool.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdalign.h>

// round up a number to the nearest power of 2
static inline size_t round_up_to_power_of_2(size_t x)
{
    if (0 == x)
        return MIN_BLOCK_SIZE;
    // improved overflow check with explicit size_t cast
    if (x > (((size_t)1 << (sizeof(size_t) * 8 - 1))))
        return 0;

    // decrement x to handle numbers that are already powers of 2
    // for example: if x is 16, we want to avoid rounding up to 32
    x--;

    // set all bits to the right of the highest set bit to 1
    // this is done through a series of right shifts and OR operations
    x |= x >> 1;  // set the bit to the right of highest set bit
    x |= x >> 2;  // set the next 2 bits
    x |= x >> 4;  // set the next 4 bits
    x |= x >> 8;  // set the next 8 bits
    x |= x >> 16; // set the next 16 bits

// for 64-bit systems, we need to handle the upper 32 bits
#if SIZE_MAX > 0xFFFFFFFF
    x |= x >> 32; // set the upper 32 bits on 64-bit systems
#endif

    // add 1 to get the next power of 2
    // all bits to the right of the highest set bit are now 1
    // adding 1 will carry over and give us the next power of a
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
        // check if the block is large enough
        if (block->size >= size)
        {
            // remove block from cache and update count
            cache->blocks[i] = cache->blocks[--cache->count];
            return block;
        }
    }
    return NULL; // return NULL if no suitable block found
}

// add block to thread local cache
static void add_to_cache(struct thread_memory_pool *pool, void *block)
{
    struct thread_cache *cache = &local_cache;
    // check if there's room in the cache
    if (cache->count < MAX_CACHE_BLOCKS)
    {
        cache->blocks[cache->count++] = block;        // add block to cache
        atomic_fetch_add(&pool->stats.cache_hits, 1); // update cache hit stats
    }
}

// merge buddy blocks to reduce fragmentation
static void merge_buddies(struct thread_memory_pool *pool, struct block_header *block)
{
    // calculate initial block size based on state level
    size_t size = MIN_BLOCK_SIZE << (block->state >> 1);

    // calculate buddy's address by XORing with size
    struct block_header *buddy = PTR_CAST(struct block_header *, (uintptr_t)block ^ size);

    // continue merging as long as buddy is within pool memory range
    while ((uintptr_t)buddy >= (uintptr_t)pool->memory &&
           (uintptr_t)buddy < (uintptr_t)pool->memory + pool->total_size)
    {
        // if buddy is occupied, stop merging
        if (!(buddy->state & 1))
            break;

        // set the lower address block as primary for next merge
        block = (block < buddy) ? block : buddy;

        // increment block's level to represent merged size
        block->state += 2;

        // double size for next level's buddy calculation
        size <<= 1;

        // calculate new buddy address
        buddy = PTR_CAST(struct block_header *, (uintptr_t)block ^ size);
    }
}

// create a new thread memory pool with specified size and max levels
struct thread_memory_pool *create_thread_pool(size_t size, int max_levels)
{
    // allocate memory for the thread pool structure
    struct thread_memory_pool *pool = aligned_alloc(CACHE_LINE_SIZE,
                                                    sizeof(struct thread_memory_pool));
    if (!pool)
        return NULL;

    // zero-initialize the memory pool structure
    memset(pool, 0, sizeof(struct thread_memory_pool));

    // align total size to the next power of 2
    size = round_up_to_power_of_2(size);
    pool->total_size = size;

    // allocate aligned memory block for the pool
    pool->memory = aligned_alloc(ALIGNMENT, size);
    if (!pool->memory)
    {
        free(pool);
        return NULL;
    }

    // initialize the first block header
    struct block_header *first = PTR_CAST(struct block_header *, pool->memory);
    first->state = ((max_levels - 1) << 1) | 1; // set level and free flag
    first->size = size - sizeof(struct block_header);

    // link the first block to the free list for the maximum level
    atomic_store(&pool->free_lists[max_levels - 1],
                 PTR_CAST(ATOMIC(struct block_header *), first));

    return pool;
}

// allocate memory from pool
void *pool_allocate(struct thread_memory_pool *pool, size_t size)
{
    // try to retrieve from thread-local cache
    void *cached = get_from_cache(size);
    if (cached)
    {
        atomic_fetch_add(&pool->stats.allocations, 1);
        return (char *)cached + sizeof(struct block_header);
    }

    int level = get_level(size);
    for (int i = level; i < DEFAULT_MAX_LEVELS; i++)
    {
        // get the current free list head at this level
        struct block_header *current = PTR_CAST(struct block_header *, atomic_load(&pool->free_lists[i]));

        while (current)
        {
            prefetch_block(current->next, 0); // prefetch next block to optimize memory access

            uint32_t expected_state = (i << 1) | 1; // check level and free flag
            uint32_t new_state = (i << 1);          // clear free flag to mark as reserved

            if (atomic_compare_exchange_strong(&current->state, &expected_state, new_state))
            {
                // split block if it is larger than needed
                while (i > level)
                {
                    i--;
                    size_t split_size = MIN_BLOCK_SIZE << i;
                    struct block_header *buddy = PTR_CAST(struct block_header *, (char *)current + split_size);
                    buddy->state = (i << 1) | 1; // set level and free flag
                    buddy->size = split_size - sizeof(struct block_header);

                    // link buddy block to free list
                    atomic_store(&buddy->next, atomic_load(&pool->free_lists[i]));
                    atomic_store(&pool->free_lists[i], PTR_CAST(ATOMIC(struct block_header *), buddy));
                }

                // update allocation stats
                atomic_fetch_add(&pool->stats.allocations, 1);
                atomic_fetch_add(&pool->stats.current_usage, size);

                // update peak usage if current usage exceeds previous peak
                size_t peak = atomic_load(&pool->stats.peak_usage);
                while (peak < pool->stats.current_usage)
                {
                    atomic_compare_exchange_weak(&pool->stats.peak_usage, &peak, pool->stats.current_usage);
                }

                return (char *)current + sizeof(struct block_header);
            }

            // move to the next block in the free list
            current = PTR_CAST(struct block_header *, atomic_load(&current->next));
        }
    }
    return NULL; // return NULL if no suitable block is available
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
    if (!ptr || !pool)
        return;

    struct block_header *header = PTR_CAST(struct block_header *,
                                           (char *)ptr - sizeof(struct block_header));

    // validate header
    if (header->size == 0 || header->size > pool->total_size)
    {
        return;
    }

    // try to add to local cache first
    if (header->size <= (MIN_BLOCK_SIZE << (DEFAULT_MAX_LEVELS / 2)))
    {
        if (pool->cache)
        {
            add_to_cache(pool, header);
            return;
        }
    }

    uint32_t state = atomic_load_explicit(&header->state, memory_order_acquire);
    atomic_store_explicit(&header->state, state | 1, memory_order_release);
    atomic_fetch_add_explicit(&pool->stats.deallocations, 1, memory_order_relaxed);
    atomic_fetch_sub_explicit(&pool->stats.current_usage, header->size, memory_order_relaxed);

    merge_buddies(pool, header);
}

void destroy_thread_pool(struct thread_memory_pool *pool)
{
    if (!pool)
        return;

    // cleanup cache first
    if (pool->cache)
    {
        for (size_t i = 0; i < MAX_CACHE_BLOCKS; i++)
        {
            if (pool->cache->blocks[i])
            {
                merge_buddies(pool, pool->cache->blocks[i]);
            }
        }
        free(pool->cache);
        pool->cache = NULL;
    }

    // cleanup memory
    void *memory = pool->memory;
    pool->memory = NULL;
    if (memory)
    {
        free(memory);
    }

    // clear thread local reference
    if (local_pool == pool)
    {
        local_pool = NULL;
    }

    free(pool);
}