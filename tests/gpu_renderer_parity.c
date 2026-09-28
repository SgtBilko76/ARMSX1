#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../psx/dev/depth.h"
#include "../psx/dev/gpu.h"
#include "../frontend/gpu_hw.h"

void log_log(int level, const char* file, int line, const char* format, ...) {
    (void)level;
    (void)file;
    (void)line;
    (void)format;
}

void psx_ic_irq(psx_ic_t* ic, int id) {
    (void)ic;
    (void)id;
}

static psx_gpu_t* make_gpu(void) {
    psx_gpu_t* gpu = psx_gpu_create();
    if (!gpu)
        return NULL;
    psx_gpu_init(gpu, NULL);
    gpu->draw_x1 = 0;
    gpu->draw_y1 = 0;
    gpu->draw_x2 = PSX_GPU_FB_WIDTH - 1;
    gpu->draw_y2 = PSX_GPU_FB_HEIGHT - 1;
    return gpu;
}

static int run_case(const char* name, vertex_t a, vertex_t b, vertex_t c, poly_data_t data, int edge) {
    psx_gpu_t* reference = make_gpu();
    psx_gpu_t* accelerated = make_gpu();
    if (!reference || !accelerated) {
        fprintf(stderr, "GPU_PARITY failed case=%s reason=allocation\n", name);
        return 1;
    }

    for (size_t index = 0; index < PSX_GPU_VRAM_SIZE / sizeof(uint16_t); ++index) {
        const uint16_t value = (uint16_t)((index * 1103515245u + 12345u) >> 16);
        reference->vram[index] = value;
        accelerated->vram[index] = value;
    }

    gpu_render_triangle(reference, a, b, c, data, edge);
    gpu_hw_render_triangle(accelerated, a, b, c, data, edge);

    const int mismatch = memcmp(reference->vram, accelerated->vram, PSX_GPU_VRAM_SIZE) != 0;
    psx_gpu_destroy(reference);
    psx_gpu_destroy(accelerated);
    if (mismatch) {
        fprintf(stderr, "GPU_PARITY failed case=%s reason=vram-mismatch\n", name);
        return 1;
    }
    printf("GPU_PARITY passed case=%s\n", name);
    return 0;
}

/* The stereo depth side channel must leave VRAM untouched and record interpolated depth. */
static int run_stereo_depth_case(void) {
    psx_gpu_t* plain = make_gpu();
    psx_gpu_t* stereo = make_gpu();
    if (!plain || !stereo) {
        fprintf(stderr, "GPU_PARITY failed case=stereo-depth reason=allocation\n");
        return 1;
    }
    memset(plain->vram, 0, PSX_GPU_VRAM_SIZE);
    memset(stereo->vram, 0, PSX_GPU_VRAM_SIZE);
    psx_gpu_set_depth_enabled(stereo, 1);
    if (!stereo->depth || !g_psx_depth_capture) {
        fprintf(stderr, "GPU_PARITY failed case=stereo-depth reason=not-enabled\n");
        return 1;
    }

    /* Vertex depths as the GTE would record them: w = H / SZ. */
    const uint32_t words[3] = {0x00140010u, 0x00200060u, 0x00700030u};
    psx_depth_record(words[0], 256, 512);  /* w = 0.5  */
    psx_depth_record(words[1], 256, 1024); /* w = 0.25 */
    psx_depth_record(words[2], 256, 256);  /* w = 1.0  */

    poly_data_t data = {.attrib = 0};
    vertex_t v[3];
    for (int i = 0; i < 3; i++) {
        v[i] = (vertex_t){.x = (int16_t)(words[i] & 0xffff), .y = (int16_t)(words[i] >> 16), .c = 0x40a0f0};
        v[i].w = psx_depth_lookup(words[i]);
        data.v[i] = v[i];
    }

    gpu_render_triangle(plain, v[0], v[1], v[2], data, 0);
    gpu_render_triangle(stereo, v[0], v[1], v[2], data, 0);

    int failed = memcmp(plain->vram, stereo->vram, PSX_GPU_VRAM_SIZE) != 0;
    if (failed)
        fprintf(stderr, "GPU_PARITY failed case=stereo-depth reason=vram-changed\n");

    /* A pixel inside the triangle lies between the vertex depths; one outside stays flat. */
    const float inside = stereo->depth[50 + 40 * 1024];
    if (!(inside > 0.25f && inside < 1.0f)) {
        fprintf(stderr, "GPU_PARITY failed case=stereo-depth reason=interpolation value=%f\n", inside);
        failed = 1;
    }
    if (stereo->depth[500 + 400 * 1024] != PSX_DEPTH_FLAT) {
        fprintf(stderr, "GPU_PARITY failed case=stereo-depth reason=untouched-not-flat\n");
        failed = 1;
    }
    if (psx_depth_lookup(0x01230456u) != PSX_DEPTH_FLAT) {
        fprintf(stderr, "GPU_PARITY failed case=stereo-depth reason=unknown-vertex\n");
        failed = 1;
    }

    psx_gpu_destroy(plain);
    psx_gpu_destroy(stereo);
    if (g_psx_depth_capture) {
        fprintf(stderr, "GPU_PARITY failed case=stereo-depth reason=capture-left-on\n");
        failed = 1;
    }
    if (!failed)
        printf("GPU_PARITY passed case=stereo-depth\n");
    return failed;
}

int main(void) {
    int failed = 0;
    poly_data_t flat = {.attrib = 0};
    flat.v[0].c = flat.v[1].c = flat.v[2].c = 0x40a0f0;
    failed |= run_case(
        "flat",
        (vertex_t){.x = 10, .y = 12, .c = 0x40a0f0},
        (vertex_t){.x = 92, .y = 24, .c = 0x40a0f0},
        (vertex_t){.x = 38, .y = 105, .c = 0x40a0f0},
        flat,
        0
    );

    poly_data_t shaded = {.attrib = PA_SHADED};
    failed |= run_case(
        "shaded-dithered",
        (vertex_t){.x = 120, .y = 80, .c = 0x0000ff},
        (vertex_t){.x = 220, .y = 100, .c = 0x00ff00},
        (vertex_t){.x = 160, .y = 210, .c = 0xff0000},
        shaded,
        0
    );

    poly_data_t transparent = {.attrib = PA_TRANSP};
    transparent.v[0].c = transparent.v[1].c = transparent.v[2].c = 0xffffff;
    failed |= run_case(
        "semi-transparent",
        (vertex_t){.x = 300, .y = 40, .c = 0xffffff},
        (vertex_t){.x = 410, .y = 70, .c = 0xffffff},
        (vertex_t){.x = 340, .y = 170, .c = 0xffffff},
        transparent,
        1
    );

    failed |= run_stereo_depth_case();

    if (failed)
        return 1;
    puts("GPU_PARITY all cases passed");
    return 0;
}
