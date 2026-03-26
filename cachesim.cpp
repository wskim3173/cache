#include "cachesim.hpp"
#include <vector>

struct Block {
    uint64_t tag = 0;
    uint64_t last_used = 0;
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

static bool has_last_miss_block = false;
static uint64_t last_miss_block_addr = 0;
static int64_t pending_stride = 0;

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

static uint64_t get_block_addr(uint64_t address, const Cache& cache) {
    return address >> cache.B;
}

static uint64_t get_block_base_addr(const Cache& cache, uint64_t set_idx, uint64_t tag) {
    uint64_t index_bits = cache.C - cache.B - cache.S;
    uint64_t block_addr = (tag << index_bits) | set_idx;
    return block_addr << cache.B;
}

static bool block_addr_out_of_range(int64_t block_addr) {
    return block_addr < 0;
}

static bool find_block_in_set(const Cache& cache, uint64_t set_idx, uint64_t tag, uint64_t& way_idx) {
    const std::vector<Block>& blocks = cache.sets[set_idx].blocks;

    for (uint64_t i = 0; i < blocks.size(); i++) {
        if (blocks[i].valid && blocks[i].tag == tag) {
            way_idx = i;
            return true;
        }
    }
    return false;
}

static bool find_invalid_block(const Cache& cache, uint64_t set_idx, uint64_t& way_idx) {
    const std::vector<Block>& blocks = cache.sets[set_idx].blocks;

    for (uint64_t i = 0; i < blocks.size(); i++) {
        if (!blocks[i].valid) {
            way_idx = i;
            return true;
        }
    }
    return false;
}

static uint64_t find_lru_block(const Cache& cache, uint64_t set_idx) {
    const std::vector<Block>& blocks = cache.sets[set_idx].blocks;

    uint64_t victim = 0;
    for (uint64_t i = 1; i < blocks.size(); i++) {
        if (blocks[i].last_used < blocks[victim].last_used) {
            victim = i;
        }
    }
    return victim;
}

static uint64_t select_fill_way(const Cache& cache, uint64_t set_idx, bool& needs_eviction) {
    uint64_t way_idx = 0;
    if (find_invalid_block(cache, set_idx, way_idx)) {
        needs_eviction = false;
        return way_idx;
    }

    needs_eviction = true;
    return find_lru_block(cache, set_idx);
}

static void touch_block(Cache& cache, uint64_t set_idx, uint64_t way_idx) {
    global_time++;
    cache.sets[set_idx].blocks[way_idx].last_used = global_time;
}

static void fill_block(Cache& cache, uint64_t set_idx, uint64_t way_idx,
                       uint64_t tag, bool dirty, bool prefetched, bool touch) {
    Block& block = cache.sets[set_idx].blocks[way_idx];
    block.tag = tag;
    block.valid = true;
    block.dirty = dirty;
    block.prefetched = prefetched;

    if (touch) {
        global_time++;
        block.last_used = global_time;
    } else {
        block.last_used = 0;  // prefetched block
    }
}

static void handle_l1_eviction(uint64_t set_idx, uint64_t way_idx, cache_stats_t* p_stats) {
    Block& victim = L1.sets[set_idx].blocks[way_idx];

    if (!victim.valid || !victim.dirty) {
        return;
    }

    uint64_t victim_addr = get_block_base_addr(L1, set_idx, victim.tag);

    uint64_t l2_set = get_set_index(victim_addr, L2);
    uint64_t l2_tag = get_tag(victim_addr, L2);

    uint64_t l2_way = 0;
    if (find_block_in_set(L2, l2_set, l2_tag, l2_way)) {
        L2.sets[l2_set].blocks[l2_way].dirty = true;
    } else {
        p_stats->write_backs++;
    }
}

static void handle_l2_eviction(uint64_t set_idx, uint64_t way_idx, cache_stats_t* p_stats) {
    Block& victim = L2.sets[set_idx].blocks[way_idx];

    if (!victim.valid || !victim.dirty) {
        return;
    }

    p_stats->write_backs++;
}

static void prefetch_block_to_L2(uint64_t address, cache_stats_t* p_stats) {
    uint64_t l2_set = get_set_index(address, L2);
    uint64_t l2_tag = get_tag(address, L2);

    uint64_t existing_way = 0;
    if (find_block_in_set(L2, l2_set, l2_tag, existing_way)) {
        return;
    }

    bool need_evict = false;
    uint64_t fill_way = select_fill_way(L2, l2_set, need_evict);

    if (need_evict) {
        handle_l2_eviction(l2_set, fill_way, p_stats);
    }

    fill_block(L2, l2_set, fill_way, l2_tag, false, true, false);
    p_stats->prefetched_blocks++;
}

static void run_prefetch_on_l2_miss(uint64_t address, cache_stats_t* p_stats) {
    uint64_t current_block_addr_u = get_block_addr(address, L2);
    int64_t current_block_addr = static_cast<int64_t>(current_block_addr_u);

    bool had_prev_miss = has_last_miss_block;
    int64_t d = 0;

    if (had_prev_miss) {
        d = current_block_addr - static_cast<int64_t>(last_miss_block_addr);
    }

    last_miss_block_addr = current_block_addr_u;
    has_last_miss_block = true;

    if (had_prev_miss && d == pending_stride) {
        for (uint32_t i = 1; i <= prefetch_k; i++) {
            int64_t target_block_addr = current_block_addr + static_cast<int64_t>(i) * pending_stride;

            if (block_addr_out_of_range(target_block_addr)) {
                continue;
            }

            uint64_t target_address = static_cast<uint64_t>(target_block_addr) << L2.B;
            prefetch_block_to_L2(target_address, p_stats);
        }
    }

    pending_stride = d;
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

    has_last_miss_block = false;
    last_miss_block_addr = 0;
    pending_stride = 0;

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
    p_stats->accesses++;

    // L1 access
    p_stats->L1_accesses++;

    if (rw == READ) {
        p_stats->reads++;
    } else {
        p_stats->writes++;
    }

    uint64_t l1_set = get_set_index(address, L1);
    uint64_t l1_tag = get_tag(address, L1);

    uint64_t l1_way = 0;
    if (find_block_in_set(L1, l1_set, l1_tag, l1_way)) { // L1 Hit
        touch_block(L1, l1_set, l1_way);

        if (rw == WRITE) {
            L1.sets[l1_set].blocks[l1_way].dirty = true;
        }
        return;
    }

    // L1 Miss
    if (rw == READ) {
        p_stats->L1_read_misses++;
    } else {
        p_stats->L1_write_misses++;
    }

    // L2 access
    p_stats->L2_accesses++;

    uint64_t l2_set = get_set_index(address, L2);
    uint64_t l2_tag = get_tag(address, L2);

    uint64_t l2_way = 0;
    if (find_block_in_set(L2, l2_set, l2_tag, l2_way)) { // L2 Hit
        if (L2.sets[l2_set].blocks[l2_way].prefetched) {
            p_stats->successful_prefetches++;
            L2.sets[l2_set].blocks[l2_way].prefetched = false;
        }

        touch_block(L2, l2_set, l2_way);

        bool l1_need_evict = false;
        uint64_t fill_l1_way = select_fill_way(L1, l1_set, l1_need_evict);

        if (l1_need_evict) {
            handle_l1_eviction(l1_set, fill_l1_way, p_stats);
        }

        fill_block(L1, l1_set, fill_l1_way, l1_tag, (rw == WRITE), false, true);
        return;
    }

    // L2 Miss
    if (rw == READ) {
        p_stats->L2_read_misses++;
    } else {
        p_stats->L2_write_misses++;
    }

    // Memory access
    // memory -> L2
    bool l2_need_evict = false;
    uint64_t fill_l2_way = select_fill_way(L2, l2_set, l2_need_evict);

    if (l2_need_evict) {
        handle_l2_eviction(l2_set, fill_l2_way, p_stats);
    }

    fill_block(L2, l2_set, fill_l2_way, l2_tag, false, false, true);

    // L2 -> L1
    bool l1_need_evict = false;
    uint64_t fill_l1_way = select_fill_way(L1, l1_set, l1_need_evict);

    if (l1_need_evict) {
        handle_l1_eviction(l1_set, fill_l1_way, p_stats);
    }

    fill_block(L1, l1_set, fill_l1_way, l1_tag, (rw == WRITE), false, true);

    run_prefetch_on_l2_miss(address, p_stats);
}

/**
 * Subroutine for cleaning up any outstanding memory operations and calculating overall statistics
 * such as miss rate or average access time.
 *
 * @p_stats Pointer to the statistics structure
 */
void complete_cache(cache_stats_t *p_stats) {
    uint64_t l1_misses = p_stats->L1_read_misses + p_stats->L1_write_misses;
    uint64_t l2_misses = p_stats->L2_read_misses + p_stats->L2_write_misses;

    double mr1 = 0.0;
    double mr2 = 0.0;

    if (p_stats->L1_accesses > 0) {
        mr1 = static_cast<double>(l1_misses) /
              static_cast<double>(p_stats->L1_accesses);
    }

    if (p_stats->L2_accesses > 0) {
        mr2 = static_cast<double>(l2_misses) /
              static_cast<double>(p_stats->L2_accesses);
    }

    double ht1 = 2.0 + 0.2 * static_cast<double>(L1.S);
    double ht2 = 4.0 + 0.4 * static_cast<double>(L2.S);
    double mp2 = 500.0;
    double mp1 = ht2 + mr2 * mp2;

    p_stats->avg_access_time = ht1 + mr1 * mp1;
}
