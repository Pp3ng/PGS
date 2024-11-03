#include "memory_pool.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdalign.h>
#include <stdbool.h>

// guard patterns for memory corruption detection
#define GUARD_PATTERN 0xDEADBEEF
#define HEADER_MAGIC 0xFEEDFACE

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
    if (rw)
    {
        __builtin_prefetch(addr, 1, 3); // prefetch for write
    }
    else
    {
        __builtin_prefetch(addr, 0, 3); // prefetch for read
    }
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

        // validate block header
        if (!block || block->magic != HEADER_MAGIC)
        {
            // remove corrupted block from cache
            cache->blocks[i] = cache->blocks[--cache->count];
            continue;
        }

        // check if block size is enough
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
    if (!pool || !block)
        return;

    struct block_header *header = PTR_CAST(struct block_header *, block);
    if (header->magic != HEADER_MAGIC)
        return;

    struct thread_cache *cache = &local_cache;
    if (cache->count < MAX_CACHE_BLOCKS)
    {
        cache->blocks[cache->count++] = block;
        atomic_fetch_add(&pool->stats.cache_hits, 1);
    }
}

// validation helper functions
static inline bool validate_pool(struct thread_memory_pool *pool)
{
    if (!pool || !pool->memory)
        return false;
    if (pool->total_size < MIN_BLOCK_SIZE)
        return false;
    if (pool->total_size > (1UL << 31))
        return false; // reasonable maximum size
    return true;
}

static inline bool validate_block(struct thread_memory_pool *pool, struct block_header *block)
{
    if (!block)
        return false;
    if (block->magic != HEADER_MAGIC)
        return false;
    if ((uintptr_t)block < (uintptr_t)pool->memory ||
        (uintptr_t)block >= (uintptr_t)pool->memory + pool->total_size)
    {
        return false;
    }
    if (block->guard_begin != GUARD_PATTERN)
        return false;

    struct block_footer *footer = PTR_CAST(struct block_footer *,
                                           (char *)block + sizeof(struct block_header) + block->size);
    if (footer->guard_end != GUARD_PATTERN)
        return false;

    return true;
}

// merge buddy blocks to reduce fragmentation
static void merge_buddies(struct thread_memory_pool *pool, struct block_header *block)
{
    if (!validate_block(pool, block))
        return;

    // calculate initial block size based on state level
    size_t size = MIN_BLOCK_SIZE << (block->state >> 1);

    // calculate buddy's address by XORing with size
    struct block_header *buddy = PTR_CAST(struct block_header *, (uintptr_t)block ^ size);

    // continue merging as long as buddy is within pool memory range
    while ((uintptr_t)buddy >= (uintptr_t)pool->memory &&
           (uintptr_t)buddy < (uintptr_t)pool->memory + pool->total_size)
    {
        // if buddy is occupied, stop merging
        if (!validate_block(pool, buddy) || !(buddy->state & 1))
            break;

        // verify buddy is at correct level
        if ((buddy->state >> 1) != (block->state >> 1))
        {
            break;
        }

        // set the lower address block as primary for next merge
        block = (block < buddy) ? block : buddy;

        // increment block's level to represent merged size
        block->state += 2;

        // update memory guards for merged block
        block->guard_begin = GUARD_PATTERN;
        struct block_footer *footer = PTR_CAST(struct block_footer *,
                                               (char *)block + sizeof(struct block_header) + block->size);
        footer->guard_end = GUARD_PATTERN;

        // double size for next level's buddy calculation
        size <<= 1;

        // calculate new buddy address
        buddy = PTR_CAST(struct block_header *, (uintptr_t)block ^ size);
    }
}

// create a new thread memory pool with specified size and max levels
struct thread_memory_pool *create_thread_pool(size_t size, int max_levels)
{
    if (size < MIN_BLOCK_SIZE || max_levels <= 0 || max_levels > DEFAULT_MAX_LEVELS)
    {
        return NULL;
    }

    // allocate memory for the thread pool structure
    struct thread_memory_pool *pool = aligned_alloc(CACHE_LINE_SIZE, sizeof(struct thread_memory_pool));
    if (!pool)
        return NULL;

    // zero-initialize the memory pool structure
    memset(pool, 0, sizeof(struct thread_memory_pool));

    // align total size to the next power of 2
    size = round_up_to_power_of_2(size);
    if (size == 0)
    {
        free(pool);
        return NULL;
    }
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
    first->magic = HEADER_MAGIC;                // set magic number for integrity check
    first->state = ((max_levels - 1) << 1) | 1; // set level and free flag
    first->size = size - (sizeof(struct block_header) + sizeof(struct block_footer));
    first->guard_begin = GUARD_PATTERN; // set guard pattern for buffer overflow detection

    // set the footer guard pattern
    struct block_footer *footer = PTR_CAST(struct block_footer *,
                                           (char *)first + sizeof(struct block_header) + first->size);
    footer->guard_end = GUARD_PATTERN; // set guard pattern at end of block

    // link the first block to the free list for the maximum level
    atomic_store(&pool->free_lists[max_levels - 1], PTR_CAST(ATOMIC(struct block_header *), first));

    return pool;
}

// allocate memory from pool
void *pool_allocate(struct thread_memory_pool *pool, size_t size)
{
    if (!validate_pool(pool) || size == 0 || size > pool->total_size - sizeof(struct block_header))
    {
        atomic_fetch_add(&pool->stats.cache_misses, 1);
        return NULL;
    }

    // align size to prevent misaligned access
    size = (size + (ALIGNMENT - 1)) & ~(ALIGNMENT - 1);

    // try cache first
    void *cached = get_from_cache(size);
    if (cached)
    {
        struct block_header *header = PTR_CAST(struct block_header *, cached);
        if (!validate_block(pool, header))
        {
            atomic_fetch_add(&pool->stats.cache_misses, 1);
            return NULL;
        }
        atomic_fetch_add(&pool->stats.allocations, 1);
        return (char *)cached + sizeof(struct block_header);
    }

    int level = get_level(size);
    if (level >= DEFAULT_MAX_LEVELS)
    {
        atomic_fetch_add(&pool->stats.cache_misses, 1);
        return NULL;
    }

    for (int i = level; i < DEFAULT_MAX_LEVELS; i++)
    {
        struct block_header *current = PTR_CAST(struct block_header *,
                                                atomic_load(&pool->free_lists[i]));

        while (current)
        {
            if (!validate_block(pool, current))
            {
                // skip corrupted block and try next
                current = PTR_CAST(struct block_header *, atomic_load(&current->next));
                continue;
            }

            prefetch_block(current->next, 0);

            uint32_t expected_state = (i << 1) | 1;
            uint32_t new_state = (i << 1);

            if (atomic_compare_exchange_strong(&current->state, &expected_state, new_state))
            {
                // initialize new block
                current->magic = HEADER_MAGIC;
                current->guard_begin = GUARD_PATTERN;

                struct block_footer *footer = PTR_CAST(struct block_footer *,
                                                       (char *)current + sizeof(struct block_header) + size);
                footer->guard_end = GUARD_PATTERN;

                // split block if needed with safety checks
                if (i > level)
                {
                    size_t remaining_size = MIN_BLOCK_SIZE << i;
                    remaining_size -= sizeof(struct block_header) + size + sizeof(struct block_footer);

                    if (remaining_size >= MIN_BLOCK_SIZE)
                    {
                        // create buddy block
                        struct block_header *buddy = PTR_CAST(struct block_header *,
                                                              (char *)current + sizeof(struct block_header) + size + sizeof(struct block_footer));

                        // assign buddy block properties
                        buddy->magic = HEADER_MAGIC;
                        buddy->state = (i << 1) | 1;
                        buddy->size = remaining_size - (sizeof(struct block_header) + sizeof(struct block_footer));
                        buddy->guard_begin = GUARD_PATTERN;

                        struct block_footer *buddy_footer = PTR_CAST(struct block_footer *,
                                                                     (char *)buddy + sizeof(struct block_header) + buddy->size);
                        buddy_footer->guard_end = GUARD_PATTERN;

                        atomic_store(&buddy->next, atomic_load(&pool->free_lists[i - 1]));
                        atomic_store(&pool->free_lists[i - 1], PTR_CAST(ATOMIC(struct block_header *), buddy));
                    }
                }

                atomic_fetch_add(&pool->stats.allocations, 1);
                atomic_fetch_add(&pool->stats.current_usage, size);

                // update peak usage atomically
                size_t current_peak = atomic_load(&pool->stats.peak_usage);
                size_t new_usage = atomic_load(&pool->stats.current_usage);
                while (new_usage > current_peak)
                {
                    if (atomic_compare_exchange_weak(&pool->stats.peak_usage,
                                                     &current_peak, new_usage))
                    {
                        break;
                    }
                }

                return (char *)current + sizeof(struct block_header);
            }

            current = PTR_CAST(struct block_header *, atomic_load(&current->next));
        }
    }
    atomic_fetch_add(&pool->stats.cache_misses, 1);
    return NULL;
}

// get thread local pool, creating if necessary
struct thread_memory_pool *get_thread_pool(void)
{
    if (!local_pool)
    {
        local_pool = create_thread_pool(1UL << 20, DEFAULT_MAX_LEVELS); // 1MB default
        if (!local_pool)
        {
            return NULL;
        }
    }
    if (!validate_pool(local_pool))
    {
        local_pool = NULL;
        return NULL;
    }

    return local_pool;
}

// deallocate memory back to pool
void pool_deallocate(struct thread_memory_pool *pool, void *ptr)
{
    if (!validate_pool(pool) || !ptr)
        return;

    struct block_header *header = PTR_CAST(struct block_header *,
                                           (char *)ptr - sizeof(struct block_header));

    if (!validate_block(pool, header))
    {
        // log corruption detection
        atomic_fetch_add(&pool->stats.cache_misses, 1);
        return;
    }

    // verify block size
    if (header->size == 0 || header->size > pool->total_size -
                                                (sizeof(struct block_header) + sizeof(struct block_footer)))
    {
        return;
    }

    // try cache first for small blocks
    if (header->size <= (MIN_BLOCK_SIZE << (DEFAULT_MAX_LEVELS / 2)))
    {
        if (pool->cache)
        {
            add_to_cache(pool, header);
            return;
        }
    }

    uint32_t state = atomic_load_explicit(&header->state, memory_order_acquire);

    // ensure block isn't already freed
    if (state & 1)
    {
        return;
    }

    atomic_store_explicit(&header->state, state | 1, memory_order_release);
    atomic_fetch_add_explicit(&pool->stats.deallocations, 1, memory_order_relaxed);
    atomic_fetch_sub_explicit(&pool->stats.current_usage, header->size, memory_order_relaxed);

    merge_buddies(pool, header);
}

void destroy_thread_pool(struct thread_memory_pool *pool)
{
    if (!validate_pool(pool))
        return;

    // cleanup cache
    if (pool->cache)
    {
        for (size_t i = 0; i < MAX_CACHE_BLOCKS; i++)
        {
            if (pool->cache->blocks[i])
            {
                struct block_header *block = PTR_CAST(struct block_header *,
                                                      pool->cache->blocks[i]);
                if (validate_block(pool, block))
                {
                    merge_buddies(pool, block);
                }
            }
        }
        free(pool->cache);
        pool->cache = NULL;
    }

    // clean all blocks' magic (prevent use after free)
    for (int i = 0; i < DEFAULT_MAX_LEVELS; i++)
    {
        struct block_header *current = PTR_CAST(struct block_header *,
                                                atomic_load(&pool->free_lists[i]));
        while (current)
        {
            if (validate_block(pool, current))
            {
                current->magic = 0; // clear magic number
                current = PTR_CAST(struct block_header *, atomic_load(&current->next));
            }
        }
    }

    // free memory block
    void *memory = pool->memory;
    pool->memory = NULL;
    if (memory)
    {
        free(memory);
    }

    // free pool structure
    if (local_pool == pool)
    {
        local_pool = NULL;
    }

    free(pool);
}