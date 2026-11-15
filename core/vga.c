/* 16-colour planar VGA (mode 12h) emulation: enough for 640x480x16 DOS games. */
#include "machine.h"
#include <string.h>

static const uint8_t ega_attr[16] = {0, 1, 2, 3, 4, 5, 0x14, 7, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F};

static void default_palette(VGA *v) {
    static const uint8_t base[8][3] = {{0,0,0},{0,0,42},{0,42,0},{0,42,42},{42,0,0},{42,0,42},{42,21,0},{42,42,42}};
    static const uint8_t brt[8][3]  = {{21,21,21},{21,21,63},{21,63,21},{21,63,63},{63,21,21},{63,21,63},{63,63,21},{63,63,63}};
    memset(v->dac, 0, sizeof(v->dac));
    for (int i = 0; i < 8; i++) { memcpy(v->dac[i], base[i], 3); memcpy(v->dac[0x38 + i], brt[i], 3); }
    memcpy(v->dac[0x14], base[6], 3);
}

void vga_init(VGA *v) {
    memset(v, 0, sizeof(*v));
    default_palette(v);
    v->dac_mask = 0xFF;
    for (int i = 0; i < 16; i++) v->attr[i] = ega_attr[i];
    v->attr[0x10] = 0x01; v->attr[0x12] = 0x0F;
    v->seq[2] = 0x0F; v->gc[8] = 0xFF; v->gc[6] = 0x05;
    v->crtc[0x13] = 0x28; v->crtc[0x17] = 0xE3;
    v->bios_mode = 3;
}

void vga_set_mode(Machine *m, int mode) {
    VGA *v = &m->vga;
    int clear = !(mode & 0x80); mode &= 0x7F;
    v->bios_mode = mode;
    if (mode == 0x12 || mode == 0x10 || mode == 0x0E || mode == 0x0D) {
        if (clear) { memset(v->plane, 0, sizeof(v->plane)); memset(v->bgplane, 0, sizeof(v->bgplane)); }
        memset(v->gc, 0, sizeof(v->gc)); v->gc[8] = 0xFF; v->gc[6] = 0x05; v->gc[7] = 0x0F;
        v->seq[2] = 0x0F; v->seq[4] = 0x06;
        for (int i = 0; i < 16; i++) v->attr[i] = ega_attr[i];
        v->attr[0x10] = 0x01; v->attr[0x12] = 0x0F; v->attr[0x14] = 0;
        v->crtc[0xC] = v->crtc[0xD] = 0; v->crtc[0x13] = 0x28; v->crtc[0x17] = 0xE3;
        default_palette(v);
    }
    v->dirty++; v->pal_gen++;
    if (m->on_vga_mode) m->on_vga_mode(m);
}

static inline uint8_t logic(int f, uint8_t d, uint8_t l) {
    switch (f) { case 1: return d & l; case 2: return d | l; case 3: return d ^ l; default: return d; }
}

uint8_t vga_read(Machine *m, uint32_t off) {
    VGA *v = &m->vga; off &= 0xFFFF;
    v->latch[0] = v->plane[0][off]; v->latch[1] = v->plane[1][off];
    v->latch[2] = v->plane[2][off]; v->latch[3] = v->plane[3][off];
    if (v->gc[5] & 8) {
        uint8_t cc = v->gc[2] & 0xF, cdc = v->gc[7] & 0xF, res = 0xFF;
        for (int p = 0; p < 4; p++) if (cdc & (1 << p)) res &= (cc & (1 << p)) ? v->latch[p] : (uint8_t)~v->latch[p];
        return res;
    }
    return v->latch[v->gc[4] & 3];
}

/* The SILV engine draws the background (and full-screen event pictures) with one routine and the characters, which it
 * composes over that background, with another one (a plain rectangle blit at IP 1D00..1D7F of SILV1.COM).  bgplane[] mirrors
 * every write except those character blits, so "plane differs from bgplane" marks exactly the character pixels. */
#define CHAR_BLIT_IP_LO 0x1D00
#define CHAR_BLIT_IP_HI 0x1D80
void vga_write(Machine *m, uint32_t off, uint8_t val) {
    if (m->vga_wr_dbg) m->vga_wr_dbg(m, off);
    VGA *v = &m->vga; off &= 0xFFFF;
    const int chr = m->cpu.ip >= CHAR_BLIT_IP_LO && m->cpu.ip < CHAR_BLIT_IP_HI;
    if (chr) v->char_gen++; else if (off < 28160) v->pic_gen++;
    uint8_t mask = v->seq[2] & 0xF, wm = v->gc[5] & 3, func = (v->gc[3] >> 3) & 3, rot = v->gc[3] & 7;
    uint8_t bm = v->gc[8], sr = v->gc[0], esr = v->gc[1];
    if ((wm == 0 || wm == 3) && rot) val = (val >> rot) | (val << (8 - rot));
    for (int p = 0; p < 4; p++) {
        if (!(mask & (1 << p))) continue;
        uint8_t l = v->latch[p], d, res;
        switch (wm) {
        case 0: d = (esr & (1 << p)) ? ((sr & (1 << p)) ? 0xFF : 0) : val; res = logic(func, d, l); res = (res & bm) | (l & ~bm); break;
        case 1: res = l; break;
        case 2: d = (val & (1 << p)) ? 0xFF : 0; res = logic(func, d, l); res = (res & bm) | (l & ~bm); break;
        default: { uint8_t b = val & bm; d = (sr & (1 << p)) ? 0xFF : 0; res = logic(func, d, l); res = (res & b) | (l & ~b); break; }
        }
        v->plane[p][off] = res;
        if (!chr) v->bgplane[p][off] = res;
    }
    v->dirty++;
}

uint8_t vga_io_in(Machine *m, uint16_t port) {
    VGA *v = &m->vga;
    switch (port) {
    case 0x3C0: return v->attr_idx;
    case 0x3C1: return v->attr[v->attr_idx & 0x1F];
    case 0x3C2: return 0x10;
    case 0x3C4: return v->seq_idx;
    case 0x3C5: return v->seq[v->seq_idx & 7];
    case 0x3C6: return v->dac_mask;
    case 0x3C7: return 0x03;
    case 0x3C8: return v->dac_wi;
    case 0x3C9: { uint8_t r = v->dac[v->dac_ri][v->dac_comp_r++]; if (v->dac_comp_r == 3) { v->dac_comp_r = 0; v->dac_ri++; } return r; }
    case 0x3CC: return v->misc;
    case 0x3CE: return v->gc_idx;
    case 0x3CF: return v->gc[v->gc_idx & 15];
    case 0x3B4: case 0x3D4: return v->crtc_idx;
    case 0x3B5: case 0x3D5: return v->crtc[v->crtc_idx & 31];
    case 0x3BA: case 0x3DA: {
        v->attr_ff = 0;
        uint64_t fl = m->ips / 60; if (!fl) fl = 1;
        uint64_t ph = m->icount % fl;
        uint64_t line = fl / 525; if (!line) line = 1;
        int vr = ph < fl * 6 / 100;
        int hb = ((m->icount % line) * 5 >= line * 4);
        return 0xC0 | (vr ? 8 : 0) | ((vr || hb) ? 1 : 0); }
    }
    return 0xFF;
}

void vga_io_out(Machine *m, uint16_t port, uint8_t val) {
    VGA *v = &m->vga;
    switch (port) {
    case 0x3C0:
        if (!v->attr_ff) { v->attr_idx = val; v->attr_ff = 1; }
        else { v->attr[v->attr_idx & 0x1F] = val; v->attr_ff = 0; v->dirty++; v->pal_gen++; }
        break;
    case 0x3C2: v->misc = val; break;
    case 0x3C4: v->seq_idx = val; break;
    case 0x3C5: v->seq[v->seq_idx & 7] = val; break;
    case 0x3C6: v->dac_mask = val; break;
    case 0x3C7: v->dac_ri = val; v->dac_comp_r = 0; break;
    case 0x3C8: v->dac_wi = val; v->dac_comp_w = 0; break;
    case 0x3C9:
        v->dac[v->dac_wi][v->dac_comp_w++] = val & 0x3F;
        if (v->dac_comp_w == 3) { v->dac_comp_w = 0; v->dac_wi++; v->dirty++; v->pal_gen++; }
        break;
    case 0x3CE: v->gc_idx = val; break;
    case 0x3CF: v->gc[v->gc_idx & 15] = val; break;
    case 0x3B4: case 0x3D4: v->crtc_idx = val; break;
    case 0x3B5: case 0x3D5: v->crtc[v->crtc_idx & 31] = val; v->dirty++; v->pal_gen++; break;
    }
}

/* final 16 colours as 8-bit RGB (r,g,b) */
void vga_palette16(Machine *m, uint8_t rgb[16][3]) {
    VGA *v = &m->vga;
    for (int i = 0; i < 16; i++) {
        int idx = v->attr[i] & 0x3F;
        if (v->attr[0x10] & 0x80) idx = (idx & 0x0F) | ((v->attr[0x14] & 3) << 4);
        idx |= (v->attr[0x14] & 0x0C) << 4;
        idx &= v->dac_mask;
        for (int k = 0; k < 3; k++) { uint8_t d = v->dac[idx][k]; rgb[i][k] = (d << 2) | (d >> 4); }
    }
}

/* decode the full 640x480 frame into 4-bit colour indices (one per byte) */
void vga_decode(Machine *m, uint8_t *dst, int w, int h) {
    VGA *v = &m->vga;
    int pitch = v->crtc[0x13] * 2; if (!pitch) pitch = 80;
    uint32_t start = (v->crtc[0xC] << 8) | v->crtc[0xD];
    for (int y = 0; y < h; y++) {
        uint32_t row = (start + (uint32_t)y * pitch) & 0xFFFF;
        uint8_t *o = dst + y * w;
        for (int bx = 0; bx < w / 8; bx++) {
            uint32_t a = (row + bx) & 0xFFFF;
            uint8_t p0 = v->plane[0][a], p1 = v->plane[1][a], p2 = v->plane[2][a], p3 = v->plane[3][a];
            for (int b = 0; b < 8; b++) {
                int s = 7 - b;
                *o++ = ((p0 >> s) & 1) | (((p1 >> s) & 1) << 1) | (((p2 >> s) & 1) << 2) | (((p3 >> s) & 1) << 3);
            }
        }
    }
}
