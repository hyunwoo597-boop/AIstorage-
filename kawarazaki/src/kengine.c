/* 河原崎家の一族 (elf, Windows "AI.exe") - clean reimplementation of the script engine for console ports.
 * Byte code: see kawarazaki/NOTES.md. Addresses in comments refer to the original AI.exe. */
#include "kai.h"
#include "gccdec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <strings.h>
#include <time.h>

/* ------------------------------------------------------------------ archives */
typedef struct { char name[33]; uint32_t off, size, ls, le; } k_ent;   /* ls/le: AWF loop frames (ls = -1: none) */
typedef struct { const char *file; int n; k_ent *e; } k_arc;
static k_arc A_MES = {"mes.ARC"}, A_GCC = {"gcc.ARC"}, A_SEQ = {"sequence.ARC"}, A_VOICE = {"voice.ARC"};
static k_arc A_AWF[4] = {{"bgm.AWF"}, {"effect.AWF"}, {"effect2.AWF"}, {"effect3.AWF"}};
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static bool arc_open(k_arc *a, int esz) {
    uint8_t h[4]; if (!plat_read_at(a->file, 0, h, 4)) { plat_log("missing %s", a->file); return false; }
    a->n = rd32(h); uint8_t *t = malloc((size_t)a->n * esz); if (!plat_read_at(a->file, 4, t, a->n * esz)) return false;
    a->e = calloc(a->n, sizeof(k_ent));
    for (int i = 0; i < a->n; i++) { memcpy(a->e[i].name, t + i * esz, 32); a->e[i].off = rd32(t + i * esz + 32); a->e[i].size = rd32(t + i * esz + 36); if (esz >= 52) { a->e[i].ls = rd32(t + i * esz + 40); a->e[i].le = rd32(t + i * esz + 44); } }
    free(t); return true;
}
static k_ent *arc_find(k_arc *a, const char *name) {
    for (int i = 0; i < a->n; i++) if (!strcasecmp(a->e[i].name, name)) return &a->e[i];
    return NULL;
}
static uint8_t *arc_load(k_arc *a, const char *name, uint32_t *len) {
    k_ent *e = arc_find(a, name); if (!e) return NULL;
    uint8_t *b = malloc(e->size + 64); if (!plat_read_at(a->file, e->off, b, e->size)) { free(b); return NULL; }
    memset(b + e->size, 0, 64); if (len) *len = e->size; return b;
}

/* ------------------------------------------------------------------ state */
#define NSURF 16
typedef struct { int w, h; uint8_t *p; uint8_t *a; bool has_a; } surf_t;   /* RGB888 top-down; a = alpha of the last masked picture (OP25 uses it once) */
static surf_t S[NSURF];
static uint32_t D[4096];        /* dword vars  (0x50bb0) */
static uint16_t W[4096];        /* word vars   (0x510e8) */
static uint8_t  F[8192];        /* 4-bit flags (0x9ec) */
static uint32_t V[32];          /* registers   (0x51080), expression bytes 0x80-0x9f */
static uint8_t  scr[640 * 480 * 3];     /* what is shown */
static int fade_level = 255; static uint32_t fade_rgb;   /* display fade: 255 = picture, 0 = solid fade_rgb */
static uint32_t text_rgb = 0xffffff;     /* 0x50ca4 */
static bool win_open; static uint8_t tl_a[640 * 480], tl_c[640 * 480 * 3];   /* text layer over the screen (coverage, colour) */
static void tl_clear(void) { memset(tl_a, 0, sizeof tl_a); }
static uint8_t ov_a[640 * 480], ov_c[640 * 480 * 3];   /* UI overlay (title menu) */
static void ov_clear(void) { memset(ov_a, 0, sizeof ov_a); }
static void ov_pic(int si, int sx, int sy, int w, int h, int dx, int dy) {   /* surface rect with its alpha onto the overlay */
    surf_t *s = &S[si];
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) { int x = dx + i, y = dy + j, u = sx + i, v = sy + j;
        if (x < 0 || y < 0 || x >= 640 || y >= 480 || u >= s->w || v >= s->h) continue;
        int a = s->a[(size_t)v * s->w + u]; if (!a) continue; int k = y * 640 + x; memcpy(ov_c + k * 3, s->p + ((size_t)v * s->w + u) * 3, 3); ov_a[k] = a; } }
static bool capture; static char cap_txt[16][128]; static int cap_n;
static int msg_no = -1; static bool skip_mode; static bool quit;

typedef struct { const char *name; uint8_t *buf; uint32_t len; uint32_t base; } script_t;
typedef struct { script_t *s; uint32_t pc; } proc_t;
static proc_t procs[256];
typedef struct { uint8_t n; uint32_t cond[16], addr[16]; script_t *s; } menu_t;
static menu_t menus[16];
static script_t *cur; static uint32_t pc;
static char goto_name[64];

static int sjis_dbcs(uint8_t c) { return (c >= 0x81 && c <= 0x9f) || (c >= 0xe0 && c <= 0xfc); }

/* ------------------------------------------------------------------ display */
const uint8_t *k_screen(void) { return scr; }
static bool wave_on; static int wave_tab[480];   /* SYS 34: rows swing sideways (0x437270, 80 ms timer) */
static void anim_tick(void);
static void present(void) {
    const surf_t *s = &S[0]; static uint8_t row[640 * 3]; int ph = (int)(plat_ms() / 80);
    for (int y = 0; y < 480; y++) {
        const uint8_t *src = s->p + (size_t)y * s->w * 3; uint8_t *d = scr + y * 640 * 3;
        if (wave_on) { int sh = wave_tab[((479 - y) - ph % 480 + 960) % 480] / 6; sh = ((sh % 640) + 640) % 640;   /* rows wrap around */
            memcpy(row + sh * 3, src, (640 - sh) * 3); memcpy(row, src + (640 - sh) * 3, sh * 3); src = row; }
        if (fade_level >= 255) memcpy(d, src, 640 * 3);
        else { int a = fade_level, fr = fade_rgb >> 16 & 255, fg = fade_rgb >> 8 & 255, fb = fade_rgb & 255;
            for (int x = 0; x < 640; x++) { d[x*3] = (src[x*3] * a + fr * (255 - a)) / 255; d[x*3+1] = (src[x*3+1] * a + fg * (255 - a)) / 255; d[x*3+2] = (src[x*3+2] * a + fb * (255 - a)) / 255; } }
    }
}
static void compose(void) {   /* message window (surface 6 rows 0-103: frame; pure red = see-through, AI.INI MIXED/BG 64) and text layer */
    if (win_open) {
        const surf_t *w = &S[6];
        for (int y = 0; y < 104; y++) { uint8_t *d = scr + (376 + y) * 640 * 3; const uint8_t *s = w->p + (size_t)y * w->w * 3;
            for (int x = 0; x < 640; x++, d += 3, s += 3) {
                if (s[0] == 255 && s[1] == 0 && s[2] == 0) { d[0] = (d[0] * 96 + 64 * 160) >> 8; d[1] = (d[1] * 96 + 64 * 160) >> 8; d[2] = (d[2] * 96 + 64 * 160) >> 8; }
                else memcpy(d, s, 3); } }
    }
    for (int i = 0; i < 640 * 480; i++) { int a = ov_a[i]; if (!a) continue; uint8_t *d = scr + i * 3, *c = ov_c + i * 3;
        d[0] = (c[0] * a + d[0] * (255 - a)) / 255; d[1] = (c[1] * a + d[1] * (255 - a)) / 255; d[2] = (c[2] * a + d[2] * (255 - a)) / 255; }
    for (int i = 0; i < 640 * 480; i++) { int a = tl_a[i]; if (!a) continue; uint8_t *d = scr + i * 3, *c = tl_c + i * 3;
        d[0] = (c[0] * a + d[0] * (255 - a)) / 255; d[1] = (c[1] * a + d[1] * (255 - a)) / 255; d[2] = (c[2] * a + d[2] * (255 - a)) / 255; }
}
static bool frame(void) { anim_tick(); present(); compose(); if (!plat_frame(scr, 640, 480)) { quit = true; return false; } return true; }
static bool key_down(int k) { return plat_key(k); }
/* waits ms (0x40eed0): a confirm press or skip mode cuts it short; returns true if cut */
static bool wait_ms(uint32_t ms) {
    uint32_t t0 = plat_ms();
    while (!quit && plat_ms() - t0 < ms) { if (skip_mode || key_down(6)) return true; if (key_down(0)) return true; if (!frame()) break; }
    return false;
}
static void wait_click(void) {
    while (!quit && key_down(0)) if (!frame()) return;     /* release */
    while (!quit) { if (key_down(0) || key_down(6) || skip_mode) break; if (!frame()) return; }
}

/* ------------------------------------------------------------------ surfaces */
static void surf_init(void) {
    for (int i = 0; i < NSURF; i++) { S[i].w = (i == 2) ? 2048 : 1024; S[i].h = (i == 2) ? 512 : 1024; S[i].p = calloc((size_t)S[i].w * S[i].h, 3); S[i].a = calloc((size_t)S[i].w * S[i].h, 1); }
}
static bool clip(const surf_t *s, int *x, int *y, int *w, int *h) {
    if (*x < 0) { *w += *x; *x = 0; } if (*y < 0) { *h += *y; *y = 0; }
    if (*x + *w > s->w) *w = s->w - *x; if (*y + *h > s->h) *h = s->h - *y;
    return *w > 0 && *h > 0;
}
/* mode 0 copy, 1 green key (0,255,0) transparent, 2 swap */
static void blit(int si, int x1, int y1, int x2, int y2, int di, int dx, int dy, int mode) {
    if (si < 0 || si >= NSURF || di < 0 || di >= NSURF) return;
    surf_t *s = &S[si], *d = &S[di]; int w = x2 - x1 + 1, h = y2 - y1 + 1;
    if (x1 < 0) { dx -= x1; w += x1; x1 = 0; } if (y1 < 0) { dy -= y1; h += y1; y1 = 0; }
    if (dx < 0) { x1 -= dx; w += dx; dx = 0; } if (dy < 0) { y1 -= dy; h += dy; dy = 0; }
    if (x1 + w > s->w) w = s->w - x1; if (y1 + h > s->h) h = s->h - y1;
    if (dx + w > d->w) w = d->w - dx; if (dy + h > d->h) h = d->h - dy;
    if (w <= 0 || h <= 0) return;
    bool overlap = s == d; uint8_t *tmp = NULL;
    if (overlap && mode != 2) { tmp = malloc((size_t)w * h * 3); for (int y = 0; y < h; y++) memcpy(tmp + (size_t)y * w * 3, s->p + ((size_t)(y1 + y) * s->w + x1) * 3, w * 3); }
    for (int y = 0; y < h; y++) {
        uint8_t *sp = tmp ? tmp + (size_t)y * w * 3 : s->p + ((size_t)(y1 + y) * s->w + x1) * 3, *dp = d->p + ((size_t)(dy + y) * d->w + dx) * 3;
        if (mode == 0) memcpy(dp, sp, w * 3);
        else if (mode == 1 && s->has_a && !tmp) { const uint8_t *ap = s->a + (size_t)(y1 + y) * s->w + x1;
            for (int x = 0; x < w; x++) { int a = ap[x]; if (a == 255) memcpy(dp + x*3, sp + x*3, 3);
                else if (a) for (int c = 0; c < 3; c++) dp[x*3+c] = (sp[x*3+c] * a + dp[x*3+c] * (255 - a)) / 255; } }
        else if (mode == 1) { for (int x = 0; x < w; x++) if (!(sp[x*3] == 0 && sp[x*3+1] == 255 && sp[x*3+2] == 0)) memcpy(dp + x*3, sp + x*3, 3); }
        else for (int x = 0; x < w * 3; x++) { uint8_t t = sp[x]; sp[x] = dp[x]; dp[x] = t; }
    }
    free(tmp);
    if (mode == 1) s->has_a = false;   /* 0x41b253: the mask is used once */
}
static void fill(int di, int x1, int y1, int x2, int y2, uint32_t rgb, bool invert) {
    if (di < 0 || di >= NSURF) return; surf_t *d = &S[di]; int x = x1, y = y1, w = x2 - x1 + 1, h = y2 - y1 + 1;
    if (!clip(d, &x, &y, &w, &h)) return;
    for (int j = 0; j < h; j++) { uint8_t *p = d->p + ((size_t)(y + j) * d->w + x) * 3;
        for (int i = 0; i < w; i++, p += 3) { if (invert) { p[0] ^= 255; p[1] ^= 255; p[2] ^= 255; } else { p[0] = rgb >> 16; p[1] = rgb >> 8; p[2] = rgb; } } }
}
/* OP23: picture name.gcc into surface D[12] at its own position (or x,y); D[8..11] = drawn rect */
static void load_pic(const char *name, int px, int py) {
    char n[64]; snprintf(n, sizeof n, "%s", name); char *dot = strrchr(n, '.'); if (dot) strcpy(dot, ".gcc"); else strcat(n, ".gcc");
    uint32_t len; uint8_t *b = arc_load(&A_GCC, n, &len);
    if (!b) { plat_log("no picture %s", n); D[8] = D[9] = 0; D[10] = D[11] = 0; return; }
    gcc_image im; if (gcc_decode(b, len, &im)) { plat_log("bad picture %s", n); free(b); return; }
    int x = px >= 0 ? px : im.x, y = py >= 0 ? py : im.y; int di = D[12] & 15; surf_t *d = &S[di];
    for (int j = 0; j < im.h; j++) {
        int dy = y + j; if (dy < 0 || dy >= d->h) continue;
        const uint8_t *src = im.bgr + (size_t)(im.h - 1 - j) * im.w * 3;   /* decoder: bottom-up BGR */
        const uint8_t *al = NULL;
        if (im.has_alpha) { int aj = j - (im.ay - im.y); if (aj >= 0 && aj < im.ah) al = im.alpha + (size_t)(im.ah - 1 - aj) * im.aw; }
        for (int i = 0; i < im.w; i++) {
            int dx = x + i; if (dx < 0 || dx >= d->w) continue; uint8_t *dp = d->p + ((size_t)dy * d->w + dx) * 3;
            int a = 255; if (im.has_alpha) { int ai = i - (im.ax - im.x); a = (al && ai >= 0 && ai < im.aw) ? al[ai] : 0; }
            dp[0] = src[i*3+2]; dp[1] = src[i*3+1]; dp[2] = src[i*3]; d->a[(size_t)dy * d->w + dx] = a;
        }
    }
    d->has_a = im.has_alpha; D[8] = x; D[9] = y; D[10] = im.w; D[11] = im.h;
    free(im.bgr); free(im.alpha); free(b);
}

/* ------------------------------------------------------------------ text (0x4116d0) */
static uint8_t *font; static uint32_t font_len;
static void draw_glyph(int di, int x, int y, const uint8_t *cell, int w) {
    if (di == 0) {   /* screen: text layer */
        for (int j = 0; j < 16; j++) for (int i = 0; i < w; i++) { int c = cell[j * w + i]; if (!c) continue;
            for (int sh = 1; sh >= 0; sh--) { int px = x + i + sh, py = y + j + sh; if (px < 0 || py < 0 || px >= 640 || py >= 480) continue;
                uint32_t col = sh ? D[7] : text_rgb; int k = py * 640 + px; if (sh && tl_a[k] && c < tl_a[k]) continue;
                tl_c[k*3] = col >> 16; tl_c[k*3+1] = col >> 8; tl_c[k*3+2] = col; if (c > tl_a[k] || !sh) tl_a[k] = c > tl_a[k] ? c : tl_a[k]; } }
        return;
    }
    surf_t *d = &S[di];
    for (int j = 0; j < 16; j++) for (int i = 0; i < w; i++) {
        int c = cell[j * w + i]; if (!c) continue;
        for (int sh = 1; sh >= 0; sh--) {   /* shadow at +1,+1 (D[7] colour), then the letter */
            int px = x + i + sh, py = y + j + sh; if (px < 0 || py < 0 || px >= d->w || py >= d->h) continue;
            uint32_t col = sh ? D[7] : text_rgb; uint8_t *p = d->p + ((size_t)py * d->w + px) * 3;
            p[0] = ((col >> 16 & 255) * c + p[0] * (255 - c)) / 255; p[1] = ((col >> 8 & 255) * c + p[1] * (255 - c)) / 255; p[2] = ((col & 255) * c + p[2] * (255 - c)) / 255;
        }
    }
}
static bool text_noframe;
static void text(const uint8_t *t) {
    int di = D[12] & 15; int cw = D[17] ? (int)D[17] : 16, lh = D[18] ? (int)D[18] : 16;
    while (*t && !quit) {
        if (t[0] == 0x81 && t[1] == 0x93) { D[0] = D[2]; D[1] += lh; t += 2; continue; }   /* '％' = new line */
        if (t[0] == 0xfd && t[1] >= 0x40 && t[2] >= 0x40) {   /* Korean patch: FD hi lo = Hangul syllable (hi-0x40)*188 + lo-0x40 */
            unsigned k = (unsigned)(t[1] - 0x40) * 188 + (t[2] - 0x40); const size_t hb = 4 + 256 * 128 + (size_t)47 * 189 * 256;
            if (D[0] + cw - 1 > D[4]) { D[0] = D[2]; D[1] += lh; }
            if (font && k < 11172 && font_len >= hb + 4 + (k + 1) * 256 && !memcmp(font + hb, "KHG1", 4)) draw_glyph(di, D[0], D[1], font + hb + 4 + k * 256, 16);
            D[0] += cw; t += 3; if (!skip_mode && D[16] && !text_noframe) wait_ms(D[16]); continue; }
        int dbcs = sjis_dbcs(*t), w = dbcs ? cw : cw / 2; const uint8_t *cell = NULL;
        if (font) {
            if (dbcs) { int l = t[0], tr = t[1]; int li = l <= 0x9f ? l - 0x81 : l - 0xe0 + 31;
                if (li >= 0 && li < 47 && tr >= 0x40 && tr <= 0xfc) cell = font + 4 + 256 * 128 + ((size_t)li * 189 + (tr - 0x40)) * 256; }
            else cell = font + 4 + *t * 128;
        }
        if (D[0] + w - 1 > D[4]) { D[0] = D[2]; D[1] += lh; }
        if (cell) draw_glyph(di, D[0], D[1], cell, dbcs ? 16 : 8);
        D[0] += w; t += dbcs ? 2 : 1;
        if (!skip_mode && D[16] && !text_noframe) wait_ms(D[16]);
    }
    if (!text_noframe) frame();
}

/* ------------------------------------------------------------------ sound */
/* AWF banks hold bare PCM16: bgm.AWF stereo 44.1 kHz, effect*.AWF mono 22.05 kHz (AI.exe 0x446174 / 0x4463c3).
   Wrapped into a RIFF/WAVE (with a smpl loop) for the platform mixer. */
static void wr32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static struct { uint8_t *b; size_t n; bool loop; } pend[SND_SLOTS];
static void play_awf(int slot, const char *name, bool loop) {
    for (int i = 0; i < 4; i++) { k_ent *e = arc_find(&A_AWF[i], name); if (!e) continue;
        int ch = i == 0 ? 2 : 1, rate = i == 0 ? 44100 : 22050; bool lp = loop || e->ls != 0xffffffffu;
        uint32_t ls = e->ls == 0xffffffffu ? 0 : e->ls, le = e->ls == 0xffffffffu ? e->size / (2 * ch) : e->le;
        size_t hl = 12 + 24 + (lp ? 8 + 60 : 0) + 8; uint8_t *b = malloc(hl + e->size); uint8_t *p = b;
        memcpy(p, "RIFF", 4); wr32(p + 4, (uint32_t)(hl - 8 + e->size)); memcpy(p + 8, "WAVE", 4); p += 12;
        memcpy(p, "fmt ", 4); wr32(p + 4, 16); p[8] = 1; p[9] = 0; p[10] = ch; p[11] = 0; wr32(p + 12, rate); wr32(p + 16, rate * 2 * ch); p[20] = 2 * ch; p[21] = 0; p[22] = 16; p[23] = 0; p += 24;
        if (lp) { memcpy(p, "smpl", 4); wr32(p + 4, 60); memset(p + 8, 0, 60); wr32(p + 8 + 28, 1); wr32(p + 8 + 44, ls); wr32(p + 8 + 48, le ? le - 1 : 0); p += 68; }
        memcpy(p, "data", 4); wr32(p + 4, e->size); p += 8;
        if (!plat_read_at(A_AWF[i].file, e->off, p, e->size)) { free(b); return; }
        free(pend[slot].b); pend[slot].b = b; pend[slot].n = hl + e->size; pend[slot].loop = lp; return; }
    plat_log("no sound %s", name);
}

/* ------------------------------------------------------------------ interpreter */
typedef struct { int type; uint32_t v; char s[64]; } arg_t;   /* type 1 int, 2 string */
static arg_t AR[16]; static int nar;
static uint8_t cb(void) { return cur->buf[cur->base + pc++]; }
static uint16_t cw16(void) { uint16_t v = cur->buf[cur->base + pc] | cur->buf[cur->base + pc + 1] << 8; pc += 2; return v; }
static uint32_t cw32(void) { uint32_t v = rd32(cur->buf + cur->base + pc); pc += 4; return v; }
static uint8_t peek(void) { return cur->buf[cur->base + pc]; }
static int getF(uint32_t i) { i &= 0x3fff; return i & 1 ? F[i >> 1] & 15 : F[i >> 1] >> 4; }
static void setF(uint32_t i, uint32_t v) { i &= 0x3fff; if (v > 15) v = 15; if (i & 1) F[i >> 1] = (F[i >> 1] & 0xf0) | v; else F[i >> 1] = (F[i >> 1] & 0x0f) | v << 4; }
static uint32_t rnd(uint32_t n) { static uint32_t s = 0x12345; s = s * 214013u + 2531011u; return n ? (s >> 16 & 0x7fff) % n : 0; }
static uint32_t expr(void) {   /* 0x427fd0 */
    uint32_t st[64]; int sp = 0;
    for (;;) {
        uint8_t b = cb(); if (b == 0xff) break;
        if (sp >= 62) { plat_log("expr overflow"); break; }
        if (b < 0x80) { st[sp++] = b; continue; }
        if (b < 0xa0) { st[sp++] = V[b - 0x80]; continue; }
        if (b < 0xe0) { if (sp) st[sp - 1] = 0; plat_log("byte array %x", b); continue; }
        uint32_t r, l;
        switch (b) {
        case 0xe5: st[sp++] = rnd(cw16()); continue;
        case 0xf1: st[sp++] = cw16(); continue;
        case 0xf2: st[sp++] = cw32(); continue;
        case 0xf3: st[sp++] = getF(cw16()); continue;
        case 0xf4: if (sp) st[sp - 1] = getF(st[sp - 1]); continue;
        case 0xf5: continue;
        case 0xf6: st[sp++] = W[cw16() & 4095]; continue;
        case 0xf7: if (sp) st[sp - 1] = W[st[sp - 1] & 4095]; continue;
        case 0xf8: st[sp++] = D[cw16() & 4095]; continue;
        case 0xf9: if (sp) st[sp - 1] = D[st[sp - 1] & 4095]; continue;
        }
        if (sp < 2) { plat_log("expr underflow %x", b); continue; }
        r = st[--sp]; l = st[sp - 1];
        switch (b) {
        case 0xe0: l = l + r; break; case 0xe1: l = l < r ? 0 : l - r; break; case 0xe2: l = l * r; break;
        case 0xe3: l = r ? l / r : 0; break; case 0xe4: l = r ? l % r : 0; break;
        case 0xe6: l = l && r; break; case 0xe7: l = l || r; break; case 0xe8: l &= r; break; case 0xe9: l |= r; break; case 0xea: l ^= r; break;
        case 0xeb: l = l < r; break; case 0xec: l = l > r; break; case 0xed: l = l <= r; break; case 0xee: l = l >= r; break;
        case 0xef: l = l == r; break; case 0xf0: l = l != r; break;
        default: plat_log("expr op %x", b);
        }
        st[sp - 1] = l;
    }
    return sp ? st[sp - 1] : 0;
}
static void args(void) {   /* 0x40e8c0 */
    nar = 0; memset(AR, 0, sizeof AR);
    for (;;) {
        uint8_t b = peek(); if (b == 0xff || b == 0xfe) { pc++; return; }
        if (nar >= 16) { plat_log("too many args"); return; }
        arg_t *a = &AR[nar++];
        if (b == 0xf5) { pc++; int n = 0; while (peek() != 0xff) { uint8_t c = cb(); if (n < 63) a->s[n++] = c; } pc++; a->s[n] = 0; a->type = 2; }
        else { a->type = 1; a->v = expr(); }
    }
}
static uint32_t A(int i) { return i < nar ? AR[i].v : 0; }
static void elist_store(int kind, uint32_t idx) {   /* SETF/SETW/SETD lists end with ff or fe */
    for (;;) {
        uint32_t v = expr();
        if (kind == 0) setF(idx, v); else if (kind == 1) W[idx & 4095] = v; else D[idx & 4095] = v;
        idx++; uint8_t b = peek(); if (b == 0xff || b == 0xfe) { pc++; return; }
    }
}

static script_t *load_script(const char *name) {
    script_t *s = calloc(1, sizeof *s); s->buf = arc_load(&A_MES, name, &s->len);
    if (!s->buf) { plat_log("no script %s", name); free(s); return NULL; }
    s->name = strdup(name); s->base = 4 + 4 * rd32(s->buf); return s;
}
static void run(void);
static void run_at(script_t *s, uint32_t at) {   /* nested dispatcher (0x411ae0) until END */
    script_t *os = cur; uint32_t op = pc; cur = s; pc = at; run(); cur = os; pc = op;
}
static void sys_call(void);
static void sound_ctl(void);
static void anim(void);
static void rec_cmd(void); static bool sys_menu(void); static bool save_slot(int slot); static bool load_slot(int slot); static int slot_pick(bool saving); static void ui_dim(void); static bool load_menu(void); static void sys_load(void); static void sys_save(void);
static bool auto_mode; static char last_txt[96]; static int resume_msg = -1;
static void menu_run(void);
static void text(const uint8_t *t);
static void wait_msg(void);
static int end_flag;
static void run(void) {
    while (!quit) {
        if (pc >= cur->len - cur->base) { plat_log("%s: ran off the end", cur->name); end_flag = 1; }
        if (end_flag) { if (end_flag == 1) end_flag = 0; return; }
        uint32_t at = pc; uint8_t op = cb();
        if (getenv("K_TRACE")) plat_log("%s %05x op %02x", cur->name, at, op);
        switch (op) {
        case 0x00: { const uint8_t *t = cur->buf + cur->base + pc; size_t n = strlen((const char *)t);
            if (capture) { if (cap_n < 16) snprintf(cap_txt[cap_n++], 128, "%s", (const char *)t); } else { snprintf(last_txt, sizeof last_txt, "%s", (const char *)t); text(t); } pc += n + 1; break; }
        case 0x01: pc = cw32(); break;
        case 0x02: sys_call(); break;
        case 0x03: args(); snprintf(goto_name, sizeof goto_name, "%s", AR[0].s); end_flag = 2; break;
        case 0x04: { args(); script_t *s = load_script(AR[0].s); if (s) { run_at(s, 0); } break; }
        case 0x05: { uint32_t i = cw16(); elist_store(0, i); break; }
        case 0x06: { uint32_t i = expr(); elist_store(0, i); break; }
        case 0x07: { uint8_t r = cb(); V[r & 31] = expr(); break; }
        case 0x08: case 0x09: { cb(); expr(); for (;;) { expr(); uint8_t b = peek(); if (b == 0xff || b == 0xfe) { pc++; break; } } plat_log("poke op %x", op); break; }
        case 0x0a: { uint32_t i = cw16(); elist_store(1, i); break; }
        case 0x0b: { uint32_t i = expr(); elist_store(1, i); break; }
        case 0x0c: { uint32_t i = cw16(); elist_store(2, i); break; }
        case 0x0d: { uint32_t i = expr(); elist_store(2, i); break; }
        case 0x0e: args(); plat_log("op0e %u %u", A(0), A(1)); break;
        case 0x0f: args(); plat_log("op0f %u %u", A(0), A(1)); break;
        case 0x10: { uint32_t c = expr(); uint32_t t = cw32(); if (!c) pc = t; break; }
        case 0x11: { uint32_t i = expr(); procs[i & 255].s = cur; procs[i & 255].pc = pc + 4; pc = cw32(); break; }
        case 0x12: { args(); proc_t *p = &procs[A(0) & 255]; if (p->s) run_at(p->s, p->pc); else plat_log("no proc %u", A(0)); break; }
        case 0x13: { uint32_t i = expr() & 15; uint32_t t = cw32(); menu_t *m = &menus[i]; const uint8_t *q = cur->buf + cur->base + t;
            m->n = q[0] > 16 ? 16 : q[0]; m->s = cur; for (int k = 0; k < m->n; k++) { m->cond[k] = rd32(q + 1 + 8 * k); m->addr[k] = rd32(q + 5 + 8 * k); }
            pc = t + 1 + 8 * q[0]; break; }
        case 0x14: args(); menu_run(); break;
        case 0x15: args(); plat_log("op15"); break;
        case 0x16: args(); text_rgb = A(0); D[7] = A(1); break;
        case 0x20: args(); wait_ms(A(0) * 20); break;
        case 0x21: plat_log("op21"); break;
        case 0x22: args(); wait_msg(); break;
        case 0x23: args(); load_pic(AR[0].s, nar > 1 ? (int)A(1) : -1, nar > 2 ? (int)A(2) : -1); if ((D[12] & 15) == 0) frame(); break;
        case 0x24: args(); blit(A(4), A(0), A(1), A(2), A(3), A(7), A(5), A(6), 0); if (A(7) == 0) frame(); break;
        case 0x25: args(); blit(A(4), A(0), A(1), A(2), A(3), A(7), A(5), A(6), 1); if (A(7) == 0) frame(); break;
        case 0x26: args(); blit(A(4), A(0), A(1), A(2), A(3), A(7), A(5), A(6), 2); if (A(7) == 0 || A(4) == 0) frame(); break;
        case 0x27: args(); fill(D[12] & 15, A(0), A(1), A(2), A(3), D[7], false); if ((D[12] & 15) == 0) frame(); break;
        case 0x28: args(); fill(D[12] & 15, A(0), A(1), A(2), A(3), 0, true); if ((D[12] & 15) == 0) frame(); break;
        case 0x2b: args(); plat_log("op2b"); break;
        case 0x2d: args(); plat_log("op2d %u", A(0)); break;
        case 0x2e: args(); anim(); break;
        case 0x2f: args(); play_awf(SND_BGM, AR[0].s, true); break;
        case 0x30: args(); play_awf(SND_SE + (A(1) % 3), AR[0].s, false); break;
        case 0x31: { args(); uint32_t n; uint8_t *b = arc_load(&A_VOICE, AR[0].s, &n); if (b) plat_snd_play(SND_VOICE, b, n, 255, false); else plat_log("no voice %s", AR[0].s); break; }
        case 0x32: args(); sound_ctl(); break;
        case 0x33: args(); plat_log("op33"); break;
        case 0x34: args(); plat_log("op34"); break;
        case 0x35: msg_no = cw32(); break;
        case 0x37: pc += 4; break;
        case 0xfe: break;
        case 0xff: return;
        default: plat_log("%s %x: bad op %02x", cur->name, at, op); quit = true; return;
        }
    }
}
/* OP22: end of a message - wait for a click, then clear the text area */
static void wait_msg(void) {
#ifdef K_HEADLESS_RAND
    { static long nmsg; nmsg++; const char *e = getenv("K_TEST_SAVE"); if (e && nmsg == atol(e)) { save_slot(0); plat_log("saved at %s msg %d", cur->name, msg_no); quit = true; return; }
      e = getenv("K_TEST_UI"); if (e && nmsg == atol(e)) { plat_log("ui"); ui_dim(); slot_pick(true); sys_menu(); }
      e = getenv("K_TEST_LOAD"); if (e && nmsg == 1) { plat_log("load -> %d", load_slot(0)); return; } }
#endif
    if (!skip_mode) {
        while (!quit && key_down(0)) if (!frame()) return;
        uint32_t t0 = plat_ms();
        for (;;) {
            if (quit || skip_mode || key_down(6)) break;
            if (key_down(0)) break;
            if (key_down(1)) { while (!quit && key_down(1)) frame(); if (sys_menu()) return; t0 = plat_ms(); continue; }
            if (auto_mode && !plat_snd_playing(SND_VOICE) && plat_ms() - t0 > 2500) break;
            if (plat_snd_playing(SND_VOICE)) t0 = plat_ms();
            if (!frame()) return; }
    }
    tl_clear(); D[0] = D[2] + 16; D[1] = D[3];
    frame();
}
static void win_show(bool on) {   /* SYS 1 / SYS 2 */
    win_open = on; tl_clear(); D[0] = D[2] + 16; D[1] = D[3]; frame();
}
static void fade(int to, uint32_t rgb) {   /* display fade, ~0.5 s */
    int from = fade_level; fade_rgb = rgb;
    for (int i = 1; i <= 16 && !quit; i++) { fade_level = from + (to - from) * i / 16; if (!skip_mode) wait_ms(30); else frame(); }
    fade_level = to;
}
/* simple pointer/pad menu: returns the chosen index; hover via d-pad or pointer over rows */
static int pick(int n, int cur0, int (*row_at)(int x, int y), void (*draw)(int sel, bool pressed)) {
    int sel = cur0, px, py, lx = -1, ly = -1; bool held = true;
    if (getenv("K_PICK")) { static const char *p; if (!p) p = getenv("K_PICK"); int v = atoi(p); const char *c = strchr(p, ','); p = c ? c + 1 : p + strlen(p); if (v >= 0 && v < n) { draw(v, true); frame(); return v; } }
    for (;;) {
        if (quit) return sel;
        plat_cursor(&px, &py, false);
        if (px != lx || py != ly) { int r = row_at(px, py); if (r >= 0) sel = r; lx = px; ly = py; }
        bool up = key_down(2), dn = key_down(3), ok = key_down(0);
        static bool pu, pd; if (up && !pu) sel = (sel + n - 1) % n; if (dn && !pd) sel = (sel + 1) % n; pu = up; pd = dn;
        if (ok && !held) { draw(sel, true); frame(); wait_ms(120); return sel; }
        held = ok; draw(sel, false); if (!frame()) return sel;
    }
}
/* title menu (SYS 37): items from title_pt (surface 7), 3 rows per item (normal, hover, pressed), logo below them */
static int tm_n, tm_map[6];
static int tm_row(int x, int y) { (void)x; for (int i = 0; i < tm_n; i++) if (y >= 232 + i * 40 && y < 268 + i * 40 && x >= 168 && x < 472) return i; return -1; }
static void tm_draw(int sel, bool pressed) {
    ov_clear(); ov_pic(7, 0, 768, 584, 120, 28, 40);
    for (int i = 0; i < tm_n; i++) ov_pic(7, 0, tm_map[i] * 108 + (i == sel ? (pressed ? 72 : 36) : 0), 584, 36, 28, 232 + i * 40);
}
static void title_menu(void) {
    tm_n = 0; for (int i = 0; i < 6; i++) if (i < 5 || getF(150)) tm_map[tm_n++] = i;
    int r = pick(tm_n, 0, tm_row, tm_draw); V[18] = tm_map[r] + 1; ov_clear(); frame();
    plat_log("title menu -> %u", V[18]);
}
/* SYS 35 (0x43a0a0): surface 1 comes onto the screen through the transition mask NN.msk (sequence.ARC, 640x480 levels):
   step t = 32..352 by 32, a pixel's share of the new picture = t + 16 - level (0..255) */
static void mask_trans(int n, bool instant) {
    char nm[16]; snprintf(nm, sizeof nm, "%02d.msk", n % 100); uint32_t len; uint8_t *m = instant || skip_mode ? NULL : arc_load(&A_SEQ, nm, &len);
    if (m && len >= 640 * 480) {
        uint8_t *old = malloc(640 * 480 * 3); for (int j = 0; j < 480; j++) memcpy(old + j * 1920, S[0].p + (size_t)j * S[0].w * 3, 1920);
        for (int t = 32; t < 0x180 && !quit; t += 32) {
            for (int j = 0; j < 480; j++) { uint8_t *d = S[0].p + (size_t)j * S[0].w * 3; const uint8_t *o = old + j * 1920, *nw = S[1].p + (size_t)j * S[1].w * 3, *mk = m + j * 640;
                for (int i = 0; i < 640; i++) { int a = t + 16 - mk[i]; if (a < 0) a = 0; if (a > 255) a = 255;
                    for (int c = 0; c < 3; c++) d[i * 3 + c] = (uint8_t)((o[i * 3 + c] * (255 - a) + nw[i * 3 + c] * a) / 255); } }
            if (wait_ms(25)) break; }
        free(old); }
    free(m);
    blit(1, 0, 0, 639, 479, 0, 0, 0, 0); frame();
}
/* SYS 5 (0x43e83d): the rect D[8..11] of surface 1 fades in over the screen */
static void xfade_rect(void) {
    int x = (int)D[8], y = (int)D[9], w = (int)D[10], h = (int)D[11];
    if (!skip_mode && w > 0 && h > 0) {
        uint8_t *old = malloc((size_t)w * h * 3);
        for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) { int X = x + i, Y = y + j; if (X < 0 || Y < 0 || X >= S[0].w || Y >= S[0].h) continue; memcpy(old + ((size_t)j * w + i) * 3, S[0].p + ((size_t)Y * S[0].w + X) * 3, 3); }
        for (int st = 1; st < 12 && !quit; st++) {
            for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) { int X = x + i, Y = y + j; if (X < 0 || Y < 0 || X >= 640 || Y >= 480) continue;
                uint8_t *d = S[0].p + ((size_t)Y * S[0].w + X) * 3; const uint8_t *o = old + ((size_t)j * w + i) * 3, *nw = S[1].p + ((size_t)Y * S[1].w + X) * 3;
                for (int c = 0; c < 3; c++) d[c] = (uint8_t)((o[c] * (12 - st) + nw[c] * st) / 12); }
            if (wait_ms(25)) break; }
        free(old); }
    blit(1, x, y, x + w - 1, y + h - 1, 0, x, y, 0); frame();
}
/* SYS 40 (0x43ad90): picture show spg01..spg04, then spg10..spg19 in turn until a press */
static void spg_show(void) {
    static const char *const first[6] = { "spg01", "spg01_1", "spg01_2", "spg02", "spg03", "spg04" };
    uint32_t keep = D[12]; D[12] = 1;
    for (int i = 0; i < 6 && !quit; i++) { load_pic(first[i], 0, 0); mask_trans(9, false); }
    for (int i = 0; !quit; i = (i + 1) % 10) { char n[16]; snprintf(n, sizeof n, "spg%02d", 10 + i); load_pic(n, 0, 0); mask_trans(9, false);
        if (key_down(0) || key_down(1)) break;
        uint32_t t0 = plat_ms(); bool hit = false; while (!quit && plat_ms() - t0 < 1500) { if (key_down(0) || key_down(1)) { hit = true; break; } if (!frame()) break; }
        if (hit) break; }
    while (!quit && (key_down(0) || key_down(1))) frame();
    D[12] = keep;
}
static void sys_call(void) {   /* OP02 (0x43e6a0): sub = arg0 */
    args(); uint32_t f = A(0);
    switch (f) {
    case 1: win_show(true); break;
    case 2: win_show(false); break;
    case 3: switch (A(1)) {
        case 0: fade(0, 0); break;                 /* to black */
        case 1: fade(255, fade_rgb); break;        /* back to the picture */
        case 2: fade(0, 0xffffff); break;          /* to white */
        default: plat_log("sys 3,%u", A(1)); }
        break;
    case 4: /* cross-fade surface 1 onto the screen */
        for (int a = 0; a < 8 && !quit; a++) { wait_ms(40); }
        blit(1, 0, 0, 639, 479, 0, 0, 0, 0); frame(); break;
    case 8: {   /* transition: rect (x, y, w, h) of surface 1 dissolves onto the screen */
        int x = A(1), y = A(2), w = A(3), h = A(4); if (nar < 5) { x = y = 0; w = 640; h = 480; }
        if (!skip_mode) { uint8_t *old = malloc(640 * 480 * 3); for (int j = 0; j < 480; j++) memcpy(old + j * 1920, S[0].p + (size_t)j * S[0].w * 3, 1920);
            for (int st = 1; st <= 10 && !quit; st++) {
                for (int j = y; j < y + h && j < 480; j++) for (int i = x; i < x + w && i < 640; i++) { if (j < 0 || i < 0) continue;
                    uint8_t *d = S[0].p + ((size_t)j * S[0].w + i) * 3; const uint8_t *a = old + (j * 640 + i) * 3, *b = S[1].p + ((size_t)j * S[1].w + i) * 3;
                    for (int c = 0; c < 3; c++) d[c] = (a[c] * (10 - st) + b[c] * st) / 10; }
                if (wait_ms(30)) break; }
            free(old); }
        blit(1, x, y, x + w - 1, y + h - 1, 0, x, y, 0); frame(); break; }
    case 5: xfade_rect(); break;
    case 24: rec_cmd(); break;                       /* scene record, read back when a save is loaded */
    case 34: wave_on = A(1) == 1; if (wave_on) for (int i = 0; i < 480; i++) wave_tab[i] = (int)(cos(i * 0.026179938333333333) * 100.0); frame(); break;
    case 35: mask_trans((int)A(1), A(2) != 0); break;
    case 36: case 47: case 49: break;                  /* Windows menu bar items */
    case 38: load_menu(); break;                    /* title: load */
    case 40: spg_show(); break;
    case 45: V[18] = rnd(2) + 1; break;
    case 46: while (!quit && key_down(0)) frame();     /* wait for a press; cancel clears v9 */
        for (;;) { if (quit || skip_mode) break; if (key_down(0)) break; if (key_down(1)) { V[9] = 0; break; } if (!frame()) break; }
        while (!quit && (key_down(0) || key_down(1))) frame();
        break;
    case 48: if (wait_ms(A(1) * 20)) V[9] = 1; break;
    case 37: title_menu(); break;
    default: plat_log("sys %u (%u %u %u)", f, A(1), A(2), A(3));
    }
}
static void snd_start(int slot) { if (pend[slot].b) { uint8_t *c = malloc(pend[slot].n); memcpy(c, pend[slot].b, pend[slot].n); plat_snd_play(slot, c, pend[slot].n, 255, pend[slot].loop); } }
static void snd_wait(int slot) { while (!quit && plat_snd_playing(slot)) { if (skip_mode || key_down(0)) break; if (!frame()) break; } }
static void sound_ctl(void) {   /* OP32 (0x434570): 0-6 music, 16-22 effect channel A(1), 32-34 voice */
    int ch = SND_SE + (A(1) % 3);
    switch (A(0)) {
    case 0: snd_start(SND_BGM); break;
    case 1: plat_snd_stop(SND_BGM); break;
    case 2: snd_start(SND_BGM); break;                       /* fade in */
    case 3: plat_snd_fade(SND_BGM, A(1) ? A(1) : 1000); break;
    case 4: plat_snd_fade(SND_BGM, A(1) ? A(1) : 1000); snd_wait(SND_BGM); break;
    case 6: snd_wait(SND_BGM); break;
    case 16: snd_start(ch); break;
    case 17: plat_snd_stop(ch); break;
    case 18: case 19: plat_snd_fade(ch, A(2) ? A(2) : 500); break;
    case 22: snd_wait(ch); break;
    case 32: plat_snd_stop(SND_VOICE); break;
    case 33: snd_wait(SND_VOICE); break;
    default: plat_log("snd %u %u %u", A(0), A(1), A(2));
    }
}
/* ------------------------------------------------------------------ animations (OP2E, 0x410570)
   .ax (sequence.ARC): u32 stream_offset[10][32], draw calls of 7 x u32 from +0x500 (type, sx, sy, w, h, dx, dy), bytecode.
   The frames come from the sheet in surface 8 (scripts load "...an.bmp" there) and go onto the screen. Same machine as
   AI5WIN (Nonomura): 0 nop, 1 n wait, 2 n stall, 3 restart, 4/ff end, 5/6 and 7/8 loops, other = draw call ARG. */
typedef struct { uint32_t state, ip, stall, wait, l1_ip, l1_cnt, l1_init, l2_ip, l2_cnt, l2_init; } an_t;
enum { AN_RUN = 0, AN_STOPREQ = 1, AN_HALT = 0xff };
static uint8_t *ax; static uint32_t ax_len; static an_t AN[10][32]; static uint32_t an_t0; static bool an_any;
static uint32_t ax32(uint32_t o) { return o + 4 <= ax_len ? rd32(ax + o) : 0xff; }
static void anim_halt_all(void) { for (int a = 0; a < 10; a++) for (int b = 0; b < 32; b++) AN[a][b].state = AN_HALT; an_any = false; }
static void anim_draw(uint32_t n) {
    uint32_t r = 0x500 + n * 28; if (r + 28 > ax_len) return;
    int t = (int)ax32(r), sx = (int)ax32(r + 4), sy = (int)ax32(r + 8), w = (int)ax32(r + 12), h = (int)ax32(r + 16), dx = (int)ax32(r + 20), dy = (int)ax32(r + 24);
    bool ha = S[8].has_a; S[8].has_a = false;                     /* sheets are G24n: green key */
    if (t == 0 || t == 5) blit(8, sx, sy, sx + w - 1, sy + h - 1, 0, dx, dy, 0);
    else if (t == 1) blit(8, sx, sy, sx + w - 1, sy + h - 1, 0, dx, dy, 1);
    S[8].has_a = ha;
}
static void anim_step(int a, int b) {
    an_t *s = &AN[a][b]; uint32_t base = ax32((uint32_t)(a * 32 + b) * 4);
    if (s->stall) { s->stall--; return; }
    if (s->wait) { if (s->state == AN_STOPREQ) s->state = AN_HALT; s->wait--; return; }
    if (base + s->ip >= ax_len) { s->state = AN_HALT; return; }
    uint8_t op = ax[base + s->ip++];
#define ARG() (s->ip += 4, ax32(base + s->ip - 4))
    switch (op) {
    case 0: break;
    case 1: if (s->state == AN_STOPREQ) s->state = AN_HALT; s->wait = ARG(); break;
    case 2: s->stall = ARG(); break;
    case 3: s->ip = 0; break;
    case 4: case 0xff: s->state = AN_HALT; break;
    case 5: s->l1_cnt = s->l1_init = ARG(); s->l1_ip = s->ip; break;
    case 6: if (!s->l1_init || --s->l1_cnt) s->ip = s->l1_ip; break;
    case 7: s->l2_cnt = s->l2_init = ARG(); s->l2_ip = s->ip; break;
    case 8: if (!s->l2_init || --s->l2_cnt) s->ip = s->l2_ip; break;
    default: anim_draw(ARG()); break;
    }
#undef ARG
}
static void anim_ticks(int n) {
    while (n-- > 0) { bool any = false;
        for (int a = 0; a < 10; a++) for (int b = 0; b < 32; b++) if (AN[a][b].state != AN_HALT) { anim_step(a, b); any = true; }
        an_any = any; if (!any) break; }
}
static void anim_tick(void) {   /* 60 steps a second, run from every frame */
    uint32_t now = plat_ms(); if (!an_any) { an_t0 = now; return; }
    int n = (int)((now - an_t0) * 60 / 1000); if (n <= 0) return; an_t0 += (uint32_t)n * 1000 / 60; if (n > 6) n = 6; anim_ticks(n);
}
static void anim_start(int a, int b) { if (!ax) return; an_t *s = &AN[a % 10][b & 31]; memset(s, 0, sizeof *s); s->state = AN_RUN; if (!an_any) an_t0 = plat_ms(); an_any = true; }
static void anim(void) {
    if (AR[0].type == 2) {   /* load: v14 = 0 loaded, 1 missing */
        anim_halt_all(); free(ax); ax = arc_load(&A_SEQ, AR[0].s, &ax_len); V[14] = ax ? 0 : 1;
        if (!ax) { ax_len = 0; plat_log("no animation %s", AR[0].s); } return; }
    switch (A(0)) {
    case 1: anim_start((int)A(1), (int)A(2)); break;                                   /* start a stream */
    case 2: if (ax) { an_t *s = &AN[A(1) % 10][A(2) & 31]; if (s->state != AN_HALT) s->state = AN_STOPREQ; } break;
    case 3: anim_start((int)A(1), (int)A(2));                                          /* run one to its end (0x409c60) */
        while (!quit && AN[A(1) % 10][A(2) & 31].state != AN_HALT) { if (skip_mode) { anim_ticks(1); continue; } if (!frame()) break; }
        break;
    case 7: for (int a = 0; a < 10; a++) for (int b = 0; b < 32; b++) if (AN[a][b].state != AN_HALT) AN[a][b].state = AN_STOPREQ; break;
    case 10: case 11: anim_halt_all(); break;
    default: break;                                                                     /* 26 etc.: nothing in this version */
    }
}

/* ------------------------------------------------------------------ scene record (SYS 24, 0x43e010 -> 0x43d3e0)
   0 clear, 1 remove id, 4 set id = (name, 6 values), 5 id present -> v18, 6 name, 7 value. A loaded save redraws the
   scene from these (0x43f6c0): 0 room, 1-5 pictures, 8 .ax, 9 its sheet, 10 its stream, 14 music, 16-18 loops, 19 wave. */
#define NREC 32
typedef struct { uint16_t id; char name[64]; uint16_t p[6]; } rec_t;
static rec_t REC[NREC]; static int nrec;
static void rec_del(int id) { for (int i = 0; i < nrec; i++) if (REC[i].id == id) { memmove(&REC[i], &REC[i + 1], (size_t)(nrec - i - 1) * sizeof *REC); nrec--; i--; } }
static rec_t *rec_get(int id) { for (int i = 0; i < nrec; i++) if (REC[i].id == id) return &REC[i]; return NULL; }
static void rec_cmd(void) {
    int id = (int)(A(2) & 255); rec_t *r;
    switch (A(1)) {
    case 0: nrec = 0; break;
    case 1: rec_del(id); break;
    case 4: rec_del(id); if (nrec == NREC) { memmove(REC, REC + 1, (NREC - 1) * sizeof *REC); nrec--; }
        r = &REC[nrec++]; memset(r, 0, sizeof *r); r->id = (uint16_t)id; if (nar > 3 && AR[3].type == 2) snprintf(r->name, sizeof r->name, "%s", AR[3].s);
        for (int k = 0; k < 6; k++) r->p[k] = (uint16_t)A(4 + k); break;
    case 5: r = rec_get(id); V[18] = r ? r->id : 0xffff; break;
    case 7: r = rec_get(id); if (r) V[18] = r->p[A(3) % 6]; break;
    default: break;
    }
}
static void play_awf(int slot, const char *name, bool loop);
static void snd_start(int slot);
static void scene_restore(void) {   /* redraw what the records say, in their order */
    anim_halt_all(); wave_on = false; tl_clear(); ov_clear(); fade_level = 255;
    for (int k = 0; k < NSURF; k++) if (k != 6 && k != 7) { memset(S[k].p, 0, (size_t)S[k].w * S[k].h * 3); S[k].has_a = false; }
    for (int i = 0; i < nrec; i++) { rec_t *r = &REC[i]; int id = r->id; uint32_t keep = D[12];
        if (id == 0) { D[12] = 1; load_pic(r->name, -1, -1); blit(1, 0, 0, 639, 479, 2, 0, 0, 0); }
        else if (id >= 1 && id <= 5) { D[12] = 3; load_pic(r->name, -1, -1); int x = (int)D[8], y = (int)D[9], w = (int)D[10], h = (int)D[11];
            bool ha = S[3].has_a; blit(3, x, y, x + w - 1, y + h - 1, 1, x, y, 1); S[3].has_a = ha;
            if (id == 1) blit(3, x, y, x + w - 1, y + h - 1, 2, x, y, 1); }
        else if (id == 9) { D[12] = 8; load_pic(r->name, -1, -1); }
        else if (id == 8) { free(ax); ax = arc_load(&A_SEQ, r->name, &ax_len); if (!ax) ax_len = 0; }
        else if (id == 14 && r->name[0]) { play_awf(SND_BGM, r->name, true); snd_start(SND_BGM); }
        else if (id >= 16 && id <= 18 && r->name[0]) { play_awf(SND_SE + id - 16, r->name, true); snd_start(SND_SE + id - 16); }
        else if (id == 19) { wave_on = r->p[0] == 1; for (int j = 0; j < 480; j++) wave_tab[j] = (int)(cos(j * 0.026179938333333333) * 100.0); }
        D[12] = keep; }
    blit(1, 0, 0, 639, 479, 0, 0, 0, 0);
    for (int i = 0; i < nrec; i++) if (REC[i].id == 10) anim_start(0, REC[i].p[0]);
}

/* ------------------------------------------------------------------ saves (port's own: 20 slots + global flags)
   slot: script, message number (the script's own table, OP35), window, all variables, scene records, a line of text.
   Global (kept across games and loads, like the original's FLAG0000): endings seen 130-199, music 1500-1599, 2000- */
#define NSAVE 20
typedef struct { char magic[4]; char script[32]; int32_t msg; uint8_t win; uint32_t text_rgb, when; char label[96];
    uint8_t F[8192]; uint16_t W[4096]; uint32_t D[4096], V[32]; int32_t nrec; rec_t rec[NREC]; } save_t;

static bool global_flag(int i) { return (i >= 260 && i < 400) || (i >= 3000 && i < 3200) || (i >= 1500 && i < 1600) || (i >= 4000); }
static void sys_save(void) { static uint8_t g[4096]; for (int i = 0; i < 8192; i++) { int f = getF(i); if (i & 1) g[i >> 1] = (g[i >> 1] & 0xf0) | f; else g[i >> 1] = (g[i >> 1] & 0x0f) | f << 4; }
    plat_save_write("KSYS.DAT", g, sizeof g); }
static void sys_load(void) { static uint8_t g[4096]; if (!plat_save_read("KSYS.DAT", g, sizeof g)) return;
    for (int i = 0; i < 8192; i++) if (global_flag(i)) { int f = i & 1 ? g[i >> 1] & 15 : g[i >> 1] >> 4; if (f > getF(i)) setF(i, f); } }
static void save_name(char *b, int slot) { snprintf(b, 16, "KSAVE%02d.DAT", slot + 1); }
static bool save_peek(int slot, save_t *s) { char n[16]; save_name(n, slot); return plat_save_read(n, s, sizeof *s) && !memcmp(s->magic, "KWS1", 4); }
static bool save_slot(int slot) {
    static save_t s; memset(&s, 0, sizeof s); memcpy(s.magic, "KWS1", 4); snprintf(s.script, sizeof s.script, "%s", cur ? cur->name : "");
    s.msg = msg_no; s.win = win_open; s.text_rgb = text_rgb; s.when = (uint32_t)time(NULL); snprintf(s.label, sizeof s.label, "%s", last_txt);
    memcpy(s.F, F, sizeof F); memcpy(s.W, W, sizeof W); memcpy(s.D, D, sizeof D); memcpy(s.V, V, sizeof V); s.nrec = nrec; memcpy(s.rec, REC, sizeof REC);
    char n[16]; save_name(n, slot); sys_save(); return plat_save_write(n, &s, sizeof s);
}
static bool load_slot(int slot) {
    static save_t s; if (!save_peek(slot, &s) || s.msg < 0) return false;
    for (int i = 0; i < 8192; i++) { int sv = (s.F[i >> 1] >> (i & 1 ? 0 : 4)) & 15; if (!global_flag(i) || sv > getF(i)) setF(i, sv); }
    memcpy(W, s.W, sizeof W); memcpy(D, s.D, sizeof D); memcpy(V, s.V, sizeof V); text_rgb = s.text_rgb;
    nrec = s.nrec < 0 || s.nrec > NREC ? 0 : s.nrec; memcpy(REC, s.rec, sizeof REC);
    for (int k = 0; k < SND_SLOTS; k++) plat_snd_stop(k);
    scene_restore(); win_open = s.win; frame();
    snprintf(goto_name, sizeof goto_name, "%s", s.script); resume_msg = s.msg; end_flag = 2; skip_mode = false;
    return true;
}

/* ------------------------------------------------------------------ the port's menu (B / + during text) */
static void ui_text(int x, int y, const char *t, uint32_t rgb) {
    uint32_t k12 = D[12], k0 = D[0], k1 = D[1], k2 = D[2], k4 = D[4], kc = text_rgb, k16 = D[16];
    D[12] = 0; D[0] = D[2] = (uint32_t)x; D[1] = (uint32_t)y; D[4] = 632; text_rgb = rgb; D[16] = 0; text_noframe = true;
    text((const uint8_t *)t); text_noframe = false;
    D[12] = k12; D[0] = k0; D[1] = k1; D[2] = k2; D[4] = k4; text_rgb = kc; D[16] = k16;
}
static void ui_dim(void) { for (int i = 0; i < 640 * 480; i++) { ov_a[i] = 170; ov_c[i * 3] = 0; ov_c[i * 3 + 1] = 0; ov_c[i * 3 + 2] = 16; } }
static int ui_n, ui_y0, ui_h; static const char *ui_lbl[24]; static const char *ui_head;
static int ui_row(int x, int y) { (void)x; int r = (y - ui_y0) / ui_h; return y >= ui_y0 && r < ui_n ? r : -1; }
static void ui_draw(int sel, bool pressed) {
    tl_clear(); if (ui_head) ui_text(48, ui_y0 - 32, ui_head, 0xffff80);
    for (int i = 0; i < ui_n; i++) ui_text(64, ui_y0 + i * ui_h + (ui_h - 16) / 2, ui_lbl[i], i == sel ? (pressed ? 0xff8080 : 0x00ffff) : 0xffffff);
}
static int ui_list(const char *head, const char **lbl, int n, int sel) {   /* -1 = cancelled */
    ui_head = head; ui_n = n; ui_h = n > 12 ? 20 : 32; ui_y0 = (480 - n * ui_h) / 2 + (head ? 12 : 0);
    for (int i = 0; i < n; i++) ui_lbl[i] = lbl[i];
    while (!quit && (key_down(1) || key_down(0))) frame();
    int px, py, lx = -1, ly = -1; bool pu = true, pd = true, held = true;
    for (;;) {
        if (quit) return -1;
        if (key_down(1)) { while (!quit && key_down(1)) frame(); return -1; }
        plat_cursor(&px, &py, false); if (px != lx || py != ly) { int r = ui_row(px, py); if (r >= 0) sel = r; lx = px; ly = py; }
        bool up = key_down(2), dn = key_down(3), ok = key_down(0);
        if (up && !pu) sel = (sel + n - 1) % n; if (dn && !pd) sel = (sel + 1) % n; pu = up; pd = dn;
        if (ok && !held) { ui_draw(sel, true); frame(); wait_ms(100); while (!quit && key_down(0)) frame(); return sel; }
        held = ok; ui_draw(sel, false); if (!frame()) return -1;
    }
}
static int slot_pick(bool saving) {
    static char lines[NSAVE][128]; const char *lbl[NSAVE]; static save_t s;
    for (int i = 0; i < NSAVE; i++) {
        if (save_peek(i, &s)) { time_t t = (time_t)s.when; struct tm *m = localtime(&t); char lb[40]; int n = 0;
            for (const uint8_t *p = (const uint8_t *)s.label; *p && n < 32; ) { int d = *p == 0xfd ? 3 : sjis_dbcs(*p) ? 2 : 1; if (n + d > 32) break; memcpy(lb + n, p, d); n += d; p += d; } lb[n] = 0;
            snprintf(lines[i], sizeof lines[i], "%02d %02d/%02d %02d:%02d %s", i + 1, m ? m->tm_mon + 1 : 0, m ? m->tm_mday : 0, m ? m->tm_hour : 0, m ? m->tm_min : 0, lb); }
        else snprintf(lines[i], sizeof lines[i], "%02d " "\x81\x7c\x81\x7c\x20\x8b\xf3\x82\xab\x20\x81\x7c\x81\x7c", i + 1);
        lbl[i] = lines[i]; }
    return ui_list(saving ? "\x82\xc7\x82\xb1\x82\xc9\x83\x5a\x81\x5b\x83\x75\x82\xb5\x82\xdc\x82\xb7\x82\xa9\x81\x48" : "\x82\xc7\x82\xea\x82\xf0\x83\x8d\x81\x5b\x83\x68\x82\xb5\x82\xdc\x82\xb7\x82\xa9\x81\x48", lbl, NSAVE, 0);
}
static bool ui_confirm(void) { const char *l[2] = { "\x82\xcd\x82\xa2", "\x82\xa2\x82\xa2\x82\xa6" }; return ui_list("\x82\xe6\x82\xeb\x82\xb5\x82\xa2\x82\xc5\x82\xb7\x82\xa9\x81\x48", l, 2, 1) == 0; }
static void ui_close(void) { tl_clear(); ov_clear(); }
/* returns true when the game moved elsewhere (a load or back to the title) */
static bool load_menu(void) {
    ui_dim(); for (;;) { int sl = slot_pick(false); if (sl < 0) break; save_t *s = malloc(sizeof *s); bool ok = save_peek(sl, s); free(s);
        if (ok && ui_confirm()) { ui_close(); if (load_slot(sl)) return true; ui_dim(); } }
    ui_close(); frame(); return false;
}
static bool sys_menu(void) {
    uint32_t keep_rgb = text_rgb; uint8_t *ka = malloc(sizeof tl_a), *kc = malloc(sizeof tl_c); memcpy(ka, tl_a, sizeof tl_a); memcpy(kc, tl_c, sizeof tl_c);
    bool moved = false; int sel = 0;
    for (;;) {
        static char l2[64], l3[64]; snprintf(l2, sizeof l2, "%s%s", "\x8a\xf9\x93\xc7\x83\x58\x83\x4c\x83\x62\x83\x76", skip_mode ? "\x81\x46\x82\x6e\x82\x6d" : "\x81\x46\x82\x6e\x82\x65\x82\x65"); snprintf(l3, sizeof l3, "%s%s", "\x83\x49\x81\x5b\x83\x67\x83\x82\x81\x5b\x83\x68", auto_mode ? "\x81\x46\x82\x6e\x82\x6d" : "\x81\x46\x82\x6e\x82\x65\x82\x65");
        const char *lbl[6] = { "\x83\x5a\x81\x5b\x83\x75", "\x83\x8d\x81\x5b\x83\x68", l2, l3, "\x83\x5e\x83\x43\x83\x67\x83\x8b\x82\xc9\x96\xdf\x82\xe9", "\x96\xdf\x82\xe9" };
        ui_dim(); sel = ui_list(NULL, lbl, 6, sel);
        if (sel < 0 || sel == 5) break;
        if (sel == 0) { int sl = slot_pick(true); if (sl >= 0) { save_t *s = malloc(sizeof *s); bool used = save_peek(sl, s); free(s); if (!used || ui_confirm()) save_slot(sl); } }
        else if (sel == 1) { if (load_menu()) { moved = true; break; } }
        else if (sel == 2) { skip_mode = !skip_mode; if (skip_mode) break; }
        else if (sel == 3) { auto_mode = !auto_mode; if (auto_mode) break; }
        else if (sel == 4) { if (ui_confirm()) { anim_halt_all(); wave_on = false; for (int k = 0; k < SND_SLOTS; k++) plat_snd_stop(k); sys_save();
                win_open = false; ui_close(); snprintf(goto_name, sizeof goto_name, "start.mes"); resume_msg = -1; end_flag = 2; skip_mode = auto_mode = false; moved = true; break; } }
    }
    ui_close(); if (!moved) { memcpy(tl_a, ka, sizeof tl_a); memcpy(tl_c, kc, sizeof tl_c); } free(ka); free(kc); text_rgb = keep_rgb; frame();
    return moved;
}

/* OP14: choices. Each item's code sets v16 and prints its label (TXT) - run them in capture mode, show the labels in the window */
static int ch_n, ch_val[16]; static char ch_lbl[16][128];
static int ch_row(int x, int y) { (void)x; for (int i = 0; i < ch_n; i++) if (y >= (int)D[3] + i * 20 && y < (int)D[3] + i * 20 + 18) return i; return -1; }
static void ch_draw(int sel, bool pressed) {
    tl_clear();
    for (int i = 0; i < ch_n; i++) { D[0] = D[2] + 16; D[1] = D[3] + i * 20; uint32_t keep = text_rgb;
        text_rgb = i == sel ? (pressed ? 0xff8080 : 0x00ffff) : 0xffffff; uint32_t sp = D[16]; D[16] = 0; capture = false; text_noframe = true; text((const uint8_t *)ch_lbl[i]); text_noframe = false; D[16] = sp; text_rgb = keep; }
}
static void menu_run(void) {
    menu_t *m = &menus[A(0) & 15]; ch_n = 0;
    for (int k = 0; k < m->n; k++) {
        if (m->cond[k]) { script_t *os = cur; uint32_t op = pc; cur = m->s; pc = m->cond[k]; uint32_t c = expr(); cur = os; pc = op; if (!c) continue; }
        capture = true; cap_n = 0; cap_txt[0][0] = 0; run_at(m->s, m->addr[k]); capture = false;
        snprintf(ch_lbl[ch_n], 128, "%s", cap_txt[0]); ch_val[ch_n] = V[16]; ch_n++;
    }
    if (!ch_n) { V[18] = 0; return; }
    int r;
#ifdef K_HEADLESS_RAND                         /* test driver: random choices to walk every branch */
    { extern int khl_choice(int n); r = khl_choice(ch_n); }
#else
    r = pick(ch_n, 0, ch_row, ch_draw);
#endif
    V[18] = r + 1; V[16] = ch_val[r];
    tl_clear(); D[0] = D[2] + 16; D[1] = D[3]; frame(); plat_log("choice -> %u", V[18]);
}

bool k_init(void) {
    if (!arc_open(&A_MES, 40) || !arc_open(&A_GCC, 40)) return false;
    arc_open(&A_SEQ, 40); arc_open(&A_VOICE, 40); for (int i = 0; i < 4; i++) arc_open(&A_AWF[i], 52);
    surf_init();
    font_len = plat_file_size("kfont16.bin"); if (font_len) { font = malloc(font_len); plat_read_at("kfont16.bin", 0, font, font_len); }
    D[16] = 0; sys_load(); return true;
}
void k_run(const char *first) {
    snprintf(goto_name, sizeof goto_name, "%s", first);
    while (!quit && goto_name[0]) {
        script_t *s = load_script(goto_name); goto_name[0] = 0; if (!s) break;
        plat_log("script %s", s->name);
        cur = s; pc = 0; end_flag = 0;
        if (resume_msg >= 0 && (uint32_t)resume_msg < rd32(s->buf)) pc = rd32(s->buf + 4 + 4 * resume_msg); resume_msg = -1;
        sys_save(); run();
        if (end_flag == 2) { end_flag = 0; continue; }
    }
}
