#ifndef PSX_H
#define PSX_H

#include "cpu.h"
#include "log.h"
#include "exe.h"

#include <stdint.h>

#define STR1(m) #m
#define STR(m) STR1(m)

#define PSXE_VERSION STR(REP_VERSION)
#define PSXE_COMMIT STR(REP_COMMIT_HASH)
#define PSXE_BUILD_OS STR(OS_INFO)

#define PSX_SPU_CYCLES_PER_SAMPLE 768 /* 33.8688 MHz / 44.1 kHz */
#define PSX_AUDIO_BUFFER_FRAMES 8192

typedef struct {
    psx_bios_t* bios;
    psx_ram_t* ram;
    psx_dma_t* dma;
    psx_exp1_t* exp1;
    psx_exp2_t* exp2;
    psx_mc1_t* mc1;
    psx_mc2_t* mc2;
    psx_mc3_t* mc3;
    psx_ic_t* ic;
    psx_scratchpad_t* scratchpad;
    psx_gpu_t* gpu;
    psx_spu_t* spu;
    psx_bus_t* bus;
    psx_cpu_t* cpu;
    psx_timer_t* timer;
    psx_cdrom_t* cdrom;
    psx_pad_t* pad;
    psx_mdec_t* mdec;

    /*
        SPU output, produced as the CPU runs: one stereo sample every PSX_SPU_CYCLES_PER_SAMPLE
        cycles, the way the real SPU keeps pace with the CPU. Generating a frame's worth at once
        after the frame lost everything that happened between register writes within it (a
        key-off's release before the next key-on), and every note change clicked.
    */
    int16_t audio[PSX_AUDIO_BUFFER_FRAMES * 2];
    uint32_t audio_read, audio_write;
    uint32_t spu_cycles;
} psx_t;

psx_t* psx_create(void);
int psx_init(psx_t*, const char*, const char*);
int psx_load_expansion(psx_t*, const char*);
int psx_load_bios(psx_t*, const char*);
void psx_hard_reset(psx_t*);
void psx_soft_reset(psx_t*);
void psx_load_state(psx_t*, const char*);
void psx_save_state(psx_t*, const char*);
void psx_load_exe(psx_t*, const char*);
void psx_update(psx_t*);
void psx_run_frame(psx_t*);
void* psx_get_display_buffer(psx_t*);
/* Stereo depth side channel (psx/dev/depth.h), off by default. The depth buffer shares the
   display buffer's layout: 1024 floats per row. NULL while off or in 24-bit display mode. */
void psx_set_stereo_depth(psx_t*, int);
const float* psx_get_display_depth(psx_t*);
/* Takes up to `max_frames` stereo samples produced since the last call; returns how many. */
int psx_read_audio(psx_t*, int16_t* out, int max_frames);
int psx_audio_available(psx_t*);
void* psx_get_vram(psx_t*);
uint32_t psx_get_dmode_width(psx_t*);
uint32_t psx_get_dmode_height(psx_t*);
uint32_t psx_get_display_width(psx_t*);
uint32_t psx_get_display_height(psx_t*);
uint32_t psx_get_display_format(psx_t*);
double psx_get_display_aspect(psx_t*);
uint32_t* psx_take_screenshot(psx_t*);
int psx_swap_disc(psx_t*, const char*);
psx_bios_t* psx_get_bios(psx_t*);
psx_ram_t* psx_get_ram(psx_t*);
psx_dma_t* psx_get_dma(psx_t*);
psx_exp1_t* psx_get_exp1(psx_t*);
psx_exp2_t* psx_get_exp2(psx_t*);
psx_mc1_t* psx_get_mc1(psx_t*);
psx_mc2_t* psx_get_mc2(psx_t*);
psx_mc3_t* psx_get_mc3(psx_t*);
psx_ic_t* psx_get_ic(psx_t*);
psx_scratchpad_t* psx_get_scratchpad(psx_t*);
psx_gpu_t* psx_get_gpu(psx_t*);
psx_spu_t* psx_get_spu(psx_t*);
psx_bus_t* psx_get_bus(psx_t*);
psx_timer_t* psx_get_timer(psx_t*);
psx_cdrom_t* psx_get_cdrom(psx_t*);
psx_pad_t* psx_get_pad(psx_t*);
psx_mdec_t* psx_get_mdec(psx_t*);
psx_cpu_t* psx_get_cpu(psx_t*);
void psx_destroy(psx_t*);

#endif