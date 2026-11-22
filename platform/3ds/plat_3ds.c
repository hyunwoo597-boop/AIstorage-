/* Nintendo 3DS backend (libctru, software framebuffers).
 * Top screen   : the scene, 640x480 averaged down to 320x240, centred in 400x240.
 * Bottom screen: dialogue / choices re-rendered from strings with the 16px font (20 full-width columns).
 * Data folder  : sdmc:/3ds/NonomuraHospitalHD/  (MES.ARC DATA.ARC BG.ARC font16.bin; optional SOUND.ARC VOICE.ARC)
 * NOTE: written without access to hardware or the toolchain; first build may need fixes. */
#include <3ds.h>
#include <stdio.h>
#include <math.h>
#include <dirent.h>
#include <time.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include "ai5.h"

u32 __stacksize__ = 0x40000;   /* the script interpreter recurses; the default 32 KB is too small */

#define DATA_DIR "sdmc:/3ds/NonomuraHospitalHD/"
static vm_t *g_vm;
static u32 g_held, g_down; static touchPosition g_touch; static bool g_touching, g_touch_armed;
static int g_cx = 320, g_cy = 100;
static int g_row_item[14];              /* bottom-screen text row -> choice index (-1 none) */
static bool g_menu_mode;                /* no text window open (save/load/settings screens): bottom screen mirrors the scene */

/* ---- files: keep a few archives open ---- */
static struct { char name[24]; FILE *f; } g_open[6];
/* Game files come from the SD folder; a CIA made with the HTML maker carries them in its RomFS instead (flat: "patch/x"
   is stored as "patch_x"). A file on the SD card always wins, so single files (e.g. DEPTH.PAK) can still be replaced. */
static bool g_romfs;
static FILE *open_data(const char *name) {
    char p[96]; snprintf(p, sizeof p, DATA_DIR "%s", name); FILE *f = fopen(p, "rb"); if (f || !g_romfs) return f;
    snprintf(p, sizeof p, "romfs:/%s", name); for (char *c = p + 7; *c; c++) if (*c == '/') *c = '_';
    return fopen(p, "rb");
}
static FILE *get_file(const char *name) {
    for (int i = 0; i < 6; i++) if (g_open[i].f && !strcmp(g_open[i].name, name)) return g_open[i].f;
    FILE *f = open_data(name); if (!f) return NULL;
    for (int i = 0; i < 6; i++) if (!g_open[i].f) { strncpy(g_open[i].name, name, 23); g_open[i].f = f; return f; }
    fclose(g_open[0].f); strncpy(g_open[0].name, name, 23); g_open[0].f = f; return f;
}
bool plat_read_at(const char *name, uint32_t off, void *dst, uint32_t len) {
    FILE *f = get_file(name); if (!f) return false; if (fseek(f, off, SEEK_SET)) return false; return fread(dst, 1, len, f) == len; }
int plat_list(const char *dir, char names[][24], int max) {   /* file names in a game-folder subdirectory (SD, then RomFS) */
    char p[96]; snprintf(p, sizeof p, DATA_DIR "%s", dir); DIR *d = opendir(p); int n = 0; struct dirent *e;
    if (d) { while ((e = readdir(d)) && n < max) { if (e->d_name[0] == '.' || strlen(e->d_name) > 23) continue; strcpy(names[n++], e->d_name); } closedir(d); }
    if (g_romfs && (d = opendir("romfs:/"))) { size_t pl = strlen(dir);
        while ((e = readdir(d)) && n < max) { if (strncmp(e->d_name, dir, pl) || e->d_name[pl] != '_') continue; const char *nm = e->d_name + pl + 1;
            bool dup = false; for (int i = 0; i < n; i++) if (!strcasecmp(names[i], nm)) dup = true; if (!dup && strlen(nm) <= 23) strcpy(names[n++], nm); }
        closedir(d); }
    return n; }
uint32_t plat_file_size(const char *name) { FILE *f = get_file(name); if (!f) return 0; fseek(f, 0, SEEK_END); return (uint32_t)ftell(f); }
void *plat_read_file(const char *name, size_t *len) { uint32_t n = plat_file_size(name); if (!n) return NULL; void *b = malloc(n + 1);
    if (!b || !plat_read_at(name, 0, b, n)) { free(b); return NULL; } if (len) *len = n; return b; }
void plat_log(const char *fmt, ...) {
    static int n; if (++n > 300) return; FILE *f = fopen(DATA_DIR "log.txt", n == 1 ? "w" : "a"); if (!f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap); fputc('\n', f); fclose(f);
}

/* ---- input ---- */
static bool g_viewport; static int g_click, g_t0x, g_t0y, g_tlx, g_tly; static bool g_dragged;
static void pump(void) {
    hidScanInput(); g_held = hidKeysHeld(); g_down = hidKeysDown();
    bool t = (g_held & KEY_TOUCH) != 0;
    if (g_click) g_click--;
    if (g_viewport) {                    /* galleries: drag pans the 1:1 window, a tap (no drag) clicks where it was released */
        if (t) { hidTouchRead(&g_touch);
            if (!g_touching) { g_t0x = g_tlx = g_touch.px; g_t0y = g_tly = g_touch.py; g_dragged = false; }
            else { int dx = g_touch.px - g_tlx, dy = g_touch.py - g_tly;
                if (abs(g_touch.px - g_t0x) + abs(g_touch.py - g_t0y) > 6) g_dragged = true;
                if (g_dragged && (dx || dy)) sys_bottom_pan(-dx, -dy); g_tlx = g_touch.px; g_tly = g_touch.py; } }
        else if (g_touching && !g_dragged) { int x, y; sys_bottom_map(g_tlx, g_tly, &x, &y); g_cx = x; g_cy = y; g_click = 2; }
        g_touching = t; g_touch_armed = false; return;
    }
    if (g_menu_mode) {                   /* bottom-screen menus: the pointer follows the finger, lifting it is the click */
        if (t) { hidTouchRead(&g_touch); int x, y; sys_bottom_map(g_touch.px, g_touch.py, &x, &y); g_cx = x; g_cy = y; g_tlx = g_touch.px; g_tly = g_touch.py; }
        else if (g_touching) { int x, y; sys_bottom_map(g_tlx, g_tly, &x, &y); g_cx = x; g_cy = y; g_click = 2; }
        g_touching = t; g_touch_armed = false; return;
    }
    if (t) { hidTouchRead(&g_touch);
        if (!g_touching) {               /* touch began: point at the choice under the finger, confirm from the next frame on */
            g_touch_armed = false;
            if (g_menu_mode) { int x, y; sys_bottom_map(g_touch.px, g_touch.py, &x, &y); g_cx = x; g_cy = y; }   /* touch -> the matching spot of the 640x480 screen */
            else { int row = (g_touch.py - 8) / 18;
                if (row >= 0 && row < 14 && g_row_item[row] >= 0) { g_cx = 100; g_cy = 384 + g_row_item[row] * 18 + 9; } }
        } else g_touch_armed = true; }
    else g_touch_armed = false;
    g_touching = t;
}
bool plat_wants_composite(void) { return false; }
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
    case 0: return (g_held & KEY_A) || (g_touching && g_touch_armed) || g_click == 1;
    case 1: {                                   /* cancel. In the dialogue/choice loop it opens the system menu: START there;
                                                   B keeps working as "back" in menus, save/load screens and the message log */
        const ui_text *u = sys_ui_text(g_vm); bool story = u->loop == 100 && !u->backlog && (u->dialogue_visible || u->menu_visible);
        if (story) return (g_held & KEY_START) && !(g_held & KEY_SELECT);
        return (g_held & (KEY_B | KEY_START)) && !((g_held & KEY_START) && (g_held & KEY_SELECT)); }
    case 2: return (g_held & KEY_DUP) != 0;
    case 3: return (g_held & KEY_DDOWN) != 0;
    case 4: return (g_held & KEY_DLEFT) != 0;
    case 5: return (g_held & KEY_DRIGHT) != 0;
    case 6:                                     /* skip: wait / effects (Shift) */
    case 7: return (g_held & KEY_R) != 0;       /* skip: the message loop of the scripts polls key 7 (Ctrl) */
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
static const uint8_t *g_fade; static int g_fade_a;
static inline void avg2(const uint8_t *r0, const uint8_t *r1, int sx, int *r, int *g, int *b) {
    const uint8_t *a = r0 + sx * 6, *c = r1 + sx * 6; *r = (a[0] + a[3] + c[0] + c[3]) >> 2; *g = (a[1] + a[4] + c[1] + c[4]) >> 2; *b = (a[2] + a[5] + c[2] + c[5]) >> 2;
}
/* eye = +1 left, -1 right */
static void draw_eye(u8 *fb, const uint8_t *rgb, const uint8_t *z, int back, int front, int eye) {
    for (int y = 0; y < 240; y++) {
        const uint8_t *r0 = rgb + (y * 2) * 640 * 3, *r1 = r0 + 640 * 3;
        if (!z || (!back && !front)) {
            if (g_fade) { const uint8_t *f0 = g_fade + (y * 2) * 640 * 3, *f1 = f0 + 640 * 3; int fa = g_fade_a, na = 256 - fa;   /* SYS 10 crossfade */
                for (int x = 0; x < 320; x++) { int r, g, b, R, G, B; avg2(r0, r1, x, &r, &g, &b); avg2(f0, f1, x, &R, &G, &B);
                    put(fb, 40 + x, y, (r * na + R * fa) >> 8, (g * na + G * fa) >> 8, (b * na + B * fa) >> 8); } continue; }
            for (int x = 0; x < 320; x++) { int r, g, b; avg2(r0, r1, x, &r, &g, &b); put(fb, 40 + x, y, r, g, b); } continue; }
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
/* ---- pseudo depth for full-screen pictures (event CGs, title): they carry no sprite layer, so a depth map is guessed
   from the picture at 160x120: colourful areas (characters: hair, skin, clothes) and detailed areas (line art) come
   forward, muted flat areas (walls, sky, floors) go back, plus a slight "lower is nearer" ramp. Recomputed only when
   the picture changes. ---- */
static uint8_t g_dm[120][160]; static uint32_t g_dm_sig = 1;
static void blur_rows(float *a, float *t, int r) {   /* separable box blur (edge-clamped), result back in a */
    float ps[161];
    for (int y = 0; y < 120; y++) { ps[0] = 0; for (int x = 0; x < 160; x++) ps[x + 1] = ps[x] + a[y * 160 + x];
        for (int x = 0; x < 160; x++) { int l = x - r < 0 ? 0 : x - r, h = x + r > 159 ? 159 : x + r; t[y * 160 + x] = (ps[h + 1] - ps[l]) / (h - l + 1); } }
    float pc[121];
    for (int x = 0; x < 160; x++) { pc[0] = 0; for (int y = 0; y < 120; y++) pc[y + 1] = pc[y] + t[y * 160 + x];
        for (int y = 0; y < 120; y++) { int l = y - r < 0 ? 0 : y - r, h = y + r > 119 ? 119 : y + r; a[y * 160 + x] = (pc[h + 1] - pc[l]) / (h - l + 1); } }
}
static void norm595(float *a) {   /* stretch so the 5th..95th percentile spans 0..1 */
    int h[256] = {0}; float mn = 1e9f, mx = -1e9f; for (int i = 0; i < 19200; i++) { if (a[i] < mn) mn = a[i]; if (a[i] > mx) mx = a[i]; }
    float sc = mx > mn ? 255.0f / (mx - mn) : 0; for (int i = 0; i < 19200; i++) h[(int)((a[i] - mn) * sc)]++;
    int c = 0, lo = 0, hi = 255; for (int k = 0; k < 256; k++) { c += h[k]; if (c < 960) lo = k; if (c < 18240) hi = k; }
    float L = mn + lo / sc, H = mn + (hi + 1) / sc; if (H <= L) H = L + 1e-3f;
    for (int i = 0; i < 19200; i++) { float v = (a[i] - L) / (H - L); a[i] = v < 0 ? 0 : v > 1 ? 1 : v; }
}
/* shape from shading: on painted characters the light catches the round forms (chest, hips, cheeks, thighs) - a pixel
   brighter than its surroundings is taken as nearer. g_shade = small-scale minus large-scale brightness, 0..1 (0.5 = flat). */
static float g_shade[19200];
static void build_shade(const uint8_t *rgb) {
    static float a[19200], b[19200], e[19200], t[19200];
    for (int y = 0; y < 120; y++) for (int x = 0; x < 160; x++) { const uint8_t *p = rgb + ((y * 4 + 1) * 640 + x * 4 + 1) * 3; a[y * 160 + x] = b[y * 160 + x] = (p[0] * 3 + p[1] * 6 + p[2]) / 2550.0f; }
    for (int y = 0; y < 120; y++) for (int x = 0; x < 160; x++) { int i = y * 160 + x; float dx = x < 159 ? fabsf(a[i + 1] - a[i]) : 0, dy = y < 119 ? fabsf(a[i + 160] - a[i]) : 0; e[i] = dx + dy; }
    blur_rows(e, t, 4);                               /* edge density: lettering and fine detail are busy, bodies are smooth */
    blur_rows(a, t, 4); blur_rows(a, t, 3);           /* only broad forms - thin strokes (logo, text) average away */
    blur_rows(b, t, 12); blur_rows(b, t, 12);
    for (int i = 0; i < 19200; i++) g_shade[i] = a[i] - b[i];
    norm595(g_shade);
    for (int i = 0; i < 19200; i++) { float w = 1.0f - (e[i] - 0.06f) * 8.0f; if (w < 0) w = 0; if (w > 1) w = 1; g_shade[i] = 0.5f + (g_shade[i] - 0.5f) * w; }
}
static void build_dm(const uint8_t *rgb) {
    static float sat[19200], edg[19200], lum[19200], tmp[19200];
    for (int y = 0; y < 120; y++) for (int x = 0; x < 160; x++) { const uint8_t *p = rgb + ((y * 4) * 640 + x * 4) * 3;
        int mx = p[0] > p[1] ? (p[0] > p[2] ? p[0] : p[2]) : (p[1] > p[2] ? p[1] : p[2]), mn = p[0] < p[1] ? (p[0] < p[2] ? p[0] : p[2]) : (p[1] < p[2] ? p[1] : p[2]);
        float v = mx / 255.0f; sat[y * 160 + x] = mx ? (float)(mx - mn) / mx * (v * 1.5f > 1 ? 1 : v * 1.5f) : 0; lum[y * 160 + x] = (p[0] + p[1] + p[2]) / 765.0f; }
    for (int y = 0; y < 120; y++) for (int x = 0; x < 160; x++) { int i = y * 160 + x; float a = lum[i];
        edg[i] = fabsf((x < 159 ? lum[i + 1] : a) - a) + fabsf((y < 119 ? lum[i + 160] : a) - a); }
    blur_rows(sat, tmp, 4); blur_rows(sat, tmp, 4); norm595(sat);
    blur_rows(edg, tmp, 5); blur_rows(edg, tmp, 5); norm595(edg);
    for (int y = 0; y < 120; y++) for (int x = 0; x < 160; x++) { int i = y * 160 + x; lum[i] = 0.55f * sat[i] + 0.25f * edg[i] + 0.20f * y / 119.0f; }
    blur_rows(lum, tmp, 3); norm595(lum);
    build_shade(rgb);                                 /* lit round forms of the characters come further forward */
    for (int i = 0; i < 19200; i++) { float v = lum[i] + 0.5f * (g_shade[i] - 0.5f) * (0.3f + 0.7f * sat[i]); lum[i] = v < 0 ? 0 : v > 1 ? 1 : v; }
    for (int i = 0; i < 19200; i++) g_dm[i / 160][i % 160] = (uint8_t)(lum[i] * 255);
}
/* background objects: the picture (160x120, lightly blurred) is split into regions of similar colour (flood fill, neighbour
   difference < 22). Big regions (walls, floor, ceiling, sky: >12% of the picture or spanning its width) follow a room
   model - near at the picture's edges, far toward a vanishing point a bit above the centre. Smaller regions are objects
   (phone, bed, vending machine, chairs, plants...) and get one depth from where they touch the ground: the lower their
   bottom edge, the nearer. Result in g_room (0 far .. 1 near). */
static float g_room[19200];
static void build_room(const uint8_t *rgb) {
    static int16_t c[19200][3]; static int lab[19200], q[19200];
    for (int y = 0; y < 120; y++) for (int x = 0; x < 160; x++) { int sum[3] = {0, 0, 0};
        for (int j = 0; j < 4; j += 2) for (int i = 0; i < 4; i += 2) { const uint8_t *p = rgb + ((y * 4 + j) * 640 + x * 4 + i) * 3; sum[0] += p[0]; sum[1] += p[1]; sum[2] += p[2]; }
        for (int k = 0; k < 3; k++) c[y * 160 + x][k] = sum[k] / 4; }
    for (int i = 0; i < 19200; i++) lab[i] = -1;
    for (int st = 0; st < 19200; st++) { if (lab[st] >= 0) continue;
        int qh = 0, qt = 0, area = 0, minx = 159, maxx = 0, maxy = 0; q[qt++] = st; lab[st] = st;
        while (qh < qt) { int i = q[qh++], x = i % 160, y = i / 160; area++; if (x < minx) minx = x; if (x > maxx) maxx = x; if (y > maxy) maxy = y;
            int nb[4] = { x > 0 ? i - 1 : -1, x < 159 ? i + 1 : -1, y > 0 ? i - 160 : -1, y < 119 ? i + 160 : -1 };
            for (int k = 0; k < 4; k++) { int n = nb[k]; if (n < 0 || lab[n] >= 0) continue;
                if (abs(c[n][0] - c[i][0]) + abs(c[n][1] - c[i][1]) + abs(c[n][2] - c[i][2]) < 22) { lab[n] = st; q[qt++] = n; } } }
        bool big = area > 2304 || (minx == 0 && maxx == 159) || area < 8;
        float obj = 0.2f + 0.8f * maxy / 119.0f;
        for (int k = 0; k < qt; k++) { int i = q[k], x = i % 160, y = i / 160;
            if (big) { float ax = fabsf(x - 80) / 80.0f, ay = fabsf(y - 54) / 66.0f; float m = ax > ay ? ax : ay; g_room[i] = 0.9f * (m > 1 ? 1 : m); }
            else g_room[i] = obj; } }
    static float t[19200]; blur_rows(g_room, t, 1);
}
/* normal scenes: characters (sprite layer) are given volume. Depth inside a sprite grows with the distance from its
   outline (rounded: sqrt), so the middle of a body - chest, hips, faces - comes forward and the silhouette edges stay
   near the screen; the backdrop sits behind with a little of the guessed depth for variation. */
/* background contrast curve (S-shaped): mid-distance steps about 1.6x larger, the far and near ends pressed together.
   Only for the backdrop behind standing characters; characters and event CGs keep the plain AI depth. */
static const uint8_t bg_curve[126] = { 0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,2,2,2,3,3,4,4,5,5,6,6,7,8,9,9,10,11,12,13,14,15,17,18,19,21,22,24,25,27,29,30,32,34,36,38,40,42,44,46,48,50,52,55,57,59,61,64,66,68,70,73,75,77,79,81,83,85,87,89,91,93,95,96,98,100,101,103,104,106,107,108,110,111,112,113,114,115,116,116,117,118,119,119,120,120,121,121,122,122,123,123,123,124,124,124,124,124,125,125,125,125,125,125,125,125,125,125,125,125 };
static float g_lo_raw[19200]; static bool g_lo_chars;   /* unblurred cell depth, and: scene of mapped characters only */
static void build_dm_layers(const uint8_t *rgb, const uint8_t *z) {
    static float dist[19200], tmp[19200]; static uint8_t spr[19200], zc[19200];
    /* layer per cell: 0/1 = backdrop/sprite without AI map, 2..127 / 128..253 = with AI depth (DEPTH.PAK) */
    int n_ai_spr = 0, n_ai_bg = 0, n_raw_bg = 0, n_raw_spr = 0;
    for (int y = 0; y < 120; y++) for (int x = 0; x < 160; x++) { int n = 0, i = y * 160 + x, sv = 0; for (int j = 0; j < 4; j += 2) for (int k = 0; k < 4; k += 2) { int v = z[(y * 4 + j) * 640 + x * 4 + k]; n += v == 1 || v >= 128; if (v >= 128 && v > sv) sv = v; }
        spr[i] = n >= 2; int c = z[(y * 4 + 1) * 640 + x * 4 + 1]; if (spr[i] && c < 128 && c != 1) c = sv ? sv : 1;   /* outline cell: depth of its sprite samples */ if (!spr[i] && (c == 1 || c >= 128)) c = 0; zc[i] = (uint8_t)c;
        if (c >= 128) n_ai_spr++; else if (c == 1) n_raw_spr++; else if (c == 0) n_raw_bg++; else n_ai_bg++; }
    if (n_raw_bg) build_room(rgb);                   /* backdrop without a map: room perspective and objects */
    bool chars = n_ai_spr > 384;                     /* mapped characters on screen */
    bool win = !chars && n_ai_bg > n_raw_bg;         /* mapped CG with only the text window as sprite */
    /* lettering (title logo, opening notice, text window) is thin: it must sit on one flat plate, else the warp bends the strokes */
    static uint8_t plate[19200]; bool text = false;
    if (n_raw_spr) { int thick = 0;
        for (int y = 3; y < 117; y++) for (int x = 3; x < 157; x++) { int i = y * 160 + x; thick += zc[i] == 1 && zc[i - 3] == 1 && zc[i + 3] == 1 && zc[i - 480] == 1 && zc[i + 480] == 1; }
        text = thick * 5 < n_raw_spr || win; }
    if (text) { static uint8_t h[19200];                 /* raw sprite cells grown by 3 cells (12 px) */
        for (int y = 0; y < 120; y++) for (int x = 0; x < 160; x++) { int f = 0; for (int k = -3; k <= 3 && !f; k++) { int xx = x + k; f = xx >= 0 && xx < 160 && zc[y * 160 + xx] == 1; } h[y * 160 + x] = (uint8_t)f; }
        for (int y = 0; y < 120; y++) for (int x = 0; x < 160; x++) { int f = 0; for (int k = -3; k <= 3 && !f; k++) { int yy = y + k; f = yy >= 0 && yy < 120 && h[yy * 160 + x]; } plate[y * 160 + x] = (uint8_t)f; } }
    if (n_raw_spr && !text) build_shade(rgb);        /* sprites without a map: shading */
    /* chamfer distance to the nearest backdrop cell (3-4 metric), two passes */
    const float BIG = 1e6f;
    if (n_raw_spr && !text) {                         /* volume from the outline: only for sprites without a map */
    for (int i = 0; i < 19200; i++) dist[i] = spr[i] ? BIG : 0;
    for (int y = 0; y < 120; y++) for (int x = 0; x < 160; x++) { int i = y * 160 + x; if (!spr[i]) continue; float d = dist[i];
        if (x > 0) d = fminf(d, dist[i - 1] + 1); if (y > 0) { d = fminf(d, dist[i - 160] + 1); if (x > 0) d = fminf(d, dist[i - 161] + 1.414f); if (x < 159) d = fminf(d, dist[i - 159] + 1.414f); }
        if (x == 0 || y == 0 || x == 159) d = fminf(d, 1 + (float)(x < y ? x : y)); dist[i] = d; }
    for (int y = 119; y >= 0; y--) for (int x = 159; x >= 0; x--) { int i = y * 160 + x; if (!spr[i]) continue; float d = dist[i];
        if (x < 159) d = fminf(d, dist[i + 1] + 1); if (y < 119) { d = fminf(d, dist[i + 160] + 1); if (x < 159) d = fminf(d, dist[i + 161] + 1.414f); if (x > 0) d = fminf(d, dist[i + 159] + 1.414f); } dist[i] = d; }
    }
    for (int i = 0; i < 19200; i++) { float v; int c = zc[i];
        if (text && plate[i] && c < 128) v = win ? 0.9f : 0.8f;                                 /* flat plate for lettering */
        else if (c >= 128) v = 0.6f + 0.35f * (c - 128) / 125.0f;   /* gentler relief inside the body keeps hair and outlines clean */                                   /* AI depth of the character */
        else if (c >= 2) v = chars ? 0.5f * bg_curve[c - 2] / 125.0f : 0.85f * (c - 2) / 125.0f;                         /* AI depth of the background / CG */
        else if (spr[i]) { float t = dist[i] / 16.0f; if (t > 1) t = 1; v = 0.5f + 0.3f * sqrtf(t) + 0.45f * (g_shade[i] - 0.5f) * (0.4f + 0.6f * t);   /* volume + lit roundness */
                      if (v < 0.45f) v = 0.45f; if (v > 1) v = 1; }
        else v = 0.5f * g_room[i];                                                               /* backdrop 0..0.5: room + objects */
        tmp[i] = v; }
    memcpy(g_lo_raw, tmp, sizeof g_lo_raw); g_lo_chars = chars && !n_raw_spr;
    blur_rows(tmp, dist, 3);   /* soften the step at the outline so the warp stretches instead of tearing */
    for (int i = 0; i < 19200; i++) g_dm[i / 160][i % 160] = (uint8_t)(tmp[i] * 255);
}
static bool full_picture(const uint8_t *z) {   /* no sprite pixels anywhere (sampled) */
    if (!z) return false; for (int y = 4; y < 480; y += 8) for (int x = 4; x < 640; x += 8) { int v = z[y * 640 + x]; if (v == 1 || v >= 128) return false; } return true;
}
static bool ai_picture(const uint8_t *z) {   /* full picture whose depth came with DEPTH.PAK */
    int n = 0; for (int y = 4; y < 480; y += 8) for (int x = 4; x < 640; x += 8) n += z[y * 640 + x] >= 2; return n > 80 * 60 * 6 / 10;
}
static uint32_t pic_sig(const uint8_t *rgb) { uint32_t h = 2166136261u; for (int y = 3; y < 480; y += 12) for (int x = 5; x < 640; x += 12) { const uint8_t *p = rgb + (y * 640 + x) * 3; h = (h ^ (p[0] | p[1] << 8 | p[2] << 16)) * 16777619u; } return h; }
/* event CGs with an AI map get their depth at the full 320x240 output size (fine detail: faces, hands, folds) */
static uint8_t g_dmh[240][320]; static bool g_hi;
static void build_dm_hi(const uint8_t *z) {
    static float a[240 * 320], t[240 * 320];
    for (int y = 0; y < 240; y++) for (int x = 0; x < 320; x++) { const uint8_t *p = z + (y * 2) * 640 + x * 2; int v = (p[0] + p[1] + p[640] + p[641]) >> 2; a[y * 320 + x] = v < 2 ? 0 : (v - 2) / 125.0f; }
    for (int y = 0; y < 240; y++) { float *r = a + y * 320, c = 0; for (int x = 0; x < 320; x++) t[y * 320 + x] = r[x];   /* 3x3 box: smooth steps, keep detail */
        for (int x = 0; x < 320; x++) { c = t[y * 320 + x] * 2; if (x > 0) c += t[y * 320 + x - 1]; else c += t[y * 320 + x]; if (x < 319) c += t[y * 320 + x + 1]; else c += t[y * 320 + x]; r[x] = c / 4; } }
    for (int x = 0; x < 320; x++) { float prev = a[x];
        for (int y = 0; y < 240; y++) { float cur = a[y * 320 + x], nx = y < 239 ? a[(y + 1) * 320 + x] : cur; float v = (prev + 2 * cur + nx) / 4; prev = cur; g_dmh[y][x] = (uint8_t)(v * 255); } }
}
/* scenes with mapped characters: the smooth cell map (soft outlines, no tearing) plus the fine relief of the 1/2-size
   character maps (faces, hands, folds) inside the body, away from the outline */
static void build_dm_hi_layers(const uint8_t *z) {
    for (int y = 0; y < 240; y++) for (int x = 0; x < 320; x++) {
        float fx = x * 0.5f - 0.25f, fy = y * 0.5f - 0.25f; if (fx < 0) fx = 0; if (fy < 0) fy = 0;
        int x0 = (int)fx, y0 = (int)fy, x1 = x0 < 159 ? x0 + 1 : 159, y1 = y0 < 119 ? y0 + 1 : 119; float ax = fx - x0, ay = fy - y0;
        #define BIL(A) ((A(y0, x0) * (1 - ax) + A(y0, x1) * ax) * (1 - ay) + (A(y1, x0) * (1 - ax) + A(y1, x1) * ax) * ay)
        #define DM(r, c) (g_dm[r][c] / 255.0f)
        #define LR(r, c) (g_lo_raw[(r) * 160 + (c)])
        float v = BIL(DM);
        int px = x * 2, py = y * 2, c = z[py * 640 + px];
        if (c >= 128) {
            int in = 0; static const int o[4][2] = { { -10, 0 }, { 10, 0 }, { 0, -10 }, { 0, 10 } };
            for (int k = 0; k < 4; k++) { int qx = px + o[k][0], qy = py + o[k][1]; in += qx >= 0 && qx < 640 && qy >= 0 && qy < 480 && z[qy * 640 + qx] >= 128; }
            if (in == 4) { float raw = 0.6f + 0.35f * (c - 128) / 125.0f; v += raw - BIL(LR); }
        }
        #undef BIL
        #undef DM
        #undef LR
        g_dmh[y][x] = (uint8_t)(v < 0 ? 0 : v > 1 ? 255 : v * 255); }
}
/* the 320x240 picture (2x2 average, crossfade mixed in) is made once per frame and shared by both eyes; the warp is
   table lookups only (no divisions per pixel) */
static uint8_t g_half[240 * 320 * 3];
static int g_lo = 0, g_hi_d = 255, g_zp = 128, g_sub = 128;
/* depth cues on top of the stereo (only with the slider up): far background slightly out of focus, a soft shadow on the
   backdrop behind characters, a little air (haze) in the distance. Made once per picture/depth change. */
static void box_u8(uint8_t *a, int w, int h, int st, int r) {   /* running-sum box blur (edge-clamped) of one channel */
    static uint8_t t[320];
    for (int y = 0; y < h; y++) { uint8_t *p = a + y * w * st; for (int x = 0; x < w; x++) t[x] = p[x * st];
        int s = 0; for (int k = -r; k <= r; k++) s += t[k < 0 ? 0 : k >= w ? w - 1 : k];
        for (int x = 0; x < w; x++) { p[x * st] = (uint8_t)(s / (2 * r + 1)); int a0 = x - r, a1 = x + r + 1; s += t[a1 >= w ? w - 1 : a1] - t[a0 < 0 ? 0 : a0]; } }
    for (int x = 0; x < w; x++) { for (int y = 0; y < h; y++) t[y] = a[(y * w + x) * st];
        int s = 0; for (int k = -r; k <= r; k++) s += t[k < 0 ? 0 : k >= h ? h - 1 : k];
        for (int y = 0; y < h; y++) { a[(y * w + x) * st] = (uint8_t)(s / (2 * r + 1)); int a0 = y - r, a1 = y + r + 1; s += t[a1 >= h ? h - 1 : a1] - t[a0 < 0 ? 0 : a0]; } }
}
static void depth_fx(const uint8_t *z) {
    int range = g_hi_d - g_lo; if (range <= 40) return;                     /* flat screens (notices, menus): untouched */
    static uint8_t B[240 * 320 * 3], M[120 * 160], S[120 * 160];
    memcpy(B, g_half, sizeof B); for (int c = 0; c < 3; c++) box_u8(B + c, 320, 240, 3, 2);   /* 5x5 blur of the picture */
    /* character mask (quarter size) and its soft shadow, cast a little right and down */
    int nm = 0; for (int y = 0; y < 120; y++) for (int x = 0; x < 160; x++) { int v = z[(y * 4 + 1) * 640 + x * 4 + 1]; M[y * 160 + x] = (v == 1 || v >= 128) ? 255 : 0; nm += M[y * 160 + x] != 0; }
    bool shadow = false; (void)nm;                  /* character shadow removed (user request); mask kept for the other effects */
    if (shadow) { for (int y = 0; y < 120; y++) for (int x = 0; x < 160; x++) { int sx = x - 3, sy = y - 2; S[y * 160 + x] = (sx >= 0 && sy >= 0) ? M[sy * 160 + sx] : 0; }
        box_u8(S, 160, 120, 1, 3); box_u8(S, 160, 120, 1, 3); }
    /* depth for the effects: the farthest value within 4 px. The depth map runs a little past a character's outline;
       without this the background right next to the character kept its sharp, clear look and showed as a pale halo */
    static uint8_t D[240 * 320], T[240 * 320];
    for (int y = 0; y < 240; y++) for (int x = 0; x < 320; x++) T[y * 320 + x] = g_hi ? g_dmh[y][x] : g_dm[y >> 1][x >> 1];
    for (int y = 0; y < 240; y++) for (int x = 0; x < 320; x++) { int m = 255; for (int k = -4; k <= 4; k++) { int xx = x + k; if (xx < 0 || xx > 319) continue; int v = T[y * 320 + xx]; if (v < m) m = v; } D[y * 320 + x] = (uint8_t)m; }
    for (int x = 0; x < 320; x++) for (int y = 0; y < 240; y++) { int m = 255; for (int k = -4; k <= 4; k++) { int yy = y + k; if (yy < 0 || yy > 239) continue; int v = D[yy * 320 + x]; if (v < m) m = v; } T[y * 320 + x] = (uint8_t)m; }
    int sn = (g_sub - g_lo) * 256 / range;
    for (int y = 0; y < 240; y++) for (int x = 0; x < 320; x++) { int i = y * 320 + x; int d = T[i];
        int nd = (d - g_lo) * 256 / range; if (nd < 0) nd = 0; if (nd > 256) nd = 256;
        int wb = (sn - nd - 26) * 205 / 102; if (wb < 0) wb = 0; if (wb > 205) wb = 205;            /* out of focus behind the subject (max 80 %) */
        int hz = 0;                                                                                /* haze removed (user request) */
        int qi = (y >> 1) * 160 + (x >> 1); int dk = (shadow && !M[qi]) ? 256 - (S[qi] * 90 >> 8) : 256;   /* shadow behind characters (max 35 %) */
        uint8_t *o = g_half + i * 3; const uint8_t *b = B + i * 3; static const int hc[3] = { 170, 180, 200 };
        for (int c = 0; c < 3; c++) { int v = o[c] + ((b[c] - o[c]) * wb >> 8); v += (hc[c] - v) * hz >> 8; v = v * dk >> 8; o[c] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); } }
}
static void make_half(const uint8_t *rgb, const uint8_t *z, bool fx) {
    /* rebuilt every frame: the old reuse test sampled a sparse grid and missed small changes (S04 frame 335: a 30-pixel
       update stayed stale on the top screen) */
    int fa = g_fade_a, na = 256 - fa;
    for (int y = 0; y < 240; y++) { const uint8_t *r0 = rgb + (y * 2) * 640 * 3, *r1 = r0 + 640 * 3; uint8_t *o = g_half + y * 320 * 3;
        const uint8_t *f0 = g_fade ? g_fade + (y * 2) * 640 * 3 : NULL;
        for (int x = 0; x < 320; x++, o += 3) { int r, g, b; avg2(r0, r1, x, &r, &g, &b);
            if (f0) { int R, G, B; avg2(f0, f0 + 640 * 3, x, &R, &G, &B); r = (r * na + R * fa) >> 8; g = (g * na + G * fa) >> 8; b = (b * na + B * fa) >> 8; }   /* crossfade stays 3D */
            o[0] = r; o[1] = g; o[2] = b; } }
    (void)fx; (void)z;                                 /* depth effects (focus blur, shadow, haze) all removed at the user's request */
}
/* per-scene depth budget: the scene's own depth range (3rd..98th percentile) is spread over the whole budget, and the
   screen plane sits between the far end and the subject (centre of the picture), so flat scenes still get full
   depth, the subject comes a little forward and only the nearest parts reach the front limit */
static int g_dtoast, g_dtoast_kind;   /* PC98 / HD picture switch label (1 PC98, 2 HD, 3 no SILK files) */
static bool g_tilt_on = true; static float g_tilt; static int g_toast;   /* circle pad: look around (motion parallax) */
static void depth_stats(void) {
    int h[256] = { 0 }, hc[256] = { 0 }, n = 0, nc = 0;
    for (int y = 0; y < 120; y++) for (int x = 0; x < 160; x++) { int d = g_hi ? g_dmh[y * 2][x * 2] : g_dm[y][x]; h[d]++; n++;
        if (x >= 40 && x < 120 && y >= 18 && y < 102) { hc[d]++; nc++; } }
    int c = 0, lo = 0, hi = 255, sub = 128;
    for (int d = 0; d < 256; d++) { c += h[d]; if (c * 100 < n * 3) lo = d + 1; }
    c = 0; for (int d = 255; d >= 0; d--) { c += h[d]; if (c * 100 < n * 2) hi = d - 1; }
    c = 0; for (int d = 0; d < 256; d++) { c += hc[d]; if (c * 100 >= nc * 70) { sub = d; break; } }
    if (hi - lo < 40) { int m = (lo + hi) / 2; lo = m - 20; hi = m + 20; }   /* nearly flat picture: keep it nearly flat */
    if (sub < lo) sub = lo; if (sub > hi) sub = hi;
    g_lo = lo; g_hi_d = hi; g_sub = sub;
}
/* big depth steps (character outline against the background) become ramps ~12 px wide, small relief stays: at full slider
   the shift difference across an outline was larger than the outline's soft edge, so pixels folded over each other and
   showed as a pale ghost line. out = blur + (depth - blur) limited to +-12 levels */
static void soften_steps(void) {
    static uint16_t t[240 * 320]; static uint32_t c[321];
    for (int y = 0; y < 240; y++) { c[0] = 0; for (int x = 0; x < 320; x++) c[x + 1] = c[x] + g_dmh[y][x];
        for (int x = 0; x < 320; x++) { int a = x - 6 < 0 ? 0 : x - 6, b = x + 7 > 320 ? 320 : x + 7; t[y * 320 + x] = (uint16_t)((c[b] - c[a]) * 16 / (b - a)); } }
    static uint32_t cc[241];
    for (int x = 0; x < 320; x++) { cc[0] = 0; for (int y = 0; y < 240; y++) cc[y + 1] = cc[y] + t[y * 320 + x];
        for (int y = 0; y < 240; y++) { int a = y - 6 < 0 ? 0 : y - 6, b = y + 7 > 240 ? 240 : y + 7; int bl = (int)((cc[b] - cc[a]) / (b - a)) / 16;
            int d = g_dmh[y][x] - bl; if (d > 12) d = 12; if (d < -12) d = -12; g_dmh[y][x] = (uint8_t)(bl + d); } }
}
static void draw_eye_dm(u8 *fb, int back, int front, int eye) {   /* backward warp with the depth map */
    /* shifts in 1/16 pixel: whole-pixel shifts turned most of the fine relief of the depth maps into the same shift (86 %
       of character pixels unchanged between the old and the detailed maps); the output pixel is mixed from the two
       neighbouring source pixels instead */
    int sh1 = g_hi ? 0 : 1; int16_t tab[256];
    for (int d = 0; d < 256; d++) { float v;   /* g_zp: screen plane, set by the slider in draw_scene */
        if (d <= g_zp) { int r = g_zp - g_lo; v = r > 0 ? back * (float)(g_zp - (d < g_lo ? g_lo : d)) / r : 0; }        /* behind the screen */
        else { int r = g_hi_d - g_zp; v = r > 0 ? -front * (float)((d > g_hi_d ? g_hi_d : d) - g_zp) / r : 0; }         /* in front */
        float t = 0; if (g_tilt_on) { int r = g_hi_d - g_lo; if (r > 40) t = g_tilt * 5.0f * ((d < g_lo ? g_lo : d > g_hi_d ? g_hi_d : d) - g_zp) / r; }   /* motion parallax: near and far slide apart */
        float sv = eye * v + t, lim = back > front ? back : front; if (sv > lim) sv = lim; if (sv < -lim) sv = -lim;   /* stick never adds to the stereo extremes */
        tab[d] = (int16_t)lroundf(sv * 16); }
    for (int y = 0; y < 240; y++) { const uint8_t *row = g_half + y * 320 * 3; const uint8_t *dm = g_hi ? g_dmh[y] : g_dm[y >> 1];
        u8 *p = fb + (40 * 240 + (239 - y)) * 3;
        for (int x = 0; x < 320; x++, p += 240 * 3) {
            /* find the source column k with k = x + shift(depth at k). Where that has no single answer the background is
               being uncovered next to a character: take the farther candidate, so the gap is filled with background
               instead of a stretched line of the character's edge */
            #define CL(v) ((v) < 0 ? 0 : (v) > 319 * 16 ? 319 * 16 : (v))
            int x16 = x * 16;
            int k1 = CL(x16 + tab[dm[x >> sh1]]);
            int k2 = CL(x16 + tab[dm[(k1 + 8) >> 4 >> sh1]]);
            int k = k2;
            if ((k2 >> 4) != (k1 >> 4)) { int k3 = CL(x16 + tab[dm[(k2 + 8) >> 4 >> sh1]]);
                if ((k3 >> 4) != (k2 >> 4)) k = dm[(k2 + 8) >> 4 >> sh1] <= dm[(k1 + 8) >> 4 >> sh1] ? k2 : k1; }
            #undef CL
            int i = k >> 4, fr = k & 15; const uint8_t *q = row + i * 3, *q2 = i < 319 ? q + 3 : q;
            p[0] = (uint8_t)((q[2] * (16 - fr) + q2[2] * fr) >> 4); p[1] = (uint8_t)((q[1] * (16 - fr) + q2[1] * fr) >> 4); p[2] = (uint8_t)((q[0] * (16 - fr) + q2[0] * fr) >> 4); } }
}
static void draw_scene(const uint8_t *rgb) {
    static bool stereo_on, ypr;
    (void)ypr;
    { circlePosition cp; hidCircleRead(&cp); float v = cp.dx / 150.0f; if (fabsf(v) < 0.12f) v = 0;   /* dead zone */
        if (v > 1) v = 1; if (v < -1) v = -1; g_tilt += (v - g_tilt) * 0.25f; if (fabsf(g_tilt) < 0.01f) g_tilt = 0; }   /* eases back when released */
    g_fade = sys_fade(&g_fade_a);
    float sl = osGet3DSliderState(); int back = (int)(sl * BACK_MAX + 0.5f), front = (int)(sl * FRONT_MAX + 0.5f);
    const uint8_t *z = sys_scene_depth(); bool st = rgb && sl > 0.01f;
    if (st != stereo_on) { gfxSet3D(st); stereo_on = st; }
    u8 *fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
    memset(fb, 0, 400 * 240 * 3); if (!rgb) return;
    if (!st) { draw_eye(fb, rgb, NULL, 0, 0, 1); return; }
    if (z) {                                          /* depth map: AI (DEPTH.PAK), else guessed for full pictures / sprite volume for scenes */
        static int since; since++;
        uint32_t sig = 2166136261u; int ai = 0; for (int y = 2; y < 480; y += 8) for (int x = 2; x < 640; x += 8) { int v = z[y * 640 + x]; ai += v >= 2; sig = (sig ^ v) * 16777619u; }
        if (ai < 80 * 60 / 2) sig ^= pic_sig(rgb);    /* AI depth does not depend on the colours; guessed depth does */
        /* while the picture keeps changing (wipes, animation) rebuild at most every 4th frame, so transitions stay smooth */
        if (sig != g_dm_sig && (since >= 4 || g_dm_sig == 0)) { since = 0; g_hi = false; if (full_picture(z) && ai_picture(z)) { build_dm_hi(z); soften_steps(); g_hi = true; }
          else if (full_picture(z) && sys_has_depth()) memset(g_dm, 150, sizeof g_dm);   /* no map (notices, opening text, menus): one flat plane, lettering stays clean */
          else if (full_picture(z)) { build_dm(rgb); build_room(rgb);   /* event CG or empty background: the nearer of both guesses */
            for (int i = 0; i < 19200; i++) { int r = (int)(g_room[i] * 0.6f * 255); if (r > g_dm[i / 160][i % 160]) g_dm[i / 160][i % 160] = (uint8_t)r; } }
          else { build_dm_layers(rgb, z); if (g_lo_chars) { build_dm_hi_layers(z); soften_steps(); g_hi = true; } } g_dm_sig = sig; depth_stats(); }
        /* slider curve: 32 steps (no jitter); the effect rises quickly at the bottom (s^0.7), the depth behind the screen
           grows to 10 px and the pop-out grows faster in the upper half (2..6 px); as the slider goes up the screen plane
           moves from halfway (subject slightly forward) towards the far end (subject clearly in front of the screen) */
        float q = (int)(sl * 32.0f + 0.5f) / 32.0f, e = powf(q, 0.7f);
        int bk = (int)(e * 9.0f + 0.5f), fb2 = (int)(e * (2.0f + 3.0f * q) + 0.5f);
        g_zp = g_lo + (int)((g_sub - g_lo) * (0.55f - 0.25f * q));
        make_half(rgb, z, true); draw_eye_dm(fb, bk, fb2, 1);
        u8 *fr = gfxGetFramebuffer(GFX_TOP, GFX_RIGHT, NULL, NULL); memset(fr, 0, 400 * 240 * 3);
        draw_eye_dm(fr, bk, fb2, -1); return;
    }
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
    g_viewport = false;
    const ui_text *u = sys_ui_text(g_vm);
    const uint8_t *own = sys_bottom_rgb();   /* title menu / confirmation dialog, drawn by the engine for this screen */
    if (own) { g_menu_mode = true; for (int y = 0; y < 240; y++) for (int x = 0; x < 320; x++) { const uint8_t *c = own + (y * 320 + x) * 3; put(fb, x, y, c[0], c[1], c[2]); } return; }
    g_menu_mode = (u->loop >= 0 && u->loop != 100) || (!u->dialogue_visible && !u->menu_visible);
    if (g_menu_mode) {                  /* full-screen menus (system menu, save/load, galleries): shown here, half size, touchable;
                                           the top screen keeps the story picture. No menu running (title animation, cut-ins): dark. */
        if (u->loop < 0 || u->backlog) { g_viewport = false; return; }
        g_viewport = sys_bottom_viewport();
        if (!g_viewport && u->loop == 100) return;   /* story with the window hidden (scene changes, events): dark */
        if (g_viewport) { const uint8_t *v = sys_viewport_rgb(!g_touching); if (!v) return;
            for (int y = 0; y < 240; y++) for (int x = 0; x < 320; x++) { const uint8_t *c = v + (y * 320 + x) * 3; put(fb, x, y, c[0], c[1], c[2]); } return; }
        const uint8_t *rgb = sys_live_rgb(); if (!rgb) return;
        for (int y = 0; y < 240; y++) { const uint8_t *r0 = rgb + (y * 2) * 640 * 3, *r1 = r0 + 640 * 3;
            for (int x = 0; x < 320; x++) { const uint8_t *a = r0 + x * 6, *b = r1 + x * 6;
                put(fb, x, y, (a[0] + a[3] + b[0] + b[3]) >> 2, (a[1] + a[4] + b[1] + b[4]) >> 2, (a[2] + a[5] + b[2] + b[5]) >> 2); } }
        return;
    }
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
        /* "press me" effect: the hint breathes (fades between 35% and 100%) and bobs 3 px, one cycle per 1.2 s */
        float ph = (float)(osGetTime() % 1200) / 1200.0f * 6.2831853f, w = 0.5f + 0.5f * sinf(ph);
        int mul = (int)(90 + 165 * w), bob = (int)(3.0f * (0.5f - 0.5f * cosf(ph)));
        int ox = 320 - next_hint_w - 2, oy = 240 - next_hint_h - 4 + bob;
        for (int y = 0; y < next_hint_h; y++) for (int xx = 0; xx < next_hint_w; xx++) { const uint8_t *q = next_hint + (y * next_hint_w + xx) * 4; int a = q[3] * mul / 255; if (!a) continue;
            if (oy + y < 0 || oy + y >= 240) continue;
            u8 *p = fb + ((ox + xx) * 240 + (239 - (oy + y))) * 3;   /* framebuffer is BGR */
            p[0] = (p[0] * (255 - a) + q[2] * a) / 255; p[1] = (p[1] * (255 - a) + q[1] * a) / 255; p[2] = (p[2] * (255 - a) + q[0] * a) / 255; }
    }
}
bool plat_frame(const uint8_t *rgb, int w, int h) {
    (void)rgb; (void)w; (void)h;
    if (!aptMainLoop()) return false;
    pump();
    if ((g_held & KEY_START) && (g_held & KEY_SELECT)) return false;
    { static bool restored; if (!restored) { restored = true; FILE *f = fopen(DATA_DIR "dosgfx.cfg", "r"); if (f) { if (fgetc(f) == '1') sys_set_dos_gfx(g_vm, 1); fclose(f); } } }
    if ((g_down & KEY_SELECT) && !(g_held & KEY_START)) {      /* SELECT: PC98 (DOS release) pictures <-> HD remaster (Windows) pictures */
        int want = !sys_dos_gfx(), got = sys_set_dos_gfx(g_vm, want);
        g_dtoast_kind = want && !got ? 3 : got ? 1 : 2; g_dtoast = 120;
        FILE *f = fopen(DATA_DIR "dosgfx.cfg", "w"); if (f) { fputc(got ? '1' : '0', f); fclose(f); } }
    draw_scene(sys_scene_rgb()); draw_text();
    if (g_dtoast > 0) { g_dtoast--;                             /* label at the top of the bottom screen for 2 s (tools/mkswtoast.py) */
        extern const int dostoast_h, dostoast_w[3]; extern const uint8_t *const dostoast_px[3];
        int k = g_dtoast_kind - 1, w = dostoast_w[k], h = dostoast_h, ox = (320 - w) / 2, oy = 6, mul = g_dtoast < 20 ? g_dtoast * 255 / 20 : 255;
        const uint8_t *a = dostoast_px[k]; u8 *bf = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);
        for (int y = oy - 3; y < oy + h + 3; y++) for (int x = ox - 8; x < ox + w + 8; x++) { if (x < 0 || x >= 320 || y < 0 || y >= 240) continue;
            u8 *p = bf + (x * 240 + (239 - y)) * 3; int d = 255 - mul * 3 / 4; p[0] = p[0] * d / 255; p[1] = p[1] * d / 255; p[2] = p[2] * d / 255; }
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { int v = a[y * w + x] * mul / 255; if (!v) continue;
            u8 *p = bf + ((ox + x) * 240 + (239 - (oy + y))) * 3; p[0] = (p[0] * (255 - v) + 80 * v) / 255; p[1] = (p[1] * (255 - v) + 220 * v) / 255; p[2] = (p[2] * (255 - v) + 255 * v) / 255; } }
    if (g_toast > 0) { g_toast--; extern const int toast_w, toast_h; extern const uint8_t toast_on[], toast_off[]; const uint8_t *t = g_tilt_on ? toast_on : toast_off;
        u8 *bf = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL); int ox = (320 - toast_w) / 2, oy = 8, mul = g_toast < 20 ? g_toast * 255 / 20 : 255;
        for (int y = 0; y < toast_h; y++) for (int x = 0; x < toast_w; x++) { const uint8_t *q = t + (y * toast_w + x) * 4; int a = q[3] * mul / 255; if (!a) continue;
            u8 *p = bf + ((ox + x) * 240 + (239 - (oy + y))) * 3; p[0] = (p[0] * (255 - a) + q[2] * a) / 255; p[1] = (p[1] * (255 - a) + q[1] * a) / 255; p[2] = (p[2] * (255 - a) + q[0] * a) / 255; } }
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
    mkdir("sdmc:/3ds", 0777);
    { struct stat st; if (stat("sdmc:/3ds/NonomuraHospitalHD", &st) != 0 && stat("sdmc:/3ds/nonoxp", &st) == 0)   /* the folder's old name */
          rename("sdmc:/3ds/nonoxp", "sdmc:/3ds/NonomuraHospitalHD");
      if (stat("sdmc:/3ds/NonomuraHospitalHD/save", &st) != 0 && stat("sdmc:/3ds/nonoxp/save", &st) == 0)
          { mkdir("sdmc:/3ds/NonomuraHospitalHD", 0777); rename("sdmc:/3ds/nonoxp/save", "sdmc:/3ds/NonomuraHospitalHD/save"); } }
    mkdir(DATA_DIR, 0777);
    if (R_SUCCEEDED(romfsInit())) { FILE *t = fopen("romfs:/MES.ARC", "rb"); if (t) { fclose(t); g_romfs = true; } }
    void snd_init(void), snd_exit(void); snd_init();
    static vm_t vm; g_vm = &vm;
    if (!splash()) { snd_exit(); gfxExit(); return 0; }
    if (!vm_init(&vm)) fatal("Archives not found (MES.ARC DATA.ARC BG.ARC).");
    else if (!vm_load_mes(&vm, "MAIN.MES")) fatal("MAIN.MES missing in MES.ARC.");
    else vm_run(&vm);
    snd_exit(); if (g_romfs) romfsExit(); gfxExit(); return 0;
}
