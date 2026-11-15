/* OPL synth CPU cost: ./opl_bench [output_rate] [active_channels]   (gcc -O2 test/opl_bench.c core/*.c -lm) */
#include "../core/machine.h"
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
static uint64_t ic;
static uint64_t clk(void *x) { (void)x; return ic * 1000ull; }
static void w(OPL *o, int reg, int v) { opl_write(o, 0x388, reg); opl_write(o, 0x389, v); }
int main(int argc, char **argv) {
    int rate = argc > 1 ? atoi(argv[1]) : 22050, nact = argc > 2 ? atoi(argv[2]) : 9;
    OPL *o = opl_new(rate); opl_attach(o, clk, 0);
    w(o, 0x01, 0x20);
    static const int s0[9] = {0,1,2,8,9,10,16,17,18};
    for (int c = 0; c < 9; c++) {
        w(o, 0x20 + s0[c], 0x21); w(o, 0x23 + s0[c], 0x22);
        w(o, 0x40 + s0[c], 0x20); w(o, 0x43 + s0[c], 0x00);
        w(o, 0x60 + s0[c], 0xF2); w(o, 0x63 + s0[c], 0xB2);
        w(o, 0x80 + s0[c], 0x24); w(o, 0x83 + s0[c], 0x24);
        w(o, 0xC0 + c, 0x0E);
        w(o, 0xA0 + c, 300 + c * 20 & 0xFF); w(o, 0xB0 + c, (c < nact ? 0x20 : 0) | (4 << 2) | 1);
    }
    static int16_t buf[4096 * 2];
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    int secs = 20; long n = 0;
    for (int s = 0; s < secs * 100; s++) {
        ic += 10000; opl_sync(o);
        if (s % 7 == 0) { w(o, 0xA0 + (s % (nact ? nact : 1)), 300 + (s % 13) * 9 & 0xFF); }
        while (opl_available(o) >= 1024) { opl_mix(o, buf, 1024); n += 1024; }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double dt = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    printf("rate %d active %d: %.1f ns/output sample, %.2f ms per audio second\n", rate, nact, dt / n * 1e9, dt / secs * 1000);
    return 0;
}
