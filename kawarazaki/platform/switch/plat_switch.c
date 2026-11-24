/* 河原崎家の一族 - Nintendo Switch platform layer (libnx + SDL2).
 * Screen  : the 640x480 picture scaled to 960x720 in the middle of the 1280x720 screen.
 * Controls: A confirm, B cancel, R/ZR (hold) skip, D-pad / left stick move, right stick pointer, touch = point and tap.
 * Files   : sdmc:/switch/Kawarazaki/ first (single-file fixes), else the installed title's RomFS. Saves and log on the SD card. */
#include <switch.h>
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <sys/stat.h>
#include "../../src/kai.h"

#define DATA_DIR "sdmc:/switch/Kawarazaki/"
static bool g_romfs;
static SDL_Window *g_win; static SDL_Renderer *g_ren; static SDL_Texture *g_tex; static SDL_Joystick *g_joy;

static FILE *open_data(const char *name) {
    char p[160]; snprintf(p, sizeof p, DATA_DIR "%s", name); FILE *f = fopen(p, "rb"); if (f || !g_romfs) return f;
    snprintf(p, sizeof p, "romfs:/%s", name); return fopen(p, "rb");
}
static struct { char name[40]; FILE *f; } g_open[12];
static FILE *get_file(const char *name) {
    for (int i = 0; i < 12; i++) if (g_open[i].f && !strcmp(g_open[i].name, name)) return g_open[i].f;
    FILE *f = open_data(name); if (!f) return NULL;
    for (int i = 0; i < 12; i++) if (!g_open[i].f) { snprintf(g_open[i].name, 40, "%s", name); g_open[i].f = f; return f; }
    fclose(g_open[0].f); snprintf(g_open[0].name, 40, "%s", name); g_open[0].f = f; return f;
}
bool plat_read_at(const char *name, uint32_t off, void *dst, uint32_t len) {
    FILE *f = get_file(name); if (!f) return false; if (fseek(f, off, SEEK_SET)) return false; return fread(dst, 1, len, f) == len; }
uint32_t plat_file_size(const char *name) { FILE *f = get_file(name); if (!f) return 0; fseek(f, 0, SEEK_END); return (uint32_t)ftell(f); }
void plat_log(const char *fmt, ...) {
    static int n; if (++n > 400) return; FILE *f = fopen(DATA_DIR "log.txt", n == 1 ? "w" : "a"); if (!f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap); fputc('\n', f); fclose(f);
}
bool plat_save_read(const char *name, void *buf, size_t len) {
    char p[160]; snprintf(p, sizeof p, DATA_DIR "save/%s", name); FILE *f = fopen(p, "rb"); if (!f) return false;
    bool ok = fread(buf, 1, len, f) == len; fclose(f); return ok; }
bool plat_save_write(const char *name, const void *buf, size_t len) {
    mkdir(DATA_DIR "save", 0777);
    char p[160]; snprintf(p, sizeof p, DATA_DIR "save/%s", name); FILE *f = fopen(p, "wb"); if (!f) return false;
    bool ok = fwrite(buf, 1, len, f) == len; fclose(f); return ok; }
uint32_t plat_ms(void) { return SDL_GetTicks(); }

/* ---- input ---- */
enum { JA = 0, JB = 1, JX = 2, JY = 3, JL = 6, JR = 7, JZL = 8, JZR = 9, JPLUS = 10, JMINUS = 11, JLEFT = 12, JUP = 13, JRIGHT = 14, JDOWN = 15 };
static bool g_btn[32]; static int g_ax[4];
static int g_cx = 320, g_cy = 240; static float g_fx = 320, g_fy = 240; static uint32_t g_cur_shown;
static bool g_touch; static int g_click; static bool g_quit;
#define DX 160
#define DW 960
static void to_game(float nx, float ny) {
    float x = (nx * 1280 - DX) * 640 / DW, y = ny * 480;
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
    int rx = g_ax[2], ry = g_ax[3];
    if (abs(rx) > 6000 || abs(ry) > 6000) {
        float sx = rx / 32768.0f, sy = ry / 32768.0f; g_fx += sx * fabsf(sx) * 9; g_fy += sy * fabsf(sy) * 9;
        if (g_fx < 0) g_fx = 0; if (g_fx > 639) g_fx = 639; if (g_fy < 0) g_fy = 0; if (g_fy > 479) g_fy = 479;
        g_cx = (int)g_fx; g_cy = (int)g_fy; g_cur_shown = SDL_GetTicks();
    }
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
    }
    return false;
}

/* ---- picture ---- */
static uint8_t g_fb[640 * 480 * 3];
static void draw_pointer(uint8_t *fb) {
    if (!g_cur_shown || SDL_GetTicks() - g_cur_shown > 2500) return;
    static const char *shape[] = { "X", "XX", "X.X", "X..X", "X...X", "X....X", "X.....X", "X..XXXX", "X.X", "XX", "X" };
    for (int j = 0; j < 11; j++) for (int i = 0; shape[j][i]; i++) { int x = g_cx + i, y = g_cy + j; if (x < 0 || y < 0 || x >= 640 || y >= 480) continue;
        uint8_t *p = fb + (y * 640 + x) * 3, v = shape[j][i] == 'X' ? 0 : 255; p[0] = p[1] = p[2] = v; }
}
bool plat_frame(const uint8_t *rgb, int w, int h) {
    (void)w; (void)h;
    if (!appletMainLoop() || g_quit) return false;
    pump();
    memcpy(g_fb, rgb, sizeof g_fb); draw_pointer(g_fb);
    SDL_UpdateTexture(g_tex, NULL, g_fb, 640 * 3);
    SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 255); SDL_RenderClear(g_ren);
    SDL_Rect dst = { DX, 0, DW, 720 }; SDL_RenderCopy(g_ren, g_tex, NULL, &dst);
    SDL_RenderPresent(g_ren);
    return true;
}

static void fatal(const char *msg) {
    plat_log("%s", msg);
    consoleInit(NULL); printf("\n  %s\n\n  Press + to exit.\n", msg);
    PadState pad; padConfigureInput(1, HidNpadStyleSet_NpadStandard); padInitializeDefault(&pad);
    while (appletMainLoop()) { padUpdate(&pad); if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break; consoleUpdate(NULL); }
    consoleExit(NULL);
}
void snd_init(void); void snd_exit(void);
int main(int argc, char **argv) {
    (void)argc; (void)argv;
    mkdir("sdmc:/switch", 0777); mkdir(DATA_DIR, 0777);
    if (R_SUCCEEDED(romfsInit())) { FILE *t = fopen("romfs:/mes.ARC", "rb"); if (t) { fclose(t); g_romfs = true; } }
    FILE *chk = open_data("mes.ARC"); if (!chk) { fatal("Game files not found (mes.ARC gcc.ARC ...)."); return 0; } fclose(chk);
    plat_log("start: romfs %d", g_romfs);
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK) < 0) { fatal(SDL_GetError()); return 0; }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    g_win = SDL_CreateWindow("Kawarazaki", 0, 0, 1280, 720, SDL_WINDOW_FULLSCREEN);
    g_ren = g_win ? SDL_CreateRenderer(g_win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC) : NULL;
    if (!g_ren && g_win) { SDL_DestroyWindow(g_win); g_win = SDL_CreateWindow("Kawarazaki", 0, 0, 1280, 720, SDL_WINDOW_FULLSCREEN); if (g_win) g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_SOFTWARE); }
    g_tex = g_ren ? SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, 640, 480) : NULL;
    if (!g_tex) { fatal("Video init failed"); return 0; }
    g_joy = SDL_JoystickOpen(0);
    snd_init();
    if (!k_init()) { fatal("Cannot open the game archives."); return 0; }
    k_run("startup.mes");
    plat_log("game ended");
    snd_exit();
    svcExitProcess();
    return 0;
}
