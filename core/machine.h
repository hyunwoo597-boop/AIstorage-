#ifndef MACHINE_H
#define MACHINE_H
#include <stdint.h>
#include <stdio.h>
#include "x86.h"

#define GUEST_IPS_DEFAULT 6000000u
#define HLE_SEG 0xF000
#define MAX_FILES 32

/* ---------------- VGA ---------------- */
typedef struct {
    uint8_t plane[4][0x10000];
    uint8_t seq[8], gc[16], crtc[32], attr[32], misc;
    uint8_t seq_idx, gc_idx, crtc_idx, attr_idx, attr_ff;
    uint8_t latch[4];
    uint8_t dac[256][3];
    uint8_t dac_wi, dac_ri, dac_comp_w, dac_comp_r, dac_mask;
    uint8_t bios_mode;
    uint32_t dirty;        /* incremented on every VRAM / palette change */
} VGA;

/* ---------------- OPL ---------------- */
typedef struct OPL OPL;

/* ---------------- DOS ---------------- */
typedef struct {
    FILE *fp;      /* NULL = closed */
    int dev;       /* 0 none, 1 EMM device, 2 console/null */
    uint16_t psp;
} DosFile;

struct Machine {
    CPU cpu;
    uint8_t mem[0x100000 + 0x10000];
    /* EMS */
    uint8_t *ems; uint32_t ems_bytes; int ems_total_pages;
    int ems_map[4];                 /* logical page mapped in each physical page, -1 none */
    uint8_t ems_alloc[256];         /* simplistic: handle table */
    uint16_t ems_handle_pages[256]; uint16_t ems_handle_base[256]; int ems_next_page;
    /* XMS */
    struct { uint8_t *p; uint32_t kb; } xms[64]; uint32_t xms_total_kb, xms_used_kb;
    void (*vga_wr_dbg)(Machine *, uint32_t off);
    void (*on_load)(Machine *, uint16_t load_seg);   /* called after INT 21h/4B01 loaded a child program */
    struct TextCap *tc;
    /* devices */
    VGA vga;
    OPL *opl;
    /* PIC */
    uint8_t pic_irr, pic_isr, pic_imr; int irq_pending;
    /* PIT ch0 */
    uint32_t pit_reload; uint8_t pit_ctl_lohi, pit_hi_phase, pit_latch_valid; uint16_t pit_latch;
    uint8_t pit_wr_tmp;
    uint64_t pit_last_irq; /* icount of last IRQ0 */
    /* keyboard */
    uint8_t kbd_data, kbd_obf; uint16_t kq[64]; int kq_h, kq_t;
    uint16_t bios_kbuf[16]; int bk_h, bk_t;
    uint8_t shift_state;
    uint8_t port61;
    /* mouse (host-updated) */
    int mouse_x, mouse_y, mouse_btn, mouse_present;
    int mouse_minx, mouse_maxx, mouse_miny, mouse_maxy;
    int mouse_visible;
    /* time */
    uint64_t icount; uint32_t ips;
    uint64_t ic_base, vt_base_ns;                /* virtual-time anchor (rebased whenever ips changes) */
    /* DOS */
    char gamedir[256];
    DosFile files[MAX_FILES];
    uint16_t cur_psp, dta_seg, dta_off;
    uint16_t mcb_first;
    int exited; int exit_code; int tsr_kept; uint16_t exit_psp;
    int verbose;
    char con[512]; int con_len;     /* last DOS console output (error messages) for on-device display */
    /* hooks for host/front-end */
    void (*on_vga_mode)(Machine *);
    uint32_t stat_unhandled;
};

/* memory */
/* slow paths (VGA / EMS window / ROM) live in machine.c; plain RAM below 0xA0000 is inlined */
uint8_t  mem_rb_slow(Machine *m, uint32_t a);
uint16_t mem_rw_slow(Machine *m, uint32_t a);
void     mem_wb_slow(Machine *m, uint32_t a, uint8_t v);
void     mem_ww_slow(Machine *m, uint32_t a, uint16_t v);
static inline uint8_t  mem_rb(Machine *m, uint32_t a) { a &= 0xFFFFF; return a < 0xA0000 ? m->mem[a] : mem_rb_slow(m, a); }
static inline uint16_t mem_rw(Machine *m, uint32_t a) { a &= 0xFFFFF; return a < 0x9FFFF ? (uint16_t)(m->mem[a] | (m->mem[a + 1] << 8)) : mem_rw_slow(m, a); }
static inline void     mem_wb(Machine *m, uint32_t a, uint8_t v) { a &= 0xFFFFF; if (a < 0xA0000) m->mem[a] = v; else mem_wb_slow(m, a, v); }
static inline void     mem_ww(Machine *m, uint32_t a, uint16_t v) { a &= 0xFFFFF; if (a < 0x9FFFF) { m->mem[a] = (uint8_t)v; m->mem[a + 1] = (uint8_t)(v >> 8); } else mem_ww_slow(m, a, v); }
/* io */
uint8_t  io_inb(Machine *m, uint16_t port);
void     io_outb(Machine *m, uint16_t port, uint8_t v);
/* machine */
Machine *machine_new(const char *gamedir);
void     machine_free(Machine *m);
void machine_con_putc(Machine *m, uint8_t ch);
uint64_t machine_vtime_ns(Machine *m);       /* emulated time since start, in ns */
void machine_set_ips(Machine *m, uint32_t ips); /* change emulated CPU speed without disturbing virtual time */
int      machine_run(Machine *m, int max_instr);   /* returns instructions executed */
void     machine_key(Machine *m, uint8_t scancode, int down, uint8_t ascii);
void     pic_raise(Machine *m, int irq);
int      pic_ack(Machine *m);
/* vga */
void vga_init(VGA *v);
uint8_t vga_read(Machine *m, uint32_t off);
void vga_write(Machine *m, uint32_t off, uint8_t val);
uint8_t vga_io_in(Machine *m, uint16_t port);
void vga_io_out(Machine *m, uint16_t port, uint8_t v);
void vga_set_mode(Machine *m, int mode);
void vga_render(Machine *m, uint8_t *rgb565_or_idx, int *w, int *h);
/* dos */
void dos_init(Machine *m);
int  dos_exec(Machine *m, const char *name, const char *args);
void hle_call(Machine *m, int id);
/* opl */
OPL *opl_new(int rate);
void opl_write(OPL *o, uint16_t port, uint8_t v);
uint8_t opl_read(OPL *o, uint16_t port);
typedef uint64_t (*opl_clock_fn)(void *ctx);
void opl_attach(OPL *o, opl_clock_fn now_ns, void *ctx);
void opl_mix(OPL *o, int16_t *out, int frames);   /* stereo interleaved */
void opl_free(OPL *o);
void opl_sync(OPL *o);                      /* render up to the machine's current virtual time */
uint32_t opl_available(OPL *o);
#endif
