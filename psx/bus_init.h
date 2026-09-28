#ifndef BUS_INIT_H
#define BUS_INIT_H

#include "dev/cdrom/cdrom.h"
#include "dev/bios.h"
#include "dev/ram.h"
#include "dev/dma.h"
#include "dev/exp1.h"
#include "dev/exp2.h"
#include "dev/mc1.h"
#include "dev/mc2.h"
#include "dev/mc3.h"
#include "dev/ic.h"
#include "dev/scratchpad.h"
#include "dev/gpu.h"
#include "dev/spu.h"
#include "dev/timer.h"
#include "dev/pad.h"
#include "dev/mdec.h"

struct psx_bus_t {
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
    psx_timer_t* timer;
    psx_cdrom_t* cdrom;
    psx_pad_t* pad;
    psx_mdec_t* mdec;

    uint32_t access_cycles;
    psx_bus_write_observer_t write_observer;
    void* write_observer_udata;
};

extern const uint32_t g_psx_bus_region_mask_table[];

/*
    psx_bus_read32 for the hot callers (instruction fetch and word loads). Code and most data live
    in RAM or the BIOS, so those two are decoded inline with exactly the semantics of their
    HANDLE_READ entries and psx_ram_read32/psx_bios_read32; anything else, and unaligned
    addresses, take the full bus path.
*/
static inline uint32_t psx_bus_read32_fast(psx_bus_t* bus, uint32_t addr) {
    const uint32_t phys = addr & g_psx_bus_region_mask_table[addr >> 29];

    if (!(phys & 0x3)) {
        const psx_ram_t* ram = bus->ram;

        if ((phys >= ram->io_base) && (phys < (ram->io_base + ram->io_size))) {
            uint32_t offset = phys - ram->io_base;

            bus->access_cycles = ram->bus_delay;

            if ((((ram->mc2->ram_size >> 9) & 7) == 3) && (offset >= 0x400000))
                return 0xffffffff;

            offset &= ram->size - 1;

            return *((const uint32_t*)(ram->buf + offset));
        }

        const psx_bios_t* bios = bus->bios;

        if ((phys >= bios->io_base) && (phys < (bios->io_base + bios->io_size))) {
            bus->access_cycles = bios->bus_delay;

            return *((const uint32_t*)(bios->buf + (phys - bios->io_base)));
        }
    }

    return psx_bus_read32(bus, addr);
}

void psx_bus_init_bios(psx_bus_t*, psx_bios_t*);
void psx_bus_init_ram(psx_bus_t*, psx_ram_t*);
void psx_bus_init_dma(psx_bus_t*, psx_dma_t*);
void psx_bus_init_exp1(psx_bus_t*, psx_exp1_t*);
void psx_bus_init_exp2(psx_bus_t*, psx_exp2_t*);
void psx_bus_init_mc1(psx_bus_t*, psx_mc1_t*);
void psx_bus_init_mc2(psx_bus_t*, psx_mc2_t*);
void psx_bus_init_mc3(psx_bus_t*, psx_mc3_t*);
void psx_bus_init_ic(psx_bus_t*, psx_ic_t*);
void psx_bus_init_scratchpad(psx_bus_t*, psx_scratchpad_t*);
void psx_bus_init_gpu(psx_bus_t*, psx_gpu_t*);
void psx_bus_init_spu(psx_bus_t*, psx_spu_t*);
void psx_bus_init_timer(psx_bus_t*, psx_timer_t*);
void psx_bus_init_cdrom(psx_bus_t*, psx_cdrom_t*);
void psx_bus_init_pad(psx_bus_t*, psx_pad_t*);
void psx_bus_init_mdec(psx_bus_t*, psx_mdec_t*);

#endif
