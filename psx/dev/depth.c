#include "depth.h"

#include <string.h>

/*
    Set-associative cache of the most recent projections, keyed by the packed SXY word exactly as
    the GTE writes it -- which is what games copy into GPU packets. A busy 3D frame projects
    several thousand vertices, and lookups accept the previous frame's too, so a direct-mapped
    table lost a real share of them to collisions: those vertices then rendered with a borrowed
    depth that changed from frame to frame, and their triangles popped in and out of the screen
    in one eye. Each set holds several keys; a new key replaces the oldest entry.
*/

#define DEPTH_SET_BITS 14
#define DEPTH_SET_COUNT (1u << DEPTH_SET_BITS)
#define DEPTH_WAYS 4

typedef struct {
    uint32_t key;
    uint32_t frame;
    float w;
} depth_entry_t;

int g_psx_depth_capture = 0;

static uint32_t g_depth_frame = 1;

static depth_entry_t g_depth_cache[DEPTH_SET_COUNT][DEPTH_WAYS];

static inline depth_entry_t* depth_set(uint32_t sxy) {
    return g_depth_cache[(sxy * 0x9e3779b1u) >> (32 - DEPTH_SET_BITS)];
}

/*
    How many vblanks a projection stays usable. A 30 fps game (Crash Bandicoot and most 3D
    titles) projects a frame's vertices, then the GPU draws its ordering table two or three
    vblanks later, while the CPU already projects the next one. With a one-vblank window those
    vertices missed at random, their triangles fell back to flat, and the scene's depth flickered
    between the eyes. Four vblanks covers 30 and 20 fps double buffering; a 2D sprite still only
    inherits depth if it lands on the exact packed coordinates of a vertex from the last moments.
*/
#define DEPTH_MAX_AGE 4u

static inline int depth_entry_live(const depth_entry_t* entry) {
    return entry->frame && ((g_depth_frame - entry->frame) <= DEPTH_MAX_AGE);
}

void psx_depth_set_capture(int enabled) {
    g_psx_depth_capture = enabled != 0;

    memset(g_depth_cache, 0, sizeof(g_depth_cache));
}

void psx_depth_record(uint32_t sxy, uint32_t h, uint32_t sz) {
    if (!sz || !h)
        return;

    depth_entry_t* set = depth_set(sxy);
    depth_entry_t* slot = &set[0];

    for (int i = 0; i < DEPTH_WAYS; i++) {
        // The same screen position again: the newest projection is the one the game is about to
        // put in a packet.
        if (set[i].key == sxy && depth_entry_live(&set[i])) {
            slot = &set[i];
            break;
        }
        // Otherwise the oldest (or never used) entry gives way.
        if ((g_depth_frame - set[i].frame) > (g_depth_frame - slot->frame))
            slot = &set[i];
    }

    slot->key = sxy;
    slot->frame = g_depth_frame;
    slot->w = (float)h / (float)sz;
}

void psx_depth_new_frame(void) {
    g_depth_frame++;
}

float psx_depth_lookup(uint32_t sxy) {
    const depth_entry_t* set = depth_set(sxy);

    for (int i = 0; i < DEPTH_WAYS; i++) {
        if (set[i].key == sxy && depth_entry_live(&set[i]))
            return set[i].w;
    }

    return PSX_DEPTH_FLAT;
}
