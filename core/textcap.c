/* Glyph-blit capture + large-type re-layout for the dialogue screen.
 *
 * The SILV engine draws every character (Japanese kana or the Korean patch glyphs) through one
 * routine that blits a 1bpp bitmap (DS:SI, CX bytes per row, DX rows) to VRAM at ES:DI.
 * We hook that routine, remember each bitmap with its screen position, and keep our picture of
 * the text in sync with VRAM by verifying cells against the actual pixels before drawing.
 */
#include "textcap.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define TC_MAX   512
#define SCR_BYTES 80

typedef struct { int16_t x, y; uint8_t cx, rows, fg, bg; uint8_t tile, hl, ca, cb, dead; uint64_t born, dead_t; uint8_t bits[64]; } Cell;

typedef struct TextCap {
    Cell c[TC_MAX]; int n;
    uint32_t gen;
    int hooked;
    uint16_t seg;
    int16_t lead_y[8]; int n_lead;          /* rows whose text line begins with a blank (a space pushed over a wrap) */
    int16_t last_blank_x, last_blank_y;
    uint64_t pg_t[4]; uint8_t pg_fg[4];     /* last glyph drawn into an off-screen page: when, and in which colour */
    struct Strip { int16_t col, row; uint8_t cx, rows, ink; uint8_t keep[80]; uint8_t tile[80][32]; } strip[16];
    int nstrip;                              /* text strips copied to the screen, waiting to be checked against VRAM */
    int unsettled;                           /* some cell is hidden or still new: VRAM was caught mid-change, look again soon */
} TextCap;

/* signature of the glyph-blit routine entry:  cmp word [e262],0 ; je +2 ; jmp +0x66 ; call 1e6b */
static const uint8_t SIG[] = {0x83, 0x3E, 0x62, 0xE2, 0x00, 0x74, 0x02};
#define BLIT_OFF 0x200A

static void cell_remove(TextCap *tc, int i) { tc->c[i] = tc->c[--tc->n]; }

static void lead_del(TextCap *tc, int y) {
    for (int i = 0; i < tc->n_lead; i++) if (abs(tc->lead_y[i] - y) <= 4) tc->lead_y[i--] = tc->lead_y[--tc->n_lead];
}
static void lead_add(TextCap *tc, int y) {
    lead_del(tc, y);
    if (tc->n_lead < 8) tc->lead_y[tc->n_lead++] = (int16_t)y;
}

/* ------------------------------------------------------------------ capture */
/* paint over what was there: clear covered byte columns of existing cells */
static void paint_over(TextCap *tc, int bx, int cx, int y, int rows, int only_tiles) {
    for (int i = 0; i < tc->n; i++) {
        Cell *e = &tc->c[i]; int any = 0, ebx = e->x / 8;
        if (only_tiles && !e->tile) continue;
        for (int r = 0; r < e->rows; r++) {
            int ay = e->y + r;
            if (ay < y || ay >= y + rows) { for (int k = 0; k < e->cx; k++) any |= e->bits[r * e->cx + k]; continue; }
            for (int k = 0; k < e->cx; k++) {
                int ax = ebx + k;
                if (ax >= bx && ax < bx + cx) e->bits[r * e->cx + k] = 0;
                any |= e->bits[r * e->cx + k];
            }
        }
        if (!any) { cell_remove(tc, i); i--; }
    }
}

static int tc_cb(CPU *c, int idx) {
    (void)idx;
    Machine *m = c->m; TextCap *tc = m->tc;
    if (!tc) return 0;
    uint16_t ds = c->seg[DS];
    int pgn = mem_rw(m, LIN(ds, 0xE262));
    if (pgn != 0) {                                              /* off-screen page: remember that text is being composed there */
        if (pgn >= 1 && pgn <= 3) { tc->pg_t[pgn] = m->icount; tc->pg_fg[pgn] = mem_rb(m, LIN(ds, 0xE27C)) & 0x0F; }
        return 0;
    }
    uint16_t seg = mem_rw(m, LIN(ds, 0xE2D0));
    if (seg < 0xA000 || seg >= 0xB000) return 0;
    uint32_t off = ((uint32_t)(seg - 0xA000) << 4) + c->r[DI];
    int cx = c->r[CX], rows = c->r[DX];
    if (cx < 1 || cx > 4 || rows < 1 || rows > 32) return 0;
    int x = (int)(off % SCR_BYTES) * 8, y = (int)(off / SCR_BYTES);
    uint8_t col = mem_rb(m, LIN(ds, 0xE27C));
    int bx = x / 8, ink = 0;
    uint8_t bits[64];
    for (int i = 0; i < cx * rows; i++) { bits[i] = mem_rb(m, LIN(ds, (uint16_t)(c->r[SI] + i))); ink |= bits[i]; }
    paint_over(tc, bx, cx, y, rows, 0);
    tc->gen++;
    m->text_on = tc->n > 0;
    /* a blank glyph at the very start of a row = the game carried a space over the wrap point */
    if (!ink) { if (x <= 24) { lead_add(tc, y); tc->last_blank_x = (int16_t)x; tc->last_blank_y = (int16_t)y; } return 0; }
    if (x <= 32 && !(tc->last_blank_y == y && tc->last_blank_x + 8 <= x && x - tc->last_blank_x <= 16)) lead_del(tc, y);
    if (tc->n >= TC_MAX) return 0;
    Cell *e = &tc->c[tc->n++];
    memset(e, 0, sizeof *e);
    e->born = m->icount;
    e->x = (int16_t)x; e->y = (int16_t)y; e->cx = (uint8_t)cx; e->rows = (uint8_t)rows;
    e->fg = col & 0x0F; e->bg = col >> 4;
    memcpy(e->bits, bits, (size_t)cx * rows);
    m->text_on = 1;
    return 0;
}


/* Choice menus are composed in an off-screen page and then copied to the screen, so the glyph hook never sees them.
 * When such a copy brings text to the visible page we read the text pixels out of the source page. */
static int tc_copy_cb(CPU *c, int idx) {
    (void)idx;
    Machine *m = c->m; TextCap *tc = m->tc;
    if (!tc) return 0;
    int sp = (c->r[AX] >> 8) & 0xFF, dp = c->r[AX] & 0xFF;
    if (dp != 0 || sp < 1 || sp > 3) return 0;
    int cx = c->r[CX], rows = c->r[DX];
    if (cx < 1 || cx > 80 || rows < 1 || rows > 32) return 0;
    if (m->icount - tc->pg_t[sp] > (uint64_t)m->ips / 3) return 0;          /* no text was drawn there just now */
    uint16_t ds = c->seg[DS];
    uint16_t tbl = (uint16_t)(0xE2D0 + 0x10 * sp);
    uint16_t pseg[4]; for (int p = 0; p < 4; p++) pseg[p] = mem_rw(m, LIN(ds, (uint16_t)(tbl + 4 * p)));
    uint16_t dseg = mem_rw(m, LIN(ds, 0xE2D0));
    if (dseg < 0xA000 || dseg >= 0xB000) return 0;
    uint32_t doff = ((uint32_t)(dseg - 0xA000) << 4) + c->r[DI];
    int dcol = (int)(doff % SCR_BYTES), drow = (int)(doff / SCR_BYTES);
    if (dcol + cx > SCR_BYTES) return 0;
    int ink = tc->pg_fg[sp];
    uint8_t tile[80][32]; int total = 0, ninkcols = 0;
    for (int k = 0; k < cx; k++) {
        int any = 0;
        for (int r = 0; r < rows; r++) {
            uint16_t o = (uint16_t)(c->r[SI] + r * SCR_BYTES + k);
            uint8_t b0 = mem_rb(m, LIN(pseg[0], o)), b1 = mem_rb(m, LIN(pseg[1], o)), b2 = mem_rb(m, LIN(pseg[2], o)), b3 = mem_rb(m, LIN(pseg[3], o));
            uint8_t v = 0;
            for (int b = 0; b < 8; b++) {
                int col = ((b0 >> b) & 1) | (((b1 >> b) & 1) << 1) | (((b2 >> b) & 1) << 2) | (((b3 >> b) & 1) << 3);
                if (col == ink) { v |= (uint8_t)(1 << b); total++; }
            }
            tile[k][r] = v; any |= v;
        }
        if (any) ninkcols++;
    }
    if (total < 6 || total * 100 > cx * rows * 8 * 45) return 0;               /* not text-like */
    /* keep tiles that look like text: ink spread over several rows and columns (frame dots and rules are not);
     * small punctuation tiles are accepted next to a solid one */
    uint8_t strong[80], weak[80], keep[80]; int nstrong = 0;   /* weak: also a lone vertical stroke (right half of a hangul vowel) */
    for (int k = 0; k < cx; k++) {
        int ink_n = 0, cols = 0, rws = 0, run = 0, best = 0; uint8_t colmask = 0;
        for (int r = 0; r < rows; r++) { uint8_t v = tile[k][r]; if (v) { rws++; if (++run > best) best = run; } else run = 0; colmask |= v; for (int b = 0; b < 8; b++) ink_n += (v >> b) & 1; }
        for (int b = 0; b < 8; b++) cols += (colmask >> b) & 1;
        strong[k] = ink_n >= 6 && rws >= 3 && cols >= 2;
        weak[k] = (ink_n >= 2 && rws >= 2 && cols >= 2) || (ink_n >= 4 && best >= 4);      /* or a solid vertical stroke (a dotted frame line is not) */
        nstrong += strong[k];
    }
    if (nstrong < 2) return 0;
    for (int k = 0; k < cx; k++) keep[k] = strong[k] || (weak[k] && ((k > 0 && strong[k - 1]) || (k + 1 < cx && strong[k + 1])));
    if (tc->nstrip >= 16) return 0;
    struct Strip *ps = &tc->strip[tc->nstrip++];
    ps->col = (int16_t)dcol; ps->row = (int16_t)drow; ps->cx = (uint8_t)cx; ps->rows = (uint8_t)rows; ps->ink = (uint8_t)ink;
    memcpy(ps->keep, keep, (size_t)cx);
    for (int k = 0; k < cx; k++) memcpy(ps->tile[k], tile[k], (size_t)rows);
    (void)ninkcols;
    tc->gen++;
    return 0;
}

static const int16_t SIG_COPY[]   = {0x80, 0xFC, 0x00, 0x74, 0x09, 0x3C, 0x00, 0x75, 0x03, 0xE9, -1, -1, 0xEB, 0x07, 0x1E, 0x06};
static const int16_t SIG_MCOPY[]  = {0x1E, 0x06, 0x56, 0x55, 0xE8, -1, -1, 0x52, 0xBA, 0xCE, 0x03, 0xB0, 0x05, 0xEE, 0xBA, 0xCF, 0x03, 0xEC};
static uint32_t find_sig(Machine *m, uint32_t base, uint32_t hint, const int16_t *sig, int n) {
    for (int pass = 0; pass < 2; pass++) {
        uint32_t lo = pass == 0 ? hint : 0, hi = pass == 0 ? hint + 1 : 0xF000;
        for (uint32_t o = lo; o < hi; o++) {
            int ok = 1; for (int i = 0; i < n && ok; i++) if (sig[i] >= 0 && m->mem[base + o + i] != (uint8_t)sig[i]) ok = 0;
            if (ok) return base + o;
        }
    }
    return 0;
}

static void tc_on_load(Machine *m, uint16_t load_seg) {
    TextCap *tc = m->tc;
    if (!tc) return;
    /* the image is loaded at load_seg:0 ; the routine is expected at BLIT_OFF, otherwise scan the image */
    uint32_t base = LIN(load_seg, 0), found = 0;
    if (memcmp(m->mem + base + BLIT_OFF, SIG, sizeof SIG) == 0) found = base + BLIT_OFF;
    for (uint32_t o = 0; !found && o < 0xF000; o++) if (memcmp(m->mem + base + o, SIG, sizeof SIG) == 0) found = base + o;
    if (!found) return;
    CPU *c = &m->cpu;
    if (c->nbp < 8) { c->bp[c->nbp++] = found; c->bp_cb = tc_cb; tc->hooked = 1; tc->seg = load_seg; }
    uint32_t cp = find_sig(m, base, 0x17AB, SIG_COPY, (int)(sizeof SIG_COPY / sizeof *SIG_COPY));
    if (cp && c->nbp < 8) { c->bp[c->nbp] = cp; c->bp_fn[c->nbp] = tc_copy_cb; c->nbp++; }
    uint32_t mp = find_sig(m, base, 0x1C95, SIG_MCOPY, (int)(sizeof SIG_MCOPY / sizeof *SIG_MCOPY));
    if (mp && c->nbp < 8) { c->bp[c->nbp] = mp; c->bp_fn[c->nbp] = tc_copy_cb; c->nbp++; }
}

void textcap_init(Machine *m) {
    m->tc = calloc(1, sizeof(TextCap));
    m->on_load = tc_on_load;
}
void textcap_free(Machine *m) { free(m->tc); m->tc = NULL; }
uint32_t textcap_gen(Machine *m) { return m->tc ? m->tc->gen : 0; }
int textcap_active(Machine *m) { return m->tc && m->tc->hooked; }
int textcap_unsettled(Machine *m) { return m->tc && m->tc->unsettled; }

/* ------------------------------------------------------------------ verification against VRAM */
static int vram_color(Machine *m, int px, int py) {
    uint32_t off = (uint32_t)py * SCR_BYTES + (px >> 3);
    if (off >= 0x10000) return -1;
    int b = 7 - (px & 7);
    return ((m->vga.plane[0][off] >> b) & 1) | (((m->vga.plane[1][off] >> b) & 1) << 1) |
           (((m->vga.plane[2][off] >> b) & 1) << 2) | (((m->vga.plane[3][off] >> b) & 1) << 3);
}

/* Cell state: 0 gone, 1 shown as drawn, 2 shown inverted (the highlighted menu entry: the glyph shape is still
 * there but in another colour on a uniform bar). */
static int cell_state(Machine *m, Cell *e) {
    int ink = 0, ok = 0, nn = 0, ci[16] = {0}, cn[16] = {0};
    for (int r = 0; r < e->rows; r++)
        for (int k = 0; k < e->cx; k++) {
            uint8_t v = e->bits[r * e->cx + k];
            for (int b = 0; b < 8; b++) {
                int col = vram_color(m, e->x + k * 8 + b, e->y + r); if (col < 0) continue;
                if ((v >> (7 - b)) & 1) { ink++; ci[col]++; if (col == e->fg) ok++; } else { nn++; cn[col]++; }
            }
        }
    if (!ink) return 0;
    if (ok * 100 >= ink * 60) { e->hl = 0; return 1; }
    int a = 0, bb = 0; for (int i = 1; i < 16; i++) { if (ci[i] > ci[a]) a = i; if (cn[i] > cn[bb]) bb = i; }
    if (a != bb && ci[a] * 100 >= ink * 70 && nn >= 8 && cn[bb] * 100 >= nn * 70) { e->hl = 1; e->ca = (uint8_t)a; e->cb = (uint8_t)bb; return 2; }
    return 0;
}

/* ------------------------------------------------------------------ layout */
typedef struct { const Cell *cell; int units; } Item;       /* cell == NULL: a blank of `units` eighths */

#define MAX_ITEMS 1024
typedef struct { int first, count; int width_u; } DLine;     /* display line over items[] (units of 8px) */

static int ink_units(const Cell *e) {
    int last = 0;
    for (int r = 0; r < e->rows; r++)
        for (int k = 0; k < e->cx; k++) if (e->bits[r * e->cx + k] && k + 1 > last) last = k + 1;
    return last < 1 ? 1 : last;
}

static int cmp_cell(const void *a, const void *b) {
    const Cell *x = *(const Cell *const *)a, *y = *(const Cell *const *)b;
    if (x->y != y->y) return x->y - y->y;
    return x->x - y->x;
}

static float samp(const Cell *e, int x, int y) {
    if (x < 0 || y < 0 || x >= e->cx * 8 || y >= e->rows) return 0.f;
    return (e->bits[y * e->cx + (x >> 3)] >> (7 - (x & 7))) & 1 ? 1.f : 0.f;
}

static void put(uint8_t *rgb, int W, int H, int x, int y, const uint8_t *col, float a) {
    if (x < 0 || y < 0 || x >= W || y >= H || a <= 0.f) return;
    uint8_t *p = rgb + ((size_t)y * W + x) * 3;
    if (a >= 1.f) { p[0] = col[0]; p[1] = col[1]; p[2] = col[2]; return; }
    for (int i = 0; i < 3; i++) p[i] = (uint8_t)(p[i] + (col[i] - p[i]) * a);
}

static float samp3(const Cell *e, const Cell *L, const Cell *R, int x, int y) {
    int w = e->cx * 8;
    if (x < 0 && L) return samp(L, x + L->cx * 8, y);
    if (x >= w && R) return samp(R, x - w, y);
    return samp(e, x, y);
}

static void draw_cell(uint8_t *rgb, int W, int H, const Cell *e, const Cell *L, const Cell *R, int units, float s, float ox, float oy, const uint8_t *col) {
    int dw = (int)ceilf(units * 8 * s), dh = (int)ceilf(e->rows * s);
    for (int dy = 0; dy < dh; dy++) {
        float v = (dy + 0.5f) / s - 0.5f; int v0 = (int)floorf(v); float fv = v - v0;
        for (int dx = 0; dx < dw; dx++) {
            float u = (dx + 0.5f) / s - 0.5f; int u0 = (int)floorf(u); float fu = u - u0;
            float a = samp3(e, L, R, u0, v0) * (1 - fu) * (1 - fv) + samp3(e, L, R, u0 + 1, v0) * fu * (1 - fv) +
                      samp3(e, L, R, u0, v0 + 1) * (1 - fu) * fv + samp3(e, L, R, u0 + 1, v0 + 1) * fu * fv;
            /* sharpen the bilinear coverage into a soft-edged shape */
            a = (a - 0.30f) / 0.40f; if (a < 0) a = 0; if (a > 1) a = 1; a = a * a * (3 - 2 * a);
            put(rgb, W, H, (int)ox + dx, (int)oy + dy, col, a);
        }
    }
}

static void rgb_of(Machine *m, int idx, uint8_t out[3]) {
    uint8_t pal[16][3];
    extern void vga_palette16(Machine *, uint8_t[16][3]);
    vga_palette16(m, pal);
    memcpy(out, pal[idx & 15], 3);
}

static float g_max_scale = 1.5f;
void textcap_set_max_scale(float s) { g_max_scale = s; }

int textcap_render(Machine *m, uint8_t *rgb, int W, int H) {
    TextCap *tc = m->tc;
    for (int i = 0; i < W * H; i++) { rgb[i * 3] = 12; rgb[i * 3 + 1] = 14; rgb[i * 3 + 2] = 28; }
    if (!tc) return 0;
    /* drop cells that are no longer visible */
    /* a glyph is recorded when its blit starts; the frontend may look at VRAM before the blit has finished,
     * so a cell is not judged until it has had a moment to appear */
    /* A cell that fails the check is only hidden: VRAM passes through odd states while the game inverts or redraws a
     * menu entry, and judging such a moment final would lose glyphs for good.  It is dropped after a while, and
     * comes back if the picture returns. */
    for (int i = 0; i < tc->n; i++) {
        Cell *e = &tc->c[i];
        if (m->icount - e->born < (uint64_t)m->ips / 20) continue;
        if (cell_state(m, e)) { e->dead = 0; continue; }
        if (!e->dead) { e->dead = 1; e->dead_t = m->icount; }
        else if (m->icount - e->dead_t > (uint64_t)m->ips * 2 / 5) { cell_remove(tc, i); i--; }
    }
    /* text strips that were copied to the screen: adopt them unless glyphs we already track cover that spot */
    for (int si = 0; si < tc->nstrip; si++) {
        struct Strip *ps = &tc->strip[si];
        int x0 = ps->col * 8, x1 = (ps->col + ps->cx) * 8, y0 = ps->row, y1 = ps->row + ps->rows, blocked = 0;
        for (int i = 0; i < tc->n && !blocked; i++) {
            const Cell *e = &tc->c[i];
            if (!e->tile && !e->dead && e->x < x1 && e->x + e->cx * 8 > x0 && e->y < y1 && e->y + e->rows > y0) blocked = 1;
        }
        if (blocked) continue;
        paint_over(tc, ps->col, ps->cx, ps->row, ps->rows, 1);
        for (int k = 0; k < ps->cx; k++) {
            if (!ps->keep[k] || tc->n >= TC_MAX) continue;
            Cell *e = &tc->c[tc->n++];
            memset(e, 0, sizeof *e);
            e->x = (int16_t)((ps->col + k) * 8); e->y = ps->row; e->cx = 1; e->rows = ps->rows; e->fg = ps->ink; e->bg = 0; e->tile = 1; e->born = m->icount;
            memcpy(e->bits, ps->tile[k], ps->rows);
            if (!cell_state(m, e)) { e->dead = 1; e->dead_t = m->icount; }   /* (the copy may not have run yet: kept hidden, re-judged later) */
        }
    }
    tc->nstrip = 0;
    const Cell *sorted[TC_MAX]; int ns = 0;
    tc->unsettled = 0;
    for (int i = 0; i < tc->n; i++) {
        if (tc->c[i].dead || m->icount - tc->c[i].born < (uint64_t)m->ips / 20) tc->unsettled = 1;
        if (!tc->c[i].dead) sorted[ns++] = &tc->c[i];
    }
    m->text_on = ns > 0;
    if (!ns) { if (!tc->n) tc->n_lead = 0; return 0; }
    qsort(sorted, ns, sizeof sorted[0], cmp_cell);
    int minx = 9999; for (int i = 0; i < ns; i++) if (sorted[i]->x < minx) minx = sorted[i]->x;

    /* Rows -> item stream with explicit blanks.  The game wraps its text at a fixed right edge, so a row that
     * reaches that edge continues on the next row: such rows are joined into one paragraph and re-wrapped
     * at word gaps for the big display.  (A space carried over the wrap is remembered by the capture.) */
    static Item items[MAX_ITEMS]; static int line_of[MAX_ITEMS];
    int ni = 0, nlines = 0, prev_right = 0, have_prev = 0;
    const int WRAP_EDGE = 592;                 /* a row ending at/after this pixel was soft-wrapped by the game */
    for (int i = 0; i < ns && ni < MAX_ITEMS - 8;) {
        int y0 = sorted[i]->y, j = i;
        while (j < ns && abs(sorted[j]->y - y0) <= 4) j++;
        int join = have_prev && prev_right >= WRAP_EDGE;
        if (!join && have_prev) nlines++;
        int lead = 0; for (int q = 0; q < tc->n_lead; q++) if (abs(tc->lead_y[q] - y0) <= 4) lead = 1;
        int cursor = minx, right = 0;
        for (int k = i; k < j && ni < MAX_ITEMS - 8; k++) {
            const Cell *e = sorted[k];
            int gap = (e->x - cursor + 4) / 8;
            if (k == i && join) { if (lead) { items[ni].cell = NULL; items[ni].units = 1; line_of[ni++] = nlines; } }
            else if (gap > 0 && k > i) { items[ni].cell = NULL; items[ni].units = gap; line_of[ni++] = nlines; }
            else if (k == i && e->x - minx >= 8) { items[ni].cell = NULL; items[ni].units = (e->x - minx) / 8; line_of[ni++] = nlines; }
            int u = ink_units(e);
            items[ni].cell = e; items[ni].units = u; line_of[ni++] = nlines;
            cursor = e->x + u * 8;
            if (e->x + e->cx * 8 > right) right = e->x + e->cx * 8;
        }
        prev_right = right; have_prev = 1;
        i = j;
    }
    if (have_prev) nlines++;

    static const float SCALES[] = {2.0f, 1.75f, 1.5f, 1.375f, 1.25f, 1.125f, 1.0f};
    const int MARGIN = 8;
    int availW = W - 2 * MARGIN, availH = H - 2 * MARGIN;
    static DLine dl[256];
    int ndl = 0; float scale = 1.0f; int lineh = 18;
    for (unsigned si = 0; si < sizeof SCALES / sizeof *SCALES; si++) {
        if (SCALES[si] > g_max_scale + 0.001f && SCALES[si] != 1.0f) continue;
        scale = SCALES[si]; lineh = (int)(18 * scale);
        int maxu = (int)(availW / (8 * scale));
        ndl = 0;
        for (int l = 0, i = 0; l < nlines && ndl < 250; l++) {
            int start = i; while (i < ni && line_of[i] == l) i++;
            int end = i;                                    /* items [start,end) form logical line l */
            int pos = start;
            while (pos < end && ndl < 250) {
                int w = 0, brk = -1, k = pos;
                for (; k < end; k++) {
                    if (w + items[k].units > maxu && k > pos) break;
                    w += items[k].units;
                    if (!items[k].cell) brk = k;            /* a blank is a break opportunity */
                }
                int stop = k;
                if (k < end && brk > pos) stop = brk + 1;   /* wrap at the last blank if there is one */
                int ww = 0; for (int q = pos; q < stop; q++) ww += items[q].units;
                dl[ndl].first = pos; dl[ndl].count = stop - pos; dl[ndl].width_u = ww; ndl++;
                pos = stop;
                while (pos < end && !items[pos].cell && k < end) pos++;   /* swallow blanks at the wrap point */
            }
        }
        if (ndl * lineh <= availH) break;
    }
    int first_line = 0;
    if (ndl * lineh > availH) first_line = ndl - availH / lineh;      /* keep the most recent lines */
    int total_h = (ndl - first_line) * lineh;
    float oy = (float)(MARGIN + (availH - total_h) / 2);
    int drawn = 0;
    for (int li = first_line; li < ndl; li++, oy += lineh) {
        float ox = (float)MARGIN;
        int hl = 0; uint8_t barc[3] = {235, 235, 235};
        for (int q = dl[li].first; q < dl[li].first + dl[li].count; q++)
            if (items[q].cell && items[q].cell->hl) {
                hl = 1; uint8_t t[3]; rgb_of(m, items[q].cell->cb, t);
                if ((t[0] * 3 + t[1] * 6 + t[2]) / 10 >= 150) { barc[0] = t[0]; barc[1] = t[1]; barc[2] = t[2]; }
            }
        if (hl) for (int dy = 0; dy < lineh; dy++) for (int dx = 4; dx < W - 4; dx++) put(rgb, W, H, dx, (int)oy + dy, barc, 1.f);
        for (int q = dl[li].first; q < dl[li].first + dl[li].count; q++) {
            const Item *it = &items[q];
            if (it->cell) {
                uint8_t col[3]; rgb_of(m, it->cell->fg, col);
                int lum = (col[0] * 3 + col[1] * 6 + col[2]) / 10;
                if (it->cell->hl) { col[0] = 20; col[1] = 20; col[2] = 40; }
                else if (lum < 170) { col[0] = col[1] = col[2] = 235; }
                if (it->cell->bg && !hl) {
                    uint8_t bgc[3]; rgb_of(m, it->cell->bg, bgc);
                    for (int dy = 0; dy < (int)ceilf(it->cell->rows * scale); dy++)
                        for (int dx = 0; dx < (int)ceilf(it->units * 8 * scale); dx++) put(rgb, W, H, (int)ox + dx, (int)oy + dy, bgc, 1.f);
                }
                const Cell *L = NULL, *R = NULL;
                if (it->cell->tile) {
                    if (q > dl[li].first && items[q - 1].cell && items[q - 1].cell->tile && items[q - 1].cell->y == it->cell->y && items[q - 1].cell->x + 8 == it->cell->x) L = items[q - 1].cell;
                    if (q + 1 < dl[li].first + dl[li].count && items[q + 1].cell && items[q + 1].cell->tile && items[q + 1].cell->y == it->cell->y && items[q + 1].cell->x == it->cell->x + 8) R = items[q + 1].cell;
                }
                draw_cell(rgb, W, H, it->cell, L, R, it->units, scale, ox, oy, col);
                drawn++;
            }
            ox += it->units * 8 * scale;
        }
    }
    return drawn;
}
