#include "memory_pool.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdalign.h>
#include <stdbool.h>

// guard patterns for memory corruption detection
#define GUARD_PATTERN 0xDEADBEEF
#define HEADER_MAGIC 0xFEEDFACE

typedef uint32_t level_t;
static void merge_buddies(struct thread_memory_pool *pool, struct block_header *block);

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
static inline level_t get_level(size_t size)
{
    if (size > ((size_t)-1) - sizeof(struct block_header))
        return (level_t)-1;

    size_t normalized = round_up_to_power_of_2(size + sizeof(struct block_header));
    if (normalized == 0 || normalized < MIN_BLOCK_SIZE)
        return (level_t)-1;

    return (level_t)__builtin_ctzl(normalized / MIN_BLOCK_SIZE);
}

// prefetch block for read/write
static inline void prefetch_block(const void *addr, int rw)
{
    if (!addr)
        return;

    if (rw)
        __builtin_prefetch(addr, 1, 3);
    else
        __builtin_prefetch(addr, 0, 3);
}

THREAD_LOCAL struct thread_memory_pool *local_pool = NULL;
THREAD_LOCAL struct thread_cache local_cache = {0};

// try to get block from thread local cache
static void *get_from_cache(size_t size)
{
    if (size == 0)
        return NULL;

    struct thread_cache *cache = &local_cache;
    for (size_t i = 0; i < cache->count; i++)
    {
        struct block_header *block = PTR_CAST(struct block_header *, cache->blocks[i]);

        if (!block || block->magic != HEADER_MAGIC)
        {
            if (i < cache->count - 1)
                cache->blocks[i] = cache->blocks[cache->count - 1];
            cache->count--;
            continue;
        }

        if (block->size >= size)
        {
            void *result = block;
            if (i < cache->count - 1)
                cache->blocks[i] = cache->blocks[cache->count - 1];
            cache->count--;
            return result;
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
        pool->stats.cache_hits++;
    }
    else
    {
        // if cache is full, merge the block back to pool
        header->state |= 1; // mark as free
        merge_buddies(pool, header);
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
        return false;
    if ((uintptr_t)pool->memory % ALIGNMENT != 0)
        return false;
    return true;
}
static inline bool validate_block(struct thread_memory_pool *pool, struct block_header *block)
{
    if (!block || !pool)
        return false;
    if (block->magic != HEADER_MAGIC)
        return false;

    // check if block is within pool bounds
    if ((uintptr_t)block < (uintptr_t)pool->memory ||
        (uintptr_t)block >= (uintptr_t)pool->memory + pool->total_size)
    {
        return false;
    }

    // check if block size is valid
    if (block->size == 0 ||
        block->size > pool->total_size - (sizeof(struct block_header) + sizeof(struct block_footer)))
    {
        return false;
    }

    // validate block boundaries
    if (block->guard_begin != GUARD_PATTERN)
        return false;

    struct block_footer *footer = PTR_CAST(struct block_footer *,
                                           (char *)block + sizeof(struct block_header) + block->size);

    // check if footer is within pool bounds
    if ((uintptr_t)footer + sizeof(struct block_footer) >
        (uintptr_t)pool->memory + pool->total_size)
    {
        return false;
    }

    if (footer->guard_end != GUARD_PATTERN)
        return false;

    return true;
}

// merge buddy blocks to reduce fragmentation
static void merge_buddies(struct thread_memory_pool *pool, struct block_header *block)
{
    if (!validate_block(pool, block))
        return;

    size_t size = MIN_BLOCK_SIZE << (block->state >> 1);
    struct block_header *buddy = PTR_CAST(struct block_header *, (uintptr_t)block ^ size);

    // limit merge iterations to prevent infinite loops
    int merge_count = 0;
    const int MAX_MERGE_ITERATIONS = 32;

    while (merge_count++ < MAX_MERGE_ITERATIONS &&
           (uintptr_t)buddy >= (uintptr_t)pool->memory &&
           (uintptr_t)buddy < (uintptr_t)pool->memory + pool->total_size)
    {
        if (!validate_block(pool, buddy) || !(buddy->state & 1))
            break;

        level_t buddy_level = buddy->state >> 1;
        level_t block_level = block->state >> 1;

        if (buddy_level != block_level)
            break;

        // ensure we merge from lower address to higher
        block = (block < buddy) ? block : buddy;
        block->state += 2; // increase level and keep free flag

        // update block metadata
        block->size = (MIN_BLOCK_SIZE << (block->state >> 1)) -
                      (sizeof(struct block_header) + sizeof(struct block_footer));
        block->guard_begin = GUARD_PATTERN;

        struct block_footer *footer = PTR_CAST(struct block_footer *,
                                               (char *)block + sizeof(struct block_header) + block->size);
        footer->guard_end = GUARD_PATTERN;

        size <<= 1;
        buddy = PTR_CAST(struct block_header *, (uintptr_t)block ^ size);
    }
}

struct thread_memory_pool *create_thread_pool(size_t size, int max_levels)
{
    if (size < MIN_BLOCK_SIZE || max_levels <= 0 || max_levels > DEFAULT_MAX_LEVELS)
        return NULL;

    struct thread_memory_pool *pool = aligned_alloc(CACHE_LINE_SIZE, sizeof(struct thread_memory_pool));
    if (!pool)
        return NULL;

    memset(pool, 0, sizeof(struct thread_memory_pool));

    size = round_up_to_power_of_2(size);
    if (size == 0)
    {
        free(pool);
        return NULL;
    }

    pool->total_size = size;
    pool->memory = aligned_alloc(ALIGNMENT, size);
    if (!pool->memory)
    {
        free(pool);
        return NULL;
    }

    // initialize first block
    struct block_header *first = PTR_CAST(struct block_header *, pool->memory);
    first->magic = HEADER_MAGIC;
    first->state = ((level_t)(max_levels - 1) << 1) | 1; // set as free
    first->size = size - (sizeof(struct block_header) + sizeof(struct block_footer));
    first->guard_begin = GUARD_PATTERN;
    first->next = NULL;

    struct block_footer *footer = PTR_CAST(struct block_footer *,
                                           (char *)first + sizeof(struct block_header) + first->size);
    footer->guard_end = GUARD_PATTERN;

    pool->free_lists[max_levels - 1] = first;

    return pool;
}
void *pool_allocate(struct thread_memory_pool *pool, size_t size)
{
    // validate input parameters
    if (!validate_pool(pool) || size == 0)
    {
        if (pool)
            pool->stats.cache_misses++;
        return NULL;
    }

    // check total size with headers
    size_t total_header_size = sizeof(struct block_header) + sizeof(struct block_footer);
    if (size > pool->total_size - total_header_size)
    {
        pool->stats.cache_misses++;
        return NULL;
    }

    // align size and check for overflow
    size_t original_size = size;
    size = (size + (ALIGNMENT - 1)) & ~(ALIGNMENT - 1);
    if (size < original_size || size > pool->total_size - total_header_size)
    {
        pool->stats.cache_misses++;
        return NULL;
    }

    // try cache first
    void *cached = get_from_cache(size);
    if (cached)
    {
        struct block_header *header = PTR_CAST(struct block_header *, cached);
        if (!validate_block(pool, header))
        {
            pool->stats.cache_misses++;
            return NULL;
        }
        pool->stats.allocations++;
        return (char *)cached + sizeof(struct block_header);
    }

    // find appropriate level
    level_t level = get_level(size);
    if (level >= DEFAULT_MAX_LEVELS)
    {
        pool->stats.cache_misses++;
        return NULL;
    }

    // search for free block
    for (level_t i = level; i < DEFAULT_MAX_LEVELS; i++)
    {
        struct block_header *current = pool->free_lists[i];
        struct block_header *prev = NULL;

        while (current)
        {
            // prefetch next block for better performance
            prefetch_block(current->next, 0);

            if (!validate_block(pool, current))
            {
                // remove invalid block from list
                if (prev)
                    prev->next = current->next;
                else
                    pool->free_lists[i] = current->next;
                current = current->next;
                continue;
            }

            if ((current->state & 1) && ((current->state >> 1) == i))
            {
                // remove from free list
                if (prev)
                    prev->next = current->next;
                else
                    pool->free_lists[i] = current->next;

                // mark as allocated
                current->state &= ~1;

                // split if needed
                if (i > level)
                {
                    size_t block_size = MIN_BLOCK_SIZE << i;
                    size_t needed_size = sizeof(struct block_header) + size + sizeof(struct block_footer);
                    size_t remaining_size = block_size - needed_size;

                    if (remaining_size >= MIN_BLOCK_SIZE)
                    {
                        // create buddy block
                        struct block_header *buddy = PTR_CAST(struct block_header *,
                                                              (char *)current + needed_size);

                        // initialize buddy block
                        buddy->magic = HEADER_MAGIC;
                        buddy->state = ((i - 1) << 1) | 1; // set as free
                        buddy->size = remaining_size - total_header_size;
                        buddy->guard_begin = GUARD_PATTERN;
                        buddy->next = NULL;

                        // setup buddy footer
                        struct block_footer *buddy_footer = PTR_CAST(struct block_footer *,
                                                                     (char *)buddy + sizeof(struct block_header) + buddy->size);
                        buddy_footer->guard_end = GUARD_PATTERN;

                        // add buddy to free list
                        buddy->next = pool->free_lists[i - 1];
                        pool->free_lists[i - 1] = buddy;

                        // update current block size
                        current->size = size;

                        // update current block footer
                        struct block_footer *current_footer = PTR_CAST(struct block_footer *,
                                                                       (char *)current + sizeof(struct block_header) + current->size);
                        current_footer->guard_end = GUARD_PATTERN;
                    }
                }

                // update stats
                pool->stats.allocations++;
                pool->stats.current_usage += current->size;
                if (pool->stats.current_usage > pool->stats.peak_usage)
                {
                    pool->stats.peak_usage = pool->stats.current_usage;
                }

                return (char *)current + sizeof(struct block_header);
            }

            prev = current;
            current = current->next;
        }
    }

    pool->stats.cache_misses++;
    return NULL;
}

struct thread_memory_pool *get_thread_pool(void)
{
    if (!local_pool)
    {
        local_pool = create_thread_pool(1UL << 20, DEFAULT_MAX_LEVELS);
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
void pool_free(struct thread_memory_pool *pool, void *ptr)
{
    if (!pool || !ptr)
        return;

    struct block_header *block = PTR_CAST(struct block_header *,
                                          (char *)ptr - sizeof(struct block_header));

    if (!validate_block(pool, block))
        return;

    // update stats
    pool->stats.frees++;
    pool->stats.current_usage -= block->size;

    // try to add to cache first
    add_to_cache(pool, block);
}

void destroy_thread_pool(struct thread_memory_pool *pool)
{
    if (!pool)
        return;

    // clear cache first
    struct thread_cache *cache = &local_cache;
    for (size_t i = 0; i < cache->count; i++)
    {
        struct block_header *block = PTR_CAST(struct block_header *, cache->blocks[i]);
        if (block && block->magic == HEADER_MAGIC)
        {
            block->magic = 0;  // invalidate block
            block->state |= 1; // mark as free
            merge_buddies(pool, block);
        }
    }
    cache->count = 0;

    // clear all blocks
    for (int i = 0; i < DEFAULT_MAX_LEVELS; i++)
    {
        struct block_header *current = pool->free_lists[i];
        while (current)
        {
            if (validate_block(pool, current))
            {
                current->magic = 0; // invalidate block
            }
            current = current->next;
        }
        pool->free_lists[i] = NULL;
    }

    // free pool memory
    if (pool->memory)
    {
        free(pool->memory);
        pool->memory = NULL;
    }

    // clear pool structure
    memset(pool, 0, sizeof(struct thread_memory_pool));
    free(pool);

    // clear thread local storage
    if (local_pool == pool)
    {
        local_pool = NULL;
    }
}

void pool_get_stats(struct thread_memory_pool *pool, struct pool_stats *stats)
{
    if (!pool || !stats)
        return;

    memcpy(stats, &pool->stats, sizeof(struct pool_stats));
}

void pool_reset_stats(struct thread_memory_pool *pool)
{
    if (!pool)
        return;

    memset(&pool->stats, 0, sizeof(struct pool_stats));
}

bool pool_validate(struct thread_memory_pool *pool)
{
    if (!validate_pool(pool))
        return false;

    // validate cache
    struct thread_cache *cache = &local_cache;
    for (size_t i = 0; i < cache->count; i++)
    {
        struct block_header *block = PTR_CAST(struct block_header *, cache->blocks[i]);
        if (!validate_block(pool, block))
            return false;
    }

    // validate all blocks in free lists
    for (level_t i = 0; i < DEFAULT_MAX_LEVELS; i++)
    {
        struct block_header *current = pool->free_lists[i];
        while (current)
        {
            if (!validate_block(pool, current))
                return false;

            // verify level matches index
            if ((current->state >> 1) != i)
                return false;

            // verify block is marked as free
            if (!(current->state & 1))
                return false;

            current = current->next;
        }
    }

    return true;
}

size_t pool_get_block_size(void *ptr)
{
    if (!ptr)
        return 0;

    struct block_header *block = PTR_CAST(struct block_header *,
                                          (char *)ptr - sizeof(struct block_header));

    if (block->magic != HEADER_MAGIC)
        return 0;

    return block->size;
}
