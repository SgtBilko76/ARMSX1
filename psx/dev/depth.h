#ifndef DEPTH_H
#define DEPTH_H

#include <stdint.h>

/*
    Stereo depth side channel (VR stereoscopic 3D).

    The PlayStation GPU has no depth buffer, but the GTE knows every projected vertex's depth.
    RTPS/RTPT record "screen position -> inverse depth" here, and when the GPU later receives a
    polygon whose vertex words match those screen positions, it interpolates the depth across the
    triangle into a buffer that sits beside VRAM (psx_gpu_t.depth).

    Values are w = H / SZ: the vertex's projection scale, which is linear in screen space and so
    interpolates correctly. w = 1 is the GTE's projection plane; smaller is farther away.
    Two sentinels mark pixels without 3D depth:
      PSX_DEPTH_FAR   (0)  cleared by a VRAM fill: the empty background
      PSX_DEPTH_FLAT  (-1) sprites, lines, uploads, polygons the GTE did not project

    None of this feeds back into emulation: VRAM contents are identical with it on or off.
*/

#define PSX_DEPTH_FAR 0.0f
#define PSX_DEPTH_FLAT -1.0f

/* Non-zero while stereo depth is being captured; checked before every record. */
extern int g_psx_depth_capture;

void psx_depth_set_capture(int enabled);

/* Called at every vblank. Projections older than the previous frame are forgotten: a 2D sprite
   drawn later at a screen position some 3D vertex once had must not inherit its depth. */
void psx_depth_new_frame(void);
void psx_depth_record(uint32_t sxy, uint32_t h, uint32_t sz);

/* The recorded w for a GPU vertex word, or PSX_DEPTH_FLAT when the GTE never produced it. */
float psx_depth_lookup(uint32_t sxy);

#endif
