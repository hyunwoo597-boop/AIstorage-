# Builds a PC test program from the 3D code in platform/3ds/plat_3ds.c (no 3DS needed):
#   python3 tools/hosttest/mk_warp_test.py > /tmp/wt.c && gcc -O2 -w -o /tmp/wt /tmp/wt.c -lm
#   /tmp/wt frame.ppm depth.pgm out     -> out_L.ppm / out_R.ppm (both eyes at full slider), out_D.pgm (depth map)
# frame.ppm / depth.pgm come from the headless build: AI5_DEPTH=1 ./ai5_headless DATA_DIR MAIN.MES <frames> <dump_every> 15
# (writes /tmp/fr_NNNNN.ppm = picture, /tmp/dp_NNNNN.pgm = 3D layer plane; see platform/headless/plat_headless.c)
import sys, os
src = open(os.path.join(os.path.dirname(__file__), '..', '..', 'platform', '3ds', 'plat_3ds.c')).read()
body = src[src.index("static uint8_t g_dm[120][160]"):src.index("static void draw_scene(")]
print('''#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8;
static const uint8_t *g_fade; static int g_fade_a; static uint8_t *g_tplate; static const uint8_t *sys_scene_plate(void) { return g_tplate; }
static inline void avg2(const uint8_t *r0, const uint8_t *r1, int sx, int *r, int *g, int *b) { const uint8_t *a = r0 + sx * 6, *c = r1 + sx * 6;
    *r = (a[0] + a[3] + c[0] + c[3]) >> 2; *g = (a[1] + a[4] + c[1] + c[4]) >> 2; *b = (a[2] + a[5] + c[2] + c[5]) >> 2; }
''' + body + r'''
int main(int c, char **v) {
    if (c < 4) { fprintf(stderr, "usage: wt frame.ppm depth.pgm outprefix [back front [plate.ppm]]\n"); return 1; }
    static uint8_t rgb[640 * 480 * 3], z[640 * 480]; char h[64]; FILE *f = fopen(v[1], "rb");
    if (!f || !fgets(h, 64, f) || !fread(rgb, 1, sizeof rgb, f)) return 1; fclose(f);
    f = fopen(v[2], "rb"); if (!f || !fgets(h, 64, f) || !fread(z, 1, sizeof z, f)) return 1; fclose(f);
    float back = c > 4 ? atof(v[4]) : 2.5f, front = c > 5 ? atof(v[5]) : 4.5f;
    g_hi = false;
    if (full_picture(z) && ai_picture(z)) { build_dm_hi(z); soften_steps(); g_hi = true; }
    else { build_dm_layers(rgb, z); if (g_lo_chars) { build_dm_hi_layers(z); soften_steps(); g_hi = true; } }
    depth_stats(); build_plate_layers(z); g_zp = g_lo + (int)((g_sub - g_lo) * 0.30f);      /* as draw_scene at full slider */
    if (c > 6) { static uint8_t pl[640 * 480 * 3]; FILE *pf = fopen(v[6], "rb"); if (pf && fgets(h, 64, pf) && fread(pl, 1, sizeof pl, pf)) g_tplate = pl; if (pf) fclose(pf); }
    make_half(rgb, z, true); make_plate();
    static u8 fb[400 * 240 * 3]; static uint8_t o[240 * 400 * 3]; char n[256];
    for (int e = -1; e <= 1; e += 2) { memset(fb, 0, sizeof fb); draw_eye_dm(fb, back, front, e);
        for (int x = 0; x < 400; x++) for (int y = 0; y < 240; y++) { u8 *p = fb + (x * 240 + 239 - y) * 3; uint8_t *q = o + (y * 400 + x) * 3; q[0] = p[2]; q[1] = p[1]; q[2] = p[0]; }
        snprintf(n, sizeof n, "%s_%c.ppm", v[3], e < 0 ? 'R' : 'L'); f = fopen(n, "wb"); fprintf(f, "P6 400 240 255\n"); fwrite(o, 1, sizeof o, f); fclose(f); }
    snprintf(n, sizeof n, "%s_D.pgm", v[3]); f = fopen(n, "wb");
    if (g_hi) { fprintf(f, "P5 320 240 255\n"); fwrite(g_dmh, 1, sizeof g_dmh, f); } else { fprintf(f, "P5 160 120 255\n"); fwrite(g_dm, 1, sizeof g_dm, f); }
    fclose(f); return 0;
}''')
