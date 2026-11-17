/* SYS layer. Items marked [EXE] were checked against AI5WIN.EXE; [guess] are inferred from script usage. */
#include "ai5.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "gccdec.h"

#define SW 640
#define SH 480
#define NSURF 12
static uint8_t *surf[NSURF];           /* RGB888, top-down */
static uint8_t *amask[NSURF];          /* [EXE] every surface carries an 8-bit mask plane (surface+0x14) */
static int sw[NSURF], sh[NSURF];       /* surfaces grow to fit loaded images (animation sheets are taller than the screen) */
#define PIX(s, x, y) (surf[s] + ((y) * sw[s] + (x)) * 3)
#define MSK(s, x, y) (amask[s][(y) * sw[s] + (x)])
#define INS(s, x, y) ((x) >= 0 && (y) >= 0 && (x) < sw[s] && (y) < sh[s])
static void surf_ensure(int s, int w, int h) {
    if (w <= sw[s] && h <= sh[s]) return; if (w < sw[s]) w = sw[s]; if (h < sh[s]) h = sh[s];
    uint8_t *c = calloc(1, (size_t)w * h * 3), *m = calloc(1, (size_t)w * h);
    for (int y = 0; y < sh[s]; y++) { memcpy(c + (size_t)y * w * 3, surf[s] + (size_t)y * sw[s] * 3, sw[s] * 3); memcpy(m + (size_t)y * w, amask[s] + (size_t)y * sw[s], sw[s]); }
    free(surf[s]); free(amask[s]); surf[s] = c; amask[s] = m; sw[s] = w; sh[s] = h;
}
static uint32_t col_fg = 0xFFFFFF, col_bg = 0;   /* COLORREF 0x00BBGGRR  [EXE: SYS 13 -> bg=p0, fg=p1] */
static const uint8_t *font; static uint32_t font_cells;
/* message windows [EXE 0x421e20: create/show/hide/rect]. How they are blended is a [guess]:
   window 0 shows the top-left of surface 6; a 50% dark veil (ini [Msg] Alpha=128) is laid first, then surface 6 is blended through its mask plane. */
static struct { int used, visible, x0, y0, x1, y1; } win[8];
static uint8_t *screen_out;
static int shake_x, shake_y;          /* [EXE UTIL 0 -> 0x40ffa0/0x41cf70] display offset used for screen shakes */
static const uint8_t *compose(void);
const uint8_t *sys_scene_rgb(void) {
    if (!surf[0]) return NULL; if (!shake_x && !shake_y) return surf[0];
    int v0 = win[0].visible, v1 = win[1].visible; win[0].visible = win[1].visible = 0; const uint8_t *r = compose(); win[0].visible = v0; win[1].visible = v1; return r;
}
static const uint8_t *compose(void) {
    if (!(win[0].used && win[0].visible) && !(win[1].used && win[1].visible) && !shake_x && !shake_y) return surf[0];
    if (!shake_x && !shake_y) memcpy(screen_out, surf[0], SW * SH * 3);
    else { memset(screen_out, 0, SW * SH * 3);
        for (int y = 0; y < SH; y++) { int sy = y - shake_y; if (sy < 0 || sy >= SH) continue;
            int sx0 = shake_x > 0 ? 0 : -shake_x, dx0 = shake_x > 0 ? shake_x : 0, n = SW - (shake_x > 0 ? shake_x : -shake_x); if (n > 0) memcpy(screen_out + (y * SW + dx0) * 3, surf[0] + (sy * SW + sx0) * 3, n * 3); } }
    for (int n = 0; n < 2; n++) { if (!win[n].used || !win[n].visible) continue;
        /* [script-derived] window 0 (dialogue) shows surface 6 from its top-left; window 1 (choices) shows surface 6 at screen coordinates */
        int w = win[n].x1 - win[n].x0, h = win[n].y1 - win[n].y0, oy = n ? win[n].y0 : 0, ox = n ? win[n].x0 : 0;
        for (int y = 0; y < h; y++) { int ty = win[n].y0 + y; if (ty < 0 || ty >= SH) continue;
            for (int x = 0; x < w; x++) { int tx = win[n].x0 + x; if (tx < 0 || tx >= SW) continue;
                if (!INS(6, ox + x, oy + y)) continue; const uint8_t *s = PIX(6, ox + x, oy + y); uint8_t *d = screen_out + (ty * SW + tx) * 3;
                int a = MSK(6, ox + x, oy + y);
                for (int c = 0; c < 3; c++) { int veil = d[c] * 127 / 255; d[c] = (veil * (255 - a) + s[c] * a) / 255; } } } }
    if (getenv("AI5_DEBUG_SURF")) { int n = atoi(getenv("AI5_DEBUG_SURF")); for (int y = 0; y < 120; y++) memcpy(screen_out + y * SW * 3, surf[n] + y * SW * 3, SW * 3); }
    return screen_out;
}
static uint8_t seen[32][16];


static void anim_reset(void);
static void sys_init(vm_t *vm) {
    if (surf[0]) return;
    for (int i = 0; i < NSURF; i++) { surf[i] = calloc(1, SW * SH * 3); amask[i] = calloc(1, SW * SH); sw[i] = SW; sh[i] = SH; }
    screen_out = calloc(1, SW * SH * 3);
    anim_reset();
    for (int i = 0; i < 8; i++) SET_SYS32(vm, i, 0x40000 + i * 0x20000);   /* [guess] file areas the scripts address through sys32[n] */
    size_t n; uint8_t *f = plat_read_file("font16.bin", &n);
    if (f && n > 12 && !memcmp(f, "AF16", 4)) { font_cells = rd32(f + 8); font = f + 12; } else plat_log("font16.bin missing");
}
static void once(vm_t *vm, uint32_t fn, const char *note) {
    uint32_t sub = vm->params[0].val; if (fn < 32 && sub < 16) { if (seen[fn][sub]) return; seen[fn][sub] = 1; }
    char b[256]; int o = snprintf(b, sizeof b, "TODO SYS %u (%s):", fn, note);
    for (int i = 0; i < vm->nparams && o < 230; i++)
        o += vm->params[i].type == 1 ? snprintf(b + o, sizeof b - o, " '%s'", vm->params[i].str) : snprintf(b + o, sizeof b - o, " %u", vm->params[i].val);
    plat_log("%s", b);
}
/* ---- graphics ---- */
/* load a GCC image into surface dst.  [EXE 0x41ab80, used by SYS 9 (0x420540) and SYS 25 (0x421b30)]
   ox/oy: -1 = use the file's position; otherwise the colour rect is moved by (ox,oy) and the mask rect is placed AT (ox,oy).
   Only the m variants write the mask plane. The resulting rectangle goes to sys16[17..20] (x, y, w, h). */
static void load_image(vm_t *vm, const char *name, int dst, int ox, int oy) {
    size_t n; uint8_t *raw = arc_load(&vm->arc_bg, name, &n); if (!raw) { plat_log("missing image %s", name); return; }
    gcc_image im; dst %= NSURF;
    if (!gcc_decode(raw, n, &im)) {
        int cx = im.x + (ox != -1 ? ox : 0), cy = im.y + (oy != -1 ? oy : 0);
        int ax = ox != -1 ? ox : im.ax, ay = oy != -1 ? oy : im.ay;
        if (dst != 0) { surf_ensure(dst, cx + im.w, cy + im.h); if (im.has_alpha) surf_ensure(dst, ax + im.aw, ay + im.ah); }
        for (int y = 0; y < im.h; y++) { int dy = cy + y; const uint8_t *s = im.bgr + (size_t)(im.h - 1 - y) * im.w * 3;
            for (int x = 0; x < im.w; x++) { int dx = cx + x; if (!INS(dst, dx, dy)) continue;
                uint8_t *d = PIX(dst, dx, dy); d[0] = s[x * 3 + 2]; d[1] = s[x * 3 + 1]; d[2] = s[x * 3]; } }
        if (im.has_alpha) for (int y = 0; y < im.ah; y++) { int dy = ay + y; const uint8_t *s = im.alpha + (size_t)(im.ah - 1 - y) * im.aw;
            for (int x = 0; x < im.aw; x++) { int dx = ax + x; if (INS(dst, dx, dy)) MSK(dst, dx, dy) = s[x]; } }
        int w = im.w, h = im.h; if (cx + w > sw[dst]) w = sw[dst] - cx; if (cy + h > sh[dst]) h = sh[dst] - cy;
        SET_SYS16(vm, 17, cx); SET_SYS16(vm, 18, cy); SET_SYS16(vm, 19, w > 0 ? w : 0); SET_SYS16(vm, 20, h > 0 ? h : 0);
        free(im.bgr); free(im.alpha);
    }
    free(raw);
}
#define EACH(body) for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { int sx = x0 + x, sy = y0 + y, tx = dx + x, ty = dy + y; if (!INS(ss, sx, sy) || !INS(ds, tx, ty)) continue; body }
/* [EXE 0x41ae60] colour copy; with_mask adds [EXE 0x41b130] mask-plane copy */
static void gfx_copy(int x0, int y0, int w, int h, int ss, int dx, int dy, int ds, int with_mask) {
    ss %= NSURF; ds %= NSURF;
    EACH(memcpy(PIX(ds, tx, ty), PIX(ss, sx, sy), 3); if (with_mask) MSK(ds, tx, ty) = MSK(ss, sx, sy);)
}
/* [EXE 0x41af20] copy skipping the key colour (COLORREF); exact compare is assumed */
static void gfx_copy_key(int x0, int y0, int w, int h, int ss, int dx, int dy, int ds, uint32_t key) {
    ss %= NSURF; ds %= NSURF; uint8_t kr = key, kg = key >> 8, kb = key >> 16;
    EACH(const uint8_t *s = PIX(ss, sx, sy); if (s[0] == kr && s[1] == kg && s[2] == kb) continue; memcpy(PIX(ds, tx, ty), s, 3);)
}
/* [EXE 0x41b640/0x41b310] blend src over dst through the source mask plane (0 = skip, 255 = copy) */
static void gfx_blend(int x0, int y0, int w, int h, int ss, int dx, int dy, int ds) {
    ss %= NSURF; ds %= NSURF;
    EACH(int a = MSK(ss, sx, sy); if (!a) continue; const uint8_t *s = PIX(ss, sx, sy); uint8_t *d = PIX(ds, tx, ty);
        if (a == 255) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; } else for (int c = 0; c < 3; c++) d[c] = (s[c] * a + d[c] * (255 - a)) / 255;)
}
/* [EXE 0x41b390] fill; clear_mask adds [EXE 0x41b040] */
static void gfx_fill(int x0, int y0, int w, int h, int s, uint32_t c, int clear_mask) {
    s %= NSURF;
    for (int y = y0; y < y0 + h; y++) for (int x = x0; x < x0 + w; x++) { if (!INS(s, x, y)) continue;
        uint8_t *d = PIX(s, x, y); d[0] = c; d[1] = c >> 8; d[2] = c >> 16; if (clear_mask) MSK(s, x, y) = 0; }
}
/* ---- text  [EXE: 0x412bf0 / 0x412950 cursor logic] ---- */
static void draw_glyph(int s, int x, int y, uint32_t idx, int w) {    /* writes fg colour and raises the mask plane by glyph coverage */
    if (!font || idx >= font_cells) return; const uint8_t *g = font + idx * 128; s %= NSURF;
    int r = col_fg & 255, gr = col_fg >> 8 & 255, b = col_fg >> 16 & 255;
    for (int j = 0; j < 16; j++) for (int i = 0; i < w; i++) { int tx = x + i, ty = y + j; if (!INS(s, tx, ty)) continue;
        int a = ((g[(j * 16 + i) >> 1] >> ((i & 1) ? 0 : 4)) & 15) * 17; if (!a) continue;
        uint8_t *d = PIX(s, tx, ty), *m = &MSK(s, tx, ty);
        if (*m == 0) { d[0] = r; d[1] = gr; d[2] = b; *m = a; }
        else { d[0] = (d[0] * (255 - a) + r * a) / 255; d[1] = (d[1] * (255 - a) + gr * a) / 255; d[2] = (d[2] * (255 - a) + b * a) / 255; if (a > *m) *m = a; } }
}
static long lit(int n) { long c = 0; for (int i = 0; i < sw[n] * 88 * 3; i++) c += surf[n][i] > 128; return c; }
uint32_t sys_glyph_index(const uint8_t **pp, int *half) {
    const uint8_t *p = *pp; uint8_t c = *p; uint32_t idx;
    if (((c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC)) && p[1]) {
        int li = c <= 0x9F ? c - 0x81 : c - 0xE0 + 31; *half = 0;
        idx = (li < 42 && p[1] >= 0x40 && p[1] <= 0xFC) ? 95 + li * 189 + (p[1] - 0x40) : 0xFFFFFFFF; p += 2;
    } else { *half = 1; idx = (c >= 0x20 && c < 0x7F) ? c - 0x20 : 0; p++; }
    *pp = p; return idx;
}
const uint8_t *sys_font(uint32_t *cells) { if (cells) *cells = font_cells; return font; }
/* text drawn into the dialogue/choice strip (surface 6, rows 0..87) is also kept as strings for platforms that re-render it */
static ui_text ui; static int ui_last_y = -1;
static void ui_capture(vm_t *vm, const uint8_t *from, const uint8_t *to) {
    if (SYS16(vm, 1) != 6 || SYS16(vm, 10) >= 88) return;
    int y = SYS16(vm, 10);
    if (y != ui_last_y || ui.nlines == 0) { if (ui.nlines >= 16) return; ui.line[ui.nlines].sjis[0] = 0; ui.line[ui.nlines].item = -1; ui.nlines++; ui_last_y = y; }
    char *d = ui.line[ui.nlines - 1].sjis; size_t n = strlen(d), add = to - from;
    if (n + add < sizeof ui.line[0].sjis) { memcpy(d + n, from, add); d[n + add] = 0; }
}
const ui_text *sys_ui_text(vm_t *vm) {
    ui.dialogue_visible = win[0].used && win[0].visible; ui.menu_visible = win[1].used && win[1].visible;
    ui.hover = (ui.menu_visible && V32(vm, 9) < 16) ? (int)V32(vm, 9) : -1;     /* v32[9] is the library's "highlighted choice" variable */
    for (int i = 0; i < ui.nlines; i++) ui.line[i].item = ui.menu_visible ? i : -1;
    return &ui;
}
void sys_text(vm_t *vm, const char *sj, bool zenkaku) {
    sys_init(vm); (void)zenkaku;
    const uint8_t *p = (const uint8_t *)sj;
    while (*p) {
        const uint8_t *start = p; int half; uint32_t idx = sys_glyph_index(&p, &half);
        if (SYS16(vm, 2) & 0x800) { SET_V32(vm, 18, V32(vm, 18) + 1); continue; }   /* [EXE] counts into v32[18] (mem+0x1134) */
        int cw = SYS16(vm, 15), lh = SYS16(vm, 16), adv = half ? cw / 2 : cw;
        ui_capture(vm, start, p);
        draw_glyph(SYS16(vm, 1), SYS16(vm, 9), SYS16(vm, 10), idx, half ? 8 : 16);
        SET_SYS16(vm, 9, SYS16(vm, 9) + adv);
        if (SYS16(vm, 9) + cw > SYS16(vm, 7)) { SET_SYS16(vm, 10, SYS16(vm, 10) + lh); SET_SYS16(vm, 9, SYS16(vm, 5)); }
        if (SYS16(vm, 10) + lh > SYS16(vm, 8)) { SET_SYS16(vm, 10, SYS16(vm, 6)); SET_SYS16(vm, 9, SYS16(vm, 5)); }
    }
}
void sys_line(vm_t *vm, int arg) {     /* [EXE: 0x411e40] */
    if (arg) return;
    SET_SYS16(vm, 10, SYS16(vm, 10) + SYS16(vm, 16)); SET_SYS16(vm, 9, SYS16(vm, 5)); SET_SYS16(vm, 2, SYS16(vm, 2) | 0x1000);
}
/* settings store (AI5WIN.INI equivalent). [guess] SYS 27: 0=get -> v16[18], 1=set. Defaults copied from the disc's INI. */
static struct { char key[40]; uint32_t val; } ini[48] = {
    {"MsgMode.AutoCancel", 0}, {"MsgMode.OneMes", 0}, {"MsgMode.AutoMes", 0}, {"MsgMode.Speed", 128},
    {"Music.IsMusic", 1}, {"Music.Volume", 255}, {"Effect.IsEffect", 1}, {"Effect.Volume", 255}, {"Voice.IsVoice", 1}, {"Voice.Volume", 255},
    {"ManVoice.Play", 1}, {"WomanVoice.Play", 1}, {"Msg.Red", 0}, {"Msg.Green", 0}, {"Msg.Blue", 0}, {"Msg.Alpha", 128}, {"DispEffect.Play", 1} };
static uint32_t *ini_slot(const char *sec, const char *key) {
    char k[40]; snprintf(k, sizeof k, "%s.%s", sec, key); int i = 0;
    for (; i < 48 && ini[i].key[0]; i++) if (!strcmp(ini[i].key, k)) return &ini[i].val;
    if (i < 48) { strcpy(ini[i].key, k); return &ini[i].val; }
    return &ini[47].val;
}
/* ---- animation [EXE: class vtable 0x44f50c; tick 0x40f320; init/start/stop 0x40f0c0..0x40f150; draw 0x40f810/0x40f880] ----
   .AX file: u32 stream_offset[320] (10 slots x 32 streams), draw calls of 7 x u32 from +0x500, then stream bytecode.
   One instruction per tick per stream. Tick rate is a [guess]: once per frame. */
typedef struct { uint32_t state, base, ip, stall, wait, l1_ip, l1_cnt, l1_init, l2_ip, l2_cnt, l2_init; } anim_stream;
static anim_stream anim[10][32]; static uint32_t anim_calls;   /* mem offset of draw-call table */
enum { AN_RUN = 0, AN_STOPREQ = 1, AN_HALT = 0xFF };
static void anim_reset(void) { for (int a = 0; a < 10; a++) for (int b = 0; b < 32; b++) anim[a][b].state = AN_HALT; }
static void anim_draw(vm_t *vm, uint32_t n) {
    const uint8_t *r = vm->mem + ((anim_calls + n * 28) & (MEM_SIZE - 1)); uint32_t t = rd32(r);
    int sx = rd32(r + 4), sy = rd32(r + 8), w = rd32(r + 12), h = rd32(r + 16), dx = rd32(r + 20), dy = rd32(r + 24);
    { static int n; if (n++ < 3) plat_log("anim draw #%u type %u src(%d,%d) %dx%d -> (%d,%d)", n, t, sx, sy, w, h, dx, dy); }
    if (t == 0) gfx_copy(sx, sy, w, h, 1, dx, dy, 0, 0);
    else if (t == 1) gfx_copy_key(sx, sy, w, h, 1, dx, dy, 0, 0x00FF00);
    else { static uint8_t warned[8]; if (t < 8 && !warned[t]) { warned[t] = 1; plat_log("TODO anim draw type %u", t); } }
}
static void anim_tick_one(vm_t *vm, anim_stream *s) {
    const uint8_t *m = vm->mem;
    if (s->stall) { s->stall--; return; }
    if (s->wait) { if (s->state == AN_STOPREQ) s->state = AN_HALT; else if (s->state == 3) s->state = 4; s->wait--; return; }
    uint8_t op = m[(s->base + s->ip++) & (MEM_SIZE - 1)];
#define ARG() (s->ip += 4, rd32(m + ((s->base + s->ip - 4) & (MEM_SIZE - 1))))
    switch (op) {
    case 0: break;
    case 1: if (s->state == AN_STOPREQ) s->state = AN_HALT; else if (s->state == 3) s->state = 4; s->wait = ARG(); break;
    case 2: s->stall = ARG(); break;
    case 3: s->ip = 0; break;
    case 4: case 0xFF: s->state = AN_HALT; break;
    case 5: s->l1_cnt = s->l1_init = ARG(); s->l1_ip = s->ip; break;
    case 6: if (!s->l1_init || --s->l1_cnt) s->ip = s->l1_ip; break;
    case 7: s->l2_cnt = s->l2_init = ARG(); s->l2_ip = s->ip; break;
    case 8: if (!s->l2_init || --s->l2_cnt) s->ip = s->l2_ip; break;
    default: anim_draw(vm, ARG()); break;
    }
}
static void anim_tick(vm_t *vm) { for (int a = 0; a < 10; a++) for (int b = 0; b < 32; b++) if (anim[a][b].state != AN_HALT) anim_tick_one(vm, &anim[a][b]); }
static bool frame(vm_t *vm) { anim_tick(vm); return plat_frame(compose(), SW, SH); }
/* ---- sound [EXE: SYS 5 -> 0x4072e0/0x41fdd0, SYS 6 -> 0x4200b0, SYS 7 -> 0x42bce0/0x420270] ---- */
static char snd_cur[SND_SLOTS][24], snd_pend[SND_SLOTS][24]; static uint8_t snd_has_pend[SND_SLOTS];
static void *snd_load(vm_t *vm, int kind, const char *name, size_t *n) {
    void *d = NULL;
    if (kind == SND_VOICE) d = arc_load(&vm->arc_voice, name, n);
    else { d = arc_load(&vm->arc_sound, name, n); if (!d) d = arc_load(&vm->arc_music, name, n); }
    if (!d) { static int warned; if (warned++ < 20) plat_log("missing sound %s", name); }
    return d;
}
static int snd_volume(int kind) {
    const char *sec = kind == SND_MUSIC ? "Music" : kind == SND_SE ? "Effect" : "Voice";
    const char *on = kind == SND_MUSIC ? "IsMusic" : kind == SND_SE ? "IsEffect" : "IsVoice";
    if (!*ini_slot(sec, on)) return 0;
    int v = *ini_slot(sec, "Volume"); return v > 255 ? 255 : v;
}
static void snd_start(vm_t *vm, int kind, int slot, const char *name) {
    plat_snd_stop(slot); snd_cur[slot][0] = 0;
    int vol = snd_volume(kind); if (!vol || !name[0]) return;
    size_t n; void *d = snd_load(vm, kind, name, &n); if (!d) return;
    strncpy(snd_cur[slot], name, 23); plat_snd_play(slot, d, n, vol);
}
static void snd_cmd(vm_t *vm, int kind, int nslots, uint32_t sub, const char *name, uint32_t ch) {
    int slot = kind + (int)(ch % (uint32_t)nslots); uint32_t r = 0;
    switch (sub) {
    case 0: if (kind == SND_MUSIC && !strcasecmp(snd_cur[slot], name) && plat_snd_playing(slot)) break;   /* [EXE 0x407378] same track keeps playing */
            snd_start(vm, kind, slot, name); break;
    case 1: plat_snd_stop(slot); snd_cur[slot][0] = 0; break;
    case 2: if (kind == SND_VOICE) { strncpy(snd_pend[slot], name, 23); snd_has_pend[slot] = 1; break; }   /* voice: reserve */
            plat_snd_fade(slot, 1000); snd_cur[slot][0] = 0; break;                                           /* music/effect: fade out [EXE: volume ramp to -10000] */
    case 3: if (kind == SND_SE) { r = plat_snd_playing(slot); break; }
            if (kind == SND_VOICE) { if (snd_has_pend[slot]) snd_start(vm, kind, slot, snd_pend[slot]); snd_has_pend[slot] = 0; break; }
            strncpy(snd_pend[slot], name, 23); snd_has_pend[slot] = 1; break;                                  /* music: reserve */
    case 4: if (kind == SND_VOICE) { r = plat_snd_playing(slot); break; }
            if (snd_has_pend[slot]) { snd_start(vm, kind, slot, snd_pend[slot]); snd_has_pend[slot] = 0; } break;
    case 5: if (kind == SND_MUSIC) r = plat_snd_playing(slot); break;
    }
    SET_V32(vm, 18, r);
}
extern void vm_run_block(vm_t *);
void sys_call(vm_t *vm, uint32_t fn) {
    sys_init(vm);
    vm_param *p = vm->params; uint32_t sub = p[0].val;
    if (getenv("AI5_TRACE")) plat_log("  sys %u: %u %u %u %u %u %u %u %u %u '%s' [s1=%u cur=%u,%u fl=%04x]", fn, p[0].val, p[1].val, p[2].val, p[3].val, p[4].val, p[5].val, p[6].val, p[7].val, p[8].val, p[1].str, SYS16(vm,1), SYS16(vm,9), SYS16(vm,10), SYS16(vm,2));
    switch (fn) {
    case 3: {                          /* [EXE 0x41fc20] */
        if (sub == 0) { uint32_t a = p[1].val % 10, b = p[2].val & 31, data = p[3].val & (MEM_SIZE - 1); anim_stream *st = &anim[a][b];
            memset(st, 0, sizeof *st); st->state = AN_HALT; st->base = data + rd32(vm->mem + ((data + (a * 32 + b) * 4) & (MEM_SIZE - 1))); anim_calls = data + 0x500; return; }
        if (sub >= 1 && sub <= 5) { anim_stream *st = &anim[p[1].val % 10][p[2].val & 31];
            if (sub == 1) st->state = AN_RUN; else if (sub == 2) st->state = AN_STOPREQ; else if (sub == 3) st->state = AN_HALT;
            else { st->state = AN_RUN;         /* [EXE 0x40f730] play and wait; sub 5 can be cancelled with key 1 */
                while (st->state != AN_HALT && !vm->quit && !(SYS16(vm, 2) & 8)) { if (sub == 5 && plat_key(1)) { st->state = AN_HALT; break; } if (!frame(vm)) vm->quit = true; vm->steps = 0; } }
            return; }
        if (sub == 6) { for (int a = 0; a < 10; a++) for (int b = 0; b < 32; b++) if (anim[a][b].state != AN_HALT) anim[a][b].state = AN_STOPREQ; return; }
        if (sub == 7) { for (int a = 0; a < 10; a++) for (int b = 0; b < 32; b++) anim[a][b].state = AN_HALT; return; }
        break; }
    case 8:
        if (sub == 0) {                /* [EXE 0x41e2b0 partly read] load a DATA.ARC file (LZSS) to the address in p2 */
            size_t n; uint8_t *buf = arc_load_lzss(&vm->arc_data, p[1].str, &n); if (!buf) { plat_log("missing data %s", p[1].str); return; }
            uint32_t addr = p[2].val & (MEM_SIZE - 1); if (addr + n <= MEM_SIZE) memcpy(vm->mem + addr, buf, n); free(buf); return; }
        if (sub == 2) {                /* [guess] load library script and run its top level (defines lib procs) */
            size_t n; uint8_t *buf = arc_load_lzss(&vm->arc_mes, p[1].str, &n); if (!buf) { plat_log("missing %s", p[1].str); return; }
            const uint8_t *smes = vm->mes; uint32_t sip = vm->ip, sbase = vm->base, snl = vm->nlabels; const uint32_t *sl = vm->labels;
            vm->mes = buf; vm->nlabels = rd32(buf); vm->labels = (const uint32_t *)(buf + 4); vm->base = 4 + 4 * vm->nlabels; vm->ip = 0;
            vm_run_block(vm);
            vm->mes = smes; vm->ip = sip; vm->base = sbase; vm->nlabels = snl; vm->labels = sl; return;
        }
        break;
    case 11: {                         /* [EXE: args = sub, x, y, w=x1-x+1, h=y1-y+1, p5.. ] */
        int x = p[1].val, y = p[2].val, w = (int)p[3].val - x + 1, h = (int)p[4].val - y + 1;
        switch (sub) {     /* [EXE 0x420a70 base table + 0x42b920 game table] */
        case 0: gfx_copy(x, y, w, h, p[5].val, p[6].val, p[7].val, p[8].val, 0); return;
        case 1: gfx_copy_key(x, y, w, h, p[5].val, p[6].val, p[7].val, p[8].val, SYS32(vm, 9)); return;
        case 2: gfx_fill(x, y, w, h, SYS16(vm, 1), col_bg, 0); return;
        case 7: gfx_blend(x, y, w, h, p[5].val, p[6].val, p[7].val, p[8].val); return;
        case 8: gfx_copy(x, y, w, h, p[5].val, p[6].val, p[7].val, p[8].val, 1); return;
        case 10: gfx_fill(x, y, w, h, SYS16(vm, 1), col_bg, 1); if (SYS16(vm, 1) == 6 && y == 0) { ui.nlines = 0; ui_last_y = -1; } return;
        }
        break; }
    case 2:                            /* pointer [script-derived]: 2 = read position into sys16[3],[4]; 3 = move it */
        if (sub == 2) { int x, y; plat_cursor(&x, &y, false); SET_SYS16(vm, 3, x); SET_SYS16(vm, 4, y); return; }
        if (sub == 3) { int x = p[1].val, y = p[2].val; plat_cursor(&x, &y, true); SET_SYS16(vm, 3, x); SET_SYS16(vm, 4, y); return; }
        break;
    case 14: {                         /* [script-derived] run a menu item body (address taken from the sys32[7] table) until END */
        uint32_t m = vm->menu_run; if (!vm->menu_mes[m]) return;
        const uint8_t *smes = vm->mes; uint32_t sip = vm->ip, sbase = vm->base, snl = vm->nlabels; const uint32_t *sl = vm->labels;
        vm->mes = vm->menu_mes[m]; vm->nlabels = rd32(vm->mes); vm->labels = (const uint32_t *)(vm->mes + 4); vm->base = 4 + 4 * vm->nlabels; vm->ip = sub;
        vm_run_block(vm);
        vm->mes = smes; vm->ip = sip; vm->base = sbase; vm->nlabels = snl; vm->labels = sl; return; }
    case 16: { uint32_t m = vm->menu_run; if (sub < vm->menu_n[m]) SET_SYS16(vm, 22, vm->menu_id[m][sub]); return; }   /* [script-derived] chosen item -> sys16[22] */
    case 12: { if (getenv("AI5_TRACE")) plat_log("  WAIT %u lit6=%ld", sub, lit(6));                         /* [EXE 0x41e620/0x429260] wait p0 ticks, key 6 skips; tick length is a guess (1/60 s) */
        for (uint32_t t = 0; t < sub && !vm->quit; t++) { if (plat_key(6) || (SYS16(vm, 2) & 8)) break; if (!frame(vm)) vm->quit = true; }
        return; }
    case 18: plat_poll(); SET_V32(vm, 18, plat_key(sub) == (p[1].val != 0)); return;   /* [script-derived] 1 when key state matches p1 (1=pressed, 0=released); result -> v32[18] */
    case 5:                            /* music: (0,name,ch) or (sub,ch) */
        if (sub == 0) snd_cmd(vm, SND_MUSIC, 2, 0, p[1].str, p[2].val); else snd_cmd(vm, SND_MUSIC, 2, sub, "", p[1].val);
        return;
    case 6:                            /* effects: same argument shape as music */
        if (sub == 0) snd_cmd(vm, SND_SE, 4, 0, p[1].str, p[2].val); else snd_cmd(vm, SND_SE, 4, sub, "", p[1].val);
        return;
    case 7:                            /* voice: (sub,name,ch). Disabled unless sys16[2] bit 0x100; key 7 (voice skip) cancels 0/3 */
        if (!(SYS16(vm, 2) & 0x100)) { SET_V32(vm, 18, 0); return; }
        if ((sub == 0 || sub == 3) && plat_key(7)) { plat_snd_stop(SND_VOICE + (p[2].val & 1)); snd_has_pend[SND_VOICE + (p[2].val & 1)] = 0; SET_V32(vm, 18, 0); return; }
        snd_cmd(vm, SND_VOICE, 2, sub, p[1].str, p[2].val);
        return;
    case 27:
        if (sub == 0) { SET_V16(vm, 18, *ini_slot(p[1].str, p[2].str)); return; }
        if (sub == 1) { *ini_slot(p[1].str, p[2].str) = p[3].val; return; }
        break;
    case 21:                           /* [EXE 0x421e20] */
        if (p[1].val < 8) { int n = p[1].val;
            if (sub == 0 || sub == 4) { win[n].used = 1; win[n].x0 = p[2].val; win[n].y0 = p[3].val; win[n].x1 = p[4].val; win[n].y1 = p[5].val; return; }
            if (sub == 1) { win[n].visible = 1; return; }
            if (sub == 2) { win[n].visible = 0; return; }
            if (sub == 3) return;      /* TODO: 0x42cb50 (rect refresh?) */
            if (sub == 5) { SET_V16(vm, 18, win[n].visible); return; } }
        break;
    case 19: if (sub == 3 || sub == 4) SET_V32(vm, 18, 0); return;                         /* backlog: [EXE] 0x405ec0, not implemented yet */
    case 13: col_bg = p[0].val; col_fg = p[1].val; return;                                 /* [EXE] */
    case 25: if (sub == 0) { load_image(vm, p[1].str, SYS16(vm, 1), -1, -1); return; } break;   /* [EXE 0x421b30 -> 0x41ab80; its image cache (0x41f430) is not reproduced] */
    case 9:                            /* [EXE 0x41e4b0 -> 0x420540] load image into surface sys16[1], optionally at (p1,p2) */
        if (vm->nparams >= 3) load_image(vm, p[0].str, SYS16(vm, 1), (int)p[1].val, (int)p[2].val);
        else load_image(vm, p[0].str, SYS16(vm, 1), -1, -1);
        return;
    }
    once(vm, fn, "unimplemented");
}
/* crossfade a rectangle of surface src onto the screen.
   [EXE 0x4102d0]: backup, blend in steps of 15 ms until alpha reaches 0x100, final plain copy. Step size (16) is a guess. */
static void crossfade(vm_t *vm, int src, int x0, int y0, int w, int h) {
    src %= NSURF; if (w <= 0 || h <= 0) return;
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x0 + w > SW) w = SW - x0; if (y0 + h > SH) h = SH - y0; if (w <= 0 || h <= 0) return;
    uint8_t *bak = malloc((size_t)w * h * 3);
    for (int y = 0; y < h; y++) memcpy(bak + (size_t)y * w * 3, PIX(0, x0, y0 + y), w * 3);
    for (int a = 16; a < 256 && !vm->quit; a += 16) {
        if (plat_key(6)) break;
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { if (!INS(src, x0 + x, y0 + y)) continue;
            const uint8_t *o = bak + ((size_t)y * w + x) * 3, *n = PIX(src, x0 + x, y0 + y); uint8_t *d = PIX(0, x0 + x, y0 + y);
            for (int c = 0; c < 3; c++) d[c] = (o[c] * (256 - a) + n[c] * a) >> 8; }
        if (!frame(vm)) vm->quit = true; vm->steps = 0;
    }
    free(bak); gfx_copy(x0, y0, w, h, src, x0, y0, 0, 0);
}
/* UTIL [EXE dispatcher 0x411330, 11 functions] */
void sys_util(vm_t *vm) {
    sys_init(vm);
    vm_param *p = vm->params; uint32_t n = p[0].val;
    switch (n) {
    case 0: shake_x = (int32_t)p[1].val; shake_y = (int32_t)p[2].val; if (!frame(vm)) vm->quit = true; return;      /* [EXE] offset + full repaint; the frame pump here is a guess */
    case 1: crossfade(vm, p[1].val, p[2].val, p[3].val, p[4].val, p[5].val); return;                                  /* [EXE 0x4102d0; src = p1 from script use] */
    case 3: case 4: crossfade(vm, p[5].val, p[1].val, p[2].val, p[3].val, p[4].val); return;                          /* [EXE 0x410e00 / 0x410460 not read: both shown as a crossfade] */
    }
    static uint8_t s[16]; if (n < 16 && !s[n]) { s[n] = 1; plat_log("TODO UTIL %u", n); }
}
/* [EXE 0x4135d0 / 0x414900] */
void sys_op1b(vm_t *vm) { vm->menu_cur = vm->params[0].val & 255; vm->menu_n[vm->menu_cur] = 0; }
void sys_menu_define(vm_t *vm, uint32_t ip) { uint32_t m = vm->menu_cur; if (vm->menu_n[m] < 16) { vm->menu_id[m][vm->menu_n[m]] = vm->params[0].val; vm->menu_item[m][vm->menu_n[m]++] = ip; } vm->menu_mes[m] = vm->mes; }
/* [EXE 0x4149e0] event loop: PROC 8 once, then PROC 9 every tick and PROC 2..7 on keys, until sys16[2] bit 4 or 8 */
void sys_menu_exec(vm_t *vm) {
    sys_init(vm);
    uint32_t m = vm->params[0].val & 255;
    SET_SYS16(vm, 21, vm->menu_n[m]);
    /* [EXE 0x4149e0 copies the item addresses into a table]; here it is the array sys32[7] points at, which the library walks with SYS 14 */
    uint32_t prev_run = vm->menu_run; vm->menu_run = m;
    for (uint32_t i = 0; i < vm->menu_n[m]; i++) wr32(vm->mem + ((SYS32(vm, 7) + 4 * i) & (MEM_SIZE - 1)), vm->menu_item[m][i]);
    vm_call_proc(vm, 8);
    if (getenv("AI5_TRACE")) plat_log("  MENUS %u enter: s106=%u s107=%u f4004=%u f4005=%u f4009=%u v16_18=%u v32_21=%u", m, SYS16(vm,106), SYS16(vm,107), FLAG(vm,4004), FLAG(vm,4005), FLAG(vm,4009), V16(vm,18), V32(vm,21));
    while (!(SYS16(vm, 2) & 0xC) && !vm->quit && !vm->want_goto) {
        vm_call_proc(vm, 9);
        if (SYS16(vm, 2) & 0xC) break;
        for (int k = 0; k < 6; k++) if (plat_key(k)) { if (getenv("AI5_TRACE")) plat_log("  key %d -> PROC %d", k, 2 + k); vm_call_proc(vm, 2 + k); break; }
        if (getenv("AI5_TRACE")) plat_log("  tick flags=%04x", SYS16(vm, 2));
        if (!frame(vm)) vm->quit = true;
        vm->steps = 0;
    }
    vm->menu_run = prev_run;
}
