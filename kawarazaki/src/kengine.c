/* 河原崎家の一族 (elf, Windows "AI.exe") - clean reimplementation of the script engine for console ports.
 * Byte code: see kawarazaki/NOTES.md. Addresses in comments refer to the original AI.exe. */
#include "kai.h"
#include "gccdec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ------------------------------------------------------------------ archives */
typedef struct { char name[33]; uint32_t off, size; } k_ent;
typedef struct { const char *file; int n; k_ent *e; } k_arc;
static k_arc A_MES = {"mes.ARC"}, A_GCC = {"gcc.ARC"}, A_SEQ = {"sequence.ARC"}, A_VOICE = {"voice.ARC"};
static k_arc A_AWF[4] = {{"bgm.AWF"}, {"effect.AWF"}, {"effect2.AWF"}, {"effect3.AWF"}};
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static bool arc_open(k_arc *a, int esz) {
    uint8_t h[4]; if (!plat_read_at(a->file, 0, h, 4)) { plat_log("missing %s", a->file); return false; }
    a->n = rd32(h); uint8_t *t = malloc((size_t)a->n * esz); if (!plat_read_at(a->file, 4, t, a->n * esz)) return false;
    a->e = calloc(a->n, sizeof(k_ent));
    for (int i = 0; i < a->n; i++) { memcpy(a->e[i].name, t + i * esz, 32); a->e[i].off = rd32(t + i * esz + 32); a->e[i].size = rd32(t + i * esz + 36); }
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
static void present(void) {
    const surf_t *s = &S[0];
    for (int y = 0; y < 480; y++) {
        const uint8_t *src = s->p + (size_t)y * s->w * 3; uint8_t *d = scr + y * 640 * 3;
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
    for (int i = 0; i < 640 * 480; i++) { int a = tl_a[i]; if (!a) continue; uint8_t *d = scr + i * 3, *c = tl_c + i * 3;
        d[0] = (c[0] * a + d[0] * (255 - a)) / 255; d[1] = (c[1] * a + d[1] * (255 - a)) / 255; d[2] = (c[2] * a + d[2] * (255 - a)) / 255; }
}
static bool frame(void) { present(); compose(); if (!plat_frame(scr, 640, 480)) { quit = true; return false; } return true; }
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
static void text(const uint8_t *t) {
    int di = D[12] & 15; int cw = D[17] ? (int)D[17] : 16, lh = D[18] ? (int)D[18] : 16;
    while (*t && !quit) {
        if (t[0] == 0x81 && t[1] == 0x93) { D[0] = D[2]; D[1] += lh; t += 2; continue; }   /* '％' = new line */
        int dbcs = sjis_dbcs(*t), w = dbcs ? cw : cw / 2; const uint8_t *cell = NULL;
        if (font) {
            if (dbcs) { int l = t[0], tr = t[1]; int li = l <= 0x9f ? l - 0x81 : l - 0xe0 + 31;
                if (li >= 0 && li < 47 && tr >= 0x40 && tr <= 0xfc) cell = font + 4 + 256 * 128 + ((size_t)li * 189 + (tr - 0x40)) * 256; }
            else cell = font + 4 + *t * 128;
        }
        if (D[0] + w - 1 > D[4]) { D[0] = D[2]; D[1] += lh; }
        if (cell) draw_glyph(di, D[0], D[1], cell, dbcs ? 16 : 8);
        D[0] += w; t += dbcs ? 2 : 1;
        if (!skip_mode && D[16]) { if (wait_ms(D[16])) skip_mode = skip_mode; }
    }
    frame();
}

/* ------------------------------------------------------------------ sound */
static void play_awf(int slot, const char *name, bool loop) {
    for (int i = 0; i < 4; i++) { k_ent *e = arc_find(&A_AWF[i], name); if (!e) continue;
        uint8_t *b = malloc(e->size); if (!plat_read_at(A_AWF[i].file, e->off, b, e->size)) { free(b); return; }
        plat_snd_play(slot, b, e->size, 255, loop); return; }
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
static void menu_run(void);
static void wait_msg(void);
static int end_flag;
static void run(void) {
    while (!quit) {
        if (pc >= cur->len - cur->base) { plat_log("%s: ran off the end", cur->name); end_flag = 1; }
        if (end_flag) { if (end_flag == 1) end_flag = 0; return; }
        uint32_t at = pc; uint8_t op = cb();
        if (getenv("K_TRACE")) plat_log("%s %05x op %02x", cur->name, at, op);
        switch (op) {
        case 0x00: { const uint8_t *t = cur->buf + cur->base + pc; size_t n = strlen((const char *)t); text(t); pc += n + 1; break; }
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
    if (!skip_mode) wait_click();
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
    case 24: break;   /* scene record for save/restore (TODO) */
    case 37: V[18] = getenv("K_TITLE") ? atoi(getenv("K_TITLE")) : 1; plat_log("title menu -> %u (stub)", V[18]); break;
    default: plat_log("sys %u (%u %u %u)", f, A(1), A(2), A(3));
    }
}
static void sound_ctl(void) { plat_log("snd %u %u %u", A(0), A(1), A(2)); }
static void anim(void) { if (AR[0].type == 2) plat_log("anim %s %u", AR[0].s, A(1)); else plat_log("op2e %u %u %u", A(0), A(1), A(2)); }
static void menu_run(void) {
    menu_t *m = &menus[A(0) & 15]; plat_log("menu %u (%d items) -> first", A(0), m->n);
    for (int k = 0; k < m->n; k++) {
        if (m->cond[k]) { script_t *os = cur; uint32_t op = pc; cur = m->s; pc = m->cond[k]; uint32_t c = expr(); cur = os; pc = op; if (!c) continue; }
        run_at(m->s, m->addr[k]); break;
    }
}

bool k_init(void) {
    if (!arc_open(&A_MES, 40) || !arc_open(&A_GCC, 40)) return false;
    arc_open(&A_SEQ, 40); arc_open(&A_VOICE, 40); for (int i = 0; i < 4; i++) arc_open(&A_AWF[i], 52);
    surf_init();
    font_len = plat_file_size("kfont16.bin"); if (font_len) { font = malloc(font_len); plat_read_at("kfont16.bin", 0, font, font_len); }
    D[16] = 0; return true;
}
void k_run(const char *first) {
    snprintf(goto_name, sizeof goto_name, "%s", first);
    while (!quit && goto_name[0]) {
        script_t *s = load_script(goto_name); goto_name[0] = 0; if (!s) break;
        plat_log("script %s", s->name);
        cur = s; pc = 0; end_flag = 0; run();
        if (end_flag == 2) { end_flag = 0; continue; }
    }
}
