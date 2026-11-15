#include "../core/machine.h"
#include "../ctr/source/vgascale.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
int main(int argc, char **argv) {
    Machine *m = machine_new(argv[1]); idle_init(m);
    m->opl = opl_new(22050); opl_attach(m->opl, (opl_clock_fn)machine_vtime_ns, m);
    dos_exec(m, "MUSICV.COM", ""); for (int i = 0; i < 400 && !m->exited; i++) machine_run(m, 100000);
    dos_exec(m, "SILV1.COM", "main.mes D D");
    for (int i = 0; i < atoi(argv[2]) * 10; i++) { machine_run(m, 100000); opl_sync(m->opl); uint32_t n = opl_available(m->opl); static int16_t b[8192]; while (n) { int c = n > 4096 ? 4096 : n; opl_mix(m->opl, b, c); n -= c; } }
    static uint16_t fb[400 * 240];
    struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
    for (int i = 0; i < 200; i++) vgascale_render_zoom(m, fb);
    clock_gettime(CLOCK_MONOTONIC, &b);
    printf("zoom render: %.3f ms/frame on host\n", ((b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6) / 200);
    FILE *f = fopen("/tmp/zoom.ppm", "wb"); fprintf(f, "P6\n400 240\n255\n");
    for (int y = 0; y < 240; y++) for (int x = 0; x < 400; x++) { uint16_t p = fb[x * 240 + (239 - y)]; uint8_t c[3] = {(p >> 11) << 3, ((p >> 5) & 63) << 2, (p & 31) << 3}; fwrite(c, 1, 3, f); }
    fclose(f);
    return 0;
}
