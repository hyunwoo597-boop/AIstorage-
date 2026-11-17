#include "ai5.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
size_t lzss_decode(const uint8_t *src, size_t n, uint8_t *dst, size_t cap) {
    uint8_t win[4096]; unsigned wp = 0xFEE; size_t o = 0, i = 0; memset(win, 0, sizeof win);
    while (i < n) {
        unsigned fl = src[i++];
        for (int k = 0; k < 8 && i < n; k++) {
            if (fl >> k & 1) { uint8_t c = src[i++]; if (o < cap) dst[o] = c; o++; win[wp] = c; wp = (wp + 1) & 0xFFF; }
            else { if (i + 1 >= n) return o; unsigned lo = src[i], hi = src[i + 1]; i += 2;
                unsigned off = lo | (hi & 0xF0) << 4, len = (hi & 0xF) + 3;
                while (len--) { uint8_t c = win[off++ & 0xFFF]; if (o < cap) dst[o] = c; o++; win[wp] = c; wp = (wp + 1) & 0xFFF; } }
        }
    }
    return o;
}
bool arc_open(arc_t *a, const char *path) {
    uint8_t h[4]; memset(a, 0, sizeof *a); strncpy(a->path, path, sizeof a->path - 1);
    if (!plat_read_at(path, 0, h, 4)) return false;
    a->n = rd32(h); if (a->n <= 0 || a->n > 20000) return false;
    uint8_t *t = malloc(a->n * 28); if (!plat_read_at(path, 4, t, a->n * 28)) { free(t); return false; }
    a->e = calloc(a->n, sizeof *a->e);
    for (int i = 0; i < a->n; i++) {
        memcpy(a->e[i].name, t + i * 28, 20);
        for (char *c = a->e[i].name; *c; c++) *c = toupper((unsigned char)*c);
        a->e[i].size = rd32(t + i * 28 + 20) ^ 0x15056109u; a->e[i].off = rd32(t + i * 28 + 24) ^ 0x32300645u;
    }
    free(t); return true;
}
static arc_entry *arc_find(arc_t *a, const char *name) {
    char up[21]; int i = 0; for (; name[i] && i < 20; i++) up[i] = toupper((unsigned char)name[i]); up[i] = 0;
    for (i = 0; i < a->n; i++) if (!strcmp(a->e[i].name, up)) return &a->e[i];
    return NULL;
}
/* Port addition: loose replacement files. patch/patch.txt lists file names (one per line); each listed name is read from
   patch/<name> instead of any archive. Lets small fixes (e.g. the Korean title logo) ship without rebuilding a 100 MB archive. */
static char ov_names[64][24]; static int ov_n = -1;
static void ov_init(void) {
    ov_n = 0; size_t n; char *t = plat_read_file("patch/patch.txt", &n); if (!t) return; t[n] = 0;
    for (char *p = strtok(t, "\r\n"); p && ov_n < 64; p = strtok(NULL, "\r\n")) { while (*p == ' ') p++; if (*p) snprintf(ov_names[ov_n++], 24, "%s", p); }
    free(t); plat_log("patch overrides: %d", ov_n);
}
static void *ov_load(const char *name, size_t *len) {
    if (ov_n < 0) ov_init();
    for (int i = 0; i < ov_n; i++) if (!strcasecmp(ov_names[i], name)) {
        char p[40]; snprintf(p, sizeof p, "patch/%s", ov_names[i]); uint32_t n = plat_file_size(p); if (!n) return NULL;
        uint8_t *b = malloc(n + 64); memset(b + n, 0, 64); if (!plat_read_at(p, 0, b, n)) { free(b); return NULL; }
        if (len) *len = n; return b; }
    return NULL;
}
void *arc_load(arc_t *a, const char *name, size_t *len) {
    void *ov = ov_load(name, len); if (ov) return ov;
    arc_entry *e = arc_find(a, name); if (!e) return NULL;
    uint8_t *b = malloc(e->size + 64); memset(b + e->size, 0, 64);
    if (!plat_read_at(a->path, e->off, b, e->size)) { free(b); return NULL; }
    if (len) *len = e->size; return b;
}
void *arc_load_lzss(arc_t *a, const char *name, size_t *len) {
    size_t n; uint8_t *raw = arc_load(a, name, &n); if (!raw) return NULL;
    size_t out = lzss_decode(raw, n, NULL, 0); uint8_t *b = malloc(out + 64);
    lzss_decode(raw, n, b, out); memset(b + out, 0, 64); free(raw); if (len) *len = out; return b;
}
