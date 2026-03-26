#include "cachesim.hpp"
#include <vector>

struct Block {
    uint64_t tag = 0;
    int64_t last_used = 0;
    bool valid = false;
    bool dirty = false;
    bool prefetched = false;
};

struct Set {
    std::vector<Block> blocks;
};

struct Cache {
    uint64_t C = 0;
    uint64_t B = 0;
    uint64_t S = 0;
    uint64_t block_size = 0;   // 2^B
    uint64_t ways = 0;         // 2^S
    uint64_t num_sets = 0;     // 2^(C-B-S)
    std::vector<Set> sets;
};

static Cache L1;
static Cache L2;
static uint64_t global_time = 0;
static uint32_t prefetch_k = 0;

static void init_cache(Cache& cache, uint64_t C, uint64_t B, uint64_t S) {
    cache.C = C;
    cache.B = B;
    cache.S = S;

    cache.block_size = 1ULL << B;
    cache.ways = 1ULL << S;
    cache.num_sets = 1ULL << (C - B - S);

    cache.sets.clear();
    cache.sets.resize(cache.num_sets);

    for (uint64_t i = 0; i < cache.num_sets; i++) {
        cache.sets[i].blocks.resize(cache.ways);
    }
}

static uint64_t get_set_index(uint64_t address, const Cache& cache) {
    if (cache.num_sets == 1) {
        return 0;
    }
    uint64_t index_bits = cache.C - cache.B - cache.S;
    uint64_t mask = (1ULL << index_bits) - 1;
    return (address >> cache.B) & mask;
}

static uint64_t get_tag(uint64_t address, const Cache& cache) {
    uint64_t index_bits = cache.C - cache.B - cache.S;
    return address >> (cache.B + index_bits);
}

/**
 * Subroutine for initializing the cache. You many add and initialize any global or heap
 * variables as needed.
 *
 * @c1 Total size of L1 in bytes is 2^C1
 * @b1 Size of each block in L1 in bytes is 2^B1
 * @s1 Number of blocks per set in L1 is 2^S1
 * @c2 Total size of L2 in bytes is 2^C2
 * @b2 Size of each block in L2 in bytes is 2^B2
 * @s2 Number of blocks per set in L2 is 2^S2
 * @k Prefetch K subsequent blocks
 */
void setup_cache(uint64_t c1, uint64_t b1, uint64_t s1, uint64_t c2, uint64_t b2, uint64_t s2, uint32_t k) {
    global_time = 0;
    prefetch_k = k;

    init_cache(L1, c1, b1, s1);
    init_cache(L2, c2, b2, s2);
}

/**
 * Subroutine that simulates the cache one trace event at a time.
 *
 * @rw The type of event. Either READ or WRITE
 * @address  The target memory address
 * @p_stats Pointer to the statistics structure
 */
void cache_access(char rw, uint64_t address, cache_stats_t* p_stats) {
    
}

/**
 * Subroutine for cleaning up any outstanding memory operations and calculating overall statistics
 * such as miss rate or average access time.
 *
 * @p_stats Pointer to the statistics structure
 */
void complete_cache(cache_stats_t *p_stats) {
}
