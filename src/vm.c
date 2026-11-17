#include "ai5.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static uint32_t vm_rand(vm_t *vm, uint32_t n) { static uint32_t s = 0x2545F491; (void)vm; s = s * 214013u + 2531011u; return n ? ((s >> 16) & 0x7FFF) % n : 0; }
#define CODE(vm) ((vm)->mes + (vm)->base)
static inline uint8_t  f8 (vm_t *vm) { return CODE(vm)[vm->ip++]; }
static inline uint16_t f16(vm_t *vm) { uint16_t v = rd16(CODE(vm) + vm->ip); vm->ip += 2; return v; }
static inline uint32_t f32(vm_t *vm) { uint32_t v = rd32(CODE(vm) + vm->ip); vm->ip += 4; return v; }

uint32_t vm_expr(vm_t *vm) {
    uint32_t st[64]; int sp = 0; uint8_t *m = vm->mem;
#define PUSH(v) (st[sp++ & 63] = (v))
#define POP()   (st[--sp & 63])
    for (;;) {
        uint8_t b = f8(vm); uint32_t a, r, v;
        switch (b) {
        case 0xFF: return sp > 0 ? st[sp - 1] : 0;
        case 0x80: PUSH(V16(vm, f8(vm))); break;
        case 0xA0: a = POP(); v = f8(vm);
            PUSH(v == 0 ? rd16(m + OFF_SYS16 + 2 * a) : rd16(m + ((V16(vm, v - 1) + 2 * a) & (MEM_SIZE - 1)))); break;
        case 0xC0: a = POP(); v = f8(vm); PUSH(m[(V16(vm, v) + a) & (MEM_SIZE - 1)]); break;
        case 0xE0: r = POP(); a = POP(); PUSH(a + r); break;
        case 0xE1: r = POP(); a = POP(); PUSH(a - r); break;
        case 0xE2: r = POP(); a = POP(); PUSH(a * r); break;
        case 0xE3: r = POP(); a = POP(); PUSH(r ? a / r : 0); break;
        case 0xE4: r = POP(); a = POP(); PUSH(r ? a % r : 0); break;
        case 0xE5: a = POP(); PUSH(vm_rand(vm, a)); break;
        case 0xE6: r = POP(); a = POP(); PUSH(a && r); break;
        case 0xE7: r = POP(); a = POP(); PUSH(a || r); break;
        case 0xE8: r = POP(); a = POP(); PUSH(a & r); break;
        case 0xE9: r = POP(); a = POP(); PUSH(a | r); break;
        case 0xEA: r = POP(); a = POP(); PUSH(a ^ r); break;
        case 0xEB: r = POP(); a = POP(); PUSH(a < r); break;
        case 0xEC: r = POP(); a = POP(); PUSH(a > r); break;
        case 0xED: r = POP(); a = POP(); PUSH(a <= r); break;
        case 0xEE: r = POP(); a = POP(); PUSH(a >= r); break;
        case 0xEF: r = POP(); a = POP(); PUSH(a == r); break;
        case 0xF0: r = POP(); a = POP(); PUSH(a != r); break;
        case 0xF1: PUSH(f16(vm)); break;
        case 0xF2: PUSH(f32(vm)); break;
        case 0xF3: PUSH(FLAG(vm, f16(vm))); break;
        case 0xF5: a = POP(); PUSH(FLAG(vm, a)); break;
        case 0xF6: PUSH(M32(vm, f16(vm))); break;
        case 0xF7: a = POP(); PUSH(rd32(m + ((OFF_M32 + 4 * a) & (MEM_SIZE - 1)))); break;
        case 0xF8: PUSH(V32(vm, f8(vm))); break;
        case 0xF9: a = POP(); v = f8(vm);
            PUSH(v == 0 ? rd32(m + OFF_SYS32 + 4 * a) : rd32(m + ((V32(vm, v - 1) + 4 * a) & (MEM_SIZE - 1)))); break;
        default: PUSH(b); break;
        }
    }
}
static void read_params(vm_t *vm) {
    vm->nparams = 0;
    for (;;) {
        uint8_t t = f8(vm); if (!t) break;
        vm_param *p = &vm->params[vm->nparams < MAX_PARAMS ? vm->nparams++ : MAX_PARAMS - 1];
        p->type = t; p->val = 0; p->str = "";
        if (t == 1) { p->str = (const char *)CODE(vm) + vm->ip; vm->ip += strlen(p->str) + 1; }
        else p->val = vm_expr(vm);
    }
    for (int i = vm->nparams; i < MAX_PARAMS; i++) { vm->params[i].type = 0; vm->params[i].val = 0; vm->params[i].str = ""; }
}
static void set_mes(vm_t *vm, const uint8_t *buf) {
    vm->mes = buf; uint32_t n = rd32(buf); vm->nlabels = n; vm->labels = (const uint32_t *)(buf + 4); vm->base = 4 + 4 * n;
}
static void run_block(vm_t *vm);
void vm_run_block(vm_t *vm) { run_block(vm); }
static void call_proc(vm_t *vm, vm_proc *p, const char *what, uint32_t no);
void vm_call_proc(vm_t *vm, uint32_t no) { if (vm->procs[no & 255].code) call_proc(vm, &vm->procs[no & 255], "proc", no); }
static void call_proc(vm_t *vm, vm_proc *p, const char *what, uint32_t no) {
    if (!p->code) { plat_log("undefined %s %u", what, no); return; }
    const uint8_t *smes = vm->mes; uint32_t sip = vm->ip, sbase = vm->base, snl = vm->nlabels; const uint32_t *sl = vm->labels;
    set_mes(vm, p->code); vm->ip = p->ip;
    run_block(vm);
    if (vm->want_goto || vm->quit) return;
    vm->mes = smes; vm->ip = sip; vm->base = sbase; vm->nlabels = snl; vm->labels = sl;
}
/* executes statements until END (returns) */
static void run_block(vm_t *vm) {
    uint8_t *m = vm->mem;
    while (!vm->quit && !vm->want_goto) {
        if (++vm->steps > 30000000ull) { plat_log("watchdog: stuck at %s:%x", vm->mes_name, vm->ip); vm->quit = true; return; }
        uint8_t op = f8(vm); uint32_t i, v;
        switch (op) {
        case 0x00: return;
        case 0x01: case 0x02: { const char *s = (const char *)CODE(vm) + vm->ip; vm->ip += strlen(s) + 1; sys_text(vm, s, op == 1); break; }
        case 0x03: i = f16(vm); do { FLAG(vm, i) = vm_expr(vm); i++; } while (f8(vm)); break;
        case 0x04: i = f8(vm);  do { SET_V16(vm, i, vm_expr(vm)); i++; } while (f8(vm)); break;
        case 0x05: i = vm_expr(vm); do { FLAG(vm, i) = vm_expr(vm); i++; } while (f8(vm)); break;
        case 0x06: i = vm_expr(vm); v = f8(vm);    /* array16[v][i] */
            do { wr16(m + (((v ? V16(vm, v - 1) : OFF_SYS16) + 2 * i) & (MEM_SIZE - 1)), vm_expr(vm)); i++; } while (f8(vm)); break;
        case 0x07: i = vm_expr(vm); v = f8(vm);
            do { wr16(m + (((v ? V16(vm, v - 1) : OFF_SYS16) + 2 * i) & (MEM_SIZE - 1)), vm_expr(vm)); i++; } while (f8(vm)); break;
        case 0x08: i = vm_expr(vm); v = f8(vm);
            do { wr32(m + (((v ? V32(vm, v - 1) : OFF_SYS32) + 4 * i) & (MEM_SIZE - 1)), vm_expr(vm)); i++; } while (f8(vm)); break;
        case 0x09: i = f16(vm); do { SET_M32(vm, i, vm_expr(vm)); i++; } while (f8(vm)); break;
        case 0x0A: i = vm_expr(vm); do { wr32(m + ((OFF_M32 + 4 * i) & (MEM_SIZE - 1)), vm_expr(vm)); i++; } while (f8(vm)); break;
        case 0x0B: v = vm_expr(vm); i = f32(vm); if (!v) vm->ip = i; break;
        case 0x0C: vm->ip = f32(vm); break;
        case 0x0D: v = vm_expr(vm); read_params(vm); sys_call(vm, v); break;
        case 0x0E: read_params(vm); strncpy(vm->goto_name, vm->params[0].str, sizeof vm->goto_name - 1); vm->want_goto = true; return;
        case 0x0F: { read_params(vm);
            const uint8_t *smes = vm->mes; uint32_t sip = vm->ip, sbase = vm->base, snl = vm->nlabels; const uint32_t *sl = vm->labels;
            size_t n; uint8_t *buf = arc_load_lzss(&vm->arc_mes, vm->params[0].str, &n);
            if (!buf) { plat_log("CALL: missing %s", vm->params[0].str); break; }
            set_mes(vm, buf); vm->ip = 0; run_block(vm);
            if (vm->want_goto || vm->quit) return;   /* buffer leaks on purpose: procs may point into it */
            vm->mes = smes; vm->ip = sip; vm->base = sbase; vm->nlabels = snl; vm->labels = sl; break; }
        case 0x10: read_params(vm); i = f32(vm); sys_menu_define(vm, vm->ip); vm->ip = i; break;
        case 0x11: read_params(vm); v = vm->params[0].val; call_proc(vm, &vm->procs[v & 255], "proc", v); break;
        case 0x12: read_params(vm); sys_util(vm); break;
        case 0x13: sys_line(vm, f8(vm)); break;
        case 0x14: v = vm_expr(vm); i = f32(vm); vm->procs[v & 255].code = vm->mes; vm->procs[v & 255].ip = vm->ip; vm->ip = i; break;
        case 0x15: read_params(vm); sys_menu_exec(vm); break;
        case 0x16: i = f8(vm); do { SET_V32(vm, i, vm_expr(vm)); i++; } while (f8(vm)); break;
        case 0x17: vm->msgid = f32(vm); break;
        case 0x18: v = vm_expr(vm); if (v < vm->nlabels) vm->ip = rd32((const uint8_t *)vm->labels + 4 * v); else plat_log("JMPLBL %u out of range", v); break;
        case 0x19: if (vm->procsp < 16) memcpy(vm->procstack[vm->procsp++], vm->procs, sizeof vm->procs); break;
        case 0x1A: if (vm->procsp > 0) memcpy(vm->procs, vm->procstack[--vm->procsp], sizeof vm->procs); break;
        case 0x1B: read_params(vm); sys_op1b(vm); break;
        case 0x1C: v = vm_expr(vm); i = f32(vm); vm->lib[v & 255].code = vm->mes; vm->lib[v & 255].ip = vm->ip; vm->ip = i; break;
        case 0x1D: read_params(vm); v = vm->params[0].val; call_proc(vm, &vm->lib[v & 255], "lib", v); break;
        case 0x1F: vm->ip += 4; break;
        default: plat_log("bad opcode %02x at %s:%x", op, vm->mes_name, vm->ip - 1); vm->quit = true; return;
        }
    }
}
bool vm_load_mes(vm_t *vm, const char *name) {
    size_t n; uint8_t *buf = arc_load_lzss(&vm->arc_mes, name, &n);
    if (!buf) { plat_log("missing script %s", name); return false; }
    set_mes(vm, buf); vm->mes_len = n; vm->ip = 0;
    strncpy(vm->mes_name, name, sizeof vm->mes_name - 1);
    return true;
}
bool vm_init(vm_t *vm) {
    memset(vm, 0, sizeof *vm);
    vm->mem = calloc(1, MEM_SIZE);
    if (!(arc_open(&vm->arc_mes, "MES.ARC") && arc_open(&vm->arc_data, "DATA.ARC") && arc_open(&vm->arc_bg, "BG.ARC"))) return false;
    arc_open(&vm->arc_sound, "SOUND.ARC"); arc_open(&vm->arc_music, "MUSIC.ARC"); arc_open(&vm->arc_voice, "VOICE.ARC");   /* all optional */
    return true;
}
void vm_run(vm_t *vm) {
    while (!vm->quit) {
        run_block(vm);
        if (vm->want_goto) { vm->want_goto = false; { extern const char *plat_goto_override(void); const char *o = plat_goto_override(); if (o) strncpy(vm->goto_name, o, sizeof vm->goto_name - 1); } memset(vm->procs, 0, sizeof vm->procs); vm->procsp = 0;
            if (!vm_load_mes(vm, vm->goto_name)) return; continue; }
        return;
    }
}
