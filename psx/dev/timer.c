#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "timer.h"

#include <math.h>
#include "../log.h"

#define T0_COUNTER     timer->timer[0].counter
#define T0_SYNC_EN     timer->timer[0].sync_enable
#define T0_SYNC_MODE   timer->timer[0].sync_mode
#define T0_RESET_TGT   timer->timer[0].reset_target
#define T0_IRQ_TGT     timer->timer[0].irq_target
#define T0_IRQ_MAX     timer->timer[0].irq_max
#define T0_IRQ_REPEAT  timer->timer[0].irq_repeat
#define T0_IRQ_TOGGLE  timer->timer[0].irq_toggle
#define T0_CLKSRC      timer->timer[0].clk_source
#define T0_IRQ         timer->timer[0].irq
#define T0_TGT_REACHED timer->timer[0].target_reached
#define T0_MAX_REACHED timer->timer[0].max_reached
#define T0_IRQ_FIRED   timer->timer[0].irq_fired
#define T0_PAUSED      timer->timer[0].paused
#define T0_BLANK_ONCE  timer->timer[0].blank_once
#define T1_COUNTER     timer->timer[1].counter
#define T1_SYNC_EN     timer->timer[1].sync_enable
#define T1_SYNC_MODE   timer->timer[1].sync_mode
#define T1_RESET_TGT   timer->timer[1].reset_target
#define T1_IRQ_TGT     timer->timer[1].irq_target
#define T1_IRQ_MAX     timer->timer[1].irq_max
#define T1_IRQ_REPEAT  timer->timer[1].irq_repeat
#define T1_IRQ_TOGGLE  timer->timer[1].irq_toggle
#define T1_CLKSRC      timer->timer[1].clk_source
#define T1_IRQ         timer->timer[1].irq
#define T1_TGT_REACHED timer->timer[1].target_reached
#define T1_MAX_REACHED timer->timer[1].max_reached
#define T1_IRQ_FIRED   timer->timer[1].irq_fired
#define T1_PAUSED      timer->timer[1].paused
#define T1_BLANK_ONCE  timer->timer[1].blank_once
#define T2_COUNTER     timer->timer[2].counter
#define T2_SYNC_EN     timer->timer[2].sync_enable
#define T2_SYNC_MODE   timer->timer[2].sync_mode
#define T2_RESET_TGT   timer->timer[2].reset_target
#define T2_IRQ_TGT     timer->timer[2].irq_target
#define T2_IRQ_MAX     timer->timer[2].irq_max
#define T2_IRQ_REPEAT  timer->timer[2].irq_repeat
#define T2_IRQ_TOGGLE  timer->timer[2].irq_toggle
#define T2_CLKSRC      timer->timer[2].clk_source
#define T2_IRQ         timer->timer[2].irq
#define T2_TGT_REACHED timer->timer[2].target_reached
#define T2_MAX_REACHED timer->timer[2].max_reached
#define T2_IRQ_FIRED   timer->timer[2].irq_fired
#define T2_PAUSED      timer->timer[2].paused
#define T2_BLANK_ONCE  timer->timer[2].blank_once
#define T2_DIV_COUNTER timer->timer[2].div_counter

// The exact per-step increment of timer i, or a negative value when its step is not a constant
// (the dotclock, which depends on the display mode) and it must be stepped one at a time.
static inline float timer_step(const psx_timer_t* timer, int i) {
    const int clk = timer->timer[i].clk_source;

    switch (i) {
        case 0: return (clk & 1) ? -1.0f : 2.0f;
        case 1: return (clk & 1) ? 0.0f : 2.0f;
        default: return (clk <= 1) ? 2.0f : 0.25f;
    }
}

// Applies the idle steps tallied since the last full update. Each of them only added a constant
// that stays exactly representable (the counter never passes FFFFh + 2 during idle steps), so one
// addition gives the very same float as the step-by-step sums.
static void timer_sync(psx_timer_t* timer) {
    if (timer->pending) {
        for (int i = 0; i < 3; i++) {
            if (!timer->timer[i].paused)
                timer->timer[i].counter += timer_step(timer, i) * (float)timer->pending;
        }

        timer->pending = 0;
    }

    timer->prev_hblank = timer->hblank;
    timer->prev_vblank = timer->vblank;
}

// Called before anything reads or changes timer state from outside psx_timer_update.
static inline void timer_touch(psx_timer_t* timer) {
    timer_sync(timer);
    timer->idle_updates = 0;
}

// How many upcoming updates are certain to do nothing but add each counter's step.
static uint32_t timer_idle_budget(const psx_timer_t* timer) {
    const uint32_t kMaxBudget = 1u << 16;
    uint32_t budget = kMaxBudget;

    for (int i = 0; i < 3; i++) {
        if (timer->timer[i].paused)
            continue;

        const float step = timer_step(timer, i);

        if (step < 0.0f)
            return 0;

        const double counter = timer->timer[i].counter;
        // Once "target reached" is latched and the target neither resets the counter nor raises
        // an IRQ, passing it again only sets the flag that is already set: only FFFFh matters.
        const int target_inert = timer->timer[i].target_reached &&
                                 !timer->timer[i].reset_target &&
                                 !timer->timer[i].irq_target;
        const double threshold = (!target_inert && ((float)timer->timer[i].target < 65535.0f))
            ? (float)timer->timer[i].target
            : 65535.0f;

        if (counter > threshold)
            return 0;

        if (step == 0.0f)
            continue;

        // The first step that ends past the threshold must run for real.
        const double idle = floor((threshold - counter) / step);

        if (idle < (double)budget)
            budget = (uint32_t)idle;
    }

    return budget;
}

uint16_t timer_get_mode(psx_timer_t* timer, int index) {
    uint16_t value = (timer->timer[index].sync_enable << 0)     |
                     (timer->timer[index].sync_mode << 1)       |
                     (timer->timer[index].reset_target << 3)    |
                     (timer->timer[index].irq_target << 4)      |
                     (timer->timer[index].irq_max << 5)         |
                     (timer->timer[index].irq_repeat << 6)      |
                     (timer->timer[index].irq_toggle << 7)      |
                     (timer->timer[index].clk_source << 8)      |
                     (timer->timer[index].irq << 10)            |
                     (timer->timer[index].target_reached << 11) |
                     (timer->timer[index].max_reached << 12);

    timer->timer[index].target_reached = 0;
    timer->timer[index].max_reached = 0;

    return value;
}

void timer_set_mode(psx_timer_t* timer, int index, uint16_t value) {
    timer->timer[index].sync_enable = (value >> 0) & 1;
    timer->timer[index].sync_mode = (value >> 1) & 3;
    timer->timer[index].reset_target = (value >> 3) & 1;
    timer->timer[index].irq_target = (value >> 4) & 1;
    timer->timer[index].irq_max = (value >> 5) & 1;
    timer->timer[index].irq_repeat = (value >> 6) & 1;
    timer->timer[index].irq_toggle = (value >> 7) & 1;
    timer->timer[index].clk_source = (value >> 8) & 3;
    timer->timer[index].target_reached = 0;
    timer->timer[index].max_reached = 0;

    // IRQ and counter are set to 0 on mode writes
    timer->timer[index].irq = 1;
    timer->timer[index].irq_fired = 0;
    timer->timer[index].counter = 0;
    timer->timer[index].div_counter = 0;
    timer->timer[index].blank_once = 0;
    timer->timer[index].paused = 0;

    // printf(
    //     "timer_set_mode %u %04x\n"
    //     "sync_enable    %u\n"
    //     "sync_mode      %u\n"
    //     "reset_target   %u\n"
    //     "irq_target     %u\n"
    //     "irq_max        %u\n"
    //     "irq_repeat     %u\n"
    //     "irq_toggle     %u\n"
    //     "clk_source     %u\n"
    //     "target_reached %u\n"
    //     "max_reached    %u\n"
    //     "target         %04x\n",
    //     index, value,
    //     timer->timer[index].sync_enable,
    //     timer->timer[index].sync_mode,
    //     timer->timer[index].reset_target,
    //     timer->timer[index].irq_target,
    //     timer->timer[index].irq_max,
    //     timer->timer[index].irq_repeat,
    //     timer->timer[index].irq_toggle,
    //     timer->timer[index].clk_source,
    //     timer->timer[index].target_reached,
    //     timer->timer[index].max_reached,
    //     timer->timer[index].target
    // );

    switch (index) {
        case 0: {
            if ((T0_SYNC_MODE == 1) || (T0_SYNC_MODE == 2) || !T0_SYNC_EN)
                return;

            T0_PAUSED = timer->hblank | (T0_SYNC_MODE == 3);
        } break;

        case 1: {
            if ((T1_SYNC_MODE == 1) || (T1_SYNC_MODE == 2) || !T1_SYNC_EN)
                return;

            T1_PAUSED = timer->vblank | (T1_SYNC_MODE == 3);
        } break;

        case 2: {
            if (!T2_SYNC_EN)
                return;

            T2_PAUSED = (T2_SYNC_MODE == 0) || (T2_SYNC_MODE == 3);
        } break;
    }
}

const char* g_psx_timer_reg_names[] = {
    "counter", 0, 0, 0,
    "mode", 0, 0, 0,
    "target", 0, 0, 0
};

psx_timer_t* psx_timer_create(void) {
    return (psx_timer_t*)malloc(sizeof(psx_timer_t));
}

void psx_timer_init(psx_timer_t* timer, psx_ic_t* ic, psx_gpu_t* gpu) {
    memset(timer, 0, sizeof(psx_timer_t));

    timer->io_base = PSX_TIMER_BEGIN;
    timer->io_size = PSX_TIMER_SIZE;

    timer->ic = ic;
    timer->gpu = gpu;
}

uint32_t psx_timer_read32(psx_timer_t* timer, uint32_t offset) {
    timer_touch(timer);

    int index = offset >> 4;
    int reg = offset & 0xf;

    switch (reg) {
        case 0: return timer->timer[index].counter;
        case 4: return timer_get_mode(timer, index);
        case 8: return timer->timer[index].target;
    }

    printf("Unhandled 32-bit TIMER read at offset %08x\n", offset);

    return 0x0;
}

uint16_t psx_timer_read16(psx_timer_t* timer, uint32_t offset) {
    timer_touch(timer);

    int index = offset >> 4;
    int reg = offset & 0xf;

    switch (reg) {
        case 0: return timer->timer[index].counter;
        case 4: return timer_get_mode(timer, index);
        case 8: return timer->timer[index].target;
    }

    printf("Unhandled 16-bit TIMER read at offset %08x\n", offset);

    return 0x0;
}

uint8_t psx_timer_read8(psx_timer_t* timer, uint32_t offset) {
    printf("Unhandled 8-bit TIMER read at offset %08x\n", offset);

    return 0x0;
}

void timer_handle_irq(psx_timer_t* timer, int i);

void psx_timer_write32(psx_timer_t* timer, uint32_t offset, uint32_t value) {
    timer_touch(timer);

    int index = offset >> 4;
    int reg = offset & 0xf;

    switch (reg) {
        case 0: timer->timer[index].counter = value & 0xffff; break;
        case 4: timer_set_mode(timer, index, value); break;
        case 8: timer->timer[index].target = value & 0xffff; break;
    }

    timer_handle_irq(timer, index);
}

void psx_timer_write16(psx_timer_t* timer, uint32_t offset, uint16_t value) {
    timer_touch(timer);

    int index = offset >> 4;
    int reg = offset & 0xf;

    switch (reg) {
        case 0: timer->timer[index].counter = value & 0xffff; break;
        case 4: timer_set_mode(timer, index, value); break;
        case 8: timer->timer[index].target = value & 0xffff; break;
    }

    timer_handle_irq(timer, index);
}

void psx_timer_write8(psx_timer_t* timer, uint32_t offset, uint8_t value) {
    printf("Unhandled 8-bit TIMER write at offset %08x (%02x)\n", offset, value);
}

void timer_handle_irq(psx_timer_t* timer, int i) {
    int irq = 0;

    int target_reached = timer->timer[i].counter > timer->timer[i].target;
    int max_reached = timer->timer[i].counter > 65535.0f;

    if (target_reached) {
        timer->timer[i].target_reached = 1;

        // if ((i == 1) && (T1_CLKSRC == 1))
        //     printf("target %04x (%f) reached\n", timer->timer[i].target, timer->timer[i].counter);

        if (timer->timer[i].reset_target)
            timer->timer[i].counter = 0;

        if (timer->timer[i].irq_target)
            irq = 1;
    }

    if (max_reached) {
        timer->timer[i].counter = 0;
        timer->timer[i].max_reached = 1;

        if (timer->timer[i].irq_max)
            irq = 1;
    }

    if (!irq)
        return;

    if (!timer->timer[i].irq_toggle) {
        timer->timer[i].irq = 0;
    } else {
        timer->timer[i].irq ^= 1;
    }

    int trigger = !timer->timer[i].irq;

    if (!timer->timer[i].irq_repeat) {
        if (trigger && !timer->timer[i].irq_fired) {
            timer->timer[i].irq_fired = 1;
        } else {
            return;
        }
    }

    timer->timer[i].irq = 1;

    if (trigger) {
        psx_ic_irq(timer->ic, 16 << i);
    }
}

// timer_handle_irq only acts once a counter has passed its target or 0xffff. Checking that
// inline (with the very same comparisons) keeps the per-instruction update from calling it.
static inline void timer_check_irq(psx_timer_t* timer, int i) {
    if ((timer->timer[i].counter > timer->timer[i].target) || (timer->timer[i].counter > 65535.0f))
        timer_handle_irq(timer, i);
}

static inline float timer_get_dotclock_div(psx_timer_t* timer) {
    static const float dmode_dotclk_div_table[] = {
        10.0f, 8.0f, 5.0f, 4.0f
    };

    if (timer->gpu->display_mode & 0x40) {
        return 11.0f / 7.0f / 7.0f;
    } else {
        return 11.0f / 7.0f / dmode_dotclk_div_table[timer->gpu->display_mode & 0x3];
    }
}

static inline void timer_update_timer0(psx_timer_t* timer, int cyc) {
    if (T0_PAUSED)
        return;

    if (T0_CLKSRC & 1) {
        // Dotclock test
        T0_COUNTER += (float)cyc * timer_get_dotclock_div(timer);
    } else {
        T0_COUNTER += (float)cyc;
    }

    timer_check_irq(timer, 0);
}

static inline void timer_update_timer1(psx_timer_t* timer, int cyc) {
    if (T1_PAUSED)
        return;

    if (T1_CLKSRC & 1) {
        // Counter is incremented in our hblank callback
    } else {
        T1_COUNTER += (float)cyc;
    }

    timer_check_irq(timer, 1);
}

static inline void timer_update_timer2(psx_timer_t* timer, int cyc) {
    if (T2_PAUSED)
        return;

    if (T2_CLKSRC <= 1) {
        T2_COUNTER += (float)cyc;
    } else {
        T2_COUNTER += ((float)cyc) / 8.0f;
    }

    timer_check_irq(timer, 2);
}

void psx_timer_update(psx_timer_t* timer, int cyc) {
    (void)cyc;

    if (timer->idle_updates) {
        timer->idle_updates--;
        timer->pending++;

        return;
    }

    timer_sync(timer);

    timer_update_timer0(timer, 2);
    timer_update_timer1(timer, 2);
    timer_update_timer2(timer, 2);

    timer->idle_updates = timer_idle_budget(timer);
}

void psxe_gpu_hblank_event_cb(psx_gpu_t* gpu) {
    psx_timer_t* timer = gpu->udata[1];

    timer_touch(timer);

    timer->hblank = 1;

    if ((T1_CLKSRC & 1) && !T1_PAUSED) {
        ++T1_COUNTER;

        timer_handle_irq(timer, 1);
    }

    if (!T0_SYNC_EN)
        return;

    switch (T0_SYNC_MODE) {
        case 0: T0_PAUSED = 1; break;
        case 1: T0_COUNTER = 0; break;
        case 2: T0_COUNTER = 0; T0_PAUSED = 0; break;
        case 3: {
            if (!T0_BLANK_ONCE) {
                T0_BLANK_ONCE = 1;
                T0_SYNC_EN = 0;
                T0_PAUSED = 0;
            }
        } break;
    }
}

void psxe_gpu_hblank_end_event_cb(psx_gpu_t* gpu) {
    psx_timer_t* timer = gpu->udata[1];

    timer_touch(timer);

    timer->hblank = 0;

    if (!T0_SYNC_EN)
        return;

    switch (T0_SYNC_MODE) {
        case 0: T0_PAUSED = 0; break;
        case 2: T0_PAUSED = 1; break;
    }
}

void psxe_gpu_vblank_timer_event_cb(psx_gpu_t* gpu) {
    psx_timer_t* timer = gpu->udata[1];

    timer_touch(timer);

    timer->vblank = 1;

    if (!T1_SYNC_EN)
        return;

    switch (T1_SYNC_MODE) {
        case 0: T1_PAUSED = 1; break;
        case 1: T1_COUNTER = 0; break;
        case 2: T1_COUNTER = 0; T1_PAUSED = 0; break;
        case 3: {
            if (!T1_BLANK_ONCE) {
                T1_BLANK_ONCE = 1;
                T1_SYNC_EN = 0;
                T1_PAUSED = 0;
            }
        } break;
    }
}

void psxe_gpu_vblank_end_event_cb(psx_gpu_t* gpu) {
    psx_timer_t* timer = gpu->udata[1];

    timer_touch(timer);

    timer->vblank = 0;

    if (!T1_SYNC_EN)
        return;

    switch (T1_SYNC_MODE) {
        case 0: T1_PAUSED = 0; break;
        case 2: T1_PAUSED = 1; break;
    }
}

void psx_timer_destroy(psx_timer_t* timer) {
    free(timer);
}