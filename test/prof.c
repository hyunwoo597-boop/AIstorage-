/* sampling profiler: where does the emulated CPU spend its instructions? */
#include "../core/machine.h"
#include "../core/textcap.h"
#include <stdlib.h>
#include <string.h>
typedef struct { uint32_t lin; long n; } B;
static B tab[4096];
int main(int argc, char **argv) {
    Machine *m = machine_new(argv[1]);
    m->opl = opl_new(22050); opl_attach(m->opl, (opl_clock_fn)machine_vtime_ns, m);
    textcap_init(m); if (!getenv("NOIDLE")) idle_init(m);
    dos_exec(m, "MUSICV.COM", ""); for (int i = 0; i < 400 && !m->exited; i++) machine_run(m, 100000);
    dos_exec(m, "SILV1.COM", "main.mes D D");
    long total = atol(argv[2]) * 1000000L, done = 0, samples = 0; long nextkey = 0; long maxkeys = getenv("KEYS") ? atol(getenv("KEYS")) : 1000;
    while (!m->exited && done < total) {
        if (done >= nextkey && nextkey < maxkeys * 15000000L) { machine_key(m, 0x1c, 1, 13); done += machine_run(m, 400000); machine_key(m, 0x1c, 0, 13); nextkey += 15000000L; }
        done += machine_run(m, 997);
        opl_sync(m->opl); uint32_t n = opl_available(m->opl); static int16_t b[8192]; while (n) { int c = n > 4096 ? 4096 : n; opl_mix(m->opl, b, c); n -= c; }
        uint32_t lin = (m->cpu.seg[CS] << 4) + m->cpu.ip; lin &= ~0x1F; samples++;
        if (done > total / 2) { uint32_t h = (lin * 2654435761u) >> 20; for (int k = 0; k < 4096; k++) { B *e = &tab[(h + k) & 4095]; if (!e->lin) { e->lin = lin | 1; e->n = 1; break; } if ((e->lin & ~1u) == lin) { e->n++; break; } } }
    }
    for (int rank = 0; rank < 14; rank++) { int bi = -1; long bn = 0; for (int i = 0; i < 4096; i++) if (tab[i].lin && tab[i].n > bn) { bn = tab[i].n; bi = i; } if (bi < 0) break;
        printf("%05X  %5.1f%%\n", tab[bi].lin & ~1u, 100.0 * bn / (samples / 2)); tab[bi].n = 0; }
    { FILE *f = fopen("/tmp/mem.bin","wb"); fwrite(m->mem,1,0x100000,f); fclose(f); }
    printf("text_on=%d poll_streak=%d\n", m->text_on, m->poll_streak);
    printf("skipped=%.1f%% of virtual instructions\n", 100.0 * m->skipped_total / (double)m->icount);
    printf("psp=%04X cs=%04X ip=%04X\n", m->cur_psp, m->cpu.seg[CS], m->cpu.ip);
    return 0;
}
