#include "vgascale.h"

extern void vga_palette16(Machine *m, uint8_t rgb[16][3]);

/* SPL[p][b]: for plane p holding byte b (8 pixels, MSB = leftmost), byte k (k=0..3) of the result is the
 * contribution of that plane to the 8-bit "pixel pair" index (left pixel in bits 7..4, right pixel in bits 3..0)
 * of source pixels (2k, 2k+1). */
static uint32_t SPL[4][256];
static int tables_ready;

static void init_tables(void) {
    for (int p = 0; p < 4; p++)
        for (int b = 0; b < 256; b++) {
            uint32_t v = 0;
            for (int k = 0; k < 4; k++) {
                int l = (b >> (7 - 2 * k)) & 1, r = (b >> (6 - 2 * k)) & 1;
                v |= (uint32_t)((l << (4 + p)) | (r << p)) << (8 * k);
            }
            SPL[p][b] = v;
        }
    tables_ready = 1;
}

void vgascale_render(Machine *m, uint16_t *fb, int x0) {
    if (!tables_ready) init_tables();
    VGA *v = &m->vga;
    uint8_t pal[16][3];
    vga_palette16(m, pal);
    /* P[pair] = packed (r<<20 | g<<10 | b) sum of the two pixels' 8-bit colours; two of these never overflow a 10-bit field */
    uint32_t P[256];
    for (int a = 0; a < 16; a++)
        for (int b = 0; b < 16; b++) {
            uint32_t r = pal[a][0] + pal[b][0], g = pal[a][1] + pal[b][1], bl = pal[a][2] + pal[b][2];
            P[(a << 4) | b] = (r << 20) | (g << 10) | bl;
        }
    int pitch = v->crtc[0x13] * 2; if (!pitch) pitch = 80;
    uint32_t start = (v->crtc[0xC] << 8) | v->crtc[0xD];
    const uint8_t *pl0 = v->plane[0], *pl1 = v->plane[1], *pl2 = v->plane[2], *pl3 = v->plane[3];

    for (int bx = 0; bx < 80; bx++) {
        uint16_t *out0 = fb + (size_t)(x0 + bx * 4) * 240 + 239;   /* output y=0 of the 4 columns; y grows downwards in memory-descending order */
        for (int oy = 0; oy < 240; oy++) {
            uint32_t ra = (start + (uint32_t)(2 * oy) * pitch + bx) & 0xFFFF;
            uint32_t rb = (start + (uint32_t)(2 * oy + 1) * pitch + bx) & 0xFFFF;
            uint32_t top = SPL[0][pl0[ra]] | SPL[1][pl1[ra]] | SPL[2][pl2[ra]] | SPL[3][pl3[ra]];
            uint32_t bot = SPL[0][pl0[rb]] | SPL[1][pl1[rb]] | SPL[2][pl2[rb]] | SPL[3][pl3[rb]];
            for (int k = 0; k < 4; k++) {
                uint32_t s = P[(top >> (8 * k)) & 0xFF] + P[(bot >> (8 * k)) & 0xFF];
                out0[(size_t)k * 240 - oy] = (uint16_t)((((s >> 25) & 31) << 11) | (((s >> 14) & 63) << 5) | ((s >> 5) & 31));
            }
        }
    }
}

/* ------------------------------------------------------------------------------------------------
 * "Zoom" view: the engine only uses rows 40..439 of the 640x480 screen (640x400 = 16:10).  Box-filter that area
 * down to 320x200 (fast, table driven) and stretch it to fill the whole 400x240 top screen with a bilinear filter. */
#define MID_W 320
#define MID_H 200
static uint16_t mid[MID_W * MID_H];

static void stage1(Machine *m, uint16_t *dst, int row0, int rows_out) {
    if (!tables_ready) init_tables();
    VGA *v = &m->vga;
    uint8_t pal[16][3];
    vga_palette16(m, pal);
    uint32_t P[256];
    for (int a = 0; a < 16; a++)
        for (int b = 0; b < 16; b++) {
            uint32_t r = pal[a][0] + pal[b][0], g = pal[a][1] + pal[b][1], bl = pal[a][2] + pal[b][2];
            P[(a << 4) | b] = (r << 20) | (g << 10) | bl;
        }
    int pitch = v->crtc[0x13] * 2; if (!pitch) pitch = 80;
    uint32_t start = (v->crtc[0xC] << 8) | v->crtc[0xD];
    const uint8_t *pl0 = v->plane[0], *pl1 = v->plane[1], *pl2 = v->plane[2], *pl3 = v->plane[3];
    for (int oy = 0; oy < rows_out; oy++) {
        uint32_t rowa = start + (uint32_t)(row0 + 2 * oy) * pitch, rowb = rowa + pitch;
        uint16_t *o = dst + oy * MID_W;
        for (int bx = 0; bx < 80; bx++) {
            uint32_t ra = (rowa + bx) & 0xFFFF, rb = (rowb + bx) & 0xFFFF;
            uint32_t top = SPL[0][pl0[ra]] | SPL[1][pl1[ra]] | SPL[2][pl2[ra]] | SPL[3][pl3[ra]];
            uint32_t bot = SPL[0][pl0[rb]] | SPL[1][pl1[rb]] | SPL[2][pl2[rb]] | SPL[3][pl3[rb]];
            for (int k = 0; k < 4; k++) {
                uint32_t s = P[(top >> (8 * k)) & 0xFF] + P[(bot >> (8 * k)) & 0xFF];
                *o++ = (uint16_t)((((s >> 25) & 31) << 11) | (((s >> 14) & 63) << 5) | ((s >> 5) & 31));
            }
        }
    }
}

#define MASK565 0x07E0F81Fu
#define EXP565(p) ((((uint32_t)(p)) | (((uint32_t)(p)) << 16)) & MASK565)
static uint16_t X0[400], X1[400]; static uint8_t XW[400];
static uint16_t Y0[240], Y1[240]; static uint8_t YW[240];
static int zoom_tables_ready;

static void init_zoom_tables(void) {
    for (int ox = 0; ox < 400; ox++) {
        float s = ((float)ox + 0.5f) * ((float)MID_W / 400.f) - 0.5f; if (s < 0) s = 0; if (s > MID_W - 1) s = MID_W - 1;
        int i = (int)s; XW[ox] = (uint8_t)((s - i) * 32.f + 0.5f); X0[ox] = (uint16_t)i; X1[ox] = (uint16_t)(i + 1 < MID_W ? i + 1 : i);
    }
    for (int oy = 0; oy < 240; oy++) {
        float s = ((float)oy + 0.5f) * ((float)MID_H / 240.f) - 0.5f; if (s < 0) s = 0; if (s > MID_H - 1) s = MID_H - 1;
        int i = (int)s; YW[oy] = (uint8_t)((s - i) * 32.f + 0.5f); Y0[oy] = (uint16_t)i; Y1[oy] = (uint16_t)(i + 1 < MID_H ? i + 1 : i);
    }
    zoom_tables_ready = 1;
}

void vgascale_render_zoom(Machine *m, uint16_t *fb) {
    if (!zoom_tables_ready) init_zoom_tables();
    stage1(m, mid, 40, MID_H);
    for (int oy = 0; oy < 240; oy++) {
        const uint16_t *r0 = mid + Y0[oy] * MID_W, *r1 = mid + Y1[oy] * MID_W;
        uint32_t wy = YW[oy];
        uint16_t *o = fb + (239 - oy);
        for (int ox = 0; ox < 400; ox++, o += 240) {
            uint32_t wx = XW[ox], x0 = X0[ox], x1 = X1[ox];
            uint32_t t = (EXP565(r0[x0]) * (32 - wx) + EXP565(r0[x1]) * wx) >> 5;
            uint32_t b = (EXP565(r1[x0]) * (32 - wx) + EXP565(r1[x1]) * wx) >> 5;
            uint32_t s = (((t & MASK565) * (32 - wy) + (b & MASK565) * wy) >> 5) & MASK565;
            *o = (uint16_t)(s | (s >> 16));
        }
    }
}

/* ------------------------------------------------------------------------------------------------
 * Picture-only view (the message window is shown on the bottom screen instead).
 *   PIC_FILL    : crops the ornamental frame and a little of the sides, then scales the rest over the WHOLE top screen
 *                 (direct bilinear from the full-resolution picture, so it stays sharp; a slight vertical stretch).
 *   PIC_STRETCH : the whole picture incl. its frame stretched over the whole top screen.
 *   PIC_FIT     : the whole picture at its own aspect ratio with black bars above and below. */
#define PIC_ROW0 40
#define PIC_ROWS 296                         /* 148 rows after the 2x2 filter */

int vgascale_window_up(Machine *m) {
    VGA *v = &m->vga;
    if (v->bios_mode != 0x12) return 0;
    int pitch = v->crtc[0x13] * 2; if (!pitch) pitch = 80;
    uint32_t start = (v->crtc[0xC] << 8) | v->crtc[0xD];
    const uint8_t *pl[4] = { v->plane[0], v->plane[1], v->plane[2], v->plane[3] };
    uint32_t fr = start + 354u * pitch, bl = start + 350u * pitch;     /* inside the frame's top border / just above it */
    uint8_t ref[4];
    for (int p = 0; p < 4; p++) { ref[p] = pl[p][(fr + 2) & 0xFFFF]; if (ref[p] != 0x00 && ref[p] != 0xFF) return 0; }
    if (!(ref[0] | ref[1] | ref[2] | ref[3])) return 0;                 /* the border is not black */
    for (int bx = 2; bx < 78; bx++)
        for (int p = 0; p < 4; p++)
            if (pl[p][(fr + bx) & 0xFFFF] != ref[p] || pl[p][(bl + bx) & 0xFFFF] != 0) return 0;
    return 1;
}

#define FILL_X0 64                           /* inside the picture's frame: x 64..575, rows 57..313 of the 640x480 screen */
#define FILL_W  512
#define FILL_Y0 57
#define FILL_H  257
static uint16_t fillsrc[FILL_W * FILL_H];

static void render_fill(Machine *m, uint16_t *fb) {
    if (!tables_ready) init_tables();
    VGA *v = &m->vga;
    uint8_t pal[16][3]; uint16_t c565[16];
    vga_palette16(m, pal);
    for (int i = 0; i < 16; i++) c565[i] = (uint16_t)(((pal[i][0] >> 3) << 11) | ((pal[i][1] >> 2) << 5) | (pal[i][2] >> 3));
    int pitch = v->crtc[0x13] * 2; if (!pitch) pitch = 80;
    uint32_t start = (v->crtc[0xC] << 8) | v->crtc[0xD];
    const uint8_t *pl0 = v->plane[0], *pl1 = v->plane[1], *pl2 = v->plane[2], *pl3 = v->plane[3];
    for (int y = 0; y < FILL_H; y++) {
        uint32_t row = start + (uint32_t)(FILL_Y0 + y) * pitch + FILL_X0 / 8;
        uint16_t *o = fillsrc + y * FILL_W;
        for (int bx = 0; bx < FILL_W / 8; bx++) {
            uint32_t r = (row + bx) & 0xFFFF;
            uint32_t q = SPL[0][pl0[r]] | SPL[1][pl1[r]] | SPL[2][pl2[r]] | SPL[3][pl3[r]];
            for (int k = 0; k < 4; k++) { uint32_t b = (q >> (8 * k)) & 0xFF; *o++ = c565[b >> 4]; *o++ = c565[b & 15]; }
        }
    }
    static uint16_t fx0[400], fx1[400], fy0[240], fy1[240]; static uint8_t fwx[400], fwy[240]; static int ready;
    if (!ready) {
        for (int ox = 0; ox < 400; ox++) {
            float s = ((float)ox + 0.5f) * ((float)FILL_W / 400.f) - 0.5f; if (s < 0) s = 0; if (s > FILL_W - 1) s = FILL_W - 1;
            int i = (int)s; fwx[ox] = (uint8_t)((s - i) * 32.f + 0.5f); fx0[ox] = (uint16_t)i; fx1[ox] = (uint16_t)(i + 1 < FILL_W ? i + 1 : i);
        }
        for (int oy = 0; oy < 240; oy++) {
            float s = ((float)oy + 0.5f) * ((float)FILL_H / 240.f) - 0.5f; if (s < 0) s = 0; if (s > FILL_H - 1) s = FILL_H - 1;
            int i = (int)s; fwy[oy] = (uint8_t)((s - i) * 32.f + 0.5f); fy0[oy] = (uint16_t)i; fy1[oy] = (uint16_t)(i + 1 < FILL_H ? i + 1 : i);
        }
        ready = 1;
    }
    for (int oy = 0; oy < 240; oy++) {
        const uint16_t *r0 = fillsrc + fy0[oy] * FILL_W, *r1 = fillsrc + fy1[oy] * FILL_W;
        uint32_t wy = fwy[oy];
        uint16_t *o = fb + (239 - oy);
        for (int ox = 0; ox < 400; ox++, o += 240) {
            uint32_t wx = fwx[ox], x0 = fx0[ox], x1 = fx1[ox];
            uint32_t t = (EXP565(r0[x0]) * (32 - wx) + EXP565(r0[x1]) * wx) >> 5;
            uint32_t b = (EXP565(r1[x0]) * (32 - wx) + EXP565(r1[x1]) * wx) >> 5;
            uint32_t s = (((t & MASK565) * (32 - wy) + (b & MASK565) * wy) >> 5) & MASK565;
            *o = (uint16_t)(s | (s >> 16));
        }
    }
}

void vgascale_render_picture(Machine *m, uint16_t *fb, int mode) {
    if (mode == PIC_FILL) { render_fill(m, fb); return; }
    if (!zoom_tables_ready) init_zoom_tables();
    stage1(m, mid, PIC_ROW0, PIC_ROWS / 2);
    const int mh = PIC_ROWS / 2, ph = mode == PIC_STRETCH ? 240 : 185, y0 = (240 - ph) / 2;
    for (int oy = 0; oy < 240; oy++) {
        uint16_t *o = fb + (239 - oy);
        if (oy < y0 || oy >= y0 + ph) { for (int ox = 0; ox < 400; ox++, o += 240) *o = 0; continue; }
        float sy = ((float)(oy - y0) + 0.5f) * ((float)mh / ph) - 0.5f; if (sy < 0) sy = 0; if (sy > mh - 1) sy = (float)(mh - 1);
        int i = (int)sy; uint32_t wy = (uint32_t)((sy - i) * 32.f + 0.5f);
        const uint16_t *r0 = mid + i * MID_W, *r1 = mid + (i + 1 < mh ? i + 1 : i) * MID_W;
        for (int ox = 0; ox < 400; ox++, o += 240) {
            uint32_t wx = XW[ox], x0 = X0[ox], x1 = X1[ox];
            uint32_t t = (EXP565(r0[x0]) * (32 - wx) + EXP565(r0[x1]) * wx) >> 5;
            uint32_t b = (EXP565(r1[x0]) * (32 - wx) + EXP565(r1[x1]) * wx) >> 5;
            uint32_t s = (((t & MASK565) * (32 - wy) + (b & MASK565) * wy) >> 5) & MASK565;
            *o = (uint16_t)(s | (s >> 16));
        }
    }
}
