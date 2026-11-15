#include "machine.h"
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ memory */
static inline uint8_t ems_rb(Machine *m, uint32_t a, int *hit) {
    int p = (a - 0xE0000) >> 14;
    int lp = m->ems_map[p];
    if (lp >= 0 && m->ems) { *hit = 1; return m->ems[((uint32_t)lp << 14) | (a & 0x3FFF)]; }
    *hit = 0; return 0;
}

uint8_t mem_rb_slow(Machine *m, uint32_t a) {
    a &= 0xFFFFF;
    if (a >= 0xA0000) {
        if (a < 0xB0000) return vga_read(m, a - 0xA0000);
        if (a >= 0xE0000 && a < 0xF0000) { int h; uint8_t v = ems_rb(m, a, &h); if (h) return v; }
    }
    return m->mem[a];
}
void mem_wb_slow(Machine *m, uint32_t a, uint8_t v) {
    a &= 0xFFFFF;
    if (a >= 0xA0000) {
        if (a < 0xB0000) { vga_write(m, a - 0xA0000, v); return; }
        if (a >= 0xE0000 && a < 0xF0000) {
            int p = (a - 0xE0000) >> 14, lp = m->ems_map[p];
            if (lp >= 0 && m->ems) { m->ems[((uint32_t)lp << 14) | (a & 0x3FFF)] = v; return; }
        }
        if (a >= 0xC0000 && a < 0x100000 && !(a >= 0xB0000 && a < 0xC0000)) return; /* ROM */
    }
    m->mem[a] = v;
}
uint16_t mem_rw_slow(Machine *m, uint32_t a) {
    a &= 0xFFFFF;
    if (a < 0x9FFFF) return (uint16_t)(m->mem[a] | (m->mem[a + 1] << 8));
    return mem_rb(m, a) | (mem_rb(m, (a + 1) & 0xFFFFF) << 8);
}
void mem_ww_slow(Machine *m, uint32_t a, uint16_t v) {
    a &= 0xFFFFF;
    if (a < 0x9FFFF) { m->mem[a] = v & 0xFF; m->mem[a + 1] = v >> 8; return; }
    mem_wb(m, a, v & 0xFF); mem_wb(m, (a + 1) & 0xFFFFF, v >> 8);
}

/* --------------------------------------------------------------------- PIC */
static void pic_update(Machine *m) {
    m->irq_pending = 0;
    for (int i = 0; i < 8; i++) {
        if (m->pic_isr & (1 << i)) break;
        if (m->pic_irr & ~m->pic_imr & (1 << i)) { m->irq_pending = 1; break; }
    }
}
void pic_raise(Machine *m, int irq) { m->pic_irr |= 1 << irq; pic_update(m); }
int pic_ack(Machine *m) {
    for (int i = 0; i < 8; i++) {
        if (m->pic_isr & (1 << i)) break;
        if (m->pic_irr & ~m->pic_imr & (1 << i)) {
            m->pic_irr &= ~(1 << i); m->pic_isr |= 1 << i; pic_update(m); return 8 + i;
        }
    }
    pic_update(m);
    return -1;
}
static void pic_eoi(Machine *m, uint8_t v) {
    if (v == 0x20) { for (int i = 0; i < 8; i++) if (m->pic_isr & (1 << i)) { m->pic_isr &= ~(1 << i); break; } }
    else if ((v & 0xE0) == 0x60) m->pic_isr &= ~(1 << (v & 7));
    else if ((v & 0xE0) == 0x40) { /* nop */ }
    pic_update(m);
}

/* --------------------------------------------------------------------- PIT */
static uint32_t pit_period(Machine *m) {
    uint32_t r = m->pit_reload ? m->pit_reload : 65536;
    uint64_t p = (uint64_t)r * m->ips / 1193182u;
    return p ? (uint32_t)p : 1;
}

/* --------------------------------------------------------------------- I/O */
extern int io_trace;
uint8_t io_inb(Machine *m, uint16_t p) {
    if (io_trace && !(p >= 0x3B4 && p <= 0x3DF) && p != 0x20 && p != 0x21 && p != 0x40 && p != 0x60 && p != 0x61 && p != 0x64 && p != 0x3C0 && p != 0x3C4 && p != 0x3C5 && p != 0x3CE && p != 0x3CF) fprintf(stderr, "[io] IN  %03X  (%04X:%04X)\n", p, m->cpu.seg[CS], m->cpu.ip);
    switch (p) {
    case 0x20: return m->pic_irr; case 0x21: return m->pic_imr;
    case 0x40: {
        uint32_t r = m->pit_reload ? m->pit_reload : 65536;
        uint64_t per = pit_period(m);
        uint64_t el = (m->icount - m->pit_last_irq) % (per ? per : 1);
        uint32_t cnt = r - (uint32_t)(el * r / (per ? per : 1));
        uint8_t v = m->pit_hi_phase ? (cnt >> 8) : cnt & 0xFF;
        m->pit_hi_phase ^= 1; return v; }
    case 0x41: return 0x80; case 0x42: return 0x80;
    case 0x60: m->kbd_obf = 0; return m->kbd_data;
    case 0x61: { m->port61 ^= 0x10; return m->port61; }
    case 0x64: return (m->kbd_obf ? 1 : 0);
    case 0x388: case 0x228: case 0x38A: case 0x389: case 0x38B: case 0x229: case 0x220: case 0x221: case 0x222: case 0x223:
        return m->opl ? opl_read(m->opl, p) : 0xFF;
    }
    if ((p >= 0x3B4 && p <= 0x3DF)) return vga_io_in(m, p);
    return 0xFF;
}
int io_trace;
void io_outb(Machine *m, uint16_t p, uint8_t v) {
    if (io_trace && !(p >= 0x3B4 && p <= 0x3DF) && p != 0x20 && p != 0x21 && p != 0x40 && p != 0x43 && p != 0x61 && p != 0x3C0 && p != 0x3C4 && p != 0x3C5 && p != 0x3CE && p != 0x3CF) fprintf(stderr, "[io] OUT %03X <- %02X  (%04X:%04X)\n", p, v, m->cpu.seg[CS], m->cpu.ip);
    switch (p) {
    case 0x20: pic_eoi(m, v); return;
    case 0x21: m->pic_imr = v; pic_update(m); return;
    case 0x40:
        if (m->pit_ctl_lohi == 3) {
            if (!m->pit_hi_phase) { m->pit_wr_tmp = v; m->pit_hi_phase = 1; }
            else { m->pit_reload = m->pit_wr_tmp | (v << 8); m->pit_hi_phase = 0; m->pit_last_irq = m->icount; }
        } else if (m->pit_ctl_lohi == 1) { m->pit_reload = v; m->pit_last_irq = m->icount; }
        else if (m->pit_ctl_lohi == 2) { m->pit_reload = v << 8; m->pit_last_irq = m->icount; }
        return;
    case 0x43:
        if (((v >> 6) & 3) == 0) { if (((v >> 4) & 3) == 0) { /* latch */ } else { m->pit_ctl_lohi = (v >> 4) & 3; m->pit_hi_phase = 0; } }
        return;
    case 0x61: m->port61 = (m->port61 & 0x10) | (v & 0x0F); return;
    case 0x388: case 0x389: case 0x38A: case 0x38B: case 0x228: case 0x229: case 0x220: case 0x221: case 0x222: case 0x223:
        if (m->opl) opl_write(m->opl, p, v);
        return;
    }
    if ((p >= 0x3B4 && p <= 0x3DF)) vga_io_out(m, p, v);
}

/* --------------------------------------------------------------- keyboard */
void machine_key(Machine *m, uint8_t scancode, int down, uint8_t ascii) {
    (void)ascii;
    int nt = (m->kq_t + 1) & 63;
    if (nt == m->kq_h) return;
    m->kq[m->kq_t] = scancode | (down ? 0 : 0x80);
    m->kq_t = nt;
}
static void kbd_pump(Machine *m) {
    if (!m->kbd_obf && m->kq_h != m->kq_t && !(m->pic_isr & 2)) {
        m->kbd_data = (uint8_t)m->kq[m->kq_h]; m->kq_h = (m->kq_h + 1) & 63; m->kbd_obf = 1; pic_raise(m, 1);
    }
}

/* --------------------------------------------------------------- lifecycle */
Machine *machine_new(const char *gamedir) {
    Machine *m = calloc(1, sizeof(Machine));
    cpu_reset(&m->cpu, m);
    strncpy(m->gamedir, gamedir, sizeof(m->gamedir) - 1);
    m->ips = GUEST_IPS_DEFAULT;
    m->pic_imr = 0x00;
    m->port61 = 0;
    m->mouse_present = 1;
    m->mouse_maxx = 639; m->mouse_maxy = 479;
    for (int i = 0; i < 4; i++) m->ems_map[i] = -1;
    vga_init(&m->vga);
    dos_init(m);
    return m;
}
void machine_free(Machine *m) { if (!m) return; free(m->ems); if (m->opl) opl_free(m->opl); free(m); }

uint64_t machine_vtime_ns(Machine *m) { return m->vt_base_ns + (m->icount - m->ic_base) * 1000000000ull / m->ips; }
void machine_set_ips(Machine *m, uint32_t ips) {
    if (ips < 200000) ips = 200000;
    m->vt_base_ns = machine_vtime_ns(m); m->ic_base = m->icount; m->ips = ips;
}

int machine_run(Machine *m, int max_instr) {
    int done = 0;
    CPU *c = &m->cpu;
    uint32_t per = pit_period(m);
    while (done < max_instr && !m->exited) {
        int n = cpu_step(c);
        if (!n) { /* halted: skip ahead to next timer tick */
            uint64_t next = m->pit_last_irq + per;
            uint64_t step = next > m->icount ? next - m->icount : 1;
            if (step > (uint64_t)(max_instr - done)) step = max_instr - done;
            m->icount += step; done += (int)step; n = 0;
        } else { m->icount += n; done += n; }
        if (m->icount - m->pit_last_irq >= per) {
            m->pit_last_irq = m->icount; pic_raise(m, 0); per = pit_period(m);
        }
        if ((done & 63) == 0 || n == 0) kbd_pump(m);
    }
    return done;
}
