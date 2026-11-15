/* Idle-loop detection for the SILV engine.
 *
 * The engine paces itself by spinning on a timer-tick counter that its INT 8 handler increments
 * ("wait N ticks" loops, key-release waits ...).  On a real PC that is free; in an interpreter on a
 * handheld it burns almost all of the CPU.  We recognise the wait loop in the loaded program and, every time
 * it comes around, jump the emulated clock to the next timer tick instead of executing the spin.  The game
 * sees exactly the same number of ticks, but the host does a few hundred times less work. */
#include "machine.h"
#include <string.h>

/* push cx ; call X ; pop cx ; push dx ; call Y ; pop dx ; cmp ax,cx ; jc -14 ; ret      (-1 = wildcard) */
static const int16_t SIG_WAIT[] = { 0x51, 0xE8, -1, -1, 0x59, 0x52, 0xE8, -1, -1, 0x5A, 0x3B, 0xC1, 0x72, 0xF2, 0xC3 };

static int match(const uint8_t *p, const int16_t *sig, int n) {
    for (int i = 0; i < n; i++) if (sig[i] >= 0 && p[i] != (uint8_t)sig[i]) return 0;
    return 1;
}

static int wait_cb(CPU *c, int idx) {
    (void)idx;
    if (c->iff) machine_skip_to_tick(c->m);      /* timer interrupts must be enabled or the loop could never end */
    return 0;
}

static void idle_on_load(Machine *m, uint16_t load_seg) {
    CPU *c = &m->cpu;
    uint32_t base = LIN(load_seg, 0);
    int n = (int)(sizeof SIG_WAIT / sizeof SIG_WAIT[0]);
    for (uint32_t o = 0; o < 0xF000; o++) {
        if (m->mem[base + o] != 0x51 || !match(m->mem + base + o, SIG_WAIT, n)) continue;
        if (c->nbp < 8) { c->bp[c->nbp] = base + o; c->bp_fn[c->nbp] = wait_cb; c->nbp++; }
        break;
    }
}

void idle_init(Machine *m) { m->on_load2 = idle_on_load; }
