/* Nintendo 3DS backend (libctru, software framebuffers).
 * Top screen   : the scene, 640x480 averaged down to 320x240, centred in 400x240.
 * Bottom screen: dialogue / choices re-rendered from strings with the 16px font (20 full-width columns).
 * Data folder  : sdmc:/3ds/nonoxp/  (MES.ARC DATA.ARC BG.ARC font16.bin; optional SOUND.ARC VOICE.ARC)
 * NOTE: written without access to hardware or the toolchain; first build may need fixes. */
#include <3ds.h>
#include <stdio.h>
#include <dirent.h>
#include <time.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "ai5.h"

u32 __stacksize__ = 0x40000;   /* the script interpreter recurses; the default 32 KB is too small */

#define DATA_DIR "sdmc:/3ds/nonoxp/"
static vm_t *g_vm;
static u32 g_held, g_down; static touchPosition g_touch; static bool g_touching, g_touch_armed;
static int g_cx = 320, g_cy = 100;
static int g_row_item[14];              /* bottom-screen text row -> choice index (-1 none) */
static bool g_menu_mode;                /* no text window open (save/load/settings screens): bottom screen mirrors the scene */

/* ---- files: keep a few archives open ---- */
static struct { char name[24]; FILE *f; } g_open[6];
static FILE *get_file(const char *name) {
    for (int i = 0; i < 6; i++) if (g_open[i].f && !strcmp(g_open[i].name, name)) return g_open[i].f;
    char p[96]; snprintf(p, sizeof p, DATA_DIR "%s", name); FILE *f = fopen(p, "rb"); if (!f) return NULL;
    for (int i = 0; i < 6; i++) if (!g_open[i].f) { strncpy(g_open[i].name, name, 23); g_open[i].f = f; return f; }
    fclose(g_open[0].f); strncpy(g_open[0].name, name, 23); g_open[0].f = f; return f;
}
bool plat_read_at(const char *name, uint32_t off, void *dst, uint32_t len) {
    FILE *f = get_file(name); if (!f) return false; if (fseek(f, off, SEEK_SET)) return false; return fread(dst, 1, len, f) == len; }
int plat_list(const char *dir, char names[][24], int max) {   /* file names in a game-folder subdirectory */
    char p[96]; snprintf(p, sizeof p, DATA_DIR "%s", dir); DIR *d = opendir(p); if (!d) return 0; int n = 0; struct dirent *e;
    while ((e = readdir(d)) && n < max) { if (e->d_name[0] == '.' || strlen(e->d_name) > 23) continue; strcpy(names[n++], e->d_name); }
    closedir(d); return n; }
uint32_t plat_file_size(const char *name) { FILE *f = get_file(name); if (!f) return 0; fseek(f, 0, SEEK_END); return (uint32_t)ftell(f); }
void *plat_read_file(const char *name, size_t *len) { uint32_t n = plat_file_size(name); if (!n) return NULL; void *b = malloc(n + 1);
    if (!b || !plat_read_at(name, 0, b, n)) { free(b); return NULL; } if (len) *len = n; return b; }
void plat_log(const char *fmt, ...) {
    static int n; if (++n > 300) return; FILE *f = fopen(DATA_DIR "log.txt", n == 1 ? "w" : "a"); if (!f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap); fputc('\n', f); fclose(f);
}

/* ---- input ---- */
static void pump(void) {
    hidScanInput(); g_held = hidKeysHeld(); g_down = hidKeysDown();
    bool t = (g_held & KEY_TOUCH) != 0;
    if (t) { hidTouchRead(&g_touch);
        if (!g_touching) {               /* touch began: point at the choice under the finger, confirm from the next frame on */
            g_touch_armed = false;
            if (g_menu_mode) { g_cx = g_touch.px * 2; g_cy = g_touch.py * 2; }          /* touch = click at that spot of the 640x480 screen */
            else { int row = (g_touch.py - 8) / 18;
                if (row >= 0 && row < 14 && g_row_item[row] >= 0) { g_cx = 100; g_cy = 384 + g_row_item[row] * 18 + 9; } }
        } else g_touch_armed = true; }
    else g_touch_armed = false;
    g_touching = t;
}
uint32_t plat_ms(void) { return (uint32_t)osGetTime(); }
void plat_datetime(int o[6]) { time_t t = time(NULL); struct tm *m = localtime(&t); o[0] = m->tm_year + 1900; o[1] = m->tm_mon + 1; o[2] = m->tm_mday; o[3] = m->tm_hour; o[4] = m->tm_min; o[5] = m->tm_sec; }
bool plat_save_read(const char *name, void *buf, size_t len) {
    char p[96]; snprintf(p, sizeof p, DATA_DIR "save/%s", name); FILE *f = fopen(p, "rb"); if (!f) return false;
    bool ok = fread(buf, 1, len, f) == len; fclose(f); return ok; }
bool plat_save_write(const char *name, const void *buf, size_t len) {
    mkdir(DATA_DIR "save", 0777);
    char p[96]; snprintf(p, sizeof p, DATA_DIR "save/%s", name); FILE *f = fopen(p, "wb");
    if (!f) { plat_log("cannot write %s", p); return false; }
    bool ok = fwrite(buf, 1, len, f) == len; fclose(f); if (!ok) plat_log("write failed %s", p); return ok; }
void plat_poll(void) { pump(); }
const char *plat_goto_override(void) { return NULL; }   /* test hook used by the headless build only */
void plat_cursor(int *x, int *y, bool set) { if (set) { g_cx = *x; g_cy = *y; } else { *x = g_cx; *y = g_cy; } }
bool plat_key(int key) {
    switch (key) {
    case 0: return (g_held & KEY_A) || (g_touching && g_touch_armed);
    case 1: {                                   /* cancel. In the dialogue/choice loop it opens the system menu: START there;
                                                   B keeps working as "back" in menus, save/load screens and the message log */
        const ui_text *u = sys_ui_text(g_vm); bool story = u->loop == 100 && !u->backlog && (u->dialogue_visible || u->menu_visible);
        if (story) return (g_held & KEY_START) && !(g_held & KEY_SELECT);
        return (g_held & (KEY_B | KEY_START)) && !((g_held & KEY_START) && (g_held & KEY_SELECT)); }
    case 2: return (g_held & KEY_DUP) != 0;
    case 3: return (g_held & KEY_DDOWN) != 0;
    case 4: return (g_held & KEY_DLEFT) != 0;
    case 5: return (g_held & KEY_DRIGHT) != 0;
    case 6: return (g_held & KEY_R) != 0;       /* skip */
    case 9: return (g_held & KEY_L) != 0;       /* message log (mouse wheel up on PC) */
    }
    return false;
}

/* ---- drawing (framebuffers are 240 wide x N tall, column-major, bottom-up, BGR) ---- */
static inline void put(u8 *fb, int x, int y, int r, int g, int b) { u8 *p = fb + (x * 240 + (239 - y)) * 3; p[0] = b; p[1] = g; p[2] = r; }
/* stereoscopic 3D: the engine tags every screen pixel as backdrop or sprite (sys_scene_depth). With the 3D slider up, the
   backdrop is pushed behind the screen (up to BACK_MAX px per eye) and characters/overlays come slightly forward
   (up to FRONT_MAX px per eye, opposite direction). Backdrop hidden behind a sprite is filled from the nearest backdrop. */
enum { BACK_MAX = 7, FRONT_MAX = 3 };
static inline void avg2(const uint8_t *r0, const uint8_t *r1, int sx, int *r, int *g, int *b) {
    const uint8_t *a = r0 + sx * 6, *c = r1 + sx * 6; *r = (a[0] + a[3] + c[0] + c[3]) >> 2; *g = (a[1] + a[4] + c[1] + c[4]) >> 2; *b = (a[2] + a[5] + c[2] + c[5]) >> 2;
}
/* eye = +1 left, -1 right */
static void draw_eye(u8 *fb, const uint8_t *rgb, const uint8_t *z, int back, int front, int eye) {
    for (int y = 0; y < 240; y++) {
        const uint8_t *r0 = rgb + (y * 2) * 640 * 3, *r1 = r0 + 640 * 3;
        if (!z || (!back && !front)) { for (int x = 0; x < 320; x++) { int r, g, b; avg2(r0, r1, x, &r, &g, &b); put(fb, 40 + x, y, r, g, b); } continue; }
        const uint8_t *z0 = z + (y * 2) * 640, *z1 = z0 + 640;
        #define NEAR(k) (z0[(k) * 2] | z0[(k) * 2 + 1] | z1[(k) * 2] | z1[(k) * 2 + 1])
        int sb = back * eye, sf = front * eye;
        for (int x = 0; x < 320; x++) {                       /* backdrop: left eye sees it further left */
            int k = x + sb; if (k < 0 || k >= 320) { put(fb, 40 + x, y, 0, 0, 0); continue; }
            int step = sb > 0 ? 1 : -1;                       /* hidden behind a sprite: stretch the backdrop at the sprite's near edge */
            while (k != x && NEAR(k)) k -= step;
            if (NEAR(k)) { int j = x; while (j >= 0 && j < 320 && NEAR(j)) j -= step; if (j >= 0 && j < 320) k = j; }
            int r, g, b; avg2(r0, r1, k, &r, &g, &b); put(fb, 40 + x, y, r, g, b); }
        for (int x = 0; x < 320; x++) if (NEAR(x)) {         /* sprites: left eye sees them further right */
            int d = x + sf; if (d < 0 || d >= 320) continue; int r, g, b; avg2(r0, r1, x, &r, &g, &b); put(fb, 40 + d, y, r, g, b); }
        #undef NEAR
    }
}
static void draw_scene(const uint8_t *rgb) {
    static bool stereo_on;
    float sl = osGet3DSliderState(); int back = (int)(sl * BACK_MAX + 0.5f), front = (int)(sl * FRONT_MAX + 0.5f);
    const uint8_t *z = sys_scene_depth(); bool st = rgb && sl > 0.01f;
    if (st != stereo_on) { gfxSet3D(st); stereo_on = st; }
    u8 *fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
    memset(fb, 0, 400 * 240 * 3); if (!rgb) return;
    if (!st) { draw_eye(fb, rgb, NULL, 0, 0, 1); return; }
    draw_eye(fb, rgb, z, back, front, 1);
    u8 *fr = gfxGetFramebuffer(GFX_TOP, GFX_RIGHT, NULL, NULL); memset(fr, 0, 400 * 240 * 3);
    draw_eye(fr, rgb, z, back, front, -1);
}
static void glyph(u8 *fb, int x, int y, uint32_t idx, int w, int r, int g, int b) {
    uint32_t cells; const uint8_t *font = sys_font(&cells); if (!font || idx >= cells) return; const uint8_t *gl = font + idx * 128;
    for (int j = 0; j < 16; j++) for (int i = 0; i < w; i++) { int tx = x + i, ty = y + j; if (tx < 0 || tx >= 320 || ty < 0 || ty >= 240) continue;
        int a = (gl[(j * 16 + i) >> 1] >> ((i & 1) ? 0 : 4)) & 15; if (!a) continue;
        u8 *p = fb + (tx * 240 + (239 - ty)) * 3; p[0] = (p[0] * (15 - a) + b * a) / 15; p[1] = (p[1] * (15 - a) + g * a) / 15; p[2] = (p[2] * (15 - a) + r * a) / 15; }
}
static void draw_text(void) {
    u8 *fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);
    memset(fb, 0, 320 * 240 * 3);
    for (int i = 0; i < 14; i++) g_row_item[i] = -1;
    const ui_text *u = sys_ui_text(g_vm);
    g_menu_mode = (u->loop >= 0 && u->loop != 100) || (!u->dialogue_visible && !u->menu_visible);
    if (g_menu_mode) return;            /* full-screen menus and the title: the picture is on the top screen only (bottom stays dark);
                                           a touch still clicks the matching spot of the 640x480 screen */
    int row = 0, x = 0;
    for (int l = 0; l < u->nlines && row < 13; l++) {
        const uint8_t *p = (const uint8_t *)u->line[l].sjis; int item = u->line[l].item, hot = item >= 0 && item == u->hover;
        if (hot) for (int yy = 6 + row * 18; yy < 6 + row * 18 + 18 && yy < 240; yy++) for (int xx = 0; xx < 320; xx++) put(fb, xx, yy, 0xE2, 0x00, 0x7F);
        if (item >= 0) g_row_item[row] = item;
        while (*p && row < 13) { int half; uint32_t idx = sys_glyph_index(&p, &half); int w = half ? 8 : 16;
            if (idx == 95 && (sys_font_flags() & 1)) { if (x) x += 8; continue; }   /* Korean text: word spaces are full-width 0x8140 */
            if (x + w > 320) { x = 0; row++; if (row >= 13) break; if (item >= 0) g_row_item[row] = item;
                if (hot) for (int yy = 6 + row * 18; yy < 6 + row * 18 + 18 && yy < 240; yy++) for (int xx = 0; xx < 320; xx++) put(fb, xx, yy, 0xE2, 0x00, 0x7F); }
            glyph(fb, x, 8 + row * 18, idx, w, 255, 255, 255); x += w; }
        /* a 640-wide line that was wrapped by the engine continues on the same paragraph; anything else starts a new row */
        if (item >= 0 || strlen(u->line[l].sjis) < 72) { x = 0; row++; }
    }
    if (u->dialogue_visible && !u->menu_visible && !u->backlog) {    /* "다음 (터치)" hint, dialogue only */
        extern const int next_hint_w, next_hint_h; extern const uint8_t next_hint[];
        int ox = 320 - next_hint_w - 2, oy = 240 - next_hint_h - 2;
        for (int y = 0; y < next_hint_h; y++) for (int xx = 0; xx < next_hint_w; xx++) { const uint8_t *q = next_hint + (y * next_hint_w + xx) * 4; int a = q[3]; if (!a) continue;
            u8 *p = fb + ((ox + xx) * 240 + (239 - (oy + y))) * 3;   /* framebuffer is BGR */
            p[0] = (p[0] * (255 - a) + q[2] * a) / 255; p[1] = (p[1] * (255 - a) + q[1] * a) / 255; p[2] = (p[2] * (255 - a) + q[0] * a) / 255; }
    }
}
bool plat_frame(const uint8_t *rgb, int w, int h) {
    (void)rgb; (void)w; (void)h;
    if (!aptMainLoop()) return false;
    pump();
    if ((g_held & KEY_START) && (g_held & KEY_SELECT)) return false;
    draw_scene(sys_scene_rgb()); draw_text();
    gfxFlushBuffers(); gfxSwapBuffers(); gspWaitForVBlank();
    return true;
}
static void fatal(const char *msg) {
    consoleInit(GFX_BOTTOM, NULL); printf("\n %s\n\n Put the game files in\n " DATA_DIR "\n\n START: exit\n", msg);
    while (aptMainLoop()) { hidScanInput(); if (hidKeysDown() & KEY_START) break; gfxFlushBuffers(); gfxSwapBuffers(); gspWaitForVBlank(); }
}
/* start-up splash: stereo artwork on the top screen (left/right eye from an MPO), prompt on the bottom; A or a touch continues */
extern const uint8_t splash_lz[]; extern const size_t splash_lz_size, splash_raw_size;
static void blit565(u8 *fb, const uint16_t *src, int w, int h, int ox) {
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { uint16_t c = src[y * w + x];
        put(fb, ox + x, y, (c >> 11) * 255 / 31, (c >> 5 & 63) * 255 / 63, (c & 31) * 255 / 31); }
}
static bool splash(void) {
    uint8_t *raw = malloc(splash_raw_size + 64); if (!raw) return true;
    if (lzss_decode(splash_lz, splash_lz_size, raw, splash_raw_size) < splash_raw_size) { free(raw); return true; }
    const uint16_t *L = (const uint16_t *)raw, *R = L + 400 * 240, *B = R + 400 * 240;
    gfxSet3D(true); bool go = true; int armed = 0;
    while ((go = aptMainLoop())) {
        hidScanInput(); u32 k = hidKeysDown(), held = hidKeysHeld();
        if (held & (KEY_A | KEY_TOUCH)) { if (armed) break; } else armed = 1;   /* ignore a key already held at launch */
        if ((held & KEY_START) && (held & KEY_SELECT)) { go = false; break; }
        (void)k;
        blit565(gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL), L, 400, 240, 0);
        blit565(gfxGetFramebuffer(GFX_TOP, GFX_RIGHT, NULL, NULL), R, 400, 240, 0);
        blit565(gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL), B, 320, 240, 0);
        gfxFlushBuffers(); gfxSwapBuffers(); gspWaitForVBlank();
    }
    /* wait for release so the press does not also act in the game */
    while (go && aptMainLoop()) { hidScanInput(); if (!(hidKeysHeld() & (KEY_A | KEY_TOUCH))) break; gspWaitForVBlank(); }
    gfxSet3D(false); free(raw); return go;
}
int main(void) {
    gfxInitDefault(); gfxSetDoubleBuffering(GFX_TOP, true); gfxSetDoubleBuffering(GFX_BOTTOM, true);
    osSetSpeedupEnable(true);
    void snd_init(void), snd_exit(void); snd_init();
    static vm_t vm; g_vm = &vm;
    if (!splash()) { snd_exit(); gfxExit(); return 0; }
    if (!vm_init(&vm)) fatal("Archives not found (MES.ARC DATA.ARC BG.ARC).");
    else if (!vm_load_mes(&vm, "MAIN.MES")) fatal("MAIN.MES missing in MES.ARC.");
    else vm_run(&vm);
    snd_exit(); gfxExit(); return 0;
}
