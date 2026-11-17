#include "ai5.h"
#include <stdio.h>
#include <dirent.h>
#include <time.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
static const char *g_dir = ".";
void plat_set_dir(const char *d) { g_dir = d; }
static FILE *op(const char *name) { char p[512]; snprintf(p, sizeof p, "%s/%s", g_dir, name); return fopen(p, "rb"); }
bool plat_read_at(const char *name, uint32_t off, void *dst, uint32_t len) {
    FILE *f = op(name); if (!f) return false; fseek(f, off, SEEK_SET); bool ok = fread(dst, 1, len, f) == len; fclose(f); return ok; }
int plat_list(const char *dir, char names[][24], int max) {   /* file names in a game-folder subdirectory */
    char p[512]; snprintf(p, sizeof p, "%s/%s", g_dir, dir); DIR *d = opendir(p); if (!d) return 0; int n = 0; struct dirent *e;
    while ((e = readdir(d)) && n < max) { if (e->d_name[0] == '.' || strlen(e->d_name) > 23) continue; strcpy(names[n++], e->d_name); }
    closedir(d); return n; }
uint32_t plat_file_size(const char *name) { FILE *f = op(name); if (!f) return 0; fseek(f, 0, SEEK_END); long n = ftell(f); fclose(f); return n; }
void *plat_read_file(const char *name, size_t *len) { uint32_t n = plat_file_size(name); if (!n) return NULL; void *b = malloc(n + 1);
    if (!plat_read_at(name, 0, b, n)) { free(b); return NULL; } if (len) *len = n; return b; }
/* headless test driver: presses "confirm" periodically, dumps selected frames */
long g_frame; static int g_limit = 3000, g_every = 40, g_dump_every = 0;
const char *plat_goto_override(void) { extern vm_t *g_vm; static int used; const char *e = getenv("AI5_GOTO"); if (!e || used) return NULL;
    const char *after = getenv("AI5_GOTO_AFTER"); if (after && strcasecmp(g_vm->mes_name, after)) return NULL; used = 1; return e; }
/* sound stub: records what would play; a played clip "lasts" 90 frames */
static long snd_until[SND_SLOTS]; static long snd_count[3];
void plat_snd_play(int slot, void *data, size_t len, int vol) {
    extern long g_frame; snd_until[slot] = g_frame + 90; snd_count[slot >= SND_VOICE ? 2 : slot >= SND_SE ? 1 : 0]++;
    if (getenv("AI5_SND")) printf("snd play slot %d len %zu vol %d magic %.4s\n", slot, len, vol, (char *)data);
    free(data); }
void plat_snd_stop(int slot) { snd_until[slot] = 0; }
void plat_snd_fade(int slot, int ms) { (void)ms; snd_until[slot] = 0; }
bool plat_snd_playing(int slot) { extern long g_frame; return g_frame < snd_until[slot]; }
void snd_report(void) { printf("sound plays: music=%ld effects=%ld voice=%ld\n", snd_count[0], snd_count[1], snd_count[2]); }
uint32_t plat_ms(void) { extern long g_frame; return (uint32_t)(g_frame * 1000 / 60); }
void plat_datetime(int o[6]) { time_t t = time(NULL); struct tm *m = localtime(&t); o[0] = m->tm_year + 1900; o[1] = m->tm_mon + 1; o[2] = m->tm_mday; o[3] = m->tm_hour; o[4] = m->tm_min; o[5] = m->tm_sec; }
bool plat_save_read(const char *name, void *buf, size_t len) {
    char p[512]; snprintf(p, sizeof p, "%s/save/%s", g_dir, name); FILE *f = fopen(p, "rb"); if (!f) return false;
    bool ok = fread(buf, 1, len, f) == len; fclose(f); return ok; }
bool plat_save_write(const char *name, const void *buf, size_t len) {
    char p[512]; snprintf(p, sizeof p, "%s/save", g_dir); mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/save/%s", g_dir, name); FILE *f = fopen(p, "wb"); if (!f) return false;
    bool ok = fwrite(buf, 1, len, f) == len; fclose(f); if (getenv("AI5_SND") || getenv("AI5_SAVE")) printf("saved %s\n", name); return ok; }
static long g_polls;
void plat_poll(void) { if (++g_polls % 20 == 0) g_frame++; }
static int g_cx = 320, g_cy = 100;
void plat_cursor(int *x, int *y, bool set) {
    const char *e = getenv("AI5_CUR");   /* "frame:x:y,..." moves the pointer once when that frame is reached */
    static long applied = -1;
    while (e && *e) { long f = strtol(e, (char **)&e, 10); if (*e != ':') break; e++; int cx = (int)strtol(e, (char **)&e, 10); if (*e != ':') break; e++;
        int cy = (int)strtol(e, (char **)&e, 10); if (g_frame >= f && f > applied) { g_cx = cx; g_cy = cy; applied = f; } if (*e == ',') e++; }
    if (set) { g_cx = *x; g_cy = *y; } else { *x = g_cx; *y = g_cy; } }
/* AI5_KEYS="frame:key,frame:key,..." holds a key for 4 frames from that frame (scripted input) */
static bool scripted(int key) {
    const char *e = getenv("AI5_KEYS"); if (!e) return false;
    while (*e) { long f = strtol(e, (char **)&e, 10); if (*e != ':') break; e++; int k = (int)strtol(e, (char **)&e, 10);
        if (k == key && g_frame >= f && g_frame < f + 4) return true; if (*e == ',') e++; }
    return false;
}
bool plat_key(int key) { if (scripted(key)) return true; if (getenv("AI5_KEYS_ONLY")) return false; if (getenv("AI5_AUTO_UNTIL") && g_frame >= atol(getenv("AI5_AUTO_UNTIL")) && !(getenv("AI5_AUTO_RESUME") && g_frame >= atol(getenv("AI5_AUTO_RESUME")))) return false; if (!g_every) return false; long ph = g_frame % g_every; return (key == 0 && ph == g_every - 1) || (key == 3 && ph == g_every / 2 && !getenv("AI5_NODOWN")); }
bool plat_frame(const uint8_t *rgb, int w, int h) {
    g_frame++;
    if (getenv("AI5_UI") && g_frame % 400 == 0) { extern vm_t *g_vm; const ui_text *u = sys_ui_text(g_vm); printf("ui@%ld: lines=%d dlg=%d menu=%d hover=%d len0=%zu\n", g_frame, u->nlines, u->dialogue_visible, u->menu_visible, u->hover, u->nlines ? strlen(u->line[0].sjis) : 0); }
    if (getenv("AI5_UIAT")) { char k[16]; snprintf(k, sizeof k, ",%ld,", g_frame); char e[512]; snprintf(e, sizeof e, ",%s,", getenv("AI5_UIAT"));
        if (strstr(e, k)) { extern vm_t *g_vm; const ui_text *u = sys_ui_text(g_vm); char n[64]; snprintf(n, sizeof n, "/tmp/ui_%05ld.txt", g_frame);
            FILE *f = fopen(n, "wb"); if (f) { fprintf(f, "loop=%d dlg=%d menu=%d hover=%d\n", u->loop, u->dialogue_visible, u->menu_visible, u->hover);
                for (int i = 0; i < u->nlines; i++) fprintf(f, "%3d|%s\n", u->line[i].item, u->line[i].sjis); fclose(f); } } }
    if (g_dump_every && g_frame % g_dump_every == 0) { char n[64]; snprintf(n, sizeof n, "/tmp/fr_%05ld.ppm", g_frame);
        FILE *f = fopen(n, "wb"); if (f) { fprintf(f, "P6 %d %d 255\n", w, h); fwrite(rgb, 1, w * h * 3, f); fclose(f); } }
    if (g_dump_every && g_frame % g_dump_every == 0 && getenv("AI5_DEPTH")) { const uint8_t *z = sys_scene_depth(); char n[64]; snprintf(n, sizeof n, "/tmp/dp_%05ld.pgm", g_frame);
        FILE *f = z ? fopen(n, "wb") : NULL; if (f) { fprintf(f, "P5 640 480 255\n"); for (int i = 0; i < 640 * 480; i++) fputc(z[i] ? 255 : 0, f); fclose(f); } }
    return g_frame < g_limit;
}
static long g_logn;
void plat_log(const char *fmt, ...) { if (++g_logn > (getenv("AI5_TRACE") ? 2000000 : 4000)) return; va_list ap; va_start(ap, fmt); if (getenv("AI5_TRACE")) printf("[%ld] ", g_frame); vprintf(fmt, ap); va_end(ap); putchar('\n'); }
vm_t *g_vm;
int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    void plat_set_dir(const char *); if (argc > 1) plat_set_dir(argv[1]);
    if (argc > 3) g_limit = atoi(argv[3]); if (argc > 4) g_dump_every = atoi(argv[4]); if (argc > 5) g_every = atoi(argv[5]);
    static vm_t vm; g_vm = &vm; if (!vm_init(&vm)) { puts("archive open failed"); return 1; }
    if (!vm_load_mes(&vm, argc > 2 ? argv[2] : "MAIN.MES")) return 1;
    vm_run(&vm); { void snd_report(void); snd_report(); } printf("stopped: steps=%llu script=%s ip=%x frames=%ld\n", (unsigned long long)vm.steps, vm.mes_name, vm.ip, g_frame); return 0;
}
