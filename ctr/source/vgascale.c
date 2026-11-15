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
