#include "../core/machine.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
extern void opl_write(OPL*, uint16_t, uint8_t);
static uint64_t ic;
static uint64_t clk(void *x) { (void)x; return ic * 1000ull; }   /* ic counts microseconds */
static void w(OPL *o, int reg, int v) { opl_write(o, 0x388, reg); opl_write(o, 0x389, v); }
static void w2(OPL *o, int reg, int v) { opl_write(o, 0x38A, reg); opl_write(o, 0x38B, v); }
static double analyse(OPL *o, int n, double *peak) {
    static int16_t buf[96000 * 2]; opl_mix(o, buf, n);
    int zc = 0; double pk = 0; int prev = 0;
    for (int i = 100; i < n; i++) { int v = buf[i * 2]; if (prev < 0 && v >= 0) zc++; prev = v; if (abs(v) > pk) pk = abs(v); }
    *peak = pk; return zc * 22050.0 / (n - 100);
}
int main(void) {
    OPL *o = opl_new(22050); opl_attach(o, clk, 0);
    /* simple 2-op FM patch, channel 0: A4 (440Hz): fnum = 440*2^(20-4)/49716 = 580 block 4 */
    w(o, 0x01, 0x20);
    w(o, 0x20, 0x21); w(o, 0x23, 0x21);       /* mult 1 both, sustained off */
    w(o, 0x40, 0x3F); w(o, 0x43, 0x00);       /* modulator silent (TL=63), carrier loud */
    w(o, 0x60, 0xF0); w(o, 0x63, 0xF0);       /* instant attack */
    w(o, 0x80, 0x0F); w(o, 0x83, 0x0F);
    w(o, 0xA0, 580 & 0xFF); w(o, 0xB0, 0x20 | (4 << 2) | (580 >> 8));
    ic += 1000000;                             /* 1 second virtual */
    opl_sync(o);
    double pk; double f = analyse(o, 16000, &pk);
    printf("sine test: freq ~ %.1f Hz (expect 440), peak %.0f\n", f, pk);
    /* key off and check decay to silence */
    w(o, 0xB0, (4 << 2) | (580 >> 8));
    ic += 2000000; opl_sync(o);
    static int16_t b2[44100 * 2]; opl_mix(o, b2, 44100);
    int pk2 = 0; for (int i = 30000; i < 44100; i++) if (abs(b2[i * 2]) > pk2) pk2 = abs(b2[i * 2]);
    printf("after release, tail peak %d (expect ~0)\n", pk2);
    /* OPL3 4-op sanity: enable OPL3, 4op pair 0/3, serial algorithm */
    OPL *p = opl_new(22050); opl_attach(p, clk, 0); ic = 0;
    w2(p, 0x05, 1); w2(p, 0x04, 1);
    int base[4] = {0x20, 0x23, 0x28, 0x2B};
    for (int i = 0; i < 4; i++) { w(p, base[i], 0x21); w(p, base[i] + 0x20, i == 3 ? 0 : 0x20); w(p, base[i] + 0x40, 0xF0); w(p, base[i] + 0x60, 0x0F); }
    w(p, 0xA0, 580 & 0xFF); w(p, 0xB0, 0x20 | (4 << 2) | (580 >> 8));
    ic += 1000000; opl_sync(p); f = analyse(p, 16000, &pk);
    printf("4op serial: freq ~ %.1f Hz, peak %.0f\n", f, pk);
    return 0;
}
