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
        if (clear) { memset(v->plane, 0, sizeof(v->plane)); memset(v->bgplane, 0, sizeof(v->bgplane)); memset(v->chplane, 0, sizeof(v->chplane)); }
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
 * every write except those character blits, so it holds the picture without the characters (what shows behind a character when it is
 * lifted out in 3D).  Which pixels ARE character pixels is tracked separately and exactly, in chplane[] (see below). */
#define CHAR_BLIT_IP_LO 0x1D00
#define CHAR_BLIT_IP_HI 0x1D80
/* The animated parts of a character (eyes blinking, mouth moving: a rectangle patched over the face) are drawn by a third routine, a
 * per-plane compositing loop whose four VRAM stores are at IP 2985/2990/299B/29A6 of SILV1.COM.  Without these, the face area counted as
 * background, stayed put in the "characters only" 3D view and looked detached from the body. */
#define FACE_BLIT_IP_LO 0x2980
#define FACE_BLIT_IP_HI 0x29B0
/* chplane[] records, per pixel, whether it is an opaque character pixel.  Comparing colours with the background is not enough: the
 * eyes, skin and clothes of a character often have exactly the colour of the (light) wall behind it, and those pixels would count as
 * background and stay behind when the character is lifted out in 3D.  The engine knows exactly which pixels are opaque (colour 8 is
 * transparent), so we take it from the blits themselves:
 *  - the character blit (IP 1D00.., and its screen->screen twin at IP 1B70..) first clears the opaque pixels with a set/reset write whose bit mask IS the opacity mask (store at IP 1D05);
 *  - the face patch (IP 2980..) composes an overlay (colour 8 = transparent) read from its source page at SI (usually the off-screen RAM
 *    picture) over a base picture; the first of its four stores is at IP 2987, and the overlay's opaque pixels are character pixels.
 * Any other write into the screen repaints background and clears the bits again (the mouse cursor routine at IP 0850..09FF is ignored). */
#define CHAR_MASK_STORE_IP 0x1D05
/* The character blit exists in four variants, picked at IP 1ADB from two flags (source in RAM / destination in RAM): RAM->screen (IP 1C95..,
 * the one that is always used for the first scenes), screen->screen (IP 1AF1.., sprites kept in off-screen video memory: same code with a
 * read-map select instead of the RAM segments; its stores are at IP 1B7A/1B99/1BAA/1BBC/1BCE) and two that write into RAM only.  The
 * screen->screen variant was not recognised either. */
#define CHAR2_BLIT_IP_LO 0x1B70
#define CHAR2_BLIT_IP_HI 0x1BD0
#define CHAR2_MASK_STORE_IP 0x1B7A
#define FACE_FIRST_STORE_IP 0x2987
/* The face patch has a twin for the case "overlay and base both come from the off-screen RAM picture, destination = screen" (descriptor
 * flags & 7 == 3, entered by a jump at IP 28A2): the same compositing, but its four stores are at IP 2A8A/2AB3/2ADC/2B06 and it keeps the
 * opaque-pixel mask in BL (not AH).  It was not recognised as a character routine, so the animated parts drawn by it counted as background. */
#define FACE2_BLIT_IP_LO 0x2A80
#define FACE2_BLIT_IP_HI 0x2B10
#define FACE2_FIRST_STORE_IP 0x2A8C
#define CURSOR_IP_LO 0x0850
#define CURSOR_IP_HI 0x0A00
/* Not every character animates its eyes/mouth with the masked face patch above.  Most of them (all but the blonde one) patch a small
 * OPAQUE rectangle of the face over the picture, with the general page copy (dispatcher IP 17AB; RAM page -> screen variant at IP 18D0, four
 * `rep movs` stores at IP 1911/192A/1943/195C, one per plane): frames of the blink/mouth animation are kept in a RAM page and copied over
 * the face (e.g. 8x80, 10x40, 11x64 bytes).  The same routine also draws the backgrounds, so it counts as a character patch only when (a) the
 * copied rectangle is small (the routine's stack frame holds the byte count per row at SP+4 and the rows still to copy at SP+6) and (b) it lands on
 * pixels that already are character pixels (chplane).  Its pixels that differ from the background are then character pixels, and the
 * background mirror is left alone (the face would otherwise become "background" and stay put when the character is lifted out in 3D). */
#define RECT_STORE_IP0 0x1911
#define RECT_STORE_IP1 0x192A
#define RECT_STORE_IP2 0x1943
#define RECT_STORE_IP3 0x195C
/* The screen->screen twin (IP 183F, off-screen video memory as the source, write mode 1 = latch copy) moves all four planes with one store at IP 1841;
 * its rows-left counter is still in DX. */
#define RECT2_STORE_IP 0x1841
#define RECT_PATCH_MAX_BYTES 16
#define RECT_PATCH_MAX_ROWS 140
static uint8_t rect_mark[65536];     /* per byte: this rectangle copy lands on character pixels (decided at the first plane's store) */
/* Opaque-pixel mask of the face patch overlay at offset SI of its source page.  The pages are four-segment tables in the engine's data
 * segment (page 0 = the screen, A0C8 x4; page 1 = four RAM plane buffers); the routine's own variable cs:27B6 tells which one is the source.
 * (The mask register AH is already overwritten by the sequencer index when the first store happens, so it has to be read from the source.) */
static uint8_t face_src_opaque(Machine *m, uint16_t si) {
    VGA *v = &m->vga; const uint16_t cs = m->cpu.seg[CS];
    const uint16_t dseg = mem_rw(m, LIN(cs, 0x27C8));              /* operand of "mov dx,<data segment>" in the page-select routine at 27C6 */
    const uint16_t sp = mem_rb(m, LIN(cs, 0x27B6)) & 1;
    uint8_t q[4];
    for (int k = 0; k < 4; k++) {
        const uint16_t seg = mem_rw(m, LIN(dseg, 0xE2D0 + sp * 16 + k * 4));
        q[k] = (seg >= 0xA000 && seg < 0xB000) ? v->plane[k][(((uint32_t)(seg - 0xA000) << 4) + si) & 0xFFFF] : mem_rb(m, LIN(seg, si));
    }
    return (uint8_t)~(~q[0] & ~q[1] & ~q[2] & q[3]);              /* everything but colour 8 (= 1000b) is opaque */
}
void vga_write(Machine *m, uint32_t off, uint8_t val) {
    if (m->vga_wr_dbg) m->vga_wr_dbg(m, off);
    VGA *v = &m->vga; off &= 0xFFFF;
    const uint16_t ip = m->cpu.ip;
    int patch = 0;                   /* part of a small rectangle copy over character pixels (see RECT_STORE_IP0) */
    int patch_first = 0;             /* this store writes the first (or only) plane of the byte */
    if (ip == RECT_STORE_IP0 || ip == RECT_STORE_IP1 || ip == RECT_STORE_IP2 || ip == RECT_STORE_IP3 || ip == RECT2_STORE_IP) {
        const uint16_t ss = m->cpu.seg[SS], sp = m->cpu.r[SP];
        const uint16_t bytes = mem_rw(m, LIN(ss, (uint16_t)(sp + 4))), rows = ip == RECT2_STORE_IP ? m->cpu.r[DX] : mem_rw(m, LIN(ss, (uint16_t)(sp + 6)));
        if (bytes <= RECT_PATCH_MAX_BYTES && rows <= RECT_PATCH_MAX_ROWS) {
            patch_first = ip == RECT_STORE_IP0 || ip == RECT2_STORE_IP;
            if (patch_first) rect_mark[off] = v->chplane[off] != 0;
            patch = rect_mark[off];
        }
    }
    const int chr = patch || (m->cpu.ip >= CHAR_BLIT_IP_LO && m->cpu.ip < CHAR_BLIT_IP_HI) || (m->cpu.ip >= CHAR2_BLIT_IP_LO && m->cpu.ip < CHAR2_BLIT_IP_HI)
                  || (m->cpu.ip >= FACE_BLIT_IP_LO && m->cpu.ip < FACE_BLIT_IP_HI) || (m->cpu.ip >= FACE2_BLIT_IP_LO && m->cpu.ip < FACE2_BLIT_IP_HI);
    if (chr) v->char_gen++; else if (off < 28160) v->pic_gen++;
    {
        uint8_t nv = v->chplane[off];
        if (ip == CHAR_MASK_STORE_IP || ip == CHAR2_MASK_STORE_IP) nv |= v->gc[8];
        else if (ip == FACE_FIRST_STORE_IP) nv |= face_src_opaque(m, m->cpu.r[SI]);
        else if (ip == FACE2_FIRST_STORE_IP) nv |= (uint8_t)m->cpu.r[BX];          /* BL = opaque mask at this point */
        else if (!chr && !(ip >= CURSOR_IP_LO && ip < CURSOR_IP_HI)) nv = 0;
        if (nv != v->chplane[off]) { v->chplane[off] = nv; v->ch_gen++; }
    }
    uint8_t mask = v->seq[2] & 0xF, wm = v->gc[5] & 3, func = (v->gc[3] >> 3) & 3, rot = v->gc[3] & 7;
    uint8_t bm = v->gc[8], sr = v->gc[0], esr = v->gc[1];
    if ((wm == 0 || wm == 3) && rot) val = (val >> rot) | (val << (8 - rot));
    uint8_t patch_diff = 0;
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
        if (patch) patch_diff |= res ^ v->bgplane[p][off];
        else if (!chr) v->bgplane[p][off] = res;
    }
    if (patch) {                     /* pixels that already were character pixels stay so (the patch lies inside the silhouette); the ones that now differ from the background join them */
        uint8_t nv = v->chplane[off] | patch_diff;
        if (nv != v->chplane[off]) { v->chplane[off] = nv; v->ch_gen++; }
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
