/* Headless test runner: loads game dir, runs MUSICV + SILV1, dumps frames. */
#include "../core/machine.h"
#include "../core/textcap.h"
#include <stdlib.h>
#include <string.h>

extern void vga_palette16(Machine *m, uint8_t rgb[16][3]);
extern void vga_decode(Machine *m, uint8_t *dst, int w, int h);

static void dump_ppm(Machine *m, const char *path) {
    static uint8_t idx[640 * 480]; uint8_t pal[16][3];
    vga_decode(m, idx, 640, 480); vga_palette16(m, pal);
    FILE *f = fopen(path, "wb"); fprintf(f, "P6\n640 480\n255\n");
    for (int i = 0; i < 640 * 480; i++) fwrite(pal[idx[i]], 1, 3, f);
    fclose(f);
}

typedef struct { long at; char kind; int a, b; } Ev;
static Ev evs[256]; static int nev;
/* SCRIPT="30:k:1c;50:m:320,400;..." (time in millions of instr; k=scancode press+release, m=mouse click, d=dump frame to /tmp/ev_N.ppm) */
static void parse_script(const char *sc) {
    char *dup = strdup(sc); char *sv = NULL;
    for (char *t = strtok_r(dup, ";", &sv); t && nev < 250; t = strtok_r(NULL, ";", &sv)) {
        long at; char kind; int a = 0, b = 0;
        if (sscanf(t, "%ld:%c:%x,%d", &at, &kind, &a, &b) >= 2 || sscanf(t, "%ld:%c", &at, &kind) >= 2) {
            if (kind == 'm') sscanf(t, "%ld:m:%d,%d", &at, &a, &b);
            evs[nev++] = (Ev){at * 1000000L, kind, a, b};
            if (kind == 'k') evs[nev++] = (Ev){at * 1000000L + 600000, 'K', a, 0};
            if (kind == 'm') evs[nev++] = (Ev){at * 1000000L + 600000, 'M', a, b};
        }
    }
    free(dup);
}

#include <stdint.h>
static struct { uint16_t cs, ip; long n; uint32_t omin, omax; } wh[512]; static int nwh; static long wh_total;
static void wr_dbg(Machine *m, uint32_t off) {
    long lo = (getenv("WLO") ? atol(getenv("WLO")) : 400) * 80, hi = (getenv("WHI") ? atol(getenv("WHI")) : 470) * 80;
    if (off < lo || off >= hi) return;
    uint16_t cs = m->cpu.seg[CS], ip = m->cpu.ip; wh_total++;
    for (int i = 0; i < nwh; i++) if (wh[i].cs == cs && (uint16_t)(wh[i].ip - ip) < 0x40) { wh[i].n++; if (off < wh[i].omin) wh[i].omin = off; if (off > wh[i].omax) wh[i].omax = off; return; }
    if (nwh < 512) { wh[nwh].cs = cs; wh[nwh].ip = ip; wh[nwh].n = 1; wh[nwh].omin = wh[nwh].omax = off; nwh++; }
}

static FILE *glog;
static int16_t *wavbuf; static long wavn, wavcap;
static void drain_audio(Machine *m) {
    opl_sync(m->opl);
    uint32_t n = opl_available(m->opl);
    if (!wavbuf) return;
    while (n) { int c = n > 4096 ? 4096 : n; if (wavn + c > wavcap) return; opl_mix(m->opl, wavbuf + wavn * 2, c); wavn += c; n -= c; }
}
static int glyph_cb(CPU *c, int idx) {
    (void)idx; Machine *m = c->m;
    uint16_t di = c->r[DI], cx = c->r[CX], dx = c->r[DX], si = c->r[SI];
    uint8_t col = mem_rb(m, LIN(c->seg[DS], 0xE27C));
    if (glog) {
        uint16_t pg = mem_rw(m, LIN(c->seg[DS], 0xE262));
        uint16_t tb = pg == 0 ? 0xE2D0 : pg == 1 ? 0xE2E0 : pg == 2 ? 0xE2F0 : 0xE300;
        fprintf(glog, "G x=%d y=%d cx=%d dx=%d col=%02X ds=%04X si=%04X icount=%llu page=%u seg=%04X,%04X,%04X,%04X\n", (di % 80) * 8, di / 80, cx, dx, col, c->seg[DS], si, (unsigned long long)m->icount, pg,
            mem_rw(m, LIN(c->seg[DS], tb)), mem_rw(m, LIN(c->seg[DS], tb + 4)), mem_rw(m, LIN(c->seg[DS], tb + 8)), mem_rw(m, LIN(c->seg[DS], tb + 12)));
        if (cx <= 4 && dx <= 32) for (int r = 0; r < dx; r++) { for (int b = 0; b < cx; b++) { uint8_t v = mem_rb(m, LIN(c->seg[DS], si + r * cx + b)); for (int k = 7; k >= 0; k--) fputc((v >> k) & 1 ? '#' : '.', glog); } fputc('\n', glog); }
    }
    return 0;
}

extern int io_trace;
extern long opl_dbg_writes; extern int opl_dbg_log;
int main(int argc, char **argv) {
    if (getenv("IOTRACE")) io_trace = 1;
    if (getenv("OPLLOG")) opl_dbg_log = atoi(getenv("OPLLOG"));
    if (argc < 2) { fprintf(stderr, "usage: run gamedir [instr_millions] [outprefix]\n"); return 1; }
    long maxm = argc > 2 ? atol(argv[2]) : 50;
    const char *outp = argc > 3 ? argv[3] : "/tmp/frame";
    Machine *m = machine_new(argv[1]);
    m->verbose = getenv("NV") ? atoi(getenv("NV")) : 0;
    m->opl = opl_new(22050); opl_attach(m->opl, (opl_clock_fn)machine_vtime_ns, m);
    textcap_init(m);
    if (dos_exec(m, "MUSICV.COM", "") < 0) return 2;
    long total = 0; int guard = 0;
    while (!m->exited && total < 20000000) { total += machine_run(m, 100000); if (++guard > 400) break; }
    fprintf(stderr, "[host] MUSICV exited=%d code=%d tsr=%d after %ld instr\n", m->exited, m->exit_code, m->tsr_kept, total);
    if (dos_exec(m, "SILV1.COM", "main.mes D D") < 0) return 3;
    if (getenv("SCRIPT")) parse_script(getenv("SCRIPT"));
    if (getenv("WATCH")) m->vga_wr_dbg = wr_dbg;
    int bp_set = 0; if (getenv("GLOG")) glog = fopen(getenv("GLOG"), "w");
    if (getenv("WAV")) { wavcap = 22050L * 120; wavbuf = malloc(wavcap * 4); }
    long dump_every = getenv("DUMPEVERY") ? atol(getenv("DUMPEVERY")) * 1000000L : 5000000L;
    long done = 0, next_dump = 0; int n = 0, nd = 0, nb = 0;
    while (!m->exited && done < maxm * 1000000L) {
        for (int i = 0; i < nev; i++) if (evs[i].at >= 0 && done >= evs[i].at) {
            Ev *e = &evs[i];
            if (e->kind == 'k') machine_key(m, (uint8_t)e->a, 1, 0);
            else if (e->kind == 'K') machine_key(m, (uint8_t)e->a, 0, 0);
            else if (e->kind == 'm') { m->mouse_x = e->a; m->mouse_y = e->b; m->mouse_btn = 1; }
            else if (e->kind == 'M') { m->mouse_btn = 0; }
            else if (e->kind == 'b') { static uint8_t buf[320 * 240 * 3]; char p[256]; snprintf(p, sizeof p, "/tmp/bot_%d.ppm", nb++); int g = textcap_render(m, buf, 320, 240); FILE *f = fopen(p, "wb"); fprintf(f, "P6\n320 240\n255\n"); fwrite(buf, 1, sizeof buf, f); fclose(f); fprintf(stderr, "[host] bottom render %s glyphs=%d active=%d\n", p, g, textcap_active(m)); }
            else if (e->kind == 'd') { char p[256]; snprintf(p, sizeof p, "/tmp/ev_%d.ppm", nd++); dump_ppm(m, p); }
            e->at = -1;
        }
        if (!bp_set && glog && m->cur_psp != 0 && m->cur_psp != m->cpu.seg[CS] - 0 && m->exit_psp != m->cur_psp) { }
        if (!bp_set && glog && done > 60000000L) { m->cpu.bp[0] = LIN((m->cur_psp + 0x10), 0x200A); m->cpu.nbp = 1; m->cpu.bp_cb = glyph_cb; bp_set = 1; fprintf(stderr, "[host] bp at %04X:200A\n", m->cur_psp + 0x10); }
        done += machine_run(m, 200000); drain_audio(m);
        if (done >= next_dump) { char p[256]; snprintf(p, sizeof p, "%s_%03d.ppm", outp, n++); dump_ppm(m, p); next_dump += dump_every; }
    }
    fprintf(stderr, "[host] SILV1 exited=%d code=%d after %ld instr  CS:IP=%04X:%04X mode=%02X unhandled=%u\n", m->exited, m->exit_code, done, m->cpu.seg[CS], m->cpu.ip, m->vga.bios_mode, m->stat_unhandled);
    dump_ppm(m, "/tmp/final.ppm");
    if (getenv("WATCH")) { fprintf(stderr, "[watch] total writes in text rows: %ld\n", wh_total); for (int i = 0; i < nwh; i++) fprintf(stderr, "[watch] %04X:%04X n=%ld off=%05X..%05X\n", wh[i].cs, wh[i].ip, wh[i].n, wh[i].omin, wh[i].omax); }
    fprintf(stderr, "[host] psp=%04X opl_writes=%ld\n", m->exit_psp, opl_dbg_writes);
    if (getenv("WAV") && wavbuf) { FILE *f = fopen(getenv("WAV"), "wb"); uint32_t dl = (uint32_t)wavn * 4, v32; uint16_t v16;
        fwrite("RIFF", 1, 4, f); v32 = 36 + dl; fwrite(&v32, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); v32 = 16; fwrite(&v32, 4, 1, f);
        v16 = 1; fwrite(&v16, 2, 1, f); v16 = 2; fwrite(&v16, 2, 1, f); v32 = 22050; fwrite(&v32, 4, 1, f); v32 = 22050 * 4; fwrite(&v32, 4, 1, f);
        v16 = 4; fwrite(&v16, 2, 1, f); v16 = 16; fwrite(&v16, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&dl, 4, 1, f); fwrite(wavbuf, 4, wavn, f); fclose(f); }
    if (getenv("DUMPMEM")) { FILE *f = fopen(getenv("DUMPMEM"), "wb"); fwrite(m->mem, 1, 0x100000, f); fclose(f); }
    return 0;
}
