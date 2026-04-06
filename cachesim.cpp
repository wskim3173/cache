#include "cachesim.hpp"
#include <vector>
#include <cstdint>

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

struct PrefetchState {
    bool has_last_miss_block = false;
    int64_t last_miss_block_addr = 0;
    int64_t pending_stride = 0;
};

struct AddressParts {
    uint64_t set_idx = 0;
    uint64_t tag = 0;
};

Cache L1;
Cache L2;
int64_t global_time = 0;
uint32_t prefetch_k = 0;
PrefetchState prefetch_state;

void initialize_cache(Cache& cache, uint64_t C, uint64_t B, uint64_t S) {
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

uint64_t get_set_index(uint64_t address, const Cache& cache) {
    if (cache.num_sets == 1) {
        return 0;
    }

    uint64_t index_bits = cache.C - cache.B - cache.S;
    uint64_t mask = (1ULL << index_bits) - 1;
    return (address >> cache.B) & mask;
}

uint64_t get_tag(uint64_t address, const Cache& cache) {
    uint64_t index_bits = cache.C - cache.B - cache.S;
    return address >> (cache.B + index_bits);
}

uint64_t get_block_addr(uint64_t address, const Cache& cache) {
    return address >> cache.B;
}

uint64_t get_block_base_addr(const Cache& cache, uint64_t set_idx, uint64_t tag) {
    uint64_t index_bits = cache.C - cache.B - cache.S;
    uint64_t block_addr = (tag << index_bits) | set_idx;
    return block_addr << cache.B;
}

AddressParts decode_address(uint64_t address, const Cache& cache) {
    AddressParts parts;
    parts.set_idx = get_set_index(address, cache);
    parts.tag = get_tag(address, cache);
    return parts;
}

bool is_invalid_prefetch_block_addr(int64_t block_addr) {
    if (block_addr < 0) {
        return true;
    }

    uint64_t max_block_addr = UINT64_MAX >> L2.B;
    return static_cast<uint64_t>(block_addr) > max_block_addr;
}

bool find_block_way(const Cache& cache, uint64_t set_idx, uint64_t tag, uint64_t& way_idx) {
    const std::vector<Block>& blocks = cache.sets[set_idx].blocks;

    for (uint64_t i = 0; i < blocks.size(); i++) {
        if (blocks[i].valid && blocks[i].tag == tag) {
            way_idx = i;
            return true;
        }
    }
    return false;
}

bool find_invalid_way(const Cache& cache, uint64_t set_idx, uint64_t& way_idx) {
    const std::vector<Block>& blocks = cache.sets[set_idx].blocks;

    for (uint64_t i = 0; i < blocks.size(); i++) {
        if (!blocks[i].valid) {
            way_idx = i;
            return true;
        }
    }
    return false;
}

uint64_t find_lru_way(const Cache& cache, uint64_t set_idx) {
    const std::vector<Block>& blocks = cache.sets[set_idx].blocks;

    uint64_t victim = 0;
    for (uint64_t i = 1; i < blocks.size(); i++) {
        if (blocks[i].last_used < blocks[victim].last_used) {
            victim = i;
        }
    }
    return victim;
}

uint64_t select_install_way(const Cache& cache, uint64_t set_idx, bool& needs_eviction) {
    uint64_t way_idx = 0;
    if (find_invalid_way(cache, set_idx, way_idx)) {
        needs_eviction = false;
        return way_idx;
    }

    needs_eviction = true;
    return find_lru_way(cache, set_idx);
}

void mark_block_as_most_recent(Cache& cache, uint64_t set_idx, uint64_t way_idx) {
    global_time++;
    cache.sets[set_idx].blocks[way_idx].last_used = global_time;
}

int64_t get_lru_timestamp_for_prefetch(const Cache& cache, uint64_t set_idx, uint64_t way_idx) {
    int64_t min_timestamp = 0;
    bool found_other_valid = false;

    for (uint64_t i = 0; i < cache.sets[set_idx].blocks.size(); i++) {
        if (i == way_idx) {
            continue;
        }

        const Block& other = cache.sets[set_idx].blocks[i];
        if (!other.valid) {
            continue;
        }

        if (!found_other_valid || other.last_used < min_timestamp) {
            min_timestamp = other.last_used;
            found_other_valid = true;
        }
    }

    return found_other_valid ? (min_timestamp - 1) : 0;
}

void install_demand_block(Cache& cache, uint64_t set_idx, uint64_t way_idx,
                                 uint64_t tag, bool dirty) {
    Block& block = cache.sets[set_idx].blocks[way_idx];
    block.tag = tag;
    block.valid = true;
    block.dirty = dirty;
    block.prefetched = false;

    global_time++;
    block.last_used = global_time;
}

void install_prefetched_block(Cache& cache, uint64_t set_idx, uint64_t way_idx,
                                     uint64_t tag) {
    Block& block = cache.sets[set_idx].blocks[way_idx];
    block.tag = tag;
    block.valid = true;
    block.dirty = false;
    block.prefetched = true;
    block.last_used = get_lru_timestamp_for_prefetch(cache, set_idx, way_idx);
}

void process_l1_eviction(uint64_t set_idx, uint64_t way_idx, cache_stats_t* p_stats) {
    Block& victim = L1.sets[set_idx].blocks[way_idx];

    if (!victim.valid || !victim.dirty) {
        return;
    }

    uint64_t victim_addr = get_block_base_addr(L1, set_idx, victim.tag);
    AddressParts l2_addr = decode_address(victim_addr, L2);

    uint64_t l2_way = 0;
    if (find_block_way(L2, l2_addr.set_idx, l2_addr.tag, l2_way)) {
        L2.sets[l2_addr.set_idx].blocks[l2_way].dirty = true;
    } else {
        p_stats->write_backs++;
    }
}

void process_l2_eviction(uint64_t set_idx, uint64_t way_idx, cache_stats_t* p_stats) {
    Block& victim = L2.sets[set_idx].blocks[way_idx];

    if (!victim.valid || !victim.dirty) {
        return;
    }

    p_stats->write_backs++;
}

void install_block_in_l1(uint64_t address, bool dirty, cache_stats_t* p_stats) {
    AddressParts l1_addr = decode_address(address, L1);

    bool needs_eviction = false;
    uint64_t fill_way = select_install_way(L1, l1_addr.set_idx, needs_eviction);

    if (needs_eviction) {
        process_l1_eviction(l1_addr.set_idx, fill_way, p_stats);
    }

    install_demand_block(L1, l1_addr.set_idx, fill_way, l1_addr.tag, dirty);
}

void install_block_in_l2_from_memory(uint64_t address, cache_stats_t* p_stats) {
    AddressParts l2_addr = decode_address(address, L2);

    bool needs_eviction = false;
    uint64_t fill_way = select_install_way(L2, l2_addr.set_idx, needs_eviction);

    if (needs_eviction) {
        process_l2_eviction(l2_addr.set_idx, fill_way, p_stats);
    }

    install_demand_block(L2, l2_addr.set_idx, fill_way, l2_addr.tag, false);
}

void install_prefetched_block_in_l2(uint64_t address, cache_stats_t* p_stats) {
    AddressParts l2_addr = decode_address(address, L2);

    uint64_t existing_way = 0;
    if (find_block_way(L2, l2_addr.set_idx, l2_addr.tag, existing_way)) {
        return;
    }

    bool needs_eviction = false;
    uint64_t fill_way = select_install_way(L2, l2_addr.set_idx, needs_eviction);

    if (needs_eviction) {
        process_l2_eviction(l2_addr.set_idx, fill_way, p_stats);
    }

    install_prefetched_block(L2, l2_addr.set_idx, fill_way, l2_addr.tag);
    p_stats->prefetched_blocks++;
}

void update_prefetcher_on_l2_miss(uint64_t address, cache_stats_t* p_stats) {
    int64_t current_block_addr = get_block_addr(address, L2);

    bool had_prev_miss = prefetch_state.has_last_miss_block;
    int64_t stride = 0;

    if (had_prev_miss) {
        stride = current_block_addr - prefetch_state.last_miss_block_addr;
    }

    prefetch_state.last_miss_block_addr = current_block_addr;
    prefetch_state.has_last_miss_block = true;

    if (had_prev_miss && stride == prefetch_state.pending_stride) {
        for (int64_t i = 1; i <= prefetch_k; i++) {
            int64_t target_block_addr = current_block_addr + i * prefetch_state.pending_stride;

            if (is_invalid_prefetch_block_addr(target_block_addr)) {
                continue;
            }

            uint64_t target_address = static_cast<uint64_t>(target_block_addr) << L2.B;
            install_prefetched_block_in_l2(target_address, p_stats);
        }
    }

    prefetch_state.pending_stride = stride;
}

void record_access_stats(char rw, cache_stats_t* p_stats) {
    p_stats->accesses++;
    p_stats->L1_accesses++;

    if (rw == READ) {
        p_stats->reads++;
    } else {
        p_stats->writes++;
    }
}

void record_l1_miss_stats(char rw, cache_stats_t* p_stats) {
    if (rw == READ) {
        p_stats->L1_read_misses++;
    } else {
        p_stats->L1_write_misses++;
    }
}

void record_l2_miss_stats(char rw, cache_stats_t* p_stats) {
    if (rw == READ) {
        p_stats->L2_read_misses++;
    } else {
        p_stats->L2_write_misses++;
    }
}

bool is_write_access(char rw) {
    return rw == WRITE;
}

bool handle_l1_hit(char rw, uint64_t address) {
    AddressParts l1_addr = decode_address(address, L1);

    uint64_t l1_way = 0;
    if (!find_block_way(L1, l1_addr.set_idx, l1_addr.tag, l1_way)) {
        return false;
    }

    mark_block_as_most_recent(L1, l1_addr.set_idx, l1_way);

    if (is_write_access(rw)) {
        L1.sets[l1_addr.set_idx].blocks[l1_way].dirty = true;
    }

    return true;
}

bool handle_l2_hit(char rw, uint64_t address, cache_stats_t* p_stats) {
    AddressParts l2_addr = decode_address(address, L2);

    uint64_t l2_way = 0;
    if (!find_block_way(L2, l2_addr.set_idx, l2_addr.tag, l2_way)) {
        return false;
    }

    Block& l2_block = L2.sets[l2_addr.set_idx].blocks[l2_way];
    if (l2_block.prefetched) {
        p_stats->successful_prefetches++;
        l2_block.prefetched = false;
    }

    mark_block_as_most_recent(L2, l2_addr.set_idx, l2_way);
    install_block_in_l1(address, is_write_access(rw), p_stats);
    return true;
}

void handle_l2_miss(char rw, uint64_t address, cache_stats_t* p_stats) {
    record_l2_miss_stats(rw, p_stats);

    install_block_in_l2_from_memory(address, p_stats);
    install_block_in_l1(address, is_write_access(rw), p_stats);
    update_prefetcher_on_l2_miss(address, p_stats);
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
void setup_cache(uint64_t c1, uint64_t b1, uint64_t s1,
                 uint64_t c2, uint64_t b2, uint64_t s2, uint32_t k) {
    global_time = 0;
    prefetch_k = k;
    prefetch_state = {};

    initialize_cache(L1, c1, b1, s1);
    initialize_cache(L2, c2, b2, s2);
}

/**
 * Subroutine that simulates the cache one trace event at a time.
 *
 * @rw The type of event. Either READ or WRITE
 * @address  The target memory address
 * @p_stats Pointer to the statistics structure
 */
void cache_access(char rw, uint64_t address, cache_stats_t* p_stats) {
    record_access_stats(rw, p_stats);

    if (handle_l1_hit(rw, address)) {
        return;
    }

    record_l1_miss_stats(rw, p_stats);
    p_stats->L2_accesses++;

    if (handle_l2_hit(rw, address, p_stats)) {
        return;
    }

    handle_l2_miss(rw, address, p_stats);
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

    double l1_miss_rate = 0.0;
    double l2_miss_rate = 0.0;

    if (p_stats->L1_accesses > 0) {
        l1_miss_rate = static_cast<double>(l1_misses) / p_stats->L1_accesses;
    }

    if (p_stats->L2_accesses > 0) {
        l2_miss_rate = static_cast<double>(l2_misses) / p_stats->L2_accesses;
    }

    double l1_hit_time = 2.0 + 0.2 * L1.S;
    double l2_hit_time = 4.0 + 0.4 * L2.S;
    double l2_miss_penalty = 500.0;
    double l1_miss_penalty = l2_hit_time + l2_miss_rate * l2_miss_penalty;

    p_stats->avg_access_time = l1_hit_time + l1_miss_rate * l1_miss_penalty;
}