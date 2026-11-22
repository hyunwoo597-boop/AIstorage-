/* Nintendo Switch platform layer (libnx + SDL2).
 * Screen   : the original PC picture (640x480, text window and menus drawn by the engine) scaled to 960x720 in the
 *            middle of the 1280x720 screen. Title menu / confirmation dialogs / message log: the engine's 320x240
 *            selection picture shown at 2x over the dimmed scene (same layout and touch areas as on the 3DS).
 * Controls : A = confirm, B = cancel, + = system menu, L = message log, R/ZR (hold) = skip, D-pad / left stick = move,
 *            right stick = pointer, touch screen = point and tap.
 * Files    : sdmc:/switch/nonoxp/ (MES.ARC DATA.ARC BG.ARC font16.bin SOUND.ARC/MUSIC.ARC VOICE.ARC DEPTH.PAK patch/),
 *            else the NRO/NSP RomFS ("patch/x" stored as "patch_x"). Saves and log always on the SD card. */
#include <switch.h>
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <dirent.h>
#include <time.h>
#include <math.h>
#include <errno.h>
#include <sys/stat.h>
#include "ai5.h"

#define DATA_DIR "sdmc:/switch/nonoxp/"
static vm_t *g_vm;
static bool g_romfs;
static SDL_Window *g_win; static SDL_Renderer *g_ren; static SDL_Texture *g_tex; static SDL_Joystick *g_joy;

/* ---- files ---- */
static FILE *open_data(const char *name) {
    char p[160]; snprintf(p, sizeof p, DATA_DIR "%s", name); FILE *f = fopen(p, "rb"); if (f || !g_romfs) return f;
    snprintf(p, sizeof p, "romfs:/%s", name); for (char *c = p + 7; *c; c++) if (*c == '/') *c = '_';
    return fopen(p, "rb");
}
static struct { char name[24]; FILE *f; } g_open[8];
static FILE *get_file(const char *name) {
    for (int i = 0; i < 8; i++) if (g_open[i].f && !strcmp(g_open[i].name, name)) return g_open[i].f;
    FILE *f = open_data(name); if (!f) return NULL;
    for (int i = 0; i < 8; i++) if (!g_open[i].f) { strncpy(g_open[i].name, name, 23); g_open[i].f = f; return f; }
    fclose(g_open[0].f); strncpy(g_open[0].name, name, 23); g_open[0].f = f; return f;
}
bool plat_read_at(const char *name, uint32_t off, void *dst, uint32_t len) {
    FILE *f = get_file(name); if (!f) return false; if (fseek(f, off, SEEK_SET)) return false; return fread(dst, 1, len, f) == len; }
uint32_t plat_file_size(const char *name) { FILE *f = get_file(name); if (!f) return 0; fseek(f, 0, SEEK_END); return (uint32_t)ftell(f); }
void *plat_read_file(const char *name, size_t *len) { uint32_t n = plat_file_size(name); if (!n) return NULL; void *b = malloc(n + 1);
    if (!b || !plat_read_at(name, 0, b, n)) { free(b); return NULL; } if (len) *len = n; return b; }
int plat_list(const char *dir, char names[][24], int max) {
    char p[160]; snprintf(p, sizeof p, DATA_DIR "%s", dir); DIR *d = opendir(p); int n = 0; struct dirent *e;
    if (d) { while ((e = readdir(d)) && n < max) { if (e->d_name[0] == '.' || strlen(e->d_name) > 23) continue; strcpy(names[n++], e->d_name); } closedir(d); }
    if (g_romfs && (d = opendir("romfs:/"))) { size_t pl = strlen(dir);
        while ((e = readdir(d)) && n < max) { if (strncmp(e->d_name, dir, pl) || e->d_name[pl] != '_') continue; const char *nm = e->d_name + pl + 1;
            bool dup = false; for (int i = 0; i < n; i++) if (!strcasecmp(names[i], nm)) dup = true; if (!dup && strlen(nm) <= 23) strcpy(names[n++], nm); }
        closedir(d); }
    return n; }
void plat_log(const char *fmt, ...) {
    static int n; if (++n > 300) return; FILE *f = fopen(DATA_DIR "log.txt", n == 1 ? "w" : "a"); if (!f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap); fputc('\n', f); fclose(f);
}
bool plat_save_read(const char *name, void *buf, size_t len) {
    char p[160]; snprintf(p, sizeof p, DATA_DIR "save/%s", name); FILE *f = fopen(p, "rb"); if (!f) return false;
    bool ok = fread(buf, 1, len, f) == len; fclose(f); return ok; }
bool plat_save_write(const char *name, const void *buf, size_t len) {
    mkdir(DATA_DIR "save", 0777);
    char p[160]; snprintf(p, sizeof p, DATA_DIR "save/%s", name); FILE *f = fopen(p, "wb"); if (!f) { plat_log("cannot write %s", p); return false; }
    bool ok = fwrite(buf, 1, len, f) == len; fclose(f); return ok; }
uint32_t plat_ms(void) { return SDL_GetTicks(); }
void plat_datetime(int o[6]) { time_t t = time(NULL); struct tm *m = localtime(&t); o[0] = m->tm_year + 1900; o[1] = m->tm_mon + 1; o[2] = m->tm_mday; o[3] = m->tm_hour; o[4] = m->tm_min; o[5] = m->tm_sec; }
const char *plat_goto_override(void) { return NULL; }
bool plat_wants_composite(void) { return true; }

/* ---- input ---- */
enum { JA = 0, JB = 1, JX = 2, JY = 3, JL = 6, JR = 7, JZL = 8, JZR = 9, JPLUS = 10, JMINUS = 11, JLEFT = 12, JUP = 13, JRIGHT = 14, JDOWN = 15 };
static bool g_btn[32]; static int g_ax[4];
static int g_cx = 320, g_cy = 240; static float g_fx = 320, g_fy = 240; static uint32_t g_cur_shown;
static bool g_touch; static int g_click;            /* touch: the pointer follows the finger, lifting it clicks (like the 3DS menus) */
static bool g_quit;
#define DX 160
#define DW 960
static void to_game(float nx, float ny) {             /* touch (0..1 of 1280x720) -> 640x480 */
    float x = (nx * 1280 - DX) * 640 / DW, y = ny * 720 * 480 / 720;
    g_fx = x < 0 ? 0 : x > 639 ? 639 : x; g_fy = y < 0 ? 0 : y > 479 ? 479 : y; g_cx = (int)g_fx; g_cy = (int)g_fy;
}
static void pump(void) {
    if (g_click > 0) g_click--;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT: g_quit = true; break;
        case SDL_JOYBUTTONDOWN: if (e.jbutton.button < 32) g_btn[e.jbutton.button] = true; break;
        case SDL_JOYBUTTONUP: if (e.jbutton.button < 32) g_btn[e.jbutton.button] = false; break;
        case SDL_JOYAXISMOTION: if (e.jaxis.axis < 4) g_ax[e.jaxis.axis] = e.jaxis.value; break;
        case SDL_FINGERDOWN: g_touch = true; to_game(e.tfinger.x, e.tfinger.y); break;
        case SDL_FINGERMOTION: to_game(e.tfinger.x, e.tfinger.y); break;
        case SDL_FINGERUP: to_game(e.tfinger.x, e.tfinger.y); g_touch = false; g_click = 2; break;
        }
    }
    int rx = g_ax[2], ry = g_ax[3];                     /* right stick moves the pointer */
    if (abs(rx) > 6000 || abs(ry) > 6000) {
        float sx = rx / 32768.0f, sy = ry / 32768.0f; g_fx += sx * fabsf(sx) * 9; g_fy += sy * fabsf(sy) * 9;
        if (g_fx < 0) g_fx = 0; if (g_fx > 639) g_fx = 639; if (g_fy < 0) g_fy = 0; if (g_fy > 479) g_fy = 479;
        g_cx = (int)g_fx; g_cy = (int)g_fy; g_cur_shown = SDL_GetTicks();
    }
}
/* - button: Windows pictures <-> DOS release pictures (needs SILK0..4.DAT of the DOS release in the data folder) */
static uint32_t g_toast_until; static int g_toast_kind;   /* 1 DOS on, 2 Windows, 3 no DOS files */
static void minus_toggle(void) {
    static bool was; bool now = g_btn[JMINUS];
    if (now && !was) {
        int want = !sys_dos_gfx(), got = sys_set_dos_gfx(g_vm, want);
        g_toast_kind = want && !got ? 3 : got ? 1 : 2; g_toast_until = SDL_GetTicks() + 2000;
        FILE *f = fopen(DATA_DIR "dosgfx.cfg", "w"); if (f) { fputc(got ? '1' : '0', f); fclose(f); }
    }
    was = now;
}
void plat_poll(void) { pump(); }
void plat_cursor(int *x, int *y, bool set) { if (set) { g_cx = *x; g_cy = *y; g_fx = *x; g_fy = *y; } else { *x = g_cx; *y = g_cy; } }
bool plat_key(int key) {
    const int dz = 16000;
    switch (key) {
    case 0: return g_btn[JA] || g_click > 0;
    case 1: return g_btn[JB] || g_btn[JPLUS];
    case 2: return g_btn[JUP] || g_ax[1] < -dz;
    case 3: return g_btn[JDOWN] || g_ax[1] > dz;
    case 4: return g_btn[JLEFT] || g_ax[0] < -dz;
    case 5: return g_btn[JRIGHT] || g_ax[0] > dz;
    case 6: return g_btn[JR] || g_btn[JZR];
    case 9: return g_btn[JL];
    }
    return false;
}

/* ---- picture ---- */
static uint8_t g_frame_buf[640 * 480 * 3];
static void glyph(uint8_t *fb, int x, int y, uint32_t idx, int w, int r, int g, int b) {   /* 320x240 RGB buffer */
    uint32_t cells; const uint8_t *font = sys_font(&cells); if (!font || idx >= cells) return; const uint8_t *gl = font + idx * 128;
    for (int j = 0; j < 16; j++) for (int i = 0; i < w; i++) { int tx = x + i, ty = y + j; if (tx < 0 || tx >= 320 || ty < 0 || ty >= 240) continue;
        int a = (gl[(j * 16 + i) >> 1] >> ((i & 1) ? 0 : 4)) & 15; if (!a) continue;
        uint8_t *p = fb + (ty * 320 + tx) * 3; p[0] = (p[0] * (15 - a) + r * a) / 15; p[1] = (p[1] * (15 - a) + g * a) / 15; p[2] = (p[2] * (15 - a) + b * a) / 15; }
}
static void backlog_page(uint8_t *fb, const uint8_t *scene) {   /* message log: same 320x240 text page as on the 3DS bottom screen */
    for (int y = 0; y < 240; y++) for (int x = 0; x < 320; x++) { const uint8_t *a = scene + ((y * 2) * 640 + x * 2) * 3; uint8_t *d = fb + (y * 320 + x) * 3; d[0] = a[0] / 5; d[1] = a[1] / 5; d[2] = a[2] / 5; }
    const ui_text *u = sys_ui_text(g_vm); int row = 0, x = 0;
    for (int l = 0; l < u->nlines && row < 13; l++) {
        const uint8_t *p = (const uint8_t *)u->line[l].sjis; int item = u->line[l].item, hot = item >= 0 && item == u->hover;
        if (hot) for (int yy = 6 + row * 18; yy < 6 + row * 18 + 18 && yy < 240; yy++) for (int xx = 0; xx < 320; xx++) { uint8_t *d = fb + (yy * 320 + xx) * 3; d[0] = 0xE2; d[1] = 0; d[2] = 0x7F; }
        while (*p && row < 13) { int half; uint32_t idx = sys_glyph_index(&p, &half); int w = half ? 8 : 16;
            if (idx == 95 && (sys_font_flags() & 1)) { if (x) x += 8; continue; }
            if (x + w > 320) { x = 0; row++; if (row >= 13) break; }
            glyph(fb, x, 8 + row * 18, idx, w, 255, 255, 255); x += w; }
        if (item >= 0 || strlen(u->line[l].sjis) < 72) { x = 0; row++; }
    }
}
static void up2(uint8_t *dst, const uint8_t *src) {   /* 320x240 -> 640x480 */
    for (int y = 0; y < 480; y++) { const uint8_t *s = src + (y / 2) * 320 * 3; uint8_t *d = dst + y * 640 * 3;
        for (int x = 0; x < 640; x++) memcpy(d + x * 3, s + (x / 2) * 3, 3); }
}
static void draw_pointer(uint8_t *fb) {               /* small arrow while the right stick is in use */
    if (SDL_GetTicks() - g_cur_shown > 2500 || !g_cur_shown) return;
    static const char *shape[] = { "X", "XX", "X.X", "X..X", "X...X", "X....X", "X.....X", "X..XXXX", "X.X", "XX", "X" };
    for (int j = 0; j < 11; j++) for (int i = 0; shape[j][i]; i++) { int x = g_cx + i, y = g_cy + j; if (x < 0 || y < 0 || x >= 640 || y >= 480) continue;
        uint8_t *p = fb + (y * 640 + x) * 3, v = shape[j][i] == 'X' ? 0 : 255; if (shape[j][i] == ' ') continue; p[0] = p[1] = p[2] = v; }
}
bool plat_frame(const uint8_t *rgb, int w, int h) {
    (void)w; (void)h;
    if (!appletMainLoop() || g_quit) return false;
    pump();
    const ui_text *u = sys_ui_text(g_vm);
    if (sys_popup_active()) { const uint8_t *pp = sys_popup_full_rgb(); if (pp) memcpy(g_frame_buf, pp, sizeof g_frame_buf); else up2(g_frame_buf, sys_bottom_rgb()); }
    else if (u->backlog) { static uint8_t page[320 * 240 * 3]; backlog_page(page, sys_scene_rgb()); up2(g_frame_buf, page); }
    else memcpy(g_frame_buf, rgb, sizeof g_frame_buf);
    minus_toggle();
    { static bool restored; if (!restored) { restored = true; FILE *f = fopen(DATA_DIR "dosgfx.cfg", "r"); if (f) { if (fgetc(f) == '1') sys_set_dos_gfx(g_vm, 1); fclose(f); } } }
    if (SDL_GetTicks() < g_toast_until) {        /* a small label at the top left for 2 s */
        extern const int swtoast_h, swtoast_w[3]; extern const uint8_t *const swtoast_px[3];   /* tools/mkswtoast.py */
        int k = g_toast_kind - 1, w = swtoast_w[k], h = swtoast_h; const uint8_t *a = swtoast_px[k];
        for (int y = 8; y < 8 + h + 4 && y < 480; y++) for (int x = 8; x < 8 + w + 12 && x < 640; x++) { uint8_t *p = g_frame_buf + (y * 640 + x) * 3; p[0] = p[0] / 4; p[1] = p[1] / 4; p[2] = p[2] / 4; }
        for (int y = 0; y < h; y++) for (int x = 0; x < w && 14 + x < 640; x++) { int v = a[y * w + x]; if (!v) continue;
            uint8_t *d = g_frame_buf + ((10 + y) * 640 + 14 + x) * 3; d[0] = (d[0] * (255 - v) + 255 * v) / 255; d[1] = (d[1] * (255 - v) + 220 * v) / 255; d[2] = (d[2] * (255 - v) + 80 * v) / 255; }
    }
    draw_pointer(g_frame_buf);
    SDL_UpdateTexture(g_tex, NULL, g_frame_buf, 640 * 3);
    SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 255); SDL_RenderClear(g_ren);
    SDL_Rect dst = { DX, 0, DW, 720 }; SDL_RenderCopy(g_ren, g_tex, NULL, &dst);
    SDL_RenderPresent(g_ren);
    return true;
}

static void fatal(const char *msg) {
    plat_log("%s", msg);
    consoleInit(NULL); printf("\n  %s\n\n  Put the game files in sdmc:/switch/nonoxp/\n  (or use the version with the files inside).\n\n  Press + to exit.\n", msg);
    PadState pad; padConfigureInput(1, HidNpadStyleSet_NpadStandard); padInitializeDefault(&pad);
    while (appletMainLoop()) { padUpdate(&pad); if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break; consoleUpdate(NULL); }
    consoleExit(NULL);
}
void snd_init(void); void snd_exit(void);
/* start-up title picture (tools/mkswsplash.py), "A 버튼으로 시작" blinking at the bottom; A, + or a touch goes on */
extern const size_t swsplash_raw_size, swsplash_lz_size; extern const uint8_t swsplash_lz[];
extern const int swprompt_w, swprompt_h; extern const uint8_t swprompt_rgba[];
static bool splash(void) {
    uint8_t *raw = malloc(swsplash_raw_size + 64); if (!raw) return true;
    if (lzss_decode(swsplash_lz, swsplash_lz_size, raw, swsplash_raw_size) < swsplash_raw_size) { free(raw); return true; }
    SDL_Texture *pic = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STATIC, 960, 720);
    SDL_Texture *pr = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, swprompt_w, swprompt_h);
    if (pic) SDL_UpdateTexture(pic, NULL, raw, 960 * 3);
    if (pr) { SDL_UpdateTexture(pr, NULL, swprompt_rgba, swprompt_w * 4); SDL_SetTextureBlendMode(pr, SDL_BLENDMODE_BLEND); }
    free(raw);
    bool go = true; uint32_t t0 = SDL_GetTicks();
    for (;;) {
        if (!appletMainLoop()) { go = false; break; }
        pump(); if (g_quit) { go = false; break; }
        if (SDL_GetTicks() - t0 > 400 && (g_btn[JA] || g_btn[JPLUS] || g_click > 0)) break;
        SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 255); SDL_RenderClear(g_ren);
        SDL_Rect d = { DX, 0, DW, 720 }; if (pic) SDL_RenderCopy(g_ren, pic, NULL, &d);
        uint32_t ph = (SDL_GetTicks() - t0) % 1200;
        if (pr && ph < 850) { SDL_Rect q = { (1280 - swprompt_w) / 2, 720 - swprompt_h - 24, swprompt_w, swprompt_h }; SDL_RenderCopy(g_ren, pr, NULL, &q); }
        SDL_RenderPresent(g_ren);
    }
    while (go && appletMainLoop() && (g_btn[JA] || g_btn[JPLUS] || g_touch)) { pump(); SDL_Delay(10); }   /* the press does not reach the game */
    g_click = 0;
    if (pic) SDL_DestroyTexture(pic); if (pr) SDL_DestroyTexture(pr);
    return go;
}
/* libnx start-up done here (instead of libnx's default __appInit) so a failure is written to sdmc:/switch/nonoxp/boot.txt
   instead of an abort with nothing left behind: the installed (NSP) build closed before main() */
extern void __libnx_init_time(void) __attribute__((weak));
static Result g_boot_rc[8];
static void boot_note(const char *what, Result rc) {
    FILE *f = fopen(DATA_DIR "boot.txt", "a"); if (f) { fprintf(f, "%s: 0x%x (%04d-%04d)\n", what, rc, 2000 + R_MODULE(rc), R_DESCRIPTION(rc)); fclose(f); }
}
void __appInit(void) {
    Result rc = smInitialize(); if (R_FAILED(rc)) diagAbortWithResult(rc);
    if (R_SUCCEEDED(setsysInitialize())) { SetSysFirmwareVersion fw; if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw))) hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro)); setsysExit(); }
    g_boot_rc[0] = fsInitialize(); if (R_SUCCEEDED(g_boot_rc[0])) fsdevMountSdmc();
    mkdir("sdmc:/switch", 0777); mkdir(DATA_DIR, 0777); remove(DATA_DIR "boot.txt");
    boot_note("fs", g_boot_rc[0]);
    g_boot_rc[1] = appletInitialize(); boot_note("applet", g_boot_rc[1]);
    g_boot_rc[2] = hidInitialize(); boot_note("hid", g_boot_rc[2]);
    g_boot_rc[3] = timeInitialize(); boot_note("time", g_boot_rc[3]); if (R_SUCCEEDED(g_boot_rc[3]) && __libnx_init_time) __libnx_init_time();
    boot_note("init done", 0);
}
void __appExit(void) { timeExit(); hidExit(); appletExit(); fsdevUnmountAll(); fsExit(); smExit(); }
int main(int argc, char **argv) {
    (void)argc; (void)argv;
    boot_note("main", 0);
    mkdir("sdmc:/switch", 0777); mkdir(DATA_DIR, 0777);
    Result rfs = romfsInit(); int rfe = 0;
    if (R_SUCCEEDED(rfs)) { FILE *t = fopen("romfs:/MES.ARC", "rb"); if (t) { fclose(t); g_romfs = true; } else rfe = errno; }
    static vm_t vm; g_vm = &vm;
    FILE *chk = open_data("MES.ARC"); if (!chk) { fatal("Game files not found (MES.ARC DATA.ARC BG.ARC)."); if (g_romfs) romfsExit(); return 0; } fclose(chk);
    plat_log("start: %s, romfs %d (romfsInit 0x%x, MES.ARC errno %d), SD MES.ARC %s", appletGetAppletType() == AppletType_Application ? "application" : "applet", g_romfs, rfs, rfe, ({ FILE *q = fopen(DATA_DIR "MES.ARC", "rb"); if (q) fclose(q); q ? "yes" : "no"; }));
    if (R_SUCCEEDED(rfs)) { DIR *rd = opendir("romfs:/"); struct dirent *de; int k = 0; if (rd) { while ((de = readdir(rd)) && k < 12) { plat_log("  romfs: %s", de->d_name); k++; } closedir(rd); } else plat_log("  romfs: cannot list (errno %d)", errno); }
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK) < 0) { char m[200]; snprintf(m, sizeof m, "SDL_Init failed: %s", SDL_GetError()); fatal(m); if (g_romfs) romfsExit(); return 0; }
    plat_log("SDL ok");
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    g_win = SDL_CreateWindow("nonoxp", 0, 0, 1280, 720, SDL_WINDOW_FULLSCREEN);
    g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    g_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, 640, 480);
    g_joy = SDL_JoystickOpen(0);
    if (!g_win || !g_ren || !g_tex) { char m[200]; snprintf(m, sizeof m, "Video init failed: %s", SDL_GetError()); SDL_Quit(); fatal(m); if (g_romfs) romfsExit(); return 0; }
    plat_log("video ok");
    if (!splash()) { plat_log("splash: quit"); if (g_joy) SDL_JoystickClose(g_joy); SDL_DestroyTexture(g_tex); SDL_DestroyRenderer(g_ren); SDL_DestroyWindow(g_win); SDL_Quit(); if (g_romfs) romfsExit(); return 0; }
    snd_init();
    plat_log("splash done");
    if (!vm_init(&vm)) { plat_log("Archives not found"); snd_exit(); SDL_DestroyTexture(g_tex); SDL_DestroyRenderer(g_ren); SDL_DestroyWindow(g_win); SDL_Quit(); g_tex = NULL;
        fatal(g_romfs ? "Cannot open the game archives inside the NSP (MES.ARC DATA.ARC BG.ARC)." : "Cannot open the game archives (MES.ARC DATA.ARC BG.ARC)."); if (g_romfs) romfsExit(); return 0; }
    plat_log("archives ok");
    if (sys_set_popup_full(true), vm_load_mes(&vm, "MAIN.MES")) vm_run(&vm); else plat_log("MAIN.MES missing");
    plat_log("game ended (quit=%d)", vm.quit);
    snd_exit();
    if (g_joy) SDL_JoystickClose(g_joy);
    SDL_DestroyTexture(g_tex); SDL_DestroyRenderer(g_ren); SDL_DestroyWindow(g_win); SDL_Quit();
    if (g_romfs) romfsExit();
    return 0;
}
