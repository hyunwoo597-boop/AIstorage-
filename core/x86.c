/* Real-mode 8086/186/386 interpreter: 16/32-bit operand size (0x66), 32-bit addressing (0x67),
 * FS/GS, and the commonly used 0x0F two-byte opcodes. No protected mode, no FPU. */
#include "x86.h"
#include "machine.h"
#include <string.h>
#include <stdio.h>

static uint8_t parity_tab[256];
static int tab_init;

static inline uint8_t rb(CPU *c, int s, uint16_t o) { return mem_rb(c->m, LIN(c->seg[s], o)); }
static inline uint16_t rw(CPU *c, int s, uint16_t o) {
    if (o == 0xFFFF) return rb(c, s, o) | (rb(c, s, 0) << 8);
    return mem_rw(c->m, LIN(c->seg[s], o));
}
static inline void wb(CPU *c, int s, uint16_t o, uint8_t v) { mem_wb(c->m, LIN(c->seg[s], o), v); }
static inline void ww(CPU *c, int s, uint16_t o, uint16_t v) {
    if (o == 0xFFFF) { wb(c, s, o, v & 0xFF); wb(c, s, 0, v >> 8); return; }
    mem_ww(c->m, LIN(c->seg[s], o), v);
}
static inline uint32_t rd32(CPU *c, int s, uint16_t o) { return rw(c, s, o) | ((uint32_t)rw(c, s, (uint16_t)(o + 2)) << 16); }
static inline void wd32(CPU *c, int s, uint16_t o, uint32_t v) { ww(c, s, o, v & 0xFFFF); ww(c, s, (uint16_t)(o + 2), v >> 16); }
static inline uint8_t fetch8(CPU *c) { uint8_t v = mem_rb(c->m, LIN(c->seg[CS], c->ip)); c->ip++; return v; }
static inline uint16_t fetch16(CPU *c) { uint16_t v = fetch8(c); v |= (uint16_t)fetch8(c) << 8; return v; }
static inline uint32_t fetch32(CPU *c) { uint32_t v = fetch16(c); v |= (uint32_t)fetch16(c) << 16; return v; }
static inline void push16(CPU *c, uint16_t v) { c->r[SP] -= 2; ww(c, SS, c->r[SP], v); }
static inline uint16_t pop16(CPU *c) { uint16_t v = rw(c, SS, c->r[SP]); c->r[SP] += 2; return v; }
static inline void push32(CPU *c, uint32_t v) { c->r[SP] -= 4; wd32(c, SS, c->r[SP], v); }
static inline uint32_t pop32(CPU *c) { uint32_t v = rd32(c, SS, c->r[SP]); c->r[SP] += 4; return v; }

/* operand-size aware helpers */
static inline uint32_t r32(CPU *c, int i) { return ((uint32_t)c->rh[i] << 16) | c->r[i]; }
static inline void w32(CPU *c, int i, uint32_t v) { c->r[i] = (uint16_t)v; c->rh[i] = (uint16_t)(v >> 16); }
static inline uint32_t getr(CPU *c, int i) { return c->osz32 ? r32(c, i) : c->r[i]; }
static inline void setr(CPU *c, int i, uint32_t v) { c->r[i] = (uint16_t)v; if (c->osz32) c->rh[i] = (uint16_t)(v >> 16); }
static inline uint32_t fetchv(CPU *c) { return c->osz32 ? fetch32(c) : fetch16(c); }
static inline uint32_t fetchv_se8(CPU *c) { int32_t x = (int8_t)fetch8(c); return c->osz32 ? (uint32_t)x : (uint16_t)x; }
static inline void pushv(CPU *c, uint32_t v) { if (c->osz32) push32(c, v); else push16(c, (uint16_t)v); }
static inline uint32_t popv(CPU *c) { return c->osz32 ? pop32(c) : pop16(c); }
static inline uint32_t rdv(CPU *c, int s, uint16_t o) { return c->osz32 ? rd32(c, s, o) : rw(c, s, o); }
static inline void wrv(CPU *c, int s, uint16_t o, uint32_t v) { if (c->osz32) wd32(c, s, o, v); else ww(c, s, o, (uint16_t)v); }
#define OB (c->osz32 ? 32 : 16)

uint16_t cpu_get_flags(CPU *c) {
    return 0x0002 | c->cf | (c->pf << 2) | (c->af << 4) | (c->zf << 6) | (c->sf << 7) | (c->tf << 8) |
           (c->iff << 9) | (c->df << 10) | (c->of << 11);
}
void cpu_set_flags(CPU *c, uint16_t f) {
    c->cf = f & 1; c->pf = (f >> 2) & 1; c->af = (f >> 4) & 1; c->zf = (f >> 6) & 1; c->sf = (f >> 7) & 1;
    c->tf = (f >> 8) & 1; c->iff = (f >> 9) & 1; c->df = (f >> 10) & 1; c->of = (f >> 11) & 1;
}

void cpu_reset(CPU *c, Machine *m) {
    if (!tab_init) {
        for (int i = 0; i < 256; i++) { int p = 0; for (int b = 0; b < 8; b++) p ^= (i >> b) & 1; parity_tab[i] = !p; }
        tab_init = 1;
    }
    memset(c, 0, sizeof(*c));
    c->m = m; c->segov = -1; c->nbp = 0;
}

void cpu_int(CPU *c, int n) {
    push16(c, cpu_get_flags(c));
    push16(c, c->seg[CS]);
    push16(c, c->ip);
    c->iff = 0; c->tf = 0;
    uint32_t a = n * 4;
    c->ip = mem_rw(c->m, a);
    c->seg[CS] = mem_rw(c->m, a + 2);
}
void cpu_iret(CPU *c) {
    c->ip = pop16(c);
    c->seg[CS] = pop16(c);
    cpu_set_flags(c, pop16(c));
}

static inline void szp(CPU *c, uint32_t r, int bits) {
    c->zf = r == 0;
    c->sf = (r >> (bits - 1)) & 1;
    c->pf = parity_tab[r & 0xFF];
}

static uint32_t alu(CPU *c, int op, uint32_t a, uint32_t b, int bits) {
    uint32_t mask = bits == 8 ? 0xFF : bits == 16 ? 0xFFFF : 0xFFFFFFFFu;
    uint32_t sign = (mask >> 1) + 1;
    uint64_t res = 0;
    a &= mask; b &= mask;
    switch (op) {
    case 0: res = (uint64_t)a + b; c->cf = res > mask; c->af = ((a ^ b ^ (uint32_t)res) >> 4) & 1; c->of = ((a ^ (uint32_t)res) & (b ^ (uint32_t)res) & sign) != 0; break;
    case 2: { uint32_t ci = c->cf; res = (uint64_t)a + b + ci; c->cf = res > mask; c->af = ((a ^ b ^ (uint32_t)res) >> 4) & 1; c->of = ((a ^ (uint32_t)res) & (b ^ (uint32_t)res) & sign) != 0; break; }
    case 5: case 7: res = (uint64_t)a - b; c->cf = a < b; c->af = ((a ^ b ^ (uint32_t)res) >> 4) & 1; c->of = ((a ^ b) & (a ^ (uint32_t)res) & sign) != 0; break;
    case 3: { uint32_t ci = c->cf; res = (uint64_t)a - b - ci; c->cf = (uint64_t)a < (uint64_t)b + ci; c->af = ((a ^ b ^ (uint32_t)res) >> 4) & 1; c->of = ((a ^ b) & (a ^ (uint32_t)res) & sign) != 0; break; }
    case 1: res = a | b; c->cf = c->of = c->af = 0; break;
    case 4: res = a & b; c->cf = c->of = c->af = 0; break;
    case 6: res = a ^ b; c->cf = c->of = c->af = 0; break;
    }
    uint32_t r = (uint32_t)res & mask;
    szp(c, r, bits);
    return r;
}

static uint32_t shiftop(CPU *c, int op, uint32_t v, unsigned cnt, int bits) {
    uint32_t mask = bits == 8 ? 0xFF : bits == 16 ? 0xFFFF : 0xFFFFFFFFu;
    uint32_t sign = (mask >> 1) + 1;
    cnt &= 0x1F;
    if (!cnt) return v;
    v &= mask;
    uint32_t orig = v;
    switch (op) {
    case 0: for (unsigned i = 0; i < cnt; i++) { c->cf = (v & sign) != 0; v = ((v << 1) | c->cf) & mask; } c->of = ((v & sign) != 0) ^ c->cf; break;
    case 1: for (unsigned i = 0; i < cnt; i++) { c->cf = v & 1; v = (v >> 1) | (c->cf ? sign : 0); } c->of = ((v >> (bits - 1)) ^ (v >> (bits - 2))) & 1; break;
    case 2: for (unsigned i = 0; i < cnt; i++) { uint8_t n = (v & sign) != 0; v = ((v << 1) | c->cf) & mask; c->cf = n; } c->of = ((v & sign) != 0) ^ c->cf; break;
    case 3: for (unsigned i = 0; i < cnt; i++) { uint8_t n = v & 1; v = (v >> 1) | (c->cf ? sign : 0); c->cf = n; } c->of = ((v >> (bits - 1)) ^ (v >> (bits - 2))) & 1; break;
    case 4: case 6:
        c->cf = cnt <= (unsigned)bits ? (orig >> (bits - cnt)) & 1 : 0;
        v = cnt >= (unsigned)bits ? 0 : (v << cnt) & mask;
        c->of = ((v & sign) != 0) ^ c->cf; szp(c, v, bits); break;
    case 5:
        c->cf = cnt <= (unsigned)bits ? (orig >> (cnt - 1)) & 1 : 0;
        v = cnt >= (unsigned)bits ? 0 : v >> cnt;
        c->of = (orig & sign) != 0; szp(c, v, bits); break;
    case 7: {
        int32_t sv = bits == 8 ? (int8_t)v : bits == 16 ? (int16_t)v : (int32_t)v;
        c->cf = cnt >= (unsigned)bits ? (sv < 0) : ((sv >> (cnt - 1)) & 1);
        sv = cnt >= (unsigned)bits ? (sv < 0 ? -1 : 0) : sv >> cnt;
        v = (uint32_t)sv & mask; c->of = 0; szp(c, v, bits); break; }
    }
    return v & mask;
}

/* SHLD / SHRD */
static uint32_t dshift(CPU *c, int left, uint32_t dst, uint32_t src, unsigned n, int bits) {
    uint32_t mask = bits == 32 ? 0xFFFFFFFFu : 0xFFFFu;
    n &= 0x1F;
    if (bits == 16 && n > 16) n &= 15;
    if (!n) return dst;
    dst &= mask; src &= mask;
    uint32_t res;
    if (left) {
        c->cf = (n <= (unsigned)bits) ? (dst >> (bits - n)) & 1 : 0;
        res = (n == (unsigned)bits) ? src : ((dst << n) | (src >> (bits - n)));
    } else {
        c->cf = (dst >> (n - 1)) & 1;
        res = (n == (unsigned)bits) ? src : ((dst >> n) | (src << (bits - n)));
    }
    res &= mask;
    c->of = ((res ^ dst) >> (bits - 1)) & 1;
    szp(c, res, bits);
    return res;
}

static inline __attribute__((always_inline)) void modrm(CPU *c) {
    uint8_t b = fetch8(c);
    c->mod = b >> 6; c->reg = (b >> 3) & 7; c->rm = b & 7;
    if (c->mod == 3) return;
    int seg = DS;
    if (!c->asz32) {
        uint16_t off = 0;
        switch (c->rm) {
        case 0: off = c->r[BX] + c->r[SI]; break;
        case 1: off = c->r[BX] + c->r[DI]; break;
        case 2: off = c->r[BP] + c->r[SI]; seg = SS; break;
        case 3: off = c->r[BP] + c->r[DI]; seg = SS; break;
        case 4: off = c->r[SI]; break;
        case 5: off = c->r[DI]; break;
        case 6: if (c->mod == 0) { off = fetch16(c); } else { off = c->r[BP]; seg = SS; } break;
        case 7: off = c->r[BX]; break;
        }
        if (c->mod == 1) off += (int8_t)fetch8(c);
        else if (c->mod == 2) off += fetch16(c);
        c->ea_off = off; c->ea32 = off;
    } else {
        uint32_t off = 0;
        if (c->rm == 4) {
            uint8_t sib = fetch8(c);
            int sc = sib >> 6, idx = (sib >> 3) & 7, base = sib & 7;
            if (idx != 4) off += r32(c, idx) << sc;
            if (base == 5 && c->mod == 0) off += fetch32(c);
            else { off += r32(c, base); if (base == SP || base == BP) seg = SS; }
        } else if (c->rm == 5 && c->mod == 0) {
            off = fetch32(c);
        } else {
            off = r32(c, c->rm); if (c->rm == BP) seg = SS;
        }
        if (c->mod == 1) off += (int32_t)(int8_t)fetch8(c);
        else if (c->mod == 2) off += fetch32(c);
        c->ea32 = off; c->ea_off = (uint16_t)off;
    }
    c->ea_seg = c->segov >= 0 ? c->segov : seg;
}
static inline uint8_t get_eb(CPU *c) { return c->mod == 3 ? R8(c, c->rm) : rb(c, c->ea_seg, c->ea_off); }
static inline uint32_t get_ev(CPU *c) { return c->mod == 3 ? getr(c, c->rm) : rdv(c, c->ea_seg, c->ea_off); }
static inline uint16_t get_ew(CPU *c) { return c->mod == 3 ? c->r[c->rm] : rw(c, c->ea_seg, c->ea_off); }
static inline void set_eb(CPU *c, uint8_t v) { if (c->mod == 3) SETR8(c, c->rm, v); else wb(c, c->ea_seg, c->ea_off, v); }
static inline void set_ev(CPU *c, uint32_t v) { if (c->mod == 3) setr(c, c->rm, v); else wrv(c, c->ea_seg, c->ea_off, v); }
static inline void set_ew(CPU *c, uint16_t v) { if (c->mod == 3) c->r[c->rm] = v; else ww(c, c->ea_seg, c->ea_off, v); }

static int cond(CPU *c, int cc) {
    int r;
    switch (cc >> 1) {
    case 0: r = c->of; break;
    case 1: r = c->cf; break;
    case 2: r = c->zf; break;
    case 3: r = c->cf | c->zf; break;
    case 4: r = c->sf; break;
    case 5: r = c->pf; break;
    case 6: r = c->sf != c->of; break;
    default: r = c->zf || (c->sf != c->of); break;
    }
    return (cc & 1) ? !r : r;
}

static void div_err(CPU *c) { cpu_int(c, 0); }

static void group3(CPU *c, int bits) {
    int op = c->reg;
    if (bits == 8) {
        uint8_t v = get_eb(c);
        switch (op) {
        case 0: case 1: { uint8_t i = fetch8(c); alu(c, 4, v, i, 8); break; }
        case 2: set_eb(c, ~v); break;
        case 3: { uint8_t r = alu(c, 5, 0, v, 8); c->cf = v != 0; set_eb(c, r); break; }
        case 4: { uint16_t r = (uint16_t)R8(c, 0) * v; c->r[AX] = r; c->cf = c->of = (r >> 8) != 0; break; }
        case 5: { int16_t r = (int16_t)(int8_t)R8(c, 0) * (int8_t)v; c->r[AX] = r; c->cf = c->of = r != (int16_t)(int8_t)r; break; }
        case 6: { if (!v) { div_err(c); return; } uint16_t a = c->r[AX]; uint16_t q = a / v; if (q > 0xFF) { div_err(c); return; } SETR8(c, 0, q); SETR8(c, 4, a % v); break; }
        case 7: { int8_t d = v; if (!d) { div_err(c); return; } int16_t a = (int16_t)c->r[AX]; int32_t q = a / d; if (q > 127 || q < -128) { div_err(c); return; } SETR8(c, 0, (uint8_t)q); SETR8(c, 4, (uint8_t)(a % d)); break; }
        }
    } else if (!c->osz32) {
        uint16_t v = get_ew(c);
        switch (op) {
        case 0: case 1: { uint16_t i = fetch16(c); alu(c, 4, v, i, 16); break; }
        case 2: set_ew(c, ~v); break;
        case 3: { uint16_t r = alu(c, 5, 0, v, 16); c->cf = v != 0; set_ew(c, r); break; }
        case 4: { uint32_t r = (uint32_t)c->r[AX] * v; c->r[AX] = r; c->r[DX] = r >> 16; c->cf = c->of = (r >> 16) != 0; break; }
        case 5: { int32_t r = (int32_t)(int16_t)c->r[AX] * (int16_t)v; c->r[AX] = r; c->r[DX] = r >> 16; c->cf = c->of = r != (int32_t)(int16_t)r; break; }
        case 6: { if (!v) { div_err(c); return; } uint32_t a = ((uint32_t)c->r[DX] << 16) | c->r[AX]; uint32_t q = a / v; if (q > 0xFFFF) { div_err(c); return; } c->r[AX] = q; c->r[DX] = a % v; break; }
        case 7: { int16_t d = v; if (!d) { div_err(c); return; } int32_t a = (int32_t)(((uint32_t)c->r[DX] << 16) | c->r[AX]); int64_t q = (int64_t)a / d; if (q > 32767 || q < -32768) { div_err(c); return; } c->r[AX] = (uint16_t)q; c->r[DX] = (uint16_t)(a % d); break; }
        }
    } else {
        uint32_t v = get_ev(c);
        switch (op) {
        case 0: case 1: { uint32_t i = fetch32(c); alu(c, 4, v, i, 32); break; }
        case 2: set_ev(c, ~v); break;
        case 3: { uint32_t r = alu(c, 5, 0, v, 32); c->cf = v != 0; set_ev(c, r); break; }
        case 4: { uint64_t r = (uint64_t)r32(c, AX) * v; w32(c, AX, (uint32_t)r); w32(c, DX, (uint32_t)(r >> 32)); c->cf = c->of = (r >> 32) != 0; break; }
        case 5: { int64_t r = (int64_t)(int32_t)r32(c, AX) * (int32_t)v; w32(c, AX, (uint32_t)r); w32(c, DX, (uint32_t)((uint64_t)r >> 32)); c->cf = c->of = r != (int64_t)(int32_t)r; break; }
        case 6: { if (!v) { div_err(c); return; } uint64_t a = ((uint64_t)r32(c, DX) << 32) | r32(c, AX); uint64_t q = a / v; if (q > 0xFFFFFFFFull) { div_err(c); return; } w32(c, AX, (uint32_t)q); w32(c, DX, (uint32_t)(a % v)); break; }
        case 7: { int32_t d = (int32_t)v; if (!d) { div_err(c); return; } int64_t a = (int64_t)(((uint64_t)r32(c, DX) << 32) | r32(c, AX)); int64_t q = a / d; if (q > 2147483647LL || q < -2147483648LL) { div_err(c); return; } w32(c, AX, (uint32_t)q); w32(c, DX, (uint32_t)(a % d)); break; }
        }
    }
}

static inline uint32_t rdsz(CPU *c, int s, uint16_t o, int sz) { return sz == 1 ? rb(c, s, o) : sz == 2 ? rw(c, s, o) : rd32(c, s, o); }
static inline void wrsz(CPU *c, int s, uint16_t o, uint32_t v, int sz) { if (sz == 1) wb(c, s, o, (uint8_t)v); else if (sz == 2) ww(c, s, o, (uint16_t)v); else wd32(c, s, o, v); }

static void string_op(CPU *c, int op, int rep) {
    /* op: A4 movsb A5 movsw A6 cmpsb A7 cmpsw AA stosb AB stosw AC lodsb AD lodsw AE scasb AF scasw 6C 6D ins 6E 6F outs */
    int w = op & 1; int sz = w ? (c->osz32 ? 4 : 2) : 1; int step = c->df ? -sz : sz;
    int srcseg = c->segov >= 0 ? c->segov : DS;
    int cmp = (op == 0xA6 || op == 0xA7 || op == 0xAE || op == 0xAF);
    for (;;) {
        if (rep && c->r[CX] == 0) break;
        switch (op) {
        case 0xA4: case 0xA5:
            wrsz(c, ES, c->r[DI], rdsz(c, srcseg, c->r[SI], sz), sz);
            c->r[SI] += step; c->r[DI] += step; break;
        case 0xA6: case 0xA7: {
            uint32_t a = rdsz(c, srcseg, c->r[SI], sz);
            uint32_t b = rdsz(c, ES, c->r[DI], sz);
            alu(c, 7, a, b, sz * 8); c->r[SI] += step; c->r[DI] += step; break; }
        case 0xAA: case 0xAB:
            wrsz(c, ES, c->r[DI], sz == 1 ? R8(c, 0) : sz == 2 ? c->r[AX] : r32(c, AX), sz);
            c->r[DI] += step; break;
        case 0xAC: case 0xAD:
            if (sz == 1) SETR8(c, 0, rb(c, srcseg, c->r[SI]));
            else if (sz == 2) c->r[AX] = rw(c, srcseg, c->r[SI]);
            else w32(c, AX, rd32(c, srcseg, c->r[SI]));
            c->r[SI] += step; break;
        case 0xAE: case 0xAF: {
            uint32_t b = rdsz(c, ES, c->r[DI], sz);
            alu(c, 7, sz == 1 ? R8(c, 0) : sz == 2 ? c->r[AX] : r32(c, AX), b, sz * 8); c->r[DI] += step; break; }
        case 0x6C: case 0x6D: {
            uint8_t lo = io_inb(c->m, c->r[DX]);
            if (w) { uint16_t v = lo | (io_inb(c->m, c->r[DX] + 1) << 8); ww(c, ES, c->r[DI], v); sz = 2; } else wb(c, ES, c->r[DI], lo);
            c->r[DI] += c->df ? -sz : sz; break; }
        case 0x6E: case 0x6F: {
            if (w) { uint16_t v = rw(c, srcseg, c->r[SI]); io_outb(c->m, c->r[DX], v & 0xFF); io_outb(c->m, c->r[DX] + 1, v >> 8); sz = 2; }
            else io_outb(c->m, c->r[DX], rb(c, srcseg, c->r[SI]));
            c->r[SI] += c->df ? -sz : sz; break; }
        }
        if (!rep) break;
        c->r[CX]--;
        if (cmp) {
            if (rep == 1 && c->zf) break;  /* REPNE: stop when equal */
            if (rep == 2 && !c->zf) break; /* REPE: stop when not equal */
        }
    }
}

/* ---- 0x0F two-byte opcodes ---- */
static void op0f(CPU *c, uint16_t start_ip) {
    Machine *m = c->m;
    uint8_t op = fetch8(c);
    switch (op) {
    case 0x80: case 0x81: case 0x82: case 0x83: case 0x84: case 0x85: case 0x86: case 0x87:
    case 0x88: case 0x89: case 0x8A: case 0x8B: case 0x8C: case 0x8D: case 0x8E: case 0x8F: {
        int32_t d = c->osz32 ? (int32_t)fetch32(c) : (int32_t)(int16_t)fetch16(c);
        if (cond(c, op & 0xF)) c->ip += d;
        break; }
    case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:
    case 0x98: case 0x99: case 0x9A: case 0x9B: case 0x9C: case 0x9D: case 0x9E: case 0x9F:
        modrm(c); set_eb(c, cond(c, op & 0xF)); break;
    case 0xA0: pushv(c, c->seg[FS]); break;
    case 0xA1: c->seg[FS] = (uint16_t)popv(c); break;
    case 0xA8: pushv(c, c->seg[GS]); break;
    case 0xA9: c->seg[GS] = (uint16_t)popv(c); break;
    case 0xA3: case 0xAB: case 0xB3: case 0xBB: case 0xBA: {
        modrm(c);
        int bits = OB; unsigned bit; int kind;
        if (op == 0xBA) { bit = fetch8(c) & (bits - 1); kind = c->reg - 4; if (kind < 0) { m->stat_unhandled++; break; } }
        else {
            kind = op == 0xA3 ? 0 : op == 0xAB ? 1 : op == 0xB3 ? 2 : 3;
            int32_t bo = c->osz32 ? (int32_t)r32(c, c->reg) : (int32_t)(int16_t)c->r[c->reg];
            if (c->mod != 3) {
                int shift = c->osz32 ? 5 : 4;
                c->ea_off += (int32_t)(bo >> shift) * (bits / 8);
            }
            bit = (unsigned)bo & (bits - 1);
        }
        uint32_t v = get_ev(c);
        c->cf = (v >> bit) & 1;
        if (kind == 1) v |= 1u << bit; else if (kind == 2) v &= ~(1u << bit); else if (kind == 3) v ^= 1u << bit;
        if (kind) set_ev(c, v);
        break; }
    case 0xA4: case 0xA5: case 0xAC: case 0xAD: {
        modrm(c);
        unsigned n = (op & 1) ? R8(c, 1) : fetch8(c);
        set_ev(c, dshift(c, op < 0xA8, get_ev(c), getr(c, c->reg), n, OB));
        break; }
    case 0xAF: { modrm(c);
        if (c->osz32) { int64_t r = (int64_t)(int32_t)r32(c, c->reg) * (int32_t)get_ev(c); w32(c, c->reg, (uint32_t)r); c->cf = c->of = r != (int64_t)(int32_t)r; }
        else { int32_t r = (int32_t)(int16_t)c->r[c->reg] * (int16_t)get_ew(c); c->r[c->reg] = (uint16_t)r; c->cf = c->of = r != (int32_t)(int16_t)r; }
        break; }
    case 0xB6: modrm(c); setr(c, c->reg, get_eb(c)); break;
    case 0xB7: modrm(c); setr(c, c->reg, get_ew(c)); break;
    case 0xBE: modrm(c); setr(c, c->reg, (uint32_t)(int32_t)(int8_t)get_eb(c)); break;
    case 0xBF: modrm(c); setr(c, c->reg, (uint32_t)(int32_t)(int16_t)get_ew(c)); break;
    case 0xB2: case 0xB4: case 0xB5: {
        modrm(c);
        uint32_t o = rdv(c, c->ea_seg, c->ea_off); uint16_t s = rw(c, c->ea_seg, c->ea_off + (c->osz32 ? 4 : 2));
        setr(c, c->reg, o);
        c->seg[op == 0xB2 ? SS : op == 0xB4 ? FS : GS] = s;
        if (op == 0xB2) c->inhibit = 1;
        break; }
    case 0xBC: case 0xBD: {
        modrm(c);
        uint32_t v = get_ev(c); int bits = OB;
        if (!v) { c->zf = 1; break; }
        c->zf = 0;
        int i;
        if (op == 0xBC) { for (i = 0; !((v >> i) & 1); i++) ; }
        else { for (i = bits - 1; !((v >> i) & 1); i--) ; }
        setr(c, c->reg, i);
        break; }
    case 0xC8: case 0xC9: case 0xCA: case 0xCB: case 0xCC: case 0xCD: case 0xCE: case 0xCF: {
        int r = op & 7; uint32_t v = r32(c, r);
        v = (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24);
        w32(c, r, v); break; }
    case 0x20: case 0x21: case 0x22: case 0x23: modrm(c); break; /* MOV CRx/DRx: ignored */
    case 0xA2: /* CPUID: pretend to be a bare 386 */
        w32(c, AX, 0); w32(c, BX, 0); w32(c, CX, 0); w32(c, DX, 0); break;
    case 0xB0: modrm(c); { uint8_t d = get_eb(c); alu(c, 7, R8(c, 0), d, 8); if (c->zf) set_eb(c, R8(c, c->reg)); else SETR8(c, 0, d); break; }
    case 0xB1: modrm(c); { uint32_t d = get_ev(c); alu(c, 7, getr(c, AX), d, OB); if (c->zf) set_ev(c, getr(c, c->reg)); else setr(c, AX, d); break; }
    case 0xC0: modrm(c); { uint8_t d = get_eb(c), s = R8(c, c->reg); uint8_t r = alu(c, 0, d, s, 8); SETR8(c, c->reg, d); set_eb(c, r); break; }
    case 0xC1: modrm(c); { uint32_t d = get_ev(c), s = getr(c, c->reg); uint32_t r = alu(c, 0, d, s, OB); setr(c, c->reg, d); set_ev(c, r); break; }
    default:
        m->stat_unhandled++;
        if (m->verbose || m->stat_unhandled < 20)
            fprintf(stderr, "[cpu] unimplemented opcode 0F %02X at %04X:%04X\n", op, c->seg[CS], start_ip);
        break;
    }
}

int cpu_step(CPU *c) {
    Machine *m = c->m;
    if (m->irq_pending && c->iff && !c->inhibit) {
        int v = pic_ack(m);
        if (v >= 0) { c->halted = 0; cpu_int(c, v); }
    }
    c->inhibit = 0;
    if (c->halted) return 0;
    if (c->nbp) {
        uint32_t la = LIN(c->seg[CS], c->ip);
        for (int i = 0; i < c->nbp; i++) if (c->bp[i] == la && c->bp_cb) c->bp_cb(c, i);
    }
    c->segov = -1; c->osz32 = c->asz32 = 0;
    int rep = 0; /* 1 = REPNE, 2 = REPE/REP */
    uint16_t start_ip = c->ip;
    uint8_t op;
    for (;;) {
        op = fetch8(c);
        switch (op) {
        case 0x26: c->segov = ES; continue;
        case 0x2E: c->segov = CS; continue;
        case 0x36: c->segov = SS; continue;
        case 0x3E: c->segov = DS; continue;
        case 0x64: c->segov = FS; continue;
        case 0x65: c->segov = GS; continue;
        case 0x66: c->osz32 = 1; continue;
        case 0x67: c->asz32 = 1; continue;
        case 0xF0: continue;
        case 0xF2: rep = 1; continue;
        case 0xF3: rep = 2; continue;
        }
        break;
    }
    int cnt = 1;
    switch (op) {
    /* ALU r/m,reg etc */
    case 0x00: case 0x08: case 0x10: case 0x18: case 0x20: case 0x28: case 0x30: case 0x38:
        modrm(c); { uint8_t r = alu(c, op >> 3, get_eb(c), R8(c, c->reg), 8); if ((op >> 3) != 7) set_eb(c, r); } break;
    case 0x01: case 0x09: case 0x11: case 0x19: case 0x21: case 0x29: case 0x31: case 0x39:
        modrm(c); { uint32_t r = alu(c, op >> 3, get_ev(c), getr(c, c->reg), OB); if ((op >> 3) != 7) set_ev(c, r); } break;
    case 0x02: case 0x0A: case 0x12: case 0x1A: case 0x22: case 0x2A: case 0x32: case 0x3A:
        modrm(c); { uint8_t r = alu(c, op >> 3, R8(c, c->reg), get_eb(c), 8); if ((op >> 3) != 7) SETR8(c, c->reg, r); } break;
    case 0x03: case 0x0B: case 0x13: case 0x1B: case 0x23: case 0x2B: case 0x33: case 0x3B:
        modrm(c); { uint32_t r = alu(c, op >> 3, getr(c, c->reg), get_ev(c), OB); if ((op >> 3) != 7) setr(c, c->reg, r); } break;
    case 0x04: case 0x0C: case 0x14: case 0x1C: case 0x24: case 0x2C: case 0x34: case 0x3C: {
        uint8_t i = fetch8(c); uint8_t r = alu(c, op >> 3, R8(c, 0), i, 8); if ((op >> 3) != 7) SETR8(c, 0, r); break; }
    case 0x05: case 0x0D: case 0x15: case 0x1D: case 0x25: case 0x2D: case 0x35: case 0x3D: {
        uint32_t i = fetchv(c); uint32_t r = alu(c, op >> 3, getr(c, AX), i, OB); if ((op >> 3) != 7) setr(c, AX, r); break; }
    case 0x06: pushv(c, c->seg[ES]); break;
    case 0x07: c->seg[ES] = (uint16_t)popv(c); break;
    case 0x0E: pushv(c, c->seg[CS]); break;
    case 0x0F: op0f(c, start_ip); break;
    case 0x16: pushv(c, c->seg[SS]); break;
    case 0x17: c->seg[SS] = (uint16_t)popv(c); c->inhibit = 1; break;
    case 0x1E: pushv(c, c->seg[DS]); break;
    case 0x1F: c->seg[DS] = (uint16_t)popv(c); break;
    case 0x27: { /* DAA */
        uint8_t al = R8(c, 0), old = al; int oc = c->cf;
        if ((al & 0xF) > 9 || c->af) { al += 6; c->af = 1; } else c->af = 0;
        if (old > 0x99 || oc) { al += 0x60; c->cf = 1; } else c->cf = 0;
        SETR8(c, 0, al); szp(c, al, 8); break; }
    case 0x2F: { /* DAS */
        uint8_t al = R8(c, 0), old = al; int oc = c->cf;
        if ((al & 0xF) > 9 || c->af) { al -= 6; c->af = 1; } else c->af = 0;
        if (old > 0x99 || oc) { al -= 0x60; c->cf = 1; } else c->cf = 0;
        SETR8(c, 0, al); szp(c, al, 8); break; }
    case 0x37: { /* AAA */
        if ((R8(c, 0) & 0xF) > 9 || c->af) { c->r[AX] += 0x106; c->af = c->cf = 1; } else c->af = c->cf = 0;
        SETR8(c, 0, R8(c, 0) & 0xF); break; }
    case 0x3F: { /* AAS */
        if ((R8(c, 0) & 0xF) > 9 || c->af) { c->r[AX] -= 6; SETR8(c, 4, R8(c, 4) - 1); c->af = c->cf = 1; } else c->af = c->cf = 0;
        SETR8(c, 0, R8(c, 0) & 0xF); break; }
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47: {
        int r = op & 7; uint8_t cf = c->cf; setr(c, r, alu(c, 0, getr(c, r), 1, OB)); c->cf = cf; break; }
    case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F: {
        int r = op & 7; uint8_t cf = c->cf; setr(c, r, alu(c, 5, getr(c, r), 1, OB)); c->cf = cf; break; }
    case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: pushv(c, getr(c, op & 7)); break;
    case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F: setr(c, op & 7, popv(c)); break;
    case 0x60: { uint32_t sp = getr(c, SP);
        pushv(c, getr(c, AX)); pushv(c, getr(c, CX)); pushv(c, getr(c, DX)); pushv(c, getr(c, BX));
        pushv(c, sp); pushv(c, getr(c, BP)); pushv(c, getr(c, SI)); pushv(c, getr(c, DI)); break; }
    case 0x61: setr(c, DI, popv(c)); setr(c, SI, popv(c)); setr(c, BP, popv(c)); popv(c);
               setr(c, BX, popv(c)); setr(c, DX, popv(c)); setr(c, CX, popv(c)); setr(c, AX, popv(c)); break;
    case 0x62: modrm(c); { int16_t lo = rw(c, c->ea_seg, c->ea_off), hi = rw(c, c->ea_seg, c->ea_off + 2); int16_t v = c->r[c->reg]; if (v < lo || v > hi) cpu_int(c, 5); break; }
    case 0x68: pushv(c, fetchv(c)); break;
    case 0x69: modrm(c); { int32_t a = (int32_t)get_ev(c); if (!c->osz32) a = (int16_t)a;
        int32_t b = (int32_t)fetchv(c); if (!c->osz32) b = (int16_t)b;
        int64_t r = (int64_t)a * b; setr(c, c->reg, (uint32_t)r);
        c->cf = c->of = c->osz32 ? r != (int64_t)(int32_t)r : r != (int64_t)(int16_t)r; break; }
    case 0x6A: pushv(c, fetchv_se8(c)); break;
    case 0x6B: modrm(c); { int32_t a = (int32_t)get_ev(c); if (!c->osz32) a = (int16_t)a;
        int32_t b = (int8_t)fetch8(c);
        int64_t r = (int64_t)a * b; setr(c, c->reg, (uint32_t)r);
        c->cf = c->of = c->osz32 ? r != (int64_t)(int32_t)r : r != (int64_t)(int16_t)r; break; }
    case 0x6C: case 0x6D: case 0x6E: case 0x6F: { uint16_t n = c->r[CX]; string_op(c, op, rep ? 2 : 0); cnt += rep ? n : 0; break; }
    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
        int8_t d = fetch8(c); if (cond(c, op & 0xF)) c->ip += d; break; }
    case 0x80: case 0x81: case 0x82: case 0x83: {
        modrm(c);
        if (op == 0x80 || op == 0x82) { uint8_t i = fetch8(c); uint8_t r = alu(c, c->reg, get_eb(c), i, 8); if (c->reg != 7) set_eb(c, r); }
        else { uint32_t i = op == 0x81 ? fetchv(c) : fetchv_se8(c); uint32_t r = alu(c, c->reg, get_ev(c), i, OB); if (c->reg != 7) set_ev(c, r); }
        break; }
    case 0x84: modrm(c); alu(c, 4, get_eb(c), R8(c, c->reg), 8); break;
    case 0x85: modrm(c); alu(c, 4, get_ev(c), getr(c, c->reg), OB); break;
    case 0x86: modrm(c); { uint8_t a = get_eb(c), b = R8(c, c->reg); set_eb(c, b); SETR8(c, c->reg, a); break; }
    case 0x87: modrm(c); { uint32_t a = get_ev(c), b = getr(c, c->reg); set_ev(c, b); setr(c, c->reg, a); break; }
    case 0x88: modrm(c); set_eb(c, R8(c, c->reg)); break;
    case 0x89: modrm(c); set_ev(c, getr(c, c->reg)); break;
    case 0x8A: modrm(c); SETR8(c, c->reg, get_eb(c)); break;
    case 0x8B: modrm(c); setr(c, c->reg, get_ev(c)); break;
    case 0x8C: modrm(c); { uint16_t sv = c->seg[c->reg < 6 ? c->reg : 0]; if (c->mod == 3 && c->osz32) setr(c, c->rm, sv); else set_ew(c, sv); break; }
    case 0x8D: modrm(c); setr(c, c->reg, c->asz32 ? c->ea32 : c->ea_off); break;
    case 0x8E: modrm(c); { uint16_t v = get_ew(c); int sr = c->reg < 6 ? c->reg : 0; c->seg[sr] = v; if (sr == SS) c->inhibit = 1; break; }
    case 0x8F: modrm(c); { uint32_t v = popv(c); set_ev(c, v); break; }
    case 0x90: break;
    case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97: { uint32_t t = getr(c, AX); setr(c, AX, getr(c, op & 7)); setr(c, op & 7, t); break; }
    case 0x98: if (c->osz32) w32(c, AX, (uint32_t)(int32_t)(int16_t)c->r[AX]); else c->r[AX] = (int16_t)(int8_t)R8(c, 0); break;
    case 0x99: if (c->osz32) w32(c, DX, (r32(c, AX) & 0x80000000u) ? 0xFFFFFFFFu : 0); else c->r[DX] = (c->r[AX] & 0x8000) ? 0xFFFF : 0; break;
    case 0x9A: { uint32_t o = fetchv(c); uint16_t s = fetch16(c); pushv(c, c->seg[CS]); pushv(c, c->ip); c->ip = (uint16_t)o; c->seg[CS] = s; break; }
    case 0x9B: break;
    case 0x9C: pushv(c, cpu_get_flags(c)); break;
    case 0x9D: { uint32_t f = popv(c); cpu_set_flags(c, (uint16_t)f); break; }
    case 0x9E: { uint8_t a = R8(c, 4); c->cf = a & 1; c->pf = (a >> 2) & 1; c->af = (a >> 4) & 1; c->zf = (a >> 6) & 1; c->sf = (a >> 7) & 1; break; }
    case 0x9F: SETR8(c, 4, (uint8_t)(cpu_get_flags(c) & 0xFF)); break;
    case 0xA0: { uint32_t o = c->asz32 ? fetch32(c) : fetch16(c); SETR8(c, 0, rb(c, c->segov >= 0 ? c->segov : DS, (uint16_t)o)); break; }
    case 0xA1: { uint32_t o = c->asz32 ? fetch32(c) : fetch16(c); setr(c, AX, rdv(c, c->segov >= 0 ? c->segov : DS, (uint16_t)o)); break; }
    case 0xA2: { uint32_t o = c->asz32 ? fetch32(c) : fetch16(c); wb(c, c->segov >= 0 ? c->segov : DS, (uint16_t)o, R8(c, 0)); break; }
    case 0xA3: { uint32_t o = c->asz32 ? fetch32(c) : fetch16(c); wrv(c, c->segov >= 0 ? c->segov : DS, (uint16_t)o, getr(c, AX)); break; }
    case 0xA4: case 0xA5: case 0xA6: case 0xA7: case 0xAA: case 0xAB: case 0xAC: case 0xAD: case 0xAE: case 0xAF: {
        uint16_t n = c->r[CX]; string_op(c, op, rep); cnt += rep ? n : 0; break; }
    case 0xA8: { uint8_t i = fetch8(c); alu(c, 4, R8(c, 0), i, 8); break; }
    case 0xA9: { uint32_t i = fetchv(c); alu(c, 4, getr(c, AX), i, OB); break; }
    case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7: SETR8(c, op & 7, fetch8(c)); break;
    case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF: setr(c, op & 7, fetchv(c)); break;
    case 0xC0: case 0xC1: case 0xD0: case 0xD1: case 0xD2: case 0xD3: {
        modrm(c);
        unsigned n = (op == 0xC0 || op == 0xC1) ? fetch8(c) : (op == 0xD0 || op == 0xD1) ? 1 : R8(c, 1);
        if (op & 1) set_ev(c, shiftop(c, c->reg, get_ev(c), n, OB)); else set_eb(c, shiftop(c, c->reg, get_eb(c), n, 8));
        break; }
    case 0xC2: { uint16_t n = fetch16(c); c->ip = (uint16_t)popv(c); c->r[SP] += n; break; }
    case 0xC3: c->ip = (uint16_t)popv(c); break;
    case 0xC4: case 0xC5: modrm(c); { uint32_t o = rdv(c, c->ea_seg, c->ea_off); uint16_t s = rw(c, c->ea_seg, c->ea_off + (c->osz32 ? 4 : 2)); setr(c, c->reg, o); c->seg[op == 0xC4 ? ES : DS] = s; break; }
    case 0xC6: modrm(c); set_eb(c, fetch8(c)); break;
    case 0xC7: modrm(c); set_ev(c, fetchv(c)); break;
    case 0xC8: { uint16_t sz = fetch16(c); uint8_t lvl = fetch8(c) & 0x1F; push16(c, c->r[BP]); uint16_t fp = c->r[SP];
        if (lvl > 0) { for (int i = 1; i < lvl; i++) { c->r[BP] -= 2; push16(c, rw(c, SS, c->r[BP])); } push16(c, fp); }
        c->r[BP] = fp; c->r[SP] -= sz; break; }
    case 0xC9: c->r[SP] = c->r[BP]; setr(c, BP, popv(c)); break;
    case 0xCA: { uint16_t n = fetch16(c); c->ip = (uint16_t)popv(c); c->seg[CS] = (uint16_t)popv(c); c->r[SP] += n; break; }
    case 0xCB: c->ip = (uint16_t)popv(c); c->seg[CS] = (uint16_t)popv(c); break;
    case 0xCC: cpu_int(c, 3); break;
    case 0xCD: { uint8_t n = fetch8(c); cpu_int(c, n); break; }
    case 0xCE: if (c->of) cpu_int(c, 4); break;
    case 0xCF: if (c->osz32) { c->ip = (uint16_t)pop32(c); c->seg[CS] = (uint16_t)pop32(c); cpu_set_flags(c, (uint16_t)pop32(c)); } else cpu_iret(c); break;
    case 0xD4: { uint8_t b = fetch8(c); if (!b) { div_err(c); break; } uint8_t al = R8(c, 0); SETR8(c, 4, al / b); SETR8(c, 0, al % b); szp(c, R8(c, 0), 8); break; }
    case 0xD5: { uint8_t b = fetch8(c); uint8_t r = R8(c, 0) + R8(c, 4) * b; SETR8(c, 0, r); SETR8(c, 4, 0); szp(c, r, 8); break; }
    case 0xD6: SETR8(c, 0, c->cf ? 0xFF : 0); break;
    case 0xD7: SETR8(c, 0, rb(c, c->segov >= 0 ? c->segov : DS, c->r[BX] + R8(c, 0))); break;
    case 0xD8: case 0xD9: case 0xDA: case 0xDB: case 0xDC: case 0xDD: case 0xDE: case 0xDF: modrm(c); break; /* FPU ignored */
    case 0xE0: { int8_t d = fetch8(c); if (--c->r[CX] != 0 && !c->zf) c->ip += d; break; }
    case 0xE1: { int8_t d = fetch8(c); if (--c->r[CX] != 0 && c->zf) c->ip += d; break; }
    case 0xE2: { int8_t d = fetch8(c); if (--c->r[CX] != 0) c->ip += d; break; }
    case 0xE3: { int8_t d = fetch8(c); if (c->r[CX] == 0) c->ip += d; break; }
    case 0xE4: SETR8(c, 0, io_inb(m, fetch8(c))); break;
    case 0xE5: { uint8_t p = fetch8(c); c->r[AX] = io_inb(m, p) | (io_inb(m, p + 1) << 8); break; }
    case 0xE6: io_outb(m, fetch8(c), R8(c, 0)); break;
    case 0xE7: { uint8_t p = fetch8(c); io_outb(m, p, R8(c, 0)); io_outb(m, p + 1, R8(c, 4)); break; }
    case 0xE8: { int32_t d = c->osz32 ? (int32_t)fetch32(c) : (int32_t)(int16_t)fetch16(c); pushv(c, c->ip); c->ip += d; break; }
    case 0xE9: { int32_t d = c->osz32 ? (int32_t)fetch32(c) : (int32_t)(int16_t)fetch16(c); c->ip += d; break; }
    case 0xEA: { uint32_t o = fetchv(c); uint16_t s = fetch16(c); c->ip = (uint16_t)o; c->seg[CS] = s; break; }
    case 0xEB: { int8_t d = fetch8(c); c->ip += d; break; }
    case 0xEC: SETR8(c, 0, io_inb(m, c->r[DX])); break;
    case 0xED: c->r[AX] = io_inb(m, c->r[DX]) | (io_inb(m, c->r[DX] + 1) << 8); break;
    case 0xEE: io_outb(m, c->r[DX], R8(c, 0)); break;
    case 0xEF: io_outb(m, c->r[DX], R8(c, 0)); io_outb(m, c->r[DX] + 1, R8(c, 4)); break;
    case 0xF1: hle_call(m, fetch8(c)); break;
    case 0xF4: c->halted = 1; break;
    case 0xF5: c->cf = !c->cf; break;
    case 0xF6: modrm(c); group3(c, 8); break;
    case 0xF7: modrm(c); group3(c, 16); break;
    case 0xF8: c->cf = 0; break;
    case 0xF9: c->cf = 1; break;
    case 0xFA: c->iff = 0; break;
    case 0xFB: c->iff = 1; c->inhibit = 1; break;
    case 0xFC: c->df = 0; break;
    case 0xFD: c->df = 1; break;
    case 0xFE: modrm(c); { uint8_t cf = c->cf; uint8_t v = get_eb(c);
        if (c->reg == 0) set_eb(c, alu(c, 0, v, 1, 8)); else if (c->reg == 1) set_eb(c, alu(c, 5, v, 1, 8));
        c->cf = cf; break; }
    case 0xFF: modrm(c); {
        switch (c->reg) {
        case 0: { uint8_t cf = c->cf; set_ev(c, alu(c, 0, get_ev(c), 1, OB)); c->cf = cf; break; }
        case 1: { uint8_t cf = c->cf; set_ev(c, alu(c, 5, get_ev(c), 1, OB)); c->cf = cf; break; }
        case 2: { uint32_t t = get_ev(c); pushv(c, c->ip); c->ip = (uint16_t)t; break; }
        case 3: { uint32_t o = rdv(c, c->ea_seg, c->ea_off); uint16_t s = rw(c, c->ea_seg, c->ea_off + (c->osz32 ? 4 : 2)); pushv(c, c->seg[CS]); pushv(c, c->ip); c->ip = (uint16_t)o; c->seg[CS] = s; break; }
        case 4: c->ip = (uint16_t)get_ev(c); break;
        case 5: { uint32_t o = rdv(c, c->ea_seg, c->ea_off); uint16_t s = rw(c, c->ea_seg, c->ea_off + (c->osz32 ? 4 : 2)); c->ip = (uint16_t)o; c->seg[CS] = s; break; }
        case 6: { uint32_t v = get_ev(c); pushv(c, v); break; }
        default: break;
        }
        break; }
    default:
        m->stat_unhandled++;
        if (m->verbose || m->stat_unhandled < 20)
            fprintf(stderr, "[cpu] unimplemented opcode %02X at %04X:%04X\n", op, c->seg[CS], start_ip);
        break;
    }
    return cnt;
}
