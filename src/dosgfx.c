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

/* paint a decoded GP4 into a bottom-up BGR canvas cw x ch (rows x1.2) */
static void paint(const Gp4 *g, uint8_t *bgr, int cw, int ch, int ox, int oy) {
    int y0 = g->y * 6 / 5 - oy, y1 = (g->y + g->h) * 6 / 5 - oy;
    for (int ty = y0; ty < y1; ty++) {
        if (ty < 0 || ty >= ch) continue;
        int sy = (ty + oy) * 5 / 6 - g->y; if (sy < 0) sy = 0; if (sy >= g->h) sy = g->h - 1;
        const uint8_t *s = g->idx + (size_t)sy * g->W; uint8_t *d = bgr + (size_t)(ch - 1 - ty) * cw * 3;
        for (int x = 0; x < g->w; x++) { int tx = g->x + x - ox; if (tx < 0 || tx >= cw) continue;
            const uint8_t *c = g->pal[s[x] & 15]; d[tx * 3] = c[2]; d[tx * 3 + 1] = c[1]; d[tx * 3 + 2] = c[0]; }
    }
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
        if (g.y >= 8 && g.y + g.h <= 300) {          /* a picture of the framed picture area (not a full-screen one like the title) */
            const DosEnt *w = find("WAKU1.GCC"); Gp4 f;
            if (w && !gp4_decode(w->data, w->size, &f)) { paint(&f, im->bgr, 640, 480, 0, 0); free(f.idx); }
        }
        paint(&g, im->bgr, 640, 480, 0, 0);
    } else {
        int y0 = g.y * 6 / 5, y1 = (g.y + g.h) * 6 / 5;
        im->x = g.x; im->y = y0; im->w = g.w; im->h = y1 - y0;
        im->bgr = calloc((size_t)im->w * im->h, 3); if (!im->bgr) { free(g.idx); return -1; }
        paint(&g, im->bgr, im->w, im->h, im->x, im->y);
    }
    if (xmask && !full) {
        /* The DOS sprites are drawn opaque, on black.  The black that is connected to the edge of the sprite is made
           transparent, so the character stands on the (DOS) picture behind it like the Windows sprite does. */
        int W = g.W, H = g.h; uint8_t *vis = calloc((size_t)W * H, 1); int *st = malloc(sizeof(int) * (size_t)W * H);
        im->has_alpha = 1; im->ax = im->x; im->ay = im->y; im->aw = im->w; im->ah = im->h;
        im->alpha = malloc((size_t)im->aw * im->ah);
        if (!vis || !st || !im->alpha) { free(vis); free(st); free(im->alpha); im->alpha = NULL; im->has_alpha = 0; }
        else {
            #define BLACK(i) (g.pal[g.idx[i] & 15][0] + g.pal[g.idx[i] & 15][1] + g.pal[g.idx[i] & 15][2] == 0)
            int sp = 0;
            for (int x = 0; x < g.w; x++) { int a = x, b2 = (H - 1) * W + x; if (BLACK(a) && !vis[a]) { vis[a] = 1; st[sp++] = a; } if (BLACK(b2) && !vis[b2]) { vis[b2] = 1; st[sp++] = b2; } }
            for (int y = 0; y < H; y++) { int a = y * W, b2 = y * W + g.w - 1; if (BLACK(a) && !vis[a]) { vis[a] = 1; st[sp++] = a; } if (BLACK(b2) && !vis[b2]) { vis[b2] = 1; st[sp++] = b2; } }
            while (sp) { int i = st[--sp], x = i % W, y = i / W;
                int nb[4] = { x > 0 ? i - 1 : -1, x + 1 < g.w ? i + 1 : -1, y > 0 ? i - W : -1, y + 1 < H ? i + W : -1 };
                for (int k = 0; k < 4; k++) { int j = nb[k]; if (j >= 0 && !vis[j] && BLACK(j)) { vis[j] = 1; st[sp++] = j; } } }
            #undef BLACK
            for (int ty = 0; ty < im->ah; ty++) {            /* the mask is stored bottom-up like the colours */
                int sy = (ty + im->y) * 5 / 6 - g.y; if (sy < 0) sy = 0; if (sy >= H) sy = H - 1;
                uint8_t *d = im->alpha + (size_t)(im->ah - 1 - ty) * im->aw;
                for (int x = 0; x < im->aw; x++) d[x] = vis[sy * W + x] ? 0 : 255;
            }
            free(vis); free(st);
        }
    } else if (xmask) {                  /* a full-screen masked picture: opaque */
        im->has_alpha = 1; im->ax = 0; im->ay = 0; im->aw = im->w; im->ah = im->h;
        im->alpha = malloc((size_t)im->aw * im->ah); if (im->alpha) memset(im->alpha, 255, (size_t)im->aw * im->ah); else im->has_alpha = 0;
    }
    free(g.idx);
    return 0;
}
