/* headless test driver: auto-clicks, dumps frames as PPM */
#include "../../src/kai.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
static const char *g_dir = "."; static long g_frame, g_limit = 3000, g_dump = 0, g_every = 30;
bool plat_read_at(const char *name, uint32_t off, void *dst, uint32_t len) {
    char p[512]; snprintf(p, sizeof p, "%s/%s", g_dir, name); FILE *f = fopen(p, "rb"); if (!f) return false;
    fseek(f, off, SEEK_SET); bool ok = fread(dst, 1, len, f) == len; fclose(f); return ok; }
uint32_t plat_file_size(const char *name) { char p[512]; snprintf(p, sizeof p, "%s/%s", g_dir, name); FILE *f = fopen(p, "rb"); if (!f) return 0; fseek(f, 0, SEEK_END); long n = ftell(f); fclose(f); return n; }
void plat_log(const char *fmt, ...) { va_list ap; va_start(ap, fmt); printf("[%ld] ", g_frame); vprintf(fmt, ap); va_end(ap); putchar('\n'); }
bool plat_frame(const uint8_t *rgb, int w, int h) {
    g_frame++;
    if (g_dump && g_frame % g_dump == 0) { char n[256]; snprintf(n, sizeof n, "%s/f_%05ld.ppm", getenv("K_FRDIR") ? getenv("K_FRDIR") : "/tmp", g_frame);
        FILE *f = fopen(n, "wb"); if (f) { fprintf(f, "P6 %d %d 255\n", w, h); fwrite(rgb, 1, w * h * 3, f); fclose(f); } }
    return g_frame < g_limit; }
void plat_poll(void) {}
bool plat_key(int key) { return key == 0 && g_frame % g_every == g_every - 1; }
void plat_cursor(int *x, int *y, bool set) { (void)set; *x = 320; *y = 240; }
uint32_t plat_ms(void) { return (uint32_t)(g_frame * 1000 / 60); }
bool plat_save_read(const char *n, void *b, size_t l) { char p[256]; snprintf(p, sizeof p, "/tmp/kw/sav/%s", n); FILE *f = fopen(p, "rb"); if (!f) return false; bool ok = fread(b, 1, l, f) == l; fclose(f); return ok; }
bool plat_save_write(const char *n, const void *b, size_t l) { char p[256]; snprintf(p, sizeof p, "/tmp/kw/sav/%s", n); FILE *f = fopen(p, "wb"); if (!f) return false; fwrite(b, 1, l, f); fclose(f); return true; }
static long snd_until[SND_SLOTS];
void plat_snd_play(int slot, void *data, size_t len, int vol, bool loop) { if (getenv("K_SND")) plat_log("snd %d len %zu %.4s loop %d", slot, len, (char *)data, loop); (void)vol; free(data); snd_until[slot] = g_frame + 90; }
void plat_snd_stop(int slot) { snd_until[slot] = 0; }
void plat_snd_fade(int slot, int ms) { (void)ms; snd_until[slot] = 0; }
bool plat_snd_playing(int slot) { return g_frame < snd_until[slot]; }
int khl_choice(int n) { static unsigned x; if (!x) x = getenv("K_SEED") ? (unsigned)atoi(getenv("K_SEED")) * 2654435761u + 1 : 1; x = x * 1103515245u + 12345u; return (int)((x >> 16) % (unsigned)n); }
int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc > 1) g_dir = argv[1]; if (argc > 3) g_limit = atol(argv[3]); if (argc > 4) g_dump = atol(argv[4]); if (argc > 5) g_every = atol(argv[5]);
    if (!k_init()) { puts("init failed"); return 1; }
    k_run(argc > 2 ? argv[2] : "startup.mes"); printf("stopped at frame %ld\n", g_frame); return 0;
}
