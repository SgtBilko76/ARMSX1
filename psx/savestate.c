#include "savestate.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bus_init.h"
#include "cpu.h"
#include "dev/cdrom/cdrom.h"
#include "dev/cdrom/queue.h"
#include "dev/mcd.h"

/*
    Save states: the complete emulated machine in one file.

    Each component is stored as its struct's raw bytes, followed by the buffers it owns (RAM,
    VRAM, sound RAM, ...). On load the bytes are copied back but every host pointer the live
    struct holds -- buffers, other components, callbacks, the open disc -- is kept, so a state
    only ever carries emulated data. Because the layout is raw, a state loads only into a build
    with the same struct sizes (checked through a fingerprint in the header), which in practice
    means the same build on the same kind of device.

    Memory cards are deliberately left out: loading a state must never overwrite the saves a
    player made on a card since.

    File: "ARMSXST1", u32 version, u32 fingerprint, u32 section count, then per section a
    4-character tag, a u32 size and the data.
*/

#define STATE_MAGIC "ARMSXST1"
#define STATE_VERSION 1u
#define STATE_MAX_SECTIONS 32

typedef struct {
    char tag[4];
    uint32_t size;
    const uint8_t* data;
} section_t;

static uint32_t fingerprint(void) {
    const size_t sizes[] = {
        offsetof(psx_cpu_t, bus), sizeof(psx_ram_t), sizeof(psx_dma_t), sizeof(psx_exp1_t),
        sizeof(psx_exp2_t), sizeof(psx_mc1_t), sizeof(psx_mc2_t), sizeof(psx_mc3_t),
        sizeof(psx_ic_t), sizeof(psx_scratchpad_t), sizeof(psx_gpu_t), sizeof(psx_spu_t),
        sizeof(psx_timer_t), sizeof(psx_cdrom_t), sizeof(psx_pad_t), sizeof(psx_mcd_t),
        sizeof(psx_mdec_t), sizeof(queue_t), sizeof(void*),
    };
    uint32_t hash = 2166136261u;

    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        hash ^= (uint32_t)sizes[i];
        hash *= 16777619u;
    }

    return hash;
}

/* ------------------------------------------------------------------------------------------ */
/* Writing */

typedef struct {
    FILE* file;
    uint32_t count;
    int error;
} writer_t;

static void put_section(writer_t* w, const char* tag, const void* data, size_t size) {
    const uint32_t size32 = (uint32_t)size;

    if (w->error)
        return;

    if ((fwrite(tag, 1, 4, w->file) != 4) ||
        (fwrite(&size32, sizeof(size32), 1, w->file) != 1) ||
        (size && (fwrite(data, 1, size, w->file) != size))) {
        w->error = 1;
        return;
    }

    w->count++;
}

static void put_queue(writer_t* w, const char* tag, const char* buf_tag, const queue_t* queue) {
    put_section(w, tag, queue, sizeof(*queue));
    put_section(w, buf_tag, queue->buf, queue->size);
}

int psx_state_save(psx_t* psx, const char* path) {
    char temp_path[4096];

    if (snprintf(temp_path, sizeof(temp_path), "%s.tmp", path) >= (int)sizeof(temp_path))
        return 1;

    writer_t w = {0};
    w.file = fopen(temp_path, "wb");

    if (!w.file)
        return 1;

    const uint32_t version = STATE_VERSION;
    const uint32_t print = fingerprint();
    uint32_t count = 0;
    long count_offset;

    fwrite(STATE_MAGIC, 1, 8, w.file);
    fwrite(&version, sizeof(version), 1, w.file);
    fwrite(&print, sizeof(print), 1, w.file);
    count_offset = ftell(w.file);
    fwrite(&count, sizeof(count), 1, w.file);

    put_section(&w, "CPU ", psx->cpu, offsetof(psx_cpu_t, bus));
    put_section(&w, "RAM ", psx->ram, sizeof(psx_ram_t));
    put_section(&w, "RAMB", psx->ram->buf, psx->ram->size);
    put_section(&w, "DMA ", psx->dma, sizeof(psx_dma_t));
    put_section(&w, "EXP1", psx->exp1, sizeof(psx_exp1_t));
    put_section(&w, "EXP2", psx->exp2, sizeof(psx_exp2_t));
    put_section(&w, "MC1 ", psx->mc1, sizeof(psx_mc1_t));
    put_section(&w, "MC2 ", psx->mc2, sizeof(psx_mc2_t));
    put_section(&w, "MC3 ", psx->mc3, sizeof(psx_mc3_t));
    put_section(&w, "IC  ", psx->ic, sizeof(psx_ic_t));
    put_section(&w, "SCRP", psx->scratchpad, sizeof(psx_scratchpad_t));
    put_section(&w, "SCRB", psx->scratchpad->buf, PSX_SCRATCHPAD_SIZE);
    put_section(&w, "GPU ", psx->gpu, sizeof(psx_gpu_t));
    put_section(&w, "VRAM", psx->gpu->vram, PSX_GPU_VRAM_SIZE);
    put_section(&w, "SPU ", psx->spu, sizeof(psx_spu_t));
    put_section(&w, "SPUR", psx->spu->ram, SPU_RAM_SIZE);
    put_section(&w, "TIMR", psx->timer, sizeof(psx_timer_t));
    put_section(&w, "CDRM", psx->cdrom, sizeof(psx_cdrom_t));
    put_queue(&w, "CDQD", "CDBD", psx->cdrom->data);
    put_queue(&w, "CDQR", "CDBR", psx->cdrom->response);
    put_queue(&w, "CDQP", "CDBP", psx->cdrom->parameters);
    put_section(&w, "PAD ", psx->pad, sizeof(psx_pad_t));

    for (int slot = 0; slot < 2; slot++) {
        if (psx->pad->mcd_slot[slot])
            put_section(&w, slot ? "MCD1" : "MCD0", psx->pad->mcd_slot[slot], sizeof(psx_mcd_t));
    }

    put_section(&w, "MDEC", psx->mdec, sizeof(psx_mdec_t));

    if (psx->mdec->input)
        put_section(&w, "MDIN", psx->mdec->input, psx->mdec->input_size);

    if (psx->mdec->output)
        put_section(&w, "MDOU", psx->mdec->output, psx->mdec->output_size);

    put_section(&w, "CORE", &psx->spu_cycles, sizeof(psx->spu_cycles));

    if (!w.error) {
        fseek(w.file, count_offset, SEEK_SET);
        w.error = fwrite(&w.count, sizeof(w.count), 1, w.file) != 1;
    }

    w.error |= fclose(w.file) != 0;

    // Written aside and renamed, so a failed save never destroys the previous state in a slot.
    if (w.error || rename(temp_path, path) != 0) {
        remove(temp_path);
        return 1;
    }

    return 0;
}

/* ------------------------------------------------------------------------------------------ */
/* Reading */

static const section_t* find(const section_t* sections, uint32_t count, const char* tag) {
    for (uint32_t i = 0; i < count; i++) {
        if (!memcmp(sections[i].tag, tag, 4))
            return &sections[i];
    }

    return NULL;
}

/* A section that must exist with exactly `size` bytes. */
static const uint8_t* need(const section_t* sections, uint32_t count, const char* tag, size_t size) {
    const section_t* s = find(sections, count, tag);

    return (s && s->size == size) ? s->data : NULL;
}

static int restore_queue(queue_t* queue, const section_t* sections, uint32_t count, const char* tag,
                         const char* buf_tag) {
    const uint8_t* saved = need(sections, count, tag, sizeof(queue_t));

    if (!saved)
        return 1;

    queue_t state;
    memcpy(&state, saved, sizeof(state));

    const uint8_t* buf = need(sections, count, buf_tag, state.size);

    if (!buf || state.size != queue->size)
        return 1;

    memcpy(queue->buf, buf, queue->size);
    queue->read_index = state.read_index;
    queue->write_index = state.write_index;

    return 0;
}

/* Replaces *buffer with a copy of a saved block, or frees it when the state had none. */
static int restore_block(void** buffer, const section_t* section, int had_buffer) {
    if (!had_buffer) {
        free(*buffer);
        *buffer = NULL;
        return 0;
    }

    if (!section)
        return 1;

    void* copy = malloc(section->size ? section->size : 1);

    if (!copy)
        return 1;

    memcpy(copy, section->data, section->size);
    free(*buffer);
    *buffer = copy;

    return 0;
}

int psx_state_load(psx_t* psx, const char* path) {
    FILE* file = fopen(path, "rb");

    if (!file)
        return 1;

    fseek(file, 0, SEEK_END);
    const long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    if (file_size < 20) {
        fclose(file);
        return 1;
    }

    uint8_t* blob = malloc((size_t)file_size);

    if (!blob || fread(blob, 1, (size_t)file_size, file) != (size_t)file_size) {
        free(blob);
        fclose(file);
        return 1;
    }

    fclose(file);

    int result = 1;
    section_t sections[STATE_MAX_SECTIONS];
    uint32_t version, print, count;

    memcpy(&version, blob + 8, 4);
    memcpy(&print, blob + 12, 4);
    memcpy(&count, blob + 16, 4);

    if (memcmp(blob, STATE_MAGIC, 8) || version != STATE_VERSION || print != fingerprint() ||
        count > STATE_MAX_SECTIONS)
        goto done;

    // Index every section first; nothing is touched until the whole file checks out.
    size_t offset = 20;

    for (uint32_t i = 0; i < count; i++) {
        if (offset + 8 > (size_t)file_size)
            goto done;

        memcpy(sections[i].tag, blob + offset, 4);
        memcpy(&sections[i].size, blob + offset + 4, 4);
        sections[i].data = blob + offset + 8;
        offset += 8 + (size_t)sections[i].size;

        if (offset > (size_t)file_size)
            goto done;
    }

    const uint8_t* cpu = need(sections, count, "CPU ", offsetof(psx_cpu_t, bus));
    const uint8_t* ram = need(sections, count, "RAM ", sizeof(psx_ram_t));
    const uint8_t* ramb = need(sections, count, "RAMB", psx->ram->size);
    const uint8_t* dma = need(sections, count, "DMA ", sizeof(psx_dma_t));
    const uint8_t* exp1 = need(sections, count, "EXP1", sizeof(psx_exp1_t));
    const uint8_t* exp2 = need(sections, count, "EXP2", sizeof(psx_exp2_t));
    const uint8_t* mc1 = need(sections, count, "MC1 ", sizeof(psx_mc1_t));
    const uint8_t* mc2 = need(sections, count, "MC2 ", sizeof(psx_mc2_t));
    const uint8_t* mc3 = need(sections, count, "MC3 ", sizeof(psx_mc3_t));
    const uint8_t* ic = need(sections, count, "IC  ", sizeof(psx_ic_t));
    const uint8_t* scrp = need(sections, count, "SCRP", sizeof(psx_scratchpad_t));
    const uint8_t* scrb = need(sections, count, "SCRB", PSX_SCRATCHPAD_SIZE);
    const uint8_t* gpu = need(sections, count, "GPU ", sizeof(psx_gpu_t));
    const uint8_t* vram = need(sections, count, "VRAM", PSX_GPU_VRAM_SIZE);
    const uint8_t* spu = need(sections, count, "SPU ", sizeof(psx_spu_t));
    const uint8_t* spur = need(sections, count, "SPUR", SPU_RAM_SIZE);
    const uint8_t* timer = need(sections, count, "TIMR", sizeof(psx_timer_t));
    const uint8_t* cdrom = need(sections, count, "CDRM", sizeof(psx_cdrom_t));
    const uint8_t* pad = need(sections, count, "PAD ", sizeof(psx_pad_t));
    const uint8_t* mdec = need(sections, count, "MDEC", sizeof(psx_mdec_t));
    const uint8_t* core = need(sections, count, "CORE", sizeof(psx->spu_cycles));

    if (!cpu || !ram || !ramb || !dma || !exp1 || !exp2 || !mc1 || !mc2 || !mc3 || !ic || !scrp ||
        !scrb || !gpu || !vram || !spu || !spur || !timer || !cdrom || !pad || !mdec || !core)
        goto done;

    {
        const queue_t* q[3] = {psx->cdrom->data, psx->cdrom->response, psx->cdrom->parameters};
        const char* tags[3][2] = {{"CDQD", "CDBD"}, {"CDQR", "CDBR"}, {"CDQP", "CDBP"}};

        for (int i = 0; i < 3; i++) {
            const uint8_t* saved = need(sections, count, tags[i][0], sizeof(queue_t));
            queue_t state;

            if (!saved)
                goto done;

            memcpy(&state, saved, sizeof(state));

            if (state.size != q[i]->size || !need(sections, count, tags[i][1], state.size))
                goto done;
        }
    }

    // Everything checks out: restore, keeping every host pointer of the live machine.

    memcpy(psx->cpu, cpu, offsetof(psx_cpu_t, bus));
    psx_cpu_invalidate_cache(psx->cpu);

    {
        psx_ram_t live = *psx->ram;
        memcpy(psx->ram, ram, sizeof(psx_ram_t));
        psx->ram->mc2 = live.mc2;
        psx->ram->buf = live.buf;
        psx->ram->size = live.size;
        memcpy(psx->ram->buf, ramb, live.size);
    }
    {
        psx_dma_t live = *psx->dma;
        memcpy(psx->dma, dma, sizeof(psx_dma_t));
        psx->dma->bus = live.bus;
        psx->dma->ic = live.ic;
    }
    {
        psx_exp1_t live = *psx->exp1;
        memcpy(psx->exp1, exp1, sizeof(psx_exp1_t));
        psx->exp1->mc1 = live.mc1;
        psx->exp1->rom = live.rom;
    }
    {
        psx_exp2_t live = *psx->exp2;
        memcpy(psx->exp2, exp2, sizeof(psx_exp2_t));
        psx->exp2->duart_udata = live.duart_udata;
        psx->exp2->atcons_udata = live.atcons_udata;
        psx->exp2->duart_tx = live.duart_tx;
        psx->exp2->atcons_tx = live.atcons_tx;
    }
    memcpy(psx->mc1, mc1, sizeof(psx_mc1_t));
    memcpy(psx->mc2, mc2, sizeof(psx_mc2_t));
    memcpy(psx->mc3, mc3, sizeof(psx_mc3_t));
    {
        psx_ic_t live = *psx->ic;
        memcpy(psx->ic, ic, sizeof(psx_ic_t));
        psx->ic->cpu = live.cpu;
    }
    {
        psx_scratchpad_t live = *psx->scratchpad;
        memcpy(psx->scratchpad, scrp, sizeof(psx_scratchpad_t));
        psx->scratchpad->buf = live.buf;
        memcpy(psx->scratchpad->buf, scrb, PSX_SCRATCHPAD_SIZE);
    }
    {
        psx_gpu_t live = *psx->gpu;
        memcpy(psx->gpu, gpu, sizeof(psx_gpu_t));
        memcpy(psx->gpu->udata, live.udata, sizeof(live.udata));
        psx->gpu->vram = live.vram;
        psx->gpu->empty = live.empty;
        psx->gpu->ic = live.ic;
        memcpy(psx->gpu->event_cb_table, live.event_cb_table, sizeof(live.event_cb_table));
#ifdef USE_HARDWARE
        psx->gpu->renderer = live.renderer;
#endif
        psx->gpu->depth = live.depth;
        memcpy(psx->gpu->vram, vram, PSX_GPU_VRAM_SIZE);

        // The stereo depth described the old picture; start it over as flat.
        if (psx->gpu->depth) {
            for (int i = 0; i < PSX_GPU_FB_WIDTH * PSX_GPU_FB_HEIGHT; i++)
                psx->gpu->depth[i] = -1.0f;
        }
    }
    {
        psx_spu_t live = *psx->spu;
        memcpy(psx->spu, spu, sizeof(psx_spu_t));
        psx->spu->ic = live.ic;
        psx->spu->ram = live.ram;
        memcpy(psx->spu->ram, spur, SPU_RAM_SIZE);
    }
    {
        psx_timer_t live = *psx->timer;
        memcpy(psx->timer, timer, sizeof(psx_timer_t));
        psx->timer->ic = live.ic;
        psx->timer->gpu = live.gpu;
    }
    {
        psx_cdrom_t live = *psx->cdrom;
        memcpy(psx->cdrom, cdrom, sizeof(psx_cdrom_t));
        psx->cdrom->disc = live.disc;
        psx->cdrom->ic = live.ic;
        psx->cdrom->data = live.data;
        psx->cdrom->response = live.response;
        psx->cdrom->parameters = live.parameters;

        if (restore_queue(psx->cdrom->data, sections, count, "CDQD", "CDBD") ||
            restore_queue(psx->cdrom->response, sections, count, "CDQR", "CDBR") ||
            restore_queue(psx->cdrom->parameters, sections, count, "CDQP", "CDBP"))
            goto done; /* unreachable: validated above */
    }
    {
        psx_pad_t live = *psx->pad;
        memcpy(psx->pad, pad, sizeof(psx_pad_t));
        psx->pad->ic = live.ic;
        memcpy(psx->pad->joy_slot, live.joy_slot, sizeof(live.joy_slot));
        memcpy(psx->pad->mcd_slot, live.mcd_slot, sizeof(live.mcd_slot));

        for (int slot = 0; slot < 2; slot++) {
            psx_mcd_t* card = psx->pad->mcd_slot[slot];
            const uint8_t* saved = need(sections, count, slot ? "MCD1" : "MCD0", sizeof(psx_mcd_t));

            // The transfer state only; the card's contents stay what is on the file.
            if (card && saved) {
                psx_mcd_t live_card = *card;
                memcpy(card, saved, sizeof(psx_mcd_t));
                card->path = live_card.path;
                card->buf = live_card.buf;
            }
        }
    }
    {
        psx_mdec_t live = *psx->mdec;
        psx_mdec_t state;
        memcpy(&state, mdec, sizeof(state));
        memcpy(psx->mdec, mdec, sizeof(psx_mdec_t));
        psx->mdec->input = live.input;
        psx->mdec->output = live.output;

        void* input = psx->mdec->input;
        void* output = psx->mdec->output;

        if (restore_block(&input, find(sections, count, "MDIN"), state.input != NULL) ||
            restore_block(&output, find(sections, count, "MDOU"), state.output != NULL)) {
            psx->mdec->input = input;
            psx->mdec->output = output;
            goto done;
        }

        psx->mdec->input = input;
        psx->mdec->output = output;
    }

    memcpy(&psx->spu_cycles, core, sizeof(psx->spu_cycles));
    psx->audio_read = psx->audio_write;

    result = 0;

done:
    free(blob);
    return result;
}
