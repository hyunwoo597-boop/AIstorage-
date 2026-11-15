/* verifies vgascale_render against vga_decode + naive 2x2 box filter */
#include "../core/machine.h"
#include "../ctr/source/vgascale.h"
#include <stdlib.h>
#include <string.h>
extern void vga_palette16(Machine *m, uint8_t rgb[16][3]);
extern void vga_decode(Machine *m, uint8_t *dst, int w, int h);
int main(int argc, char **argv) {
    Machine *m = machine_new(argv[1]);
    m->opl = opl_new(22050); opl_attach(m->opl, (opl_clock_fn)machine_vtime_ns, m);
    dos_exec(m, "MUSICV.COM", ""); for (int i = 0; i < 400 && !m->exited; i++) machine_run(m, 100000);
    dos_exec(m, "SILV1.COM", "main.mes D D");
    for (int i = 0; i < atoi(argv[2]) * 10; i++) { machine_run(m, 100000); opl_sync(m->opl); uint32_t n = opl_available(m->opl); static int16_t b[8192]; while (n) { int c = n > 4096 ? 4096 : n; opl_mix(m->opl, b, c); n -= c; } }
    static uint8_t idx[640 * 480]; uint8_t pal[16][3];
    vga_decode(m, idx, 640, 480); vga_palette16(m, pal);
    static uint16_t fb[400 * 240]; memset(fb, 0, sizeof fb);
    vgascale_render(m, fb, 40);
    long bad = 0, maxd = 0;
    for (int y = 0; y < 240; y++) for (int x = 0; x < 320; x++) {
        int r = 0, g = 0, b = 0;
        for (int dy = 0; dy < 2; dy++) for (int dx = 0; dx < 2; dx++) { uint8_t *c = pal[idx[(2 * y + dy) * 640 + 2 * x + dx]]; r += c[0]; g += c[1]; b += c[2]; }
        uint16_t e = ((r >> 5) << 11) | ((g >> 4) << 5) | (b >> 5), a = fb[(40 + x) * 240 + (239 - y)];
        if (a != e) { bad++; long d = abs((a >> 11) - (e >> 11)) + abs(((a >> 5) & 63) - ((e >> 5) & 63)) + abs((a & 31) - (e & 31)); if (d > maxd) maxd = d; }
    }
    printf("mismatches=%ld maxdiff=%ld (of %d px)\n", bad, maxd, 320 * 240);
    /* write a PPM (unrotate) for eyeballing */
    FILE *f = fopen("/tmp/scaled.ppm", "wb"); fprintf(f, "P6\n400 240\n255\n");
    for (int y = 0; y < 240; y++) for (int x = 0; x < 400; x++) { uint16_t p = fb[x * 240 + (239 - y)]; uint8_t c[3] = {(p >> 11) << 3, ((p >> 5) & 63) << 2, (p & 31) << 3}; fwrite(c, 1, 3, f); }
    fclose(f);
    return 0;
}
