/* Port addition: the DOS release's 16-colour pictures (GP4 files in SILK0..4.DAT) in place of the Windows pictures.
 *
 * 308 of the DOS pictures have the same name as a Windows one (G05.GP4 <-> G05.GCC).  With the DOS look switched on, a
 * picture that has a DOS twin is drawn from the DOS file instead: the DOS screen is 640x400 and was shown on a 4:3 monitor,
 * so it is stretched to 640x480 (rows x1.2).  Where the Windows picture fills the whole screen, the DOS frame (WAKU1) is drawn
 * around the DOS picture, which then sits where it was on the DOS screen.  Character sprites are opaque on the DOS release
 * (black around them), so the DOS version of a masked Windows sprite gets a full mask.
 *
 * SILK*.DAT: text header, u32 table size at 0x1F4, table at 0x1F8 coded as a XOR chain (plain[i] = enc[i] ^ enc[i-1], the byte
 * before the first being 0xAA), 24-byte entries {name[16], u32 offset, u32 size}, data from 0x1F8 + table size.
 * GP4: see dosgfx_decode below (a reimplementation of the DOS engine's routine). */
#include "ai5.h"
#include "gccdec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct { char name[16]; const uint8_t *data; uint32_t size; } DosEnt;
static DosEnt *ents; static int nents; static int tried;

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void load_all(void) {
    if (tried) return; tried = 1;
    for (int k = 0; k < 5; k++) {
        char fn[16]; snprintf(fn, sizeof fn, "SILK%d.DAT", k);
        size_t n; uint8_t *d = plat_read_file(fn, &n);
        if (!d) { snprintf(fn, sizeof fn, "silk%d.dat", k); d = plat_read_file(fn, &n); }
        if (!d || n < 0x200) { free(d); continue; }
        uint32_t ts = le32(d + 0x1F4); if (ts % 24 || 0x1F8 + (size_t)ts > n) { free(d); continue; }
        uint8_t *t = malloc(ts); if (!t) { free(d); continue; }
        uint8_t prev = 0xAA; for (uint32_t i = 0; i < ts; i++) { t[i] = d[0x1F8 + i] ^ prev; prev = d[0x1F8 + i]; }
        uint32_t base = 0x1F8 + ts, cnt = ts / 24;
        DosEnt *ne = realloc(ents, sizeof *ents * (nents + cnt)); if (!ne) { free(t); free(d); continue; } ents = ne;
        for (uint32_t i = 0; i < cnt; i++) {
            const uint8_t *e = t + i * 24; uint32_t off = le32(e + 16), sz = le32(e + 20);
            if ((size_t)base + off + sz > n) continue;
            DosEnt *x = &ents[nents++]; memcpy(x->name, e, 15); x->name[15] = 0; x->data = d + base + off; x->size = sz;
        }
        free(t);                         /* the archive itself stays in memory: entries point into it */
    }
    plat_log("DOS pictures: %d files in SILK*.DAT", nents);
}
int dosgfx_available(void) { load_all(); return nents > 0; }

static const DosEnt *find(const char *gccname) {
    const char *b = strrchr(gccname, '\\'); if (b) gccname = b + 1;
    char want[20]; int i = 0; for (; gccname[i] && gccname[i] != '.' && i < 12; i++) want[i] = gccname[i]; want[i] = 0;
    strcat(want, ".GP4");
    for (int k = 0; k < nents; k++) if (!strcasecmp(ents[k].name, want)) return &ents[k];
    return NULL;
}

/* ---- GP4 decoder: 4-pixel columns, left to right; each row of a column is a literal (4 colours, move-to-front coded
 * against the previous colour) or a copy of N rows from the same column or k columns back ---- */
typedef struct { const uint8_t *d; uint32_t n, p; int left; uint8_t cur; } Bits;
static int bit(Bits *b) { if (!b->left) { b->cur = b->p < b->n ? b->d[b->p] : 0; b->p++; b->left = 8; } b->left--; return (b->cur >> b->left) & 1; }
static int bits(Bits *b, int n) { int v = 0; while (n--) v = v << 1 | bit(b); return v; }
typedef struct { int x, y, w, h, W; uint8_t pal[16][3]; uint8_t *idx; } Gp4;
static int gp4_decode(const uint8_t *d, uint32_t n, Gp4 *g) {
    if (n < 40) return -1;
    g->x = d[0] << 8 | d[1]; g->y = d[2] << 8 | d[3]; g->w = (d[4] << 8 | d[5]) + 1; g->h = (d[6] << 8 | d[7]) + 1;
    if (g->w > 640 || g->h > 400 || g->w < 1 || g->h < 1) return -1;
    for (int i = 0; i < 16; i++) { int v = d[8 + 2 * i] << 8 | d[9 + 2 * i];
        g->pal[i][0] = ((v >> 7) & 15) * 17; g->pal[i][1] = ((v >> 12) & 15) * 17; g->pal[i][2] = ((v >> 2) & 15) * 17; }
    int ncol = ((g->w + 7) >> 3) * 2, h = g->h; g->W = ncol * 4;
    g->idx = calloc((size_t)g->W * h, 1); if (!g->idx) return -1;
    uint8_t mtf[17][16]; for (int j = 0; j < 17; j++) for (int i = 0; i < 16; i++) mtf[j][i] = (uint8_t)((j + i) & 15);
    Bits b = { d, n, 40, 0, 0 };
    for (int c = 0; c < ncol; c++) {
        int prev = 16, r = 0;
        while (r < h) {
            uint8_t *row = g->idx + (size_t)r * g->W + c * 4;
            if (!bit(&b)) {
                for (int k = 0; k < 4; k++) { uint8_t *t = mtf[prev]; int j = 0; while (bit(&b) && j < 15) j++;
                    uint8_t v = t[j]; if (j) { memmove(t + 1, t, (size_t)j); t[0] = v; } row[k] = v; prev = v; }
                r++; continue;
            }
            int back, a, cnt;
            if (!bit(&b)) back = 1; else if (!bit(&b)) back = 0; else { back = 2; while (bit(&b)) back++; }
            if (back == 0) { a = bits(&b, 3) - 8; if (a <= -7) a = a == -8 ? -16 : -8; } else a = bits(&b, 4) - 8;
            if (!bit(&b)) cnt = 2 + bit(&b); else if (!bit(&b)) cnt = 4 + bits(&b, 2); else if (!bit(&b)) cnt = 8 + bits(&b, 3);
            else { cnt = 16 + bits(&b, 6); if (cnt == 79) { int hi = bits(&b, 2); cnt = (hi << 8 | bits(&b, 8)) + 79; } }
            int sc = c - back;
            for (int i = 0; i < cnt && r < h; i++, r++) {
                int sr = r + a; uint8_t *dst = g->idx + (size_t)r * g->W + c * 4;
                if (sc >= 0 && sr >= 0 && sr < h) memcpy(dst, g->idx + (size_t)sr * g->W + sc * 4, 4); else memset(dst, 0, 4);
            }
        }
    }
    return 0;
}

/* The DOS pictures are dithered 16-colour art made for a 400-line screen on a CRT.  Shown pixel-sharp and stretched by
 * repeating every fifth row, the dither patterns turn into stripes and odd colours.  So the picture is first softened the
 * way a CRT blends neighbouring dots (3x3 [1 2 1] filter, transparent dots left out), then stretched to 480 rows with
 * linear interpolation. */
static uint8_t *smooth(const Gp4 *g, int sx, int sy, int w, int h, const uint8_t *transp, int key8) {
    uint8_t *o = malloc((size_t)w * h * 4); if (!o) return NULL;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        int r = 0, gg = 0, b = 0, wsum = 0, gy0 = sy + y - g->y, gx0 = sx + x - g->x; uint8_t *d = o + ((size_t)y * w + x) * 4;
        int opaque = 0;
        if (gy0 >= 0 && gy0 < g->h && gx0 >= 0 && gx0 < g->W) { size_t i = (size_t)gy0 * g->W + gx0;
            opaque = !(transp && transp[i]) && !(key8 && (g->idx[i] & 15) == 8); }
        if (!opaque) { d[0] = d[1] = d[2] = d[3] = 0; continue; }
        for (int ky = -1; ky <= 1; ky++) for (int kx = -1; kx <= 1; kx++) {
            int yy = gy0 + ky, xx = gx0 + kx; if (yy < 0 || yy >= g->h || xx < 0 || xx >= g->w) continue;
            if (y + ky < 0 || y + ky >= h || x + kx < 0 || x + kx >= w) continue;     /* stay inside the block (animation cells sit side by side) */
            size_t i = (size_t)yy * g->W + xx; if ((transp && transp[i]) || (key8 && (g->idx[i] & 15) == 8)) continue;
            int wt = (ky ? 1 : 2) * (kx ? 1 : 2); const uint8_t *c = g->pal[g->idx[i] & 15];
            r += c[0] * wt; gg += c[1] * wt; b += c[2] * wt; wsum += wt; }
        d[0] = (uint8_t)(r / wsum); d[1] = (uint8_t)(gg / wsum); d[2] = (uint8_t)(b / wsum); d[3] = 255;
    }
    return o;
}
/* rows x1.2: source row position for target row ty (DOS y = dy at the top of the block) */
static void put_scaled(const uint8_t *rgba, int w, int h, int dx, int dy, void (*put)(void *ctx, int x, int y, const uint8_t *rgb), void *ctx, void (*putalpha)(void *ctx, int x, int y, int a)) {
    int y0 = dy * 6 / 5, y1 = (dy + h) * 6 / 5;
    for (int ty = y0; ty < y1; ty++) {
        float fy = (ty + 0.5f) * 5.0f / 6.0f - 0.5f - (float)dy; int i0 = (int)(fy < 0 ? 0 : fy); float t = fy - (float)i0;
        if (i0 >= h - 1) { i0 = h - 1; t = 0; } if (t < 0) t = 0;
        const uint8_t *a = rgba + (size_t)i0 * w * 4, *b = rgba + (size_t)(i0 + (i0 < h - 1)) * w * 4;
        for (int x = 0; x < w; x++) {
            const uint8_t *p = a + x * 4, *q = b + x * 4; uint8_t c[3]; int ap = p[3], aq = q[3];
            if (!ap && !aq) { if (putalpha) putalpha(ctx, dx + x, ty, 0); continue; }
            float u = ap && aq ? t : ap ? 0.0f : 1.0f;
            for (int k = 0; k < 3; k++) c[k] = (uint8_t)(p[k] + (q[k] - p[k]) * u + 0.5f);
            put(ctx, dx + x, ty, c);
            if (putalpha) putalpha(ctx, dx + x, ty, (t < 0.5f ? ap : aq) ? 255 : 0);
        }
    }
}
typedef struct { uint8_t *bgr, *alpha; int cw, ch, ox, oy; } Canvas;
static void cv_put(void *ctx, int x, int y, const uint8_t *rgb) { Canvas *c = ctx; x -= c->ox; y -= c->oy;
    if (x < 0 || y < 0 || x >= c->cw || y >= c->ch) return; uint8_t *d = c->bgr + ((size_t)(c->ch - 1 - y) * c->cw + x) * 3; d[0] = rgb[2]; d[1] = rgb[1]; d[2] = rgb[0]; }
static void cv_alpha(void *ctx, int x, int y, int a) { Canvas *c = ctx; x -= c->ox; y -= c->oy;
    if (!c->alpha || x < 0 || y < 0 || x >= c->cw || y >= c->ch) return; c->alpha[(size_t)(c->ch - 1 - y) * c->cw + x] = (uint8_t)a; }
static void paint(const Gp4 *g, const uint8_t *transp, Canvas *cv) {
    uint8_t *rgba = smooth(g, g->x, g->y, g->w, g->h, transp, 0); if (!rgba) return;
    put_scaled(rgba, g->w, g->h, g->x, g->y, cv_put, cv, cv->alpha ? cv_alpha : NULL);
    free(rgba);
}
/* black connected to the edge of a sprite (the DOS sprites are drawn opaque on black) */
static uint8_t *edge_black(const Gp4 *g) {
    int W = g->W, H = g->h; uint8_t *vis = calloc((size_t)W * H, 1); int *st = malloc(sizeof(int) * (size_t)W * H);
    if (!vis || !st) { free(vis); free(st); return NULL; }
    #define BLACK(i) (g->pal[g->idx[i] & 15][0] + g->pal[g->idx[i] & 15][1] + g->pal[g->idx[i] & 15][2] == 0)
    int sp = 0;
    for (int x = 0; x < g->w; x++) { int a = x, b2 = (H - 1) * W + x; if (BLACK(a) && !vis[a]) { vis[a] = 1; st[sp++] = a; } if (BLACK(b2) && !vis[b2]) { vis[b2] = 1; st[sp++] = b2; } }
    for (int y = 0; y < H; y++) { int a = y * W, b2 = y * W + g->w - 1; if (BLACK(a) && !vis[a]) { vis[a] = 1; st[sp++] = a; } if (BLACK(b2) && !vis[b2]) { vis[b2] = 1; st[sp++] = b2; } }
    while (sp) { int i = st[--sp], x = i % W, y = i / W;
        int nb[4] = { x > 0 ? i - 1 : -1, x + 1 < g->w ? i + 1 : -1, y > 0 ? i - W : -1, y + 1 < H ? i + W : -1 };
        for (int k = 0; k < 4; k++) { int j = nb[k]; if (j >= 0 && !vis[j] && BLACK(j)) { vis[j] = 1; st[sp++] = j; } } }
    #undef BLACK
    free(st); return vis;
}

/* Build the DOS version of a Windows picture.  xw, xh, xmask: the Windows picture's size and whether it has a mask. */
int dosgfx_make(const char *gccname, int xw, int xh, int xmask, gcc_image *im) {
    load_all(); if (!nents) return -1;
    const char *b = strrchr(gccname, '\\'); b = b ? b + 1 : gccname;
    if (!strncasecmp(b, "MWAKU", 5)) return -1;                 /* the Windows window/menu parts sheet is something else */
    size_t bl = strcspn(b, "."); if (bl >= 2 && !strncasecmp(b + bl - 2, "AN", 2)) return -1;   /* animation sheets: layouts differ */
    const DosEnt *e = find(b); if (!e) return -1;
    Gp4 g; if (gp4_decode(e->data, e->size, &g)) return -1;
    memset(im, 0, sizeof *im);
    int full = xw >= 600 && xh >= 440;
    if (full) {
        im->x = 0; im->y = 0; im->w = 640; im->h = 480;
        im->bgr = calloc(640 * 480, 3); if (!im->bgr) { free(g.idx); return -1; }
        Canvas cv = { im->bgr, NULL, 640, 480, 0, 0 };
        if (g.y >= 8 && g.y + g.h <= 300) {          /* a picture of the framed picture area (not a full-screen one like the title) */
            const DosEnt *w = find("WAKU1.GCC"); Gp4 f;
            if (w && !gp4_decode(w->data, w->size, &f)) { paint(&f, NULL, &cv); free(f.idx); }
        }
        paint(&g, NULL, &cv);
        if (xmask) { im->has_alpha = 1; im->ax = 0; im->ay = 0; im->aw = im->w; im->ah = im->h;
            im->alpha = malloc((size_t)im->aw * im->ah); if (im->alpha) memset(im->alpha, 255, (size_t)im->aw * im->ah); else im->has_alpha = 0; }
    } else {
        int y0 = g.y * 6 / 5, y1 = (g.y + g.h) * 6 / 5;
        im->x = g.x; im->y = y0; im->w = g.w; im->h = y1 - y0;
        im->bgr = calloc((size_t)im->w * im->h, 3); if (!im->bgr) { free(g.idx); return -1; }
        uint8_t *vis = xmask ? edge_black(&g) : NULL;
        if (xmask && vis) { im->has_alpha = 1; im->ax = im->x; im->ay = im->y; im->aw = im->w; im->ah = im->h;
            im->alpha = calloc((size_t)im->aw * im->ah, 1); if (!im->alpha) im->has_alpha = 0; }
        Canvas cv = { im->bgr, im->has_alpha ? im->alpha : NULL, im->w, im->h, im->x, im->y };
        paint(&g, vis, &cv);
        free(vis);
    }
    free(g.idx);
    return 0;
}

/* ---- DOS animations (.S4 + <name>AN.GP4 sheet), played in place of the Windows ones while the DOS pictures are on ----
 * S4: u8 n (=10 streams), n x u16 stream start offsets, then 33-byte draw entries {u8 type|flags, u8 sx/8, u16 sy, u8 sx2/8,
 * u16 sy2, u8 dx/8, u16 dy, ...}.  Stream bytes: 01 stop here if asked to, 02 n wait n ticks, 03 restart, 04/FF end,
 * 05 n / 06 and 07 n / 08 loops, >= 0x14 draw entry (op - 0x14).  Type 0x10/0x30 copy, 0x20 copy with colour 8 transparent;
 * flags bit 1: source is the sheet (else the screen), bit 0: destination is the sheet (ignored).  The DOS engine ticks at the
 * music driver hook INT D1, measured 36 Hz. */
enum { DA_SLOTS = 10, DA_STREAMS = 10 };
typedef struct { uint8_t state, wait, l1, l2; uint16_t start, ip, l1ip, l2ip; } DaStream;
static struct { char name[16]; const uint8_t *s4; uint32_t n4; Gp4 sheet; int ok; DaStream st[DA_STREAMS]; } da[DA_SLOTS];
static double da_acc;

void dosgfx_anim_reset(void) { for (int a = 0; a < DA_SLOTS; a++) for (int b = 0; b < DA_STREAMS; b++) da[a].st[b].state = 0; }
static int base_of(const char *fn, char *out) {
    const char *b = strrchr(fn, '\\'); b = b ? b + 1 : fn; int i = 0;
    for (; b[i] && b[i] != '.' && i < 10; i++) out[i] = (char)(b[i] >= 'a' && b[i] <= 'z' ? b[i] - 32 : b[i]); out[i] = 0; return i;
}
/* the Windows engine loaded animation file axname into slot a */
void dosgfx_anim_slot(int a, const char *axname) {
    if (a < 0 || a >= DA_SLOTS) return; load_all();
    char base[16]; base_of(axname, base);
    if (da[a].ok && !strcmp(da[a].name, base)) return;
    free(da[a].sheet.idx); memset(&da[a], 0, sizeof da[a]); snprintf(da[a].name, sizeof da[a].name, "%s", base);
    char fn[24]; const DosEnt *s = NULL, *g = NULL;
    snprintf(fn, sizeof fn, "%s.S4", base); for (int k = 0; k < nents; k++) if (!strcasecmp(ents[k].name, fn)) s = &ents[k];
    snprintf(fn, sizeof fn, "%sAN.GP4", base); for (int k = 0; k < nents; k++) if (!strcasecmp(ents[k].name, fn)) g = &ents[k];
    if (!s || !g || s->size < 21 || gp4_decode(g->data, g->size, &da[a].sheet)) return;
    da[a].s4 = s->data; da[a].n4 = s->size; da[a].ok = 1; plat_log("DOS animation %s in slot %d", base, a);
}
void dosgfx_anim_ctl(int a, int b, int sub) {
    if (sub == 6) { for (int i = 0; i < DA_SLOTS; i++) for (int j = 1; j < DA_STREAMS; j++) if (da[i].st[j].state) da[i].st[j].state = 1; return; }
    if (sub == 7) { dosgfx_anim_reset(); return; }
    if (a < 0 || a >= DA_SLOTS || b < 0 || b >= DA_STREAMS || !da[a].ok) return;
    DaStream *s = &da[a].st[b]; const uint8_t *d = da[a].s4; if (getenv("AI5_ANIMLOG")) plat_log("dos ctl slot %d stream %d sub %d", a, b, sub);
    switch (sub) {
    case 0: { int n = d[0]; uint16_t o = b < n ? (uint16_t)(d[1 + 2 * b] | d[2 + 2 * b] << 8) : 0; memset(s, 0, sizeof *s); s->start = s->ip = o; break; }
    case 1: case 4: case 5: s->state = 0xFF; s->ip = s->start; s->wait = 0; break;
    case 2: if (s->state) s->state = 1; break;
    case 3: s->state = 0; break;
    }
}
typedef void (*da_put_fn)(int x, int y, const uint8_t rgb[3]);
typedef int (*da_get_fn)(int x, int y, uint8_t rgb[3]);
static void da_put_wrap(void *ctx, int x, int y, const uint8_t *rgb) { ((da_put_fn)ctx)(x, y, rgb); }
static void da_draw(int a, int e, da_put_fn put) {
    const uint8_t *d = da[a].s4; uint32_t off = 1 + 2u * d[0] + 33u * (uint32_t)e; if (off + 10 > da[a].n4) return;
    const uint8_t *r = d + off; int type = r[0] & 0xF0, fl = r[0] & 15;
    if (type != 0x10 && type != 0x20 && type != 0x30) return;      /* palette changes and the rest are not reproduced */
    if (!(fl & 2) || (fl & 1)) return;                              /* only sheet -> screen */
    int sx = r[1] * 8, sy = r[2] | r[3] << 8, w = (r[4] - r[1] + 1) * 8, h = (r[5] | r[6] << 8) - sy + 1, dx = r[7] * 8, dy = r[8] | r[9] << 8;
    const Gp4 *g = &da[a].sheet; if (w <= 0 || h <= 0) return;
    if (getenv("AI5_ANIMLOG")) plat_log("dos draw %d e%d %d,%d %dx%d -> %d,%d", a, e, sx, sy, w, h, dx, dy);
    uint8_t *rgba = smooth(g, sx, sy, w, h, NULL, type == 0x20); if (!rgba) return;
    put_scaled(rgba, w, h, dx, dy, da_put_wrap, (void *)put, NULL);
    free(rgba);
}
static void da_tick_one(int a, DaStream *s, da_put_fn put) {
    const uint8_t *d = da[a].s4; uint32_t n = da[a].n4;
    if (!s->state) return;
    if (s->wait) { s->wait--; return; }
    for (int guard = 0; guard < 256; guard++) {
        if (s->ip >= n) { s->state = 0; return; }
        uint8_t op = d[s->ip++];
        switch (op) {
        case 1: if (s->state == 1) s->state = 0; return;
        case 2: s->wait = s->ip < n ? d[s->ip++] : 0; return;
        case 3: s->ip = s->start; return;
        case 4: case 0xFF: s->state = 0; return;
        case 5: s->l1 = s->ip < n ? d[s->ip++] : 0; s->l1ip = s->ip; return;
        case 6: if (--s->l1) s->ip = s->l1ip; return;
        case 7: s->l2 = s->ip < n ? d[s->ip++] : 0; s->l2ip = s->ip; return;
        case 8: if (--s->l2) s->ip = s->l2ip; return;
        default: if (op >= 0x14) da_draw(a, op - 0x14, put); else return; break;
        }
    }
}
/* one host frame of dt seconds */
void dosgfx_anim_frame(double dt, da_put_fn put) {
    da_acc += dt * 36.0; if (da_acc > 8) da_acc = 8;
    while (da_acc >= 1) { da_acc -= 1;
        for (int a = 0; a < DA_SLOTS; a++) if (da[a].ok) for (int b = 0; b < DA_STREAMS; b++) da_tick_one(a, &da[a].st[b], put); }
}
