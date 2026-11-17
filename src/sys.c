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
static uint8_t *dpth[NSURF];           /* port addition for stereoscopic 3D: per-pixel layer, 0 = backdrop (drawn behind the
                                          screen), 1 = sprite/overlay (character blends, key copies, text) at the screen plane */
static int sw[NSURF], sh[NSURF];       /* surfaces grow to fit loaded images (animation sheets are taller than the screen) */
#define PIX(s, x, y) (surf[s] + ((y) * sw[s] + (x)) * 3)
#define MSK(s, x, y) (amask[s][(y) * sw[s] + (x)])
#define DEP(s, x, y) (dpth[s][(y) * sw[s] + (x)])
#define INS(s, x, y) ((x) >= 0 && (y) >= 0 && (x) < sw[s] && (y) < sh[s])
static void surf_ensure(int s, int w, int h) {
    if (w <= sw[s] && h <= sh[s]) return; if (w < sw[s]) w = sw[s]; if (h < sh[s]) h = sh[s];
    uint8_t *c = calloc(1, (size_t)w * h * 3), *m = calloc(1, (size_t)w * h), *z = calloc(1, (size_t)w * h);
    for (int y = 0; y < sh[s]; y++) { memcpy(c + (size_t)y * w * 3, surf[s] + (size_t)y * sw[s] * 3, sw[s] * 3); memcpy(m + (size_t)y * w, amask[s] + (size_t)y * sw[s], sw[s]);
        memcpy(z + (size_t)y * w, dpth[s] + (size_t)y * sw[s], sw[s]); }
    free(surf[s]); free(amask[s]); free(dpth[s]); surf[s] = c; amask[s] = m; dpth[s] = z; sw[s] = w; sh[s] = h;
}
static uint32_t col_fg = 0xFFFFFF, col_bg = 0;   /* COLORREF 0x00BBGGRR  [EXE: SYS 13 -> bg=p0, fg=p1] */
static const uint8_t *font; static uint32_t font_cells; static int font_flags;   /* font16.bin header u16 at +6: bit 0 = Korean glyph remap */
/* message windows [EXE 0x421e20: create/show/hide/rect]. How they are blended is a [guess]:
   window 0 shows the top-left of surface 6; a 50% dark veil (ini [Msg] Alpha=128) is laid first, then surface 6 is blended through its mask plane. */
static struct { int used, visible, x0, y0, x1, y1; } win[8];
static uint8_t *screen_out;
static int shake_x, shake_y;          /* [EXE UTIL 0 -> 0x40ffa0/0x41cf70] display offset used for screen shakes */
static int fx_left;                   /* SYS 10,1: frames left of the fade from the snapshot in surface 11 to the live screen */
static const uint8_t *compose(void);
static int cur_loop = -1;              /* MENUS loop being run (100 = story), -1 none */
/* title menu popup (UTIL 10,1) drawn over the scene; see title_menu() */
static struct { bool on; int x, y, n, hl, pressed, w; uint8_t *rgb, *a; } pop;
static struct { bool on; int x, y, sel; gcc_image im; } dlg;   /* confirmation dialog (SYS 28) */
static void dlg_draw(uint8_t *out, int OW, int OH) {
    /* sheet (272x184, BGR bottom-up): panel rows 4..107 with (0,255,0) as transparent; button sprites 94x18,
       state rows 115 / 139 / 163, x 7 (확인) and 115 (취소); the panel's buttons sit at (29,58) and (149,58), 96x22 */
    const gcc_image *im = &dlg.im;
    #define SRC(x, y) (im->bgr + ((size_t)(im->h - 1 - (y)) * im->w + (x)) * 3)
    for (int y = 4; y < 108; y++) for (int x = 4; x < im->w - 4; x++) { const uint8_t *c = SRC(x, y); if (c[0] == 0 && c[1] == 255 && c[2] == 0) continue;
        int tx = dlg.x + x, ty = dlg.y + y; if (tx < 0 || ty < 0 || tx >= OW || ty >= OH) continue; uint8_t *d = out + (ty * OW + tx) * 3; d[0] = c[2]; d[1] = c[1]; d[2] = c[0]; }
    for (int b = 0; b < 2; b++) if (b == dlg.sel) {          /* focused button: the outlined sprite */
        int sx = b ? 115 : 7, sy = 163, dx = dlg.x + (b ? 149 : 29) + 1, dy = dlg.y + 58 + 2;
        for (int y = 0; y < 18; y++) for (int x = 0; x < 94; x++) { const uint8_t *c = SRC(sx + x, sy + y); if (c[0] == 0 && c[1] == 255 && c[2] == 0) continue; if (c[0] == 0 && c[1] == 0 && c[2] == 0 && (x < 2 || x > 91)) continue;
            int tx = dx + x, ty = dy + y; if (tx < 0 || ty < 0 || tx >= OW || ty >= OH) continue; uint8_t *d = out + (ty * OW + tx) * 3; d[0] = c[2]; d[1] = c[1]; d[2] = c[0]; } }
    #undef SRC
}
static void pop_draw(uint8_t *out, int OW, int OH) {   /* item i, state st (0 normal, 1 highlight, 2 pressed) = rows (i*3+st)*36 of the sheet */
    if (dlg.on) dlg_draw(out, OW, OH);
    if (!pop.on) return;
    for (int i = 0; i < pop.n; i++) { int st = i == pop.pressed ? 2 : i == pop.hl ? 1 : 0, sy = (i * 3 + st) * 36;
        for (int y = 0; y < 36; y++) for (int x = 0; x < pop.w; x++) { int tx = pop.x + x, ty = pop.y + i * 36 + y; if (tx < 0 || ty < 0 || tx >= OW || ty >= OH) continue;
            int a = pop.a[(sy + y) * pop.w + x]; if (!a) continue; const uint8_t *c = pop.rgb + ((sy + y) * pop.w + x) * 3; uint8_t *d = out + (ty * OW + tx) * 3;
            for (int k = 0; k < 3; k++) d[k] = (d[k] * (255 - a) + c[k] * a) / 255; } }
}
/* Port: selection UIs live on the 3DS bottom screen. The title menu and the confirmation dialogs are drawn 1:1 onto a
   320x240 picture of the (dimmed) screen; full-screen menus (MENUS loops other than 100: system menu, save/load) are
   shown on the bottom screen by the platform while the top screen keeps the picture from before the menu opened. */
static uint8_t bottom_buf[320 * 240 * 3];
static uint8_t *snap_rgb, *snap_z; static bool snap_on, snap_pending;
const uint8_t *sys_live_rgb(void) { return surf[0]; }
/* System menu without "hide window" and "settings" (LIBLARY.LIB, LIBDEF 6): constant patches, applied only where the
   bytes match the original (Japanese and Korean releases share the layout; tools/patch_sysmenu.py has the same list). */
void sys_fix_script(const char *name, uint8_t *code, size_t n) {
    if (strcasecmp(name, "LIBLARY.LIB")) return;
    static const struct { uint16_t addr; uint8_t off, old, now; } fx[] = {
        {0x1d7e, 3, 0, 0x63}, {0x1e16, 3, 1, 0x63}, {0x22ad, 3, 0, 0x63}, {0x22cc, 3, 1, 0x63}, {0x23d6, 3, 0, 2}, {0x240b, 3, 0, 2},
        {0x2580, 2, 0, 2}, {0x20f4, 4, 7, 0x63}, {0x215d, 4, 7, 0x63}, {0x212c, 4, 0, 0x63} };
    int done = 0;
    for (size_t i = 0; i < sizeof fx / sizeof fx[0]; i++) { size_t a = 4 + fx[i].addr + fx[i].off; if (a < n && code[a] == fx[i].old) { code[a] = fx[i].now; done++; } }
    plat_log("system menu patch: %d", done);
}
/* ---- bottom-screen layouts for the full-screen menus (picked by the hit table the library loaded last) ---- */
static char cur_a6[24];
static int slot_page;
static char slot_text[10][96];                 /* text the save/load screen printed into each slot row */
static void slot_capture(vm_t *vm, const char *sj) {
    if (cur_loop < 0 || cur_loop == 100) return;
    int x = SYS16(vm, 9), y = SYS16(vm, 10); if (x < 40 || x >= 556 || y < 52 || y >= 444) return;
    int r = (y - 52) / 40; if (r < 0 || r > 9) return;
    if (x < 70) slot_text[r][0] = 0;           /* a row is (re)drawn from its left edge */
    size_t l = strlen(slot_text[r]); if (l && x >= 200) { if (l + 2 < sizeof slot_text[r]) { strcat(slot_text[r], " "); l++; } }
    snprintf(slot_text[r] + l, sizeof slot_text[r] - l, "%s", sj);
}
typedef struct { int sx, sy, w, h, dx, dy, mx, my; } lay_rect;   /* screen rect -> bottom position; (mx,my) = the button's shape in mwaku.gcc */
static const lay_rect menu_lay[] = { {436, 16, 192, 36, 64, 2, 0, 208}, {436, 64, 192, 36, 64, 41, 192, 208}, {436, 112, 192, 36, 64, 80, 384, 208},
                                     {20, 388, 192, 36, 64, 119, 384, 244}, {20, 436, 192, 36, 64, 158, 576, 244}, {436, 436, 192, 36, 64, 197, 576, 208} };
static int bottom_kind(void) {               /* 1 system menu, 2 save/load list, 3 save/memo dialog, 4 load dialog */
    if (cur_loop < 0 || cur_loop == 100) return 0;
    if (!strcasecmp(cur_a6, "menu.a6")) return 1;
    if (!strcasecmp(cur_a6, "sl.a6")) return 2;
    if (!strcasecmp(cur_a6, "save_dlg.a6") || !strcasecmp(cur_a6, "memo_dlg.a6")) return 3;
    if (!strcasecmp(cur_a6, "load_dlg.a6")) return 4;
    return 0;
}
static void dim_bg(void) {                    /* plain dark backdrop for the bottom-screen layouts */
    for (int y = 0; y < 240; y++) { int c = 10 + y * 22 / 239; for (int x = 0; x < 320; x++) { uint8_t *d = bottom_buf + (y * 320 + x) * 3; d[0] = c / 2; d[1] = c / 2; d[2] = c + 12; } }
}
static void blit_fit(int sx, int sy, int w, int h, int *ox, int *oy, int *ow, int *oh) {   /* crop of the screen, scaled down to fit, centred */
    int dw = w, dh = h; if (dw > 316) { dh = dh * 316 / dw; dw = 316; } if (dh > 236) { dw = dw * 236 / dh; dh = 236; }
    *ox = (320 - dw) / 2; *oy = (240 - dh) / 2; *ow = dw; *oh = dh;
    for (int y = 0; y < dh; y++) for (int x = 0; x < dw; x++) { int fx = sx + x * w / dw, fy = sy + y * h / dh; if (fx >= SW || fy >= SH) continue;
        memcpy(bottom_buf + ((*oy + y) * 320 + *ox + x) * 3, surf[0] + (fy * SW + fx) * 3, 3); }
}
static void bt_text(int x, int y, const char *sj, int r, int g, int b) {   /* font16 text into bottom_buf (Korean font: half-width word spaces) */
    const uint8_t *p = (const uint8_t *)sj;
    while (*p && x < 320) { int half; uint32_t idx = sys_glyph_index(&p, &half); int w = half ? 8 : 16;
        if (idx == 95 && (font_flags & 1)) { x += 8; continue; }
        if (font && idx < font_cells) { const uint8_t *gl = font + idx * 128;
            for (int j = 0; j < 16; j++) for (int i = 0; i < w; i++) { int tx = x + i, ty = y + j; if (tx < 0 || tx >= 320 || ty < 0 || ty >= 240) continue;
                int a = ((gl[(j * 16 + i) >> 1] >> ((i & 1) ? 0 : 4)) & 15) * 17; if (!a) continue; uint8_t *d = bottom_buf + (ty * 320 + tx) * 3;
                d[0] = (d[0] * (255 - a) + r * a) / 255; d[1] = (d[1] * (255 - a) + g * a) / 255; d[2] = (d[2] * (255 - a) + b * a) / 255; } }
        x += w; }
}
static void fill_b(int x0, int y0, int w, int h, int r, int g, int b) { for (int y = y0; y < y0 + h; y++) for (int x = x0; x < x0 + w; x++) if (x >= 0 && y >= 0 && x < 320 && y < 240) { uint8_t *d = bottom_buf + (y * 320 + x) * 3; d[0] = r; d[1] = g; d[2] = b; } }
static int fit_ox, fit_oy, fit_w, fit_h, fit_sx, fit_sy, fit_sw, fit_sh;
static const uint8_t *menu_bottom(void) {
    int k = bottom_kind(); if (!k || !surf[0]) return NULL;
    dim_bg();
    if (k == 1) for (size_t i = 0; i < sizeof menu_lay / sizeof menu_lay[0]; i++) { const lay_rect *L = &menu_lay[i];   /* live button pixels, cut out by its shape */
        for (int y = 0; y < L->h; y++) for (int x = 0; x < L->w; x++) { int a = INS(7, L->mx + x, L->my + y) ? MSK(7, L->mx + x, L->my + y) : 255; if (!a) continue;
            const uint8_t *c = surf[0] + ((L->sy + y) * SW + L->sx + x) * 3; uint8_t *d = bottom_buf + ((L->dy + y) * 320 + L->dx + x) * 3;
            for (int q = 0; q < 3; q++) d[q] = (d[q] * (255 - a) + c[q] * a) / 255; } }
    if (k == 2) {                                /* slot list as text rows (22 px) + page tabs */
        int cx, cy; plat_cursor(&cx, &cy, false); int hot = (cx >= 40 && cx < 556 && cy >= 52 && cy < 444) ? (cy - 52) / 40 : -1;
        fill_b(0, 0, 320, 240, 24, 16, 10);
        for (int r = 0; r < 10; r++) { int y = r * 22, sy = 52 + 40 * r + 5;   /* the row's number/date/"unused" are images: crop them 1:1 */
            for (int yy = 0; yy < 22; yy++) memcpy(bottom_buf + ((y + yy) * 320 + 2) * 3, surf[0] + ((sy + yy) * SW + 44) * 3, 316 * 3);
            if (r == hot) { fill_b(0, y, 320, 2, 239, 0, 95); fill_b(0, y + 20, 320, 2, 239, 0, 95); fill_b(0, y, 2, 22, 239, 0, 95); fill_b(318, y, 2, 22, 239, 0, 95); } }
        /* page tabs: "1쪽".."4쪽", "닫기" through the Korean glyph slots; digits / "X" with the Japanese font */
        static const char *kr[5] = { "1\x99\xf9", "2\x99\xf9", "3\x99\xf9", "4\x99\xf9", "\x90\x70\x8b\xa3" }, *jp[5] = { "1", "2", "3", "4", "X" };
        for (int t = 0; t < 5; t++) { int x = t * 64; const char *l = (font_flags & 1) ? kr[t] : jp[t]; int w = (int)strlen(l) == 1 ? 8 : (t < 4 ? 24 : 32);
            bool on = false; fill_b(x + 2, 222, 60, 17, on ? 255 : 230, on ? 200 : 220, on ? 120 : 190);
            bt_text(x + 32 - w / 2, 223, l, 40, 20, 10); }
    }
    if (k == 3) { fit_sx = 128; fit_sy = 188; fit_sw = 384; fit_sh = 104; }
    if (k == 4) { fit_sx = 184; fit_sy = 184; fit_sw = 272; fit_sh = 112; }
    if (k >= 3) blit_fit(fit_sx, fit_sy, fit_sw, fit_sh, &fit_ox, &fit_oy, &fit_w, &fit_h);
    return bottom_buf;
}
/* galleries (CG / scenes / music / endings: the story loop with no text window): the bottom screen shows the screen 1:1
   in a 320x240 window that follows the pointer (D-pad) and can be dragged; the top screen shows the whole page. */
static int vp_x, vp_y, vp_px = -1, vp_py = -1;
bool sys_bottom_viewport(void) { return cur_loop == 100 && !(win[0].used && win[0].visible) && !(win[1].used && win[1].visible) && !pop.on && !dlg.on; }
static void vp_clamp(void) { if (vp_x < 0) vp_x = 0; if (vp_y < 0) vp_y = 0; if (vp_x > SW - 320) vp_x = SW - 320; if (vp_y > SH - 240) vp_y = SH - 240; }
void sys_bottom_pan(int dx, int dy) { vp_x += dx; vp_y += dy; vp_clamp(); }
const uint8_t *sys_viewport_rgb(bool follow) {
    if (!surf[0]) return NULL;
    int cx, cy; plat_cursor(&cx, &cy, false);
    if (follow && (cx != vp_px || cy != vp_py)) {   /* pointer moved by the game (D-pad): keep it inside the window */
        if (cx < vp_x + 40) vp_x = cx - 40; if (cx > vp_x + 280) vp_x = cx - 280; if (cy < vp_y + 40) vp_y = cy - 40; if (cy > vp_y + 200) vp_y = cy - 200; vp_clamp(); }
    vp_px = cx; vp_py = cy;
    for (int y = 0; y < 240; y++) memcpy(bottom_buf + y * 320 * 3, surf[0] + ((vp_y + y) * SW + vp_x) * 3, 320 * 3);
    return bottom_buf;
}
bool sys_bottom_map(int bx, int by, int *x, int *y) {
    if (sys_bottom_viewport() && !bottom_kind()) { *x = vp_x + bx; *y = vp_y + by; vp_px = *x; vp_py = *y; return true; }
    if (pop.on || dlg.on) { *x = bx * 2; *y = by * 2; return true; }
    switch (bottom_kind()) {
    case 1: for (size_t i = 0; i < sizeof menu_lay / sizeof menu_lay[0]; i++) { const lay_rect *L = &menu_lay[i];
                if (bx >= L->dx && bx < L->dx + L->w && by >= L->dy && by < L->dy + L->h) { *x = L->sx + bx - L->dx; *y = L->sy + by - L->dy; return true; } }
            *x = 320; *y = 240; return true;            /* empty space: no button */
    case 2: if (by < 220) { *x = 298; *y = 52 + (by / 22) * 40 + 16; return true; }
            { int t = bx / 64; *x = 586; *y = t < 4 ? 88 + t * 72 : 396; return true; }
    case 3: case 4: if (fit_w <= 0) break;
            *x = fit_sx + (bx - fit_ox) * fit_sw / fit_w; *y = fit_sy + (by - fit_oy) * fit_sh / fit_h; return true;
    }
    *x = bx * 2; *y = by * 2; return false;
}
const uint8_t *sys_bottom_rgb(void) {
    if (!pop.on && !dlg.on) return menu_bottom();
    for (int y = 0; y < 240; y++) for (int x = 0; x < 320; x++) { const uint8_t *a = surf[0] + ((y * 2) * SW + x * 2) * 3; uint8_t *d = bottom_buf + (y * 320 + x) * 3;
        for (int k = 0; k < 3; k++) d[k] = a[k] / 5; }
    pop_draw(bottom_buf, 320, 240); return bottom_buf;
}
const uint8_t *sys_scene_depth(void) {
    if (snap_on && snap_z) return snap_z;
    return (surf[0] && !shake_x && !shake_y) ? dpth[0] : NULL; }
const uint8_t *sys_scene_rgb(void) {
    if (!surf[0]) return NULL;
    if (snap_on && snap_rgb) return snap_rgb;
    if (!shake_x && !shake_y) return surf[0];
    int v0 = win[0].visible, v1 = win[1].visible; win[0].visible = win[1].visible = 0; const uint8_t *r = compose(); win[0].visible = v0; win[1].visible = v1; return r;
}
static const uint8_t *compose(void) {
    if (!(win[0].used && win[0].visible) && !(win[1].used && win[1].visible) && !shake_x && !shake_y && !fx_left) return surf[0];
    if (fx_left) { int a = fx_left * 255 / 16; fx_left--;      /* [EXE 0x42080a] 16-step fade from the snapshot */
        for (int i = 0; i < SW * SH * 3; i++) { int sx = i / 3 % SW, sy = i / 3 / SW; uint8_t o = INS(11, sx, sy) ? PIX(11, sx, sy)[i % 3] : 0;
            screen_out[i] = (o * a + surf[0][i] * (255 - a)) / 255; } }
    else if (!shake_x && !shake_y) memcpy(screen_out, surf[0], SW * SH * 3);
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
static uint32_t *ini_slot(const char *sec, const char *key);
static void snd_start(vm_t *vm, int kind, int slot, const char *name);
static void load_image(vm_t *vm, const char *name, int dst, int ox, int oy);
static void sys_init(vm_t *vm) {
    if (surf[0]) return;
    for (int i = 0; i < NSURF; i++) { surf[i] = calloc(1, SW * SH * 3); amask[i] = calloc(1, SW * SH); dpth[i] = calloc(1, SW * SH); sw[i] = SW; sh[i] = SH; }
    screen_out = calloc(1, SW * SH * 3);
    anim_reset();
    for (int i = 1; i < 8; i++) SET_SYS32(vm, i, 0x40000 + i * 0x20000);   /* [guess] file areas the scripts address through sys32[n] */
    SET_SYS16(vm, 0, 0x1310);          /* [EXE 0x4057b8] base of the save-list area (45 bytes per slot) */
    SET_SYS16(vm, 23, 10);             /* [EXE 0x4058b2] working surface: scenes are composed on surface 10, then wiped onto the screen */
    FLAG(vm, 4016) = 1;                /* port choice: f4016 ("opening already seen", set by S01) makes START.MES show the title menu;
                                          kept set so the menu appears from the first launch (also after flag loads in SYS 4,3/4,4) */
    /* [EXE 0x4058c8..0x40594b] start-up flags: 0x402, display effects (ini DispEffect.Play) 0x2000, 0x8000 */
    SET_SYS16(vm, 2, SYS16(vm, 2) | 0x402 | 0x8000 | (*ini_slot("DispEffect", "Play") ? 0x2000 : 0));
    SET_SYS32(vm, 0, 0);               /* [EXE 0x4057ce: real memory base; here offsets] sys32[0] is the memory base: scripts pass sys32[0]+offset as a pointer */
    { /* [EXE 0x4070eb] the engine itself loads the menu/window parts sheet into surface 7 at start-up */
      uint16_t r[4] = { SYS16(vm, 17), SYS16(vm, 18), SYS16(vm, 19), SYS16(vm, 20) };
      load_image(vm, "mwaku.gcc", 7, -1, -1);
      SET_SYS16(vm, 17, r[0]); SET_SYS16(vm, 18, r[1]); SET_SYS16(vm, 19, r[2]); SET_SYS16(vm, 20, r[3]); }
    size_t n; uint8_t *f = plat_read_file("font16.bin", &n);
    if (f && n > 12 && !memcmp(f, "AF16", 4)) { font_cells = rd32(f + 8); font = f + 12; font_flags = rd16(f + 6); } else plat_log("font16.bin missing");
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
    /* Port addition: small cache of decoded images. Some screens (CG gallery) reload the same sheet many times
       per frame; the PC decodes fast enough, the 3DS does not. */
    enum { NCACHE = 4 };
    static struct { char name[32]; gcc_image im; unsigned use; } cache[NCACHE]; static unsigned tick;
    gcc_image im; dst %= NSURF; int hit = -1;
    for (int i = 0; i < NCACHE; i++) if (cache[i].im.bgr && !strcasecmp(cache[i].name, name)) { hit = i; break; }
    if (hit < 0) {
        size_t n; uint8_t *raw = arc_load(&vm->arc_bg, name, &n); if (!raw) { plat_log("missing image %s", name); return; }
        int rc = gcc_decode(raw, n, &im); free(raw); if (rc) return;
        hit = 0; for (int i = 1; i < NCACHE; i++) if (!cache[i].im.bgr || (cache[hit].im.bgr && cache[i].use < cache[hit].use)) hit = i;
        free(cache[hit].im.bgr); free(cache[hit].im.alpha);
        cache[hit].im = im; snprintf(cache[hit].name, sizeof cache[hit].name, "%s", name);
    }
    cache[hit].use = ++tick; im = cache[hit].im;
    {
        int cx = im.x + (ox != -1 ? ox : 0), cy = im.y + (oy != -1 ? oy : 0);
        int ax = ox != -1 ? ox : im.ax, ay = oy != -1 ? oy : im.ay;
        if (dst != 0) { surf_ensure(dst, cx + im.w, cy + im.h); if (im.has_alpha) surf_ensure(dst, ax + im.aw, ay + im.ah); }
        for (int y = 0; y < im.h; y++) { int dy = cy + y; const uint8_t *s = im.bgr + (size_t)(im.h - 1 - y) * im.w * 3;
            for (int x = 0; x < im.w; x++) { int dx = cx + x; if (!INS(dst, dx, dy)) continue;
                uint8_t *d = PIX(dst, dx, dy); d[0] = s[x * 3 + 2]; d[1] = s[x * 3 + 1]; d[2] = s[x * 3]; DEP(dst, dx, dy) = 0; } }
        if (im.has_alpha) for (int y = 0; y < im.ah; y++) { int dy = ay + y; const uint8_t *s = im.alpha + (size_t)(im.ah - 1 - y) * im.aw;
            for (int x = 0; x < im.aw; x++) { int dx = ax + x; if (INS(dst, dx, dy)) MSK(dst, dx, dy) = s[x]; } }
        int w = im.w, h = im.h; if (cx + w > sw[dst]) w = sw[dst] - cx; if (cy + h > sh[dst]) h = sh[dst] - cy;
        SET_SYS16(vm, 17, cx); SET_SYS16(vm, 18, cy); SET_SYS16(vm, 19, w > 0 ? w : 0); SET_SYS16(vm, 20, h > 0 ? h : 0);
    }
}
#define EACH(body) for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { int sx = x0 + x, sy = y0 + y, tx = dx + x, ty = dy + y; if (!INS(ss, sx, sy) || !INS(ds, tx, ty)) continue; body }
/* [EXE 0x41ae60] colour copy; with_mask adds [EXE 0x41b130] mask-plane copy */
static void gfx_copy(int x0, int y0, int w, int h, int ss, int dx, int dy, int ds, int with_mask) {   /* with_mask 2: keep the destination's 3D layer */
    ss %= NSURF; ds %= NSURF;
    EACH(memcpy(PIX(ds, tx, ty), PIX(ss, sx, sy), 3); if (with_mask == 1) MSK(ds, tx, ty) = MSK(ss, sx, sy); if (with_mask != 2) DEP(ds, tx, ty) = DEP(ss, sx, sy);)
}
/* [EXE 0x41af20] copy skipping the key colour (COLORREF); exact compare is assumed */
static void gfx_copy_key(int x0, int y0, int w, int h, int ss, int dx, int dy, int ds, uint32_t key, int keep_layer) {
    ss %= NSURF; ds %= NSURF; uint8_t kr = key, kg = key >> 8, kb = key >> 16;
    EACH(const uint8_t *s = PIX(ss, sx, sy); if (s[0] == kr && s[1] == kg && s[2] == kb) continue; memcpy(PIX(ds, tx, ty), s, 3); if (!keep_layer) DEP(ds, tx, ty) = 1;)
}
/* [EXE 0x41b640/0x41b310] blend src over dst through the source mask plane (0 = skip, 255 = copy) */
static void gfx_blend(int x0, int y0, int w, int h, int ss, int dx, int dy, int ds) {
    ss %= NSURF; ds %= NSURF;
    EACH(int a = MSK(ss, sx, sy); if (!a) continue; const uint8_t *s = PIX(ss, sx, sy); uint8_t *d = PIX(ds, tx, ty); if (a >= 128) DEP(ds, tx, ty) = 1;
        if (a == 255) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; } else for (int c = 0; c < 3; c++) d[c] = (s[c] * a + d[c] * (255 - a)) / 255;)
}
/* [EXE 0x41b390] fill; clear_mask adds [EXE 0x41b040] */
static void gfx_fill(int x0, int y0, int w, int h, int s, uint32_t c, int clear_mask) {
    s %= NSURF;
    for (int y = y0; y < y0 + h; y++) for (int x = x0; x < x0 + w; x++) { if (!INS(s, x, y)) continue;
        uint8_t *d = PIX(s, x, y); d[0] = c; d[1] = c >> 8; d[2] = c >> 16; DEP(s, x, y) = 0; if (clear_mask) MSK(s, x, y) = 0; }
}
/* ---- text  [EXE: 0x412bf0 / 0x412950 cursor logic] ---- */
static void draw_glyph(int s, int x, int y, uint32_t idx, int w) {    /* writes fg colour and raises the mask plane by glyph coverage */
    if (!font || idx >= font_cells) return; const uint8_t *g = font + idx * 128; s %= NSURF;
    int r = col_fg & 255, gr = col_fg >> 8 & 255, b = col_fg >> 16 & 255;
    for (int j = 0; j < 16; j++) for (int i = 0; i < w; i++) { int tx = x + i, ty = y + j; if (!INS(s, tx, ty)) continue;
        int a = ((g[(j * 16 + i) >> 1] >> ((i & 1) ? 0 : 4)) & 15) * 17; if (!a) continue;
        uint8_t *d = PIX(s, tx, ty), *m = &MSK(s, tx, ty); if (a >= 128) DEP(s, tx, ty) = 1;
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
int sys_font_flags(void) { return font_flags; }
const uint8_t *sys_font(uint32_t *cells) { if (cells) *cells = font_cells; return font; }
/* text drawn into the dialogue/choice strip (surface 6, rows 0..87) is also kept as strings for platforms that re-render it */
static ui_text ui; static int ui_last_y = -1;
/* ---- message log  [EXE SYS 19: 0x405ec0 / 0x41eaf0; viewer 0x42bf40]
   66 entries of 0x800 bytes. While sys16[2] bit 0x10 (logging on) is set, the first character printed opens an entry
   (bit 0x20 = entry open), text is appended, and SYS 19,2 closes it. 0 clear, 3 count -> v32[18], 4 n -> text "pointer",
   5 n -> voice, 6 = viewer (here: on the small screen, see sys_ui_text). The voice field is a port addition for replay. ---- */
enum { BL_N = 66, BL_TAG = 0xB0000000u };
static struct { char t[0x800]; int len, done; char voice[24]; } bl[BL_N];
static int bl_cur; static char bl_voice[24];
static struct { bool on; int sel, top; } blv;
static void bl_begin(vm_t *vm) {
    SET_SYS16(vm, 2, SYS16(vm, 2) | 0x20);
    if (bl_cur == BL_N) { memmove(&bl[0], &bl[1], sizeof bl[0] * (BL_N - 1)); bl_cur = BL_N - 1; }
    bl[bl_cur].len = 0; bl[bl_cur].t[0] = 0; bl[bl_cur].done = 0;
    memcpy(bl[bl_cur].voice, bl_voice, sizeof bl_voice); bl_voice[0] = 0;
}
static void bl_add(vm_t *vm, const uint8_t *from, size_t n) {
    uint16_t f = SYS16(vm, 2); if (!(f & 0x10) || (f & 0x800)) return;
    if (!(f & 0x20)) bl_begin(vm);
    if (bl_cur >= BL_N) return;
    int *L = &bl[bl_cur].len; if (*L + (int)n < (int)sizeof bl[0].t) { memcpy(bl[bl_cur].t + *L, from, n); *L += (int)n; bl[bl_cur].t[*L] = 0; }
}
static int bl_count(void) { int n = 0; while (n < BL_N && bl[n].done) n++; return n; }
static void ui_capture(vm_t *vm, const uint8_t *from, const uint8_t *to) {
    if (SYS16(vm, 1) != 6 || SYS16(vm, 10) >= 88) return;
    int y = SYS16(vm, 10);
    if (y != ui_last_y || ui.nlines == 0) { if (ui.nlines >= 16) return; ui.line[ui.nlines].sjis[0] = 0; ui.line[ui.nlines].item = -1; ui.nlines++; ui_last_y = y; }
    char *d = ui.line[ui.nlines - 1].sjis; size_t n = strlen(d), add = to - from;
    if (n + add < sizeof ui.line[0].sjis) { memcpy(d + n, from, add); d[n + add] = 0; }
}
/* log viewer for small screens: entries wrapped to 40 half-width columns, newest at the bottom; the selected entry is the
   highlighted "item". Last row is a key hint. */
enum { BL_ROWS = 12, BL_COLS = 40 };
static int bl_wrap(const char *t, char out[][BL_COLS + 2], int max) {   /* returns row count; out may be NULL */
    int rows = 0, col = 0; char cur[BL_COLS + 2]; int cl = 0; const uint8_t *q = (const uint8_t *)t;
    while (*q) {
        if (*q == '\n') { if (cl) { if (out && rows < max) { cur[cl] = 0; strcpy(out[rows], cur); } rows++; cl = col = 0; } q++; continue; }
        int w = (((*q >= 0x81 && *q <= 0x9F) || *q >= 0xE0) && q[1]) ? 2 : 1;
        if (col + w > BL_COLS) { if (out && rows < max) { cur[cl] = 0; strcpy(out[rows], cur); } rows++; cl = col = 0; }
        memcpy(cur + cl, q, w); cl += w; col += w; q += w;
    }
    if (cl) { if (out && rows < max) { cur[cl] = 0; strcpy(out[rows], cur); } rows++; }
    if (!rows && out && max) out[0][0] = 0;
    return rows ? rows : 1;
}
static void bl_fix_top(void) {
    if (blv.top > blv.sel) blv.top = blv.sel;
    for (;;) { int r = 0; for (int e = blv.top; e <= blv.sel; e++) r += bl_wrap(bl[e].t, NULL, 0); if (r <= BL_ROWS || blv.top >= blv.sel) break; blv.top++; }
}
static ui_text blui;
static const ui_text *bl_ui(void) {
    int n = bl_count(); blui.nlines = 0; blui.dialogue_visible = true; blui.menu_visible = false; blui.loop = 100; blui.hover = blv.sel; blui.backlog = true;
    static char rows[24][BL_COLS + 2];
    for (int e = blv.top; e < n && blui.nlines < BL_ROWS; e++) {
        int r = bl_wrap(bl[e].t, rows, 24);
        for (int i = 0; i < r && i < 24 && blui.nlines < BL_ROWS; i++) { strcpy(blui.line[blui.nlines].sjis, rows[i]); blui.line[blui.nlines++].item = e; }
    }
    while (blui.nlines < BL_ROWS) { blui.line[blui.nlines].sjis[0] = 0; blui.line[blui.nlines++].item = -1; }
    /* "Ｂ：戻る　Ａ：音声　↑↓：選択" in Shift-JIS */
    if (font_flags & 1)   /* Korean font: "Ｂ：닫기　Ａ：음성　↑↓：선택" through the Korean patch's glyph slots (assets/kr/nonokr_map.json) */
        strcpy(blui.line[blui.nlines].sjis, "\x82\x61\x81\x46\x90\x70\x8b\xa3\x81\x40\x82\x60\x81\x46\x99\x5e\x96\x64\x81\x40\x81\xaa\x81\xab\x81\x46\x96\x48\x9a\xf2");
    else
    strcpy(blui.line[blui.nlines].sjis, "\x82\x61\x81\x46\x96\xdf\x82\xe9\x81\x40\x82\x60\x81\x46\x89\xb9\x90\xba\x81\x40\x81\xaa\x81\xab\x81\x46\x91\x49\x91\xf0");
    blui.line[blui.nlines++].item = -1;
    return &blui;
}
const ui_text *sys_ui_text(vm_t *vm) {
    if (blv.on) return bl_ui();
    ui.backlog = false;
    ui.dialogue_visible = win[0].used && win[0].visible; ui.menu_visible = win[1].used && win[1].visible; ui.loop = cur_loop;
    ui.hover = (ui.menu_visible && V32(vm, 9) < 16) ? (int)V32(vm, 9) : -1;     /* v32[9] is the library's "highlighted choice" variable */
    for (int i = 0; i < ui.nlines; i++) ui.line[i].item = ui.menu_visible ? i : -1;
    return &ui;
}
/* ---- port addition: large full-screen narration (opening disclaimer, some endings). Those scripts print 16 px text onto
   surface 3 with the text area set to the whole screen; on the 3DS top screen that ends up 8 px tall. Such text is collected
   and, when surface 3 is next used as a source, laid out 3x larger (48 px), word-wrapped and centred. ---- */
enum { BIG = 3, BIG_PARA = 8 };
static struct { int n; char t[BIG_PARA][512]; int y[BIG_PARA]; } big;
static bool big_mode(vm_t *vm) { return SYS16(vm, 1) == 3 && SYS16(vm, 5) == 0 && SYS16(vm, 6) == 0 && SYS16(vm, 7) >= 640 && SYS16(vm, 8) >= 480; }
static void draw_glyph_big(int s, int x, int y, uint32_t idx, int w) {   /* bilinear 3x upscale of a 16x16 4bpp glyph */
    if (!font || idx >= font_cells) return; const uint8_t *g = font + idx * 128; s %= NSURF;
    int r = col_fg & 255, gr = col_fg >> 8 & 255, b = col_fg >> 16 & 255;
    #define COV(i, j) (((i) < 0 || (j) < 0 || (i) >= 16 || (j) >= 16) ? 0 : ((g[((j) * 16 + (i)) >> 1] >> (((i) & 1) ? 0 : 4)) & 15) * 17)
    for (int j = 0; j < 16 * BIG; j++) for (int i = 0; i < w * BIG; i++) { int tx = x + i, ty = y + j; if (!INS(s, tx, ty)) continue;
        int fx = (i * 2 + 1) * 128 / BIG - 128, fy = (j * 2 + 1) * 128 / BIG - 128;   /* source position in 1/256 px */
        int ix = fx >> 8, iy = fy >> 8, ax = fx & 255, ay = fy & 255;
        int a = (COV(ix, iy) * (256 - ax) * (256 - ay) + COV(ix + 1, iy) * ax * (256 - ay) + COV(ix, iy + 1) * (256 - ax) * ay + COV(ix + 1, iy + 1) * ax * ay) >> 16;
        if (a < 8) continue; if (a > 255) a = 255;
        uint8_t *d = PIX(s, tx, ty), *m = &MSK(s, tx, ty); DEP(s, tx, ty) = 1;
        d[0] = (d[0] * (255 - a) + r * a) / 255; d[1] = (d[1] * (255 - a) + gr * a) / 255; d[2] = (d[2] * (255 - a) + b * a) / 255; if (a > *m) *m = a; }
    #undef COV
}
static int big_w(const uint8_t *c, int *len) {   /* width of one character at 3x; Korean font word spaces (0x8140) are half width */
    int half; const uint8_t *q = c; uint32_t idx = sys_glyph_index(&q, &half); *len = (int)(q - c);
    if (idx == 95 && (font_flags & 1)) return 8 * BIG; return (half ? 8 : 16) * BIG;
}
static void big_flush(void) {
    if (!big.n) return;
    enum { MARGIN = 40, LH = 16 * BIG + 10 }; int maxw = 640 - 2 * MARGIN;
    static struct { const uint8_t *a, *e; int w, para; } ln[48]; int nl = 0;
    for (int k = 0; k < big.n; k++) {                   /* word-wrap each paragraph */
        const uint8_t *p = (const uint8_t *)big.t[k];
        while (*p && nl < 48) {
            const uint8_t *q = p, *brk = NULL; int w = 0, wbrk = 0;
            while (*q) { int l, cw = big_w(q, &l); if (w + cw > maxw && q != p) break;
                if (q[0] == 0x81 && q[1] == 0x40 && q != p) { brk = q; wbrk = w; } w += cw; q += l; }
            if (*q && brk) { q = brk; w = wbrk; }
            ln[nl].a = p; ln[nl].e = q; ln[nl].w = w; ln[nl].para = k; nl++;
            p = q; if (*q && brk) p += 2;               /* drop the space at the break */
        }
    }
    enum { GAP = 20 };                                  /* extra space between paragraphs */
    int y = (480 - nl * LH - (big.n - 1) * GAP) / 2; if (y < 0) y = 0;
    for (int i = 0; i < nl; i++, y += LH) {
        if (i && ln[i].para != ln[i - 1].para) y += GAP;
        int x = (640 - ln[i].w) / 2;
        for (const uint8_t *q = ln[i].a; q < ln[i].e; ) { int l, cw = big_w(q, &l), half; const uint8_t *qq = q; uint32_t idx = sys_glyph_index(&qq, &half);
            if (!(idx == 95 && (font_flags & 1))) draw_glyph_big(3, x, y, idx, half ? 8 : 16); x += cw; q += l; }
    }
    big.n = 0;
}
void sys_text(vm_t *vm, const char *sj, bool zenkaku) {
    sys_init(vm); (void)zenkaku;
    if (big_mode(vm) && !(SYS16(vm, 2) & 0x800)) {      /* collect; a new paragraph whenever the script moved the cursor */
        int y = SYS16(vm, 10); bl_add(vm, (const uint8_t *)sj, strlen(sj));
        if (!big.n || big.y[big.n - 1] != y) { if (big.n < BIG_PARA) { big.y[big.n] = y; big.t[big.n][0] = 0; big.n++; } }
        char *t = big.t[big.n - 1]; size_t l = strlen(t); snprintf(t + l, sizeof big.t[0] - l, "%s", sj);
        SET_SYS16(vm, 10, y + 1);                       /* mark the cursor as used so a SETA@ 9 to the same row still starts a new paragraph... */
        big.y[big.n - 1] = y + 1;                       /* ...but consecutive text without a cursor move continues this one */
        return;
    }
    if (!(SYS16(vm, 2) & 0x800)) slot_capture(vm, sj);
    const uint8_t *p = (const uint8_t *)sj;
    while (*p) {
        const uint8_t *start = p; int half; uint32_t idx = sys_glyph_index(&p, &half);
        if (SYS16(vm, 2) & 0x800) { SET_V32(vm, 18, V32(vm, 18) + (half ? 1 : 2)); continue; }   /* [EXE] counts bytes into v32[18] (mem+0x1134) */
        bl_add(vm, start, (size_t)(p - start));
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
    if (SYS16(vm, 2) & 0x20) bl_add(vm, (const uint8_t *)"\n", 1);
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
    /* [EXE 0x40f4e7 / 0x40f940] 0 = copy, 1 = green-key copy, 2 = nothing, 3/4 = sound effect (thunder), 5 = copy, 6 = full refresh */
    switch (t) {
    case 0: case 5: gfx_copy(sx, sy, w, h, 1, dx, dy, 0, 2); break;        /* animation frames sit on whatever they cover (3D layer kept) */
    case 1: gfx_copy_key(sx, sy, w, h, 1, dx, dy, 0, 0x00FF00, 1); break;
    case 3: snd_start(vm, SND_SE, SND_SE, "ns_se59.wav"); break;
    case 4: snd_start(vm, SND_SE, SND_SE, "ns_se58.wav"); break;
    default: break;
    }
}
static void anim_tick_one(vm_t *vm, anim_stream *s) {
    const uint8_t *m = vm->mem;
    if (s->state == 4) return;        /* paused (SYS 3,8) */
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
static bool frame(vm_t *vm) { if (snap_pending && !fx_left && (cur_loop < 0 || cur_loop == 100)) snap_on = snap_pending = false;
    anim_tick(vm); return plat_frame(compose(), SW, SH); }
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
/* ---- saves [EXE SYS 4 -> 0x406160 -> 0x41dc30]. A file is VM memory 0x0000..0x1FFF (script name, flags, variables).
   FLAG00 holds global data (seen flags, save-list info); FLAG01.. are slots. ---- */
#define SAVE_SIZE 0x2000
static void save_name(char *b, uint32_t slot) { snprintf(b, 16, "FLAG%02u.dat", slot % 100); }
static char snap_name[OFF_FLAG];      /* filled at MSGID (see sys_msgid) */
static void save_cmd(vm_t *vm, uint32_t sub, uint32_t slot, uint32_t start, uint32_t count) {
    char name[16]; save_name(name, slot); uint8_t *m = vm->mem;
    static uint8_t tmp[SAVE_SIZE];
    switch (sub) {
    case 0:                            /* load everything and restart the saved script (it resumes via its own label variable) */
        if (!plat_save_read(name, tmp, SAVE_SIZE)) { plat_log("load: %s not found", name); return; }
        memcpy(m, tmp, SAVE_SIZE); m[OFF_FLAG - 1] = 0;
        SET_SYS16(vm, 2, SYS16(vm, 2) | 8);                               /* leaves any running menu loop */
        snprintf(vm->goto_name, sizeof vm->goto_name, "%s", (char *)m); vm->want_goto = true;
        return;
    case 1: plat_save_write(name, m, SAVE_SIZE);                         /* save everything */
        /* port change: the original merges flags into FLAG00 only when returning to the title (SYS 4,2). On a handheld the game is
           usually closed with HOME/power, so the merge is done on every slot save; otherwise "Load" (flag 4008) stays disabled */
        if (slot != 0) save_cmd(vm, 2, 0, 0, 0);
        return;
    case 2:                            /* merge current flags into the file (keeps the larger value) */
        if (!plat_save_read(name, tmp, SAVE_SIZE)) memset(tmp, 0, SAVE_SIZE);
        for (int i = 0; i < 0x1000; i++) if (tmp[OFF_FLAG + i] < m[OFF_FLAG + i]) tmp[OFF_FLAG + i] = m[OFF_FLAG + i];
        plat_save_write(name, tmp, SAVE_SIZE); return;
    case 3: if (plat_save_read(name, tmp, SAVE_SIZE)) memcpy(m + OFF_FLAG, tmp + OFF_FLAG, 0x1000); FLAG(vm, 4016) = 1; return;   /* load flags only */
    case 4: memset(m + OFF_FLAG, 0, 0x1000); memset(m + 0x1180, 0, 0x190); memset(m + 0x1310, 0, 0xCF0); FLAG(vm, 4016) = 1; return;  /* clear */
    case 5: case 7: {                  /* load part: 5 = u16 array at 0x1180, 7 = bytes at 0x1310 */
        uint32_t base = sub == 5 ? 0x1180 : 0x1310, unit = sub == 5 ? 2 : 1;
        if (!plat_save_read(name, tmp, SAVE_SIZE)) return;
        for (uint32_t i = start; i < start + count; i++) { uint32_t o = base + i * unit; if (o + unit <= SAVE_SIZE) memcpy(m + o, tmp + o, unit); }
        return; }
    case 6: case 8: {                  /* store part into the file */
        uint32_t base = sub == 6 ? 0x1180 : 0x1310, unit = sub == 6 ? 2 : 1;
        if (!plat_save_read(name, tmp, SAVE_SIZE)) memset(tmp, 0, SAVE_SIZE);
        for (uint32_t i = start; i < start + count; i++) { uint32_t o = base + i * unit; if (o + unit <= SAVE_SIZE) memcpy(tmp + o, m + o, unit); }
        plat_save_write(name, tmp, SAVE_SIZE); return; }
    case 9:                            /* write the current script name into the file header */
        if (!plat_save_read(name, tmp, SAVE_SIZE)) memset(tmp, 0, SAVE_SIZE);
        memset(tmp, 0, OFF_FLAG); strncpy((char *)tmp, snap_name[0] ? snap_name : vm->mes_name, OFF_FLAG - 1);   /* [EXE] name from the MSGID snapshot */
        plat_save_write(name, tmp, SAVE_SIZE); return;
    }
}
/* ---- scene log [EXE SYS 24 -> 0x4215f0 -> 0x4211a0, object +0x240]: what is on screen/playing, so a loaded game can rebuild it.
   34-byte records {u16 type, char name[20], u16 v[6]}, kept sorted by type, max 30. Saved per slot as SaveDataNN (u32 count + records). */
typedef struct { uint16_t type; char name[20]; uint16_t v[6]; } scene_rec;
static scene_rec scene_log[30]; static int scene_n;
/* [EXE 0x411eb0] every MSGID takes the save snapshot: script name, message number (sys32[8], used by JMPLBL on load) and a copy of the scene log */
static scene_rec snap_log[30]; static int snap_n;
void sys_msgid(vm_t *vm, uint32_t id) {
    strncpy(snap_name, (const char *)vm->mem, OFF_FLAG - 1); SET_SYS32(vm, 8, id);
    memcpy(snap_log, scene_log, sizeof scene_log); snap_n = scene_n;
}
static void scene_cmd(vm_t *vm, vm_param *p) {
    uint32_t sub = p[0].val;
    switch (sub) {
    case 0: scene_n = 0; return;
    case 1: { int k = 0; for (int i = 0; i < scene_n; i++) if (scene_log[i].type != (uint16_t)p[1].val) scene_log[k++] = scene_log[i]; scene_n = k; return; }
    case 2: case 3: {
        char name[24]; snprintf(name, sizeof name, "SaveData%02u.dat", p[1].val % 100); uint8_t b[0x400];
        if (sub == 3) { memset(b, 0, sizeof b); wr32(b, snap_n);          /* [EXE 0x42129f] writes the MSGID snapshot */
            for (int i = 0; i < snap_n; i++) { uint8_t *r = b + 4 + i * 34; wr16(r, snap_log[i].type); memcpy(r + 2, snap_log[i].name, 20);
                for (int k = 0; k < 6; k++) wr16(r + 22 + 2 * k, snap_log[i].v[k]); }
            plat_save_write(name, b, sizeof b); return; }
        if (!plat_save_read(name, b, sizeof b)) { scene_n = 0; return; }
        scene_n = (int)rd32(b); if (scene_n < 0 || scene_n > 30) scene_n = 0;
        for (int i = 0; i < scene_n; i++) { const uint8_t *r = b + 4 + i * 34; scene_log[i].type = rd16(r); memcpy(scene_log[i].name, r + 2, 20); scene_log[i].name[19] = 0;
            for (int k = 0; k < 6; k++) scene_log[i].v[k] = rd16(r + 22 + 2 * k); }
        return; }
    case 4: {                          /* add (type, name, v0..v5), then sort by type (insertion keeps equal types in order) */
        if (scene_n >= 30) return; scene_rec r; memset(&r, 0, sizeof r); r.type = p[1].val; strncpy(r.name, p[2].str, 19);
        for (int k = 0; k < 6; k++) r.v[k] = p[3 + k].val;
        int i = scene_n++; while (i > 0 && scene_log[i - 1].type > r.type) { scene_log[i] = scene_log[i - 1]; i--; } scene_log[i] = r; return; }
    case 5: SET_V16(vm, 18, p[1].val < (uint32_t)scene_n ? scene_log[p[1].val].type : 0xFFFF); return;
    case 6: if (p[1].val < (uint32_t)scene_n && p[3].val + 20 < MEM_SIZE) strcpy((char *)vm->mem + p[3].val, scene_log[p[1].val].name); return;
    case 7: if (p[1].val < (uint32_t)scene_n && p[2].val < 6) SET_V16(vm, 18, scene_log[p[1].val].v[p[2].val]); return;   /* (7, index, k) */
    }
}
extern void vm_run_block(vm_t *);
static void backlog_view(vm_t *vm) {   /* SYS 19,6: modal; B or L closes, up/down select, left/right page, A replays the voice */
    int n = bl_count(); if (!n) return;
    blv.on = true; blv.sel = n - 1; blv.top = 0; bl_fix_top();   /* newest entry at the bottom of a full page */
    int keys[6] = {0, 1, 2, 3, 4, 5}, held[6] = {0}; bool armed = false;
    while (!vm->quit) {
        if (!frame(vm)) { vm->quit = true; break; }
        bool any = plat_key(9); for (int i = 0; i < 6; i++) any |= plat_key(keys[i]);
        if (!armed) { armed = !any; continue; }                 /* wait until the key that opened the viewer is released */
        if (plat_key(9)) break;
        bool fire[6];
        for (int i = 0; i < 6; i++) { if (plat_key(keys[i])) held[i]++; else held[i] = 0; fire[i] = held[i] == 1 || (held[i] > 20 && held[i] % 4 == 0); }
        if (fire[1]) break;
        int old = blv.sel;
        if (fire[2] && blv.sel > 0) blv.sel--;
        if (fire[3] && blv.sel < n - 1) blv.sel++;
        if (fire[4]) blv.sel = blv.sel > 5 ? blv.sel - 5 : 0;
        if (fire[5]) blv.sel = blv.sel + 5 < n ? blv.sel + 5 : n - 1;
        if (blv.sel != old) bl_fix_top();
        if (fire[0] && bl[blv.sel].voice[0] && (SYS16(vm, 2) & 0x100)) snd_start(vm, SND_VOICE, SND_VOICE, bl[blv.sel].voice);
    }
    blv.on = false; plat_snd_stop(SND_VOICE);
    for (int k = 0; k < 120 && !vm->quit; k++) {             /* swallow the closing key so the game does not see it too */
        bool any = plat_key(9); for (int i = 0; i < 6; i++) any |= plat_key(keys[i]); if (!any) break;
        if (!frame(vm)) vm->quit = true; }
}
static void sys28(vm_t *vm, uint32_t sub);
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
        if (sub == 8) {                /* [EXE 0x40f6a0] pause all at their next wait: running -> 3, wait until none is left in 3 */
            for (int a = 0; a < 10; a++) for (int b = 0; b < 32; b++) if (anim[a][b].state == AN_RUN) anim[a][b].state = 3;
            for (int k = 0; k < 600 && !vm->quit; k++) { int busy = 0;
                for (int a = 0; a < 10; a++) for (int b = 0; b < 32; b++) busy |= anim[a][b].state == 3;
                if (!busy) break; if (!frame(vm)) vm->quit = true; vm->steps = 0; }
            return; }
        if (sub == 9) { for (int a = 0; a < 10; a++) for (int b = 0; b < 32; b++) if (anim[a][b].state == 4) anim[a][b].state = AN_RUN; return; }   /* [EXE 0x40f590] */
        break; }
    case 8:
        if (sub == 0) {                /* [EXE 0x41e2b0 partly read] load a DATA.ARC file (LZSS) to the address in p2 */
            size_t n; uint8_t *buf = arc_load_lzss(&vm->arc_data, p[1].str, &n); if (!buf) { plat_log("missing data %s", p[1].str); return; }
            { size_t l = strlen(p[1].str); if (l > 3 && !strcasecmp(p[1].str + l - 3, ".a6")) snprintf(cur_a6, sizeof cur_a6, "%s", p[1].str); }
            uint32_t addr = p[2].val & (MEM_SIZE - 1); if (addr + n <= MEM_SIZE) memcpy(vm->mem + addr, buf, n); free(buf); return; }
        if (sub == 2) {                /* [guess] load library script and run its top level (defines lib procs) */
            size_t n; uint8_t *buf = arc_load_lzss(&vm->arc_mes, p[1].str, &n); if (!buf) { plat_log("missing %s", p[1].str); return; }
            sys_fix_script(p[1].str, buf, n);
            const uint8_t *smes = vm->mes; uint32_t sip = vm->ip, sbase = vm->base, snl = vm->nlabels; const uint32_t *sl = vm->labels;
            vm->mes = buf; vm->nlabels = rd32(buf); vm->labels = (const uint32_t *)(buf + 4); vm->base = 4 + 4 * vm->nlabels; vm->ip = 0;
            vm_run_block(vm);
            vm->mes = smes; vm->ip = sip; vm->base = sbase; vm->nlabels = snl; vm->labels = sl; return;
        }
        break;
    case 11: {                         /* [EXE: args = sub, x, y, w=x1-x+1, h=y1-y+1, p5.. ] */
        if (big.n && ((sub == 0 || sub == 1 || sub == 7 || sub == 8) && p[5].val == 3)) big_flush();
        if (big.n && (sub == 2 || sub == 10) && SYS16(vm, 1) == 3 && p[1].val == 0 && p[2].val == 0) big.n = 0;   /* surface 3 cleared */
        int x = p[1].val, y = p[2].val, w = (int)p[3].val - x + 1, h = (int)p[4].val - y + 1;
        switch (sub) {     /* [EXE 0x420a70 base table + 0x42b920 game table] */
        case 0: gfx_copy(x, y, w, h, p[5].val, p[6].val, p[7].val, p[8].val, 0); return;
        case 1: gfx_copy_key(x, y, w, h, p[5].val, p[6].val, p[7].val, p[8].val, SYS32(vm, 9), 0); return;
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
    case 14: {                         /* [EXE 0x41e7e0 -> 0x41e800] run bytecode at a pointer until END: a menu item (tagged value
                                          from the MENUS table) or code stored in VM memory (e.g. save memos) */
        const uint8_t *smes = vm->mes; uint32_t sip = vm->ip, sbase = vm->base, snl = vm->nlabels; const uint32_t *sl = vm->labels;
        if (sub & 0x80000000u) { uint32_t m = (sub >> 8) & 255, i = sub & 255; if (!vm->menu_mes[m] || i >= vm->menu_n[m]) return;
            vm->mes = vm->menu_mes[m]; vm->nlabels = rd32(vm->mes); vm->labels = (const uint32_t *)(vm->mes + 4); vm->base = 4 + 4 * vm->nlabels; vm->ip = vm->menu_item[m][i]; }
        else { if (sub >= MEM_SIZE - 1) return; vm->mes = vm->mem; vm->nlabels = 0; vm->labels = NULL; vm->base = 0; vm->ip = sub; }
        vm_run_block(vm);
        vm->mes = smes; vm->ip = sip; vm->base = sbase; vm->nlabels = snl; vm->labels = sl; return; }
    case 15: {                         /* [EXE 0x41e870 -> 0x40f9d0] hit test: records {u16 id, x0, y0, x1, y1} at p2, 0xFFFF ends -> v16[18] */
        uint32_t x = p[0].val & 0xFFFF, y = p[1].val & 0xFFFF, a = p[2].val; uint16_t r = 0xFFFF;
        for (int k = 0; k < 4096 && a + 10 <= MEM_SIZE; k++, a += 10) { const uint8_t *e = vm->mem + a; if (rd16(e) == 0xFFFF) break;
            if (rd16(e + 2) <= x && x <= rd16(e + 6) && rd16(e + 4) <= y && y <= rd16(e + 8)) { r = rd16(e); break; } }
        SET_V16(vm, 18, r); return; }
    case 17: {                         /* [EXE 0x406380] local date/time as 6 x u16: year, month, day, hour, minute, second */
        int dt[6]; plat_datetime(dt); uint32_t a = sub & (MEM_SIZE - 1);
        for (int i = 0; i < 6 && a + 2 * i + 2 <= MEM_SIZE; i++) wr16(vm->mem + a + 2 * i, dt[i]);
        return; }
    case 26: {                         /* [EXE 0x41f000] read-text bitmap (MesSkip.dat, 0x85E bytes): 0 load 1 save 2 clear 3 set bit 4 test bit -> v16[18] */
        static uint8_t seen_bits[0x85E]; uint32_t n = p[1].val; uint16_t r = 0;
        switch (sub) {
        case 0: if (!plat_save_read("MesSkip.dat", seen_bits, sizeof seen_bits)) memset(seen_bits, 0, sizeof seen_bits); break;
        case 1: plat_save_write("MesSkip.dat", seen_bits, sizeof seen_bits); break;
        case 2: memset(seen_bits, 0, sizeof seen_bits); break;
        case 3: if (n / 8 < sizeof seen_bits) seen_bits[n / 8] |= 0x80 >> (n & 7); break;
        case 4: r = n / 8 < sizeof seen_bits && (seen_bits[n / 8] & (0x80 >> (n & 7))); break;
        }
        SET_V16(vm, 18, r); return; }
    case 16: { uint32_t m = vm->menu_run; if (sub < vm->menu_n[m]) SET_SYS16(vm, 22, vm->menu_id[m][sub]); return; }   /* [script-derived] chosen item -> sys16[22] */
    case 12: { if (getenv("AI5_TRACE")) plat_log("  WAIT %u lit6=%ld", sub, lit(6));                         /* [EXE 0x41e620/0x429260] wait p0 ticks, key 6 skips; tick length is a guess (1/60 s) */
        for (uint32_t t = 0; t < sub && !vm->quit; t++) { if (plat_key(6) || (SYS16(vm, 2) & 8)) break; if (!frame(vm)) vm->quit = true; }
        return; }
    case 18: plat_poll(); SET_V32(vm, 18, plat_key(sub) == (p[1].val != 0)); return;   /* [script-derived] 1 when key state matches p1 (1=pressed, 0=released); result -> v32[18] */
    case 4: save_cmd(vm, sub, p[1].val, p[2].val, p[3].val); return;
    case 24: scene_cmd(vm, p); return;
    case 5:                            /* music: (0,name,ch) or (sub,ch) */
        if (sub == 0) snd_cmd(vm, SND_MUSIC, 2, 0, p[1].str, p[2].val); else snd_cmd(vm, SND_MUSIC, 2, sub, "", p[1].val);
        return;
    case 6:                            /* effects: same argument shape as music */
        if (sub == 0) snd_cmd(vm, SND_SE, 4, 0, p[1].str, p[2].val); else snd_cmd(vm, SND_SE, 4, sub, "", p[1].val);
        return;
    case 7:                            /* voice: (sub,name,ch). Disabled unless sys16[2] bit 0x100; key 7 (voice skip) cancels 0/3 */
        if (!(SYS16(vm, 2) & 0x100)) { SET_V32(vm, 18, 0); return; }
        if ((sub == 0 || sub == 3) && plat_key(7)) { plat_snd_stop(SND_VOICE + (p[2].val & 1)); snd_has_pend[SND_VOICE + (p[2].val & 1)] = 0; SET_V32(vm, 18, 0); return; }
        if ((sub == 0 || sub == 2) && p[1].str[0]) snprintf(bl_voice, sizeof bl_voice, "%s", p[1].str);   /* for the log viewer */
        snd_cmd(vm, SND_VOICE, 2, sub, p[1].str, p[2].val);
        return;
    case 1: {                          /* [EXE 0x41f650] print a number as full-width digits */
        char t[24]; int n = 0; uint32_t v = sub; char dg[12]; int k = 0; do { dg[k++] = v % 10; v /= 10; } while (v && k < 10);
        while (k--) { t[n++] = (char)0x82; t[n++] = (char)(0x4F + dg[k]); } t[n] = 0; sys_text(vm, t, true); return; }
    case 10:                           /* [EXE 0x4206d0] 1 = snapshot the screen to surface 11 and fade from it over 16 frames (p1: fill surface 10 first) */
        if (sub == 1) {   /* crossfade the screen to the working surface 10: snapshot 0 -> 11, (p1: fill 10), 16 frames, waits like 0x41db50.
                             Library use: compose on sys16[23], then SYS 10,1 shows it (LIB 0x2c22 copies directly instead on later passes). */
            SET_SYS16(vm, 2, SYS16(vm, 2) & ~2);
            gfx_copy(0, 0, SW, SH, 0, 0, 0, 11, 0); if (p[1].val) gfx_fill(0, 0, SW, SH, 10, p[1].val, 0);
            gfx_copy(0, 0, SW, SH, 10, 0, 0, 0, 0); fx_left = 16;
            while (fx_left && !vm->quit) if (!frame(vm)) vm->quit = true;
            return; }
        break;
    case 29: SET_V32(vm, 18, plat_ms()); return;   /* [EXE 0x41f250] timeGetTime -> v32[18] */
    case 0: return;                    /* [EXE 0x421a50] Windows cursor/window setup: nothing to do here */
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
    case 28: sys28(vm, sub); return;
    case 19: switch (sub) {
        case 0: if (SYS16(vm, 2) & 0x10) { memset(bl, 0, sizeof bl); bl_cur = 0; } return;
        case 1: if ((SYS16(vm, 2) & 0x30) == 0x10) bl_begin(vm); return;
        case 2: if ((SYS16(vm, 2) & 0x30) == 0x30) { SET_SYS16(vm, 2, SYS16(vm, 2) & ~0x20); if (bl_cur < BL_N) bl[bl_cur++].done = 1; } return;
        case 3: SET_V32(vm, 18, (uint32_t)bl_count()); return;
        case 4: SET_V32(vm, 18, p[1].val < BL_N && bl[p[1].val].done ? BL_TAG | p[1].val : 0); return;
        case 5: SET_V32(vm, 18, 0); return;
        case 6: backlog_view(vm); return;
        } return;
    case 20: {                         /* [script-derived] length of a text in half-width units -> v32[18] (memo centring, auto-mode wait) */
        uint32_t a = sub; if ((a & 0xFF000000u) == BL_TAG) { SET_V32(vm, 18, (uint32_t)bl[a & 0xFF].len); return; }
        if (!a || a >= MEM_SIZE - 1) { SET_V32(vm, 18, 0); return; }
        uint16_t f = SYS16(vm, 2); SET_SYS16(vm, 2, f | 0x800); SET_V32(vm, 18, 0);
        const uint8_t *smes = vm->mes; uint32_t sip = vm->ip, sbase = vm->base, snl = vm->nlabels; const uint32_t *sl = vm->labels;
        vm->mes = vm->mem; vm->nlabels = 0; vm->labels = NULL; vm->base = 0; vm->ip = a; vm_run_block(vm);
        vm->mes = smes; vm->ip = sip; vm->base = sbase; vm->nlabels = snl; vm->labels = sl;
        SET_SYS16(vm, 2, (SYS16(vm, 2) & ~0x800) | (f & 0x800)); return; }
    case 13: col_bg = p[0].val; col_fg = p[1].val; return;                                 /* [EXE] */
    case 25: if (sub == 0) { load_image(vm, p[1].str, SYS16(vm, 1), -1, -1); return; } break;   /* [EXE 0x421b30 -> 0x41ab80; its image cache (0x41f430) is not reproduced] */
    case 9:                            /* [EXE 0x41e4b0 -> 0x420540] load image into surface sys16[1], optionally at (p1,p2) */
        if (vm->nparams >= 4) { char n[64]; uint32_t a = p[3].val; if (a >= MEM_SIZE - 1) return;   /* 4 args: name read from VM memory at p3 */
            strncpy(n, (const char *)vm->mem + a, sizeof n - 1); n[sizeof n - 1] = 0; load_image(vm, n, SYS16(vm, 1), -1, -1); }
        else if (vm->nparams >= 3) load_image(vm, p[0].str, SYS16(vm, 1), (int)p[1].val, (int)p[2].val);
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
/* rule-image transitions from surface 10 (the scene being built) to the screen. Rule = DATA.ARC "%02u.bin", 640x480 bytes.
   [EXE 0x410e00] UTIL 3: hard wipe, pixels revealed in rule-value order (dir 0: 0->255, else 255->1); rule 3 = column wipe (0x410750).
   [EXE 0x410460] UTIL 4: soft wipe, alpha = t - rule for t = 32..352 step 32 (15 ms steps); dir != 0 = plain copy.
   Both end with a full copy. They only animate when sys16[2] bit 0x2000 (display effects) is set and skip (key 6) is not held.
   Steps per frame for UTIL 3 are a choice (the original redraws as fast as it can). */
static bool effects_on(vm_t *vm) { return (SYS16(vm, 2) & 0x2000) && !plat_key(6); }
static void rule_wipe(vm_t *vm, int kind, int x0, int y0, int w, int h, uint32_t rule, uint32_t dir) {
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x0 + w > SW) w = SW - x0; if (y0 + h > SH) h = SH - y0;
    if (w > 0 && h > 0 && effects_on(vm) && !(kind == 4 && dir)) {
        if (kind == 3 && rule == 3) {                       /* column wipe, left to right */
            for (int c = 0; c < w && !vm->quit; c += 20) { gfx_copy(x0 + c, y0, 20, h, 10, x0 + c, y0, 0, 0);
                if (!effects_on(vm)) break; if (!frame(vm)) vm->quit = true; vm->steps = 0; }
        } else {
            char nm[16]; snprintf(nm, sizeof nm, "%02u.bin", rule % 100); size_t n; uint8_t *r = arc_load_lzss(&vm->arc_data, nm, &n);
            if (r && n >= (size_t)SW * SH) {
                if (kind == 3) {
                    for (int step = 0; step < 256 && !vm->quit; ) {
                        for (int k = 0; k < 8 && step < 256; k++, step++) { int v = dir ? 255 - step : step;
                            for (int y = y0; y < y0 + h; y++) for (int x = x0; x < x0 + w; x++) if (r[y * SW + x] == v && INS(10, x, y)) { memcpy(PIX(0, x, y), PIX(10, x, y), 3); DEP(0, x, y) = DEP(10, x, y); } }
                        if (!effects_on(vm)) break; if (!frame(vm)) vm->quit = true; vm->steps = 0;
                    }
                } else {
                    uint8_t *old = malloc((size_t)SW * SH * 3); memcpy(old, surf[0], (size_t)SW * SH * 3);
                    for (int t = 32; t < 384 && !vm->quit; t += 32) {
                        for (int y = y0; y < y0 + h; y++) for (int x = x0; x < x0 + w; x++) { if (!INS(10, x, y)) continue;
                            int a = t - r[y * SW + x]; if (a <= 0) continue; if (a > 255) a = 255;
                            const uint8_t *o = old + (y * SW + x) * 3, *nw = PIX(10, x, y); uint8_t *d = PIX(0, x, y);
                            for (int c = 0; c < 3; c++) d[c] = (o[c] * (255 - a) + nw[c] * a) / 255; }
                        if (!effects_on(vm)) break; if (!frame(vm)) vm->quit = true; vm->steps = 0;
                    }
                    free(old);
                }
            } else plat_log("rule %s missing", nm);
            free(r);
        }
    }
    if (w > 0 && h > 0) gfx_copy(x0, y0, w, h, 10, x0, y0, 0, 0);
}
/* UTIL 10,1 [EXE 0x411567 -> 0x42bf40, result -> v16[18]]: the title menu, a popup window drawn from the sheet the script
   has just loaded into surface 4 (titile_pt.gcc: 6 items x 3 states of 408x36). The original's popup class is not
   reproduced; this is an equivalent: items stacked under the logo, D-pad/A or pointer/touch, returns 1..6 (0 = cancel). */
static int title_menu(vm_t *vm) {
    int src = 4, w = sw[src] < 408 ? sw[src] : 408, n = sh[src] / 108; if (n > 6) n = 6; if (n <= 0) return 1;
    pop.w = w; pop.n = n; pop.rgb = malloc((size_t)w * n * 108 * 3); pop.a = malloc((size_t)w * n * 108);
    for (int y = 0; y < n * 108; y++) for (int x = 0; x < w; x++) { memcpy(pop.rgb + (y * w + x) * 3, PIX(src, x, y), 3); pop.a[y * w + x] = MSK(src, x, y); }
    pop.x = (320 - w) / 2; pop.y = (240 - n * 36) / 2; pop.hl = 0;   /* bottom-screen coordinates */ pop.pressed = -1; pop.on = true;
    int held[6] = {0}, result = -1, cx0 = -1, cy0 = -1; bool armed = false;
    while (!vm->quit && result < 0) {
        if (!frame(vm)) { vm->quit = true; break; }
        bool k[6]; bool any = false; for (int i = 0; i < 6; i++) { k[i] = plat_key(i); any |= k[i]; }
        if (!armed) { armed = !any; continue; }
        bool fire[6]; for (int i = 0; i < 6; i++) { held[i] = k[i] ? held[i] + 1 : 0; fire[i] = held[i] == 1 || (i >= 2 && held[i] > 24 && (held[i] - 24) % 6 == 0); }
        int cx, cy; plat_cursor(&cx, &cy, false); int bx = cx / 2, by = cy / 2;   /* pointer in 640x480 space = bottom screen x2 */
        if ((cx != cx0 || cy != cy0) && cx0 >= 0 && bx >= 0 && bx < 320 && by >= pop.y && by < pop.y + n * 36) pop.hl = (by - pop.y) / 36;   /* touch */
        cx0 = cx; cy0 = cy;
        if (fire[2]) pop.hl = (pop.hl + n - 1) % n;
        if (fire[3]) pop.hl = (pop.hl + 1) % n;
        if (fire[0]) result = pop.hl + 1;
    }
    if (result > 0) { pop.pressed = pop.hl; for (int f = 0; f < 8 && !vm->quit; f++) if (!frame(vm)) vm->quit = true; }   /* show the pressed state briefly */
    for (int f = 0; f < 120 && !vm->quit; f++) { bool any = false; for (int i = 0; i < 6; i++) any |= plat_key(i); if (!any) break; if (!frame(vm)) vm->quit = true; }
    pop.on = false; free(pop.rgb); free(pop.a); pop.rgb = pop.a = NULL;
    return result < 0 ? 0 : result;
}
/* SYS 28 confirmation: bktitlen.gcc / endn.gcc ("처음 화면으로 돌아갈까요?" / "게임을 종료할까요?"). Left/right (or up/down) move,
   A confirms, B cancels, touch on a button chooses it. Returns true for 확인. */
static bool confirm_dialog(vm_t *vm, const char *sheet) {
    size_t n; uint8_t *raw = arc_load(&vm->arc_bg, sheet, &n); if (!raw) return true;
    int rc = gcc_decode(raw, n, &dlg.im); free(raw); if (rc || dlg.im.w < 272 || dlg.im.h < 184) { if (!rc) { free(dlg.im.bgr); free(dlg.im.alpha); } return true; }
    dlg.x = (320 - 272) / 2; dlg.y = (240 - 112) / 2;   /* bottom-screen coordinates */ dlg.sel = 0; dlg.on = true;
    int held[6] = {0}, result = -1, cx0 = -1, cy0 = -1; bool armed = false;
    while (!vm->quit && result < 0) {
        if (!frame(vm)) { vm->quit = true; break; }
        bool k[6], any = false; for (int i = 0; i < 6; i++) { k[i] = plat_key(i); any |= k[i]; }
        if (!armed) { armed = !any; continue; }
        bool fire[6]; for (int i = 0; i < 6; i++) { held[i] = k[i] ? held[i] + 1 : 0; fire[i] = held[i] == 1; }
        int cx, cy; plat_cursor(&cx, &cy, false);
        int bx = cx / 2, by = cy / 2;
        if ((cx != cx0 || cy != cy0) && cx0 >= 0 && by >= dlg.y + 58 && by < dlg.y + 80) { if (bx >= dlg.x + 29 && bx < dlg.x + 125) dlg.sel = 0; if (bx >= dlg.x + 149 && bx < dlg.x + 245) dlg.sel = 1; }
        cx0 = cx; cy0 = cy;
        if (fire[2] || fire[3] || fire[4] || fire[5]) dlg.sel ^= 1;
        if (fire[0]) result = dlg.sel == 0; else if (fire[1]) result = 0;
    }
    for (int f = 0; f < 120 && !vm->quit; f++) { bool any = false; for (int i = 0; i < 6; i++) any |= plat_key(i); if (!any) break; if (!frame(vm)) vm->quit = true; }
    dlg.on = false; free(dlg.im.bgr); free(dlg.im.alpha); dlg.im.bgr = dlg.im.alpha = NULL;
    return result == 1;
}
static void sys28(vm_t *vm, uint32_t sub) {   /* [script-derived] system menu: 0 = back to the title, 1 = quit, 2 = settings (not offered) */
    if (sub == 0 && confirm_dialog(vm, "bktitlen.gcc")) {
        for (int i = 0; i < SND_SLOTS; i++) plat_snd_stop(i); anim_reset();
        SET_SYS16(vm, 2, SYS16(vm, 2) | 8);                   /* leave every running menu loop, like a load */
        snprintf(vm->goto_name, sizeof vm->goto_name, "MAIN.MES"); vm->want_goto = true;
    } else if (sub == 1 && confirm_dialog(vm, "endn.gcc")) vm->quit = true;
}
/* UTIL [EXE dispatcher 0x411330, 11 functions] */
void sys_util(vm_t *vm) {
    sys_init(vm);
    vm_param *p = vm->params; uint32_t n = p[0].val;
    if (getenv("AI5_TRACE")) plat_log("  util %u: %u %u %u %u %u %u", n, p[1].val, p[2].val, p[3].val, p[4].val, p[5].val, p[6].val);
    switch (n) {
    case 0: shake_x = (int32_t)p[1].val; shake_y = (int32_t)p[2].val; if (!frame(vm)) vm->quit = true; return;      /* [EXE] offset + full repaint; the frame pump here is a guess */
    case 1: crossfade(vm, p[1].val, p[2].val, p[3].val, p[4].val, p[5].val); return;                                  /* [EXE 0x4102d0; src = p1 from script use] */
    case 3: case 4: rule_wipe(vm, n, p[1].val, p[2].val, p[3].val, p[4].val, p[5].val, p[6].val); return;
    case 5:                            /* [EXE 0x410010] 0: close the dialogue window if open, open the choice window; else close the choice window */
        if (p[1].val == 0) { win[0].visible = 0; win[1].visible = 1; } else win[1].visible = 0;
        return;
    case 10: if (p[1].val == 1) SET_V16(vm, 18, title_menu(vm)); return;   /* 0 = close the popup [0x42cf20]: nothing to do here */
    case 9: return;                   /* [EXE 0x411555 -> 0x410150] PC window/menu-bar state around the system menu: not needed */
    case 6: {                          /* [EXE 0x4109a0] (x, y, w, h, surface): mask plane = 0x80 (half transparent) */
        int x0 = p[1].val, y0 = p[2].val, w = p[3].val, h = p[4].val, sn = p[5].val % NSURF;
        for (int y = y0; y < y0 + h; y++) for (int x = x0; x < x0 + w; x++) if (INS(sn, x, y)) MSK(sn, x, y) = 0x80;
        return; }
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
    uint32_t prev_run = vm->menu_run; vm->menu_run = m; int prev_loop = cur_loop; cur_loop = (int)m;
    if (m != 100) memset(slot_text, 0, sizeof slot_text);
    if (m != 100 && !snap_on && surf[0]) {          /* keep the story picture on the top screen while this menu runs (bottom screen) */
        if (!snap_rgb) { snap_rgb = malloc(SW * SH * 3); snap_z = malloc(SW * SH); }
        memcpy(snap_rgb, surf[0], SW * SH * 3); memcpy(snap_z, dpth[0], SW * SH); snap_on = true; }
    else if (m != 100 && snap_on) snap_pending = false;   /* another menu right after: keep the same picture */
    for (uint32_t i = 0; i < vm->menu_n[m]; i++) wr32(vm->mem + ((SYS32(vm, 7) + 4 * i) & (MEM_SIZE - 1)), 0x80000000u | m << 8 | i);   /* stands in for the item-text pointer */
    vm_call_proc(vm, 8);
    if (getenv("AI5_TRACE")) plat_log("  MENUS %u enter: s106=%u s107=%u f4004=%u f4005=%u f4009=%u v16_18=%u v32_21=%u", m, SYS16(vm,106), SYS16(vm,107), FLAG(vm,4004), FLAG(vm,4005), FLAG(vm,4009), V16(vm,18), V32(vm,21));
    while (!(SYS16(vm, 2) & 0xC) && !vm->quit && !vm->want_goto) {
        vm_call_proc(vm, 9);
        if (SYS16(vm, 2) & 0xC) break;
        /* [EXE 0x414bb4..0x414c83] after a direction key the original calls a short wait (0x14) with that key; at 60 fps a held
           D-pad would otherwise step every frame, so directions fire once, then repeat after ~0.4 s every ~0.1 s */
        static int dir_held[6];
        for (int k = 0; k < 6; k++) {
            bool down = plat_key(k);
            if (k >= 2) { if (!down) { dir_held[k] = 0; continue; } int h = ++dir_held[k]; if (!(h == 1 || (h > 24 && (h - 24) % 6 == 0))) continue; }
            else if (!down) continue;
            if (getenv("AI5_TRACE")) plat_log("  key %d -> PROC %d", k, 2 + k); vm_call_proc(vm, 2 + k); break; }
        if (getenv("AI5_TRACE")) plat_log("  tick flags=%04x", SYS16(vm, 2));
        if (!frame(vm)) vm->quit = true;
        vm->steps = 0;
    }
    vm->menu_run = prev_run; cur_loop = prev_loop;
    if (m != 100 && snap_on) snap_pending = true;   /* released at the next frame without a running crossfade (the scene is back by then) */
}
