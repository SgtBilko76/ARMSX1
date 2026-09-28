#include "depth.h"

#include <string.h>

/*
    Direct-mapped cache of the most recent projections, keyed by the packed SXY word exactly as the
    GTE writes it -- which is what games copy into GPU packets. A frame projects a few thousand
    vertices at most, so collisions only evict older entries, and a vertex that misses simply
    renders flat.
*/

#define DEPTH_CACHE_BITS 14
#define DEPTH_CACHE_SIZE (1u << DEPTH_CACHE_BITS)

typedef struct {
    uint32_t key;
    float w;
} depth_entry_t;

int g_psx_depth_capture = 0;

static depth_entry_t g_depth_cache[DEPTH_CACHE_SIZE];

static inline uint32_t depth_hash(uint32_t sxy) {
    return (sxy * 0x9e3779b1u) >> (32 - DEPTH_CACHE_BITS);
}

void psx_depth_set_capture(int enabled) {
    g_psx_depth_capture = enabled != 0;

    for (uint32_t i = 0; i < DEPTH_CACHE_SIZE; i++) {
        g_depth_cache[i].key = 0;
        g_depth_cache[i].w = PSX_DEPTH_FLAT;
    }
}

void psx_depth_record(uint32_t sxy, uint32_t h, uint32_t sz) {
    if (!sz || !h)
        return;

    depth_entry_t* entry = &g_depth_cache[depth_hash(sxy)];

    entry->key = sxy;
    entry->w = (float)h / (float)sz;
}

float psx_depth_lookup(uint32_t sxy) {
    const depth_entry_t* entry = &g_depth_cache[depth_hash(sxy)];

    return (entry->key == sxy) ? entry->w : PSX_DEPTH_FLAT;
}
