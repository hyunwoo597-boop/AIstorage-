/* Nonomura Hospital - native Nintendo 3DS frontend.
 * Runs the original (Korean-patched) DOS game on a built-in x86 emulator.
 * Top screen: the game picture (640x480 box-filtered to 320x240). Bottom screen: the dialogue text, re-laid out in large type. */
#include <3ds.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "../../core/machine.h"
#include "../../core/textcap.h"
#include "vgascale.h"
#include "font8x8.h"

u32 __stacksize__ = 0x40000;

#define APP_VERSION "0.1"
#define TICKS_PER_SEC ((u64)SYSCLOCK_ARM11)
#define FRAME_TICKS (TICKS_PER_SEC / 60)
#define C565(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))

/* ------------------------------------------------------------------ config */
#ifndef GAME_DIR0
#define GAME_DIR0 "sdmc:/3ds/nonomura"
#endif
static const char *GAME_DIRS[] = { GAME_DIR0, "sdmc:/nonomura", NULL };
#define IPS_MIN 1200000u
#define IPS_MAX 4000000u      /* Auto never runs the emulated CPU faster than the nominal speed: iteration-based delay loops would speed the game up */
static const uint32_t CPU_FIXED[] = { 0, 2000000, 3000000, 4000000, 5000000, 6000000 };   /* 0 = auto */
#define N_CPU_MODES 6
static int cpu_mode = 0, mouse_level = 3;
static int win_hide = 1;              /* 1: while the engine's message window is up, the top screen shows only the picture (the text is on the bottom screen) */
static int win_state, win_cand, win_cnt, win_force;   /* current view, candidate view, frames the candidate has held, redraw request */
static int top_zoom = 1;              /* 1: 640x400 game area stretched over the whole top screen, 0: original 4:3 picture */
static int text_size = 1;             /* bottom-screen text: 0 large, 1 medium, 2 small, 3 tiny */
static const float TEXT_SCALE[] = { 2.0f, 1.5f, 1.25f, 1.0f };
static const char *TEXT_NAME[] = { "Large", "Medium", "Small", "Tiny" };
static char cfg_path[320];
static void cfg_save(void) {
    FILE *f = fopen(cfg_path, "w"); if (!f) return;
    fprintf(f, "cpu_mode=%d\nmouse_level=%d\ntop_zoom=%d\ntext_size=%d\nwin_hide=%d\n", cpu_mode, mouse_level, top_zoom, text_size, win_hide);
    fclose(f);
}
static void cfg_load(void) {
    FILE *f = fopen(cfg_path, "r"); if (!f) return;
    char line[64]; int v;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "cpu_mode=%d", &v) == 1 && v >= 0 && v < 6) cpu_mode = v;
        else if (sscanf(line, "mouse_level=%d", &v) == 1 && v >= 1 && v <= 5) mouse_level = v;
        else if (sscanf(line, "top_zoom=%d", &v) == 1) top_zoom = v ? 1 : 0;
        else if (sscanf(line, "win_hide=%d", &v) == 1) win_hide = v ? 1 : 0;
        else if (sscanf(line, "text_size=%d", &v) == 1 && v >= 0 && v < 4) text_size = v;
    }
    fclose(f);
}
static const float MOUSE_SPEED[] = { 0, 5.f, 8.f, 11.f, 15.f, 20.f };   /* pixels per frame at full circle-pad deflection */

/* ------------------------------------------------------------------ log */
static FILE *logfp;
static u64 t_start;
static void LOG(const char *fmt, ...) {
    if (!logfp) return;
    va_list ap; va_start(ap, fmt);
    fprintf(logfp, "[%6.1f] ", (double)(svcGetSystemTick() - t_start) / TICKS_PER_SEC);
    vfprintf(logfp, fmt, ap); fputc('\n', logfp); fflush(logfp);
    va_end(ap);
}

/* ------------------------------------------------------------------ tiny drawing helpers (column-major RGB565) */
static uint16_t *fbTop, *fbBot;
static void fb_get(void) {
    u16 w, h;
    fbTop = (uint16_t *)gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &w, &h);
    fbBot = (uint16_t *)gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &w, &h);
}
static void fill(uint16_t *fb, int fbw, uint16_t c) { for (int i = 0; i < fbw * 240; i++) fb[i] = c; }
static void gputc(uint16_t *fb, int fbw, int x, int y, int ch, uint16_t fg, int bg, int sc) {
    const unsigned char *g = &font8x8[(ch & 255) * 8];
    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 8; c++) {
            int on = g[r] & (0x80 >> c);
            if (!on && bg < 0) continue;
            uint16_t col = on ? fg : (uint16_t)bg;
            for (int sy = 0; sy < sc; sy++)
                for (int sx = 0; sx < sc; sx++) {
                    int px = x + c * sc + sx, py = y + r * sc + sy;
                    if (px < 0 || px >= fbw || py < 0 || py >= 240) continue;
                    fb[px * 240 + (239 - py)] = col;
                }
        }
}
/* draws s with '\n' support and word-agnostic wrapping at maxcols; returns the y after the last line */
static int gtext(uint16_t *fb, int fbw, int x, int y, int maxcols, const char *s, uint16_t fg, int bg, int sc) {
    int col = 0, x0 = x;
    for (; *s; s++) {
        if (*s == '\n' || col >= maxcols) { x = x0; y += 8 * sc + 2; col = 0; if (*s == '\n') continue; }
        gputc(fb, fbw, x, y, (unsigned char)*s, fg, bg, sc); x += 8 * sc; col++;
    }
    return y + 8 * sc + 2;
}
static void present(void) { gfxFlushBuffers(); gfxSwapBuffers(); gspWaitForVBlank(); }

/* message screen on both displays; waits for START (or B) */
static void message_screen(const char *title, const char *body) {
    while (aptMainLoop()) {
        fb_get();
        fill(fbTop, 400, C565(10, 10, 30));
        gtext(fbTop, 400, 8, 8, 23, title, C565(255, 220, 80), -1, 2);
        fill(fbBot, 320, C565(10, 10, 30));
        gtext(fbBot, 320, 6, 6, 38, body, C565(230, 230, 230), -1, 1);
        gtext(fbBot, 320, 6, 228, 38, "Press START to exit", C565(150, 200, 255), -1, 1);
        present();
        hidScanInput();
        if (hidKeysDown() & (KEY_START | KEY_B)) break;
    }
}

/* ------------------------------------------------------------------ audio */
#ifndef NDSP_SAMPLE_RATE
#define NDSP_SAMPLE_RATE 32728.498          /* the DSP's own rate: playing at exactly this rate needs no resampling on the DSP side */
#endif
#define AUD_RATE 32728                       /* synth output rate (integer; the DSP consumes 0.0015% faster, which the pump absorbs) */
#define NWB 8
#define AFRAMES 1024
#define PREBUF 4                      /* chunks queued before playback (re)starts: ~65 ms cushion against frame-time jitter */
static unsigned aud_underruns, aud_dropped;
static ndspWaveBuf wbuf[NWB];
static int16_t *abuf;
static int audio_ok;
static void audio_init(void) {
    if (R_FAILED(ndspInit())) { LOG("ndspInit failed (dspfirm.cdc missing?) - no audio"); return; }
    abuf = linearAlloc((size_t)NWB * AFRAMES * 4);
    if (!abuf) { ndspExit(); return; }
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnReset(0);
    ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
    ndspChnSetRate(0, (float)NDSP_SAMPLE_RATE);
    ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
    float mix[12] = { 0 }; mix[0] = mix[1] = 1.0f;
    ndspChnSetMix(0, mix);
    ndspChnSetPaused(0, true);
    memset(wbuf, 0, sizeof wbuf);
    for (int i = 0; i < NWB; i++) wbuf[i].data_pcm16 = abuf + (size_t)i * AFRAMES * 2;
    audio_ok = 1;
}
static int audio_started;
static float audio_rate = (float)NDSP_SAMPLE_RATE;
static int audio_queued(void) {
    int n = 0;
    for (int i = 0; i < NWB; i++) if (wbuf[i].status == NDSP_WBUF_QUEUED || wbuf[i].status == NDSP_WBUF_PLAYING) n++;
    return n;
}
/* playback speed follows the emulation speed, so a machine that cannot keep up sounds slow instead of crackling */
static void audio_set_speed(double ratio) {
    if (!audio_ok) return;
    if (ratio > 1.0) ratio = 1.0;
    if (ratio < 0.5) ratio = 0.5;
    float r = ratio >= 0.97 ? (float)NDSP_SAMPLE_RATE : (float)(NDSP_SAMPLE_RATE * ratio);
    if (r != audio_rate) { audio_rate = r; ndspChnSetRate(0, r); }
}
static void audio_pump(Machine *m) {
    opl_sync(m->opl);
    uint32_t n = opl_available(m->opl);
    if (!audio_ok) { opl_discard(m->opl, n); return; }
    /* never throw samples away just because every buffer is busy: they wait in the synth ring for the next frame.
     * Only a really big backlog (the game ran ahead by >0.2 s) is trimmed, keeping the newest 0.1 s. */
    if (n > AUD_RATE / 5) { opl_discard(m->opl, n - AUD_RATE / 10); n = AUD_RATE / 10; aud_dropped++; }
    while (n) {
        ndspWaveBuf *wb = NULL;
        for (int i = 0; i < NWB; i++)
            if (wbuf[i].status == NDSP_WBUF_FREE || wbuf[i].status == NDSP_WBUF_DONE) { wb = &wbuf[i]; break; }
        if (!wb) break;
        uint32_t c = n > AFRAMES ? AFRAMES : n;
        opl_mix(m->opl, wb->data_pcm16, c);
        DSP_FlushDataCache(wb->data_pcm16, c * 4);
        wb->nsamples = c;
        ndspChnWaveBufAdd(0, wb);
        n -= c;
    }
    int q = audio_queued();
    if (audio_started && q == 0) { ndspChnSetPaused(0, true); audio_started = 0; aud_underruns++; }   /* ran dry: rebuild the cushion */
    else if (!audio_started && q >= PREBUF) { ndspChnSetPaused(0, false); audio_started = 1; }
}

/* ------------------------------------------------------------------ input */
typedef struct { uint32_t key; uint8_t sc; uint8_t ascii; } KeyMap;
static const KeyMap KEYS[] = {
    { KEY_DUP, 0x48, 0 }, { KEY_DDOWN, 0x50, 0 }, { KEY_DLEFT, 0x4B, 0 }, { KEY_DRIGHT, 0x4D, 0 },
    { KEY_X, 0x1C, 13 },  /* Enter */
    { KEY_Y, 0x01, 27 },  /* Esc */
    { KEY_L, 0x1D, 0 },   /* Ctrl */
    { KEY_R, 0x39, ' ' }, /* Space */
};
static uint32_t prev_held;
static int key_age[sizeof KEYS / sizeof KEYS[0]];      /* frames since the key went down (-1: up) */
static uint32_t key_sent;                               /* bit i: key i is currently down in the emulated keyboard */
static float mx_acc, my_acc;
static int lb_frames, rb_frames;          /* remaining frames a click is held (min press length) */
static int touch_active, touch_x0, touch_y0, touch_lx, touch_ly, touch_age, touch_moved;

static void mouse_move(Machine *m, float dx, float dy) {
    mx_acc += dx; my_acc += dy;
    int ix = (int)mx_acc, iy = (int)my_acc;
    mx_acc -= ix; my_acc -= iy;
    m->mouse_x += ix; m->mouse_y += iy;
    if (m->mouse_x < m->mouse_minx) m->mouse_x = m->mouse_minx;
    if (m->mouse_x > m->mouse_maxx) m->mouse_x = m->mouse_maxx;
    if (m->mouse_y < m->mouse_miny) m->mouse_y = m->mouse_miny;
    if (m->mouse_y > m->mouse_maxy) m->mouse_y = m->mouse_maxy;
}

static void input_frame(Machine *m) {
    uint32_t held = hidKeysHeld();
    /* keyboard keys: press edges immediately; releases only after >= 4 frames so the game's tick-rate polling never misses a tap */
    for (unsigned i = 0; i < sizeof KEYS / sizeof KEYS[0]; i++) {
        uint32_t k = KEYS[i].key; uint32_t bit = 1u << i;
        if (key_sent & bit) key_age[i]++;
        if (held & k) {
            if (!(key_sent & bit)) { machine_key(m, KEYS[i].sc, 1, KEYS[i].ascii); key_sent |= bit; key_age[i] = 0; }
        } else if ((key_sent & bit) && key_age[i] >= 4) {
            machine_key(m, KEYS[i].sc, 0, KEYS[i].ascii); key_sent &= ~bit;
        }
    }
    /* mouse buttons with a minimum press length so a quick tap is never missed by the game's polling */
    if ((held & KEY_A) && !(prev_held & KEY_A)) lb_frames = 3;
    if ((held & KEY_B) && !(prev_held & KEY_B)) rb_frames = 3;
    /* circle pad -> mouse */
    circlePosition cp; hidCircleRead(&cp);
    float sp = MOUSE_SPEED[mouse_level];
    if (held & KEY_ZL) sp *= 0.3f;
    if (held & KEY_ZR) sp *= 2.0f;
    const float DZ = 16.f, MAXC = 150.f;
    float ax = (float)cp.dx, ay = (float)cp.dy;
    float mag = sqrtf(ax * ax + ay * ay);
    if (mag > DZ) {
        float n = (mag - DZ) / (MAXC - DZ); if (n > 1.f) n = 1.f;
        float v = (0.5f * n * n + 0.5f * n) * sp;
        mouse_move(m, ax / mag * v, -ay / mag * v);
    }
    /* touch screen = trackpad (drag moves, short tap clicks) */
    int left_touch = 0;
    if (held & KEY_TOUCH) {
        touchPosition tp; hidTouchRead(&tp);
        if (!touch_active) { touch_active = 1; touch_x0 = touch_lx = tp.px; touch_y0 = touch_ly = tp.py; touch_age = 0; touch_moved = 0; }
        else {
            mouse_move(m, (tp.px - touch_lx) * 2.0f, (tp.py - touch_ly) * 2.0f);
            touch_lx = tp.px; touch_ly = tp.py; touch_age++;
            if (abs(tp.px - touch_x0) + abs(tp.py - touch_y0) > 8) touch_moved = 1;
        }
    } else if (touch_active) {
        if (!touch_moved && touch_age < 15) left_touch = 1;
        touch_active = 0;
    }
    if (left_touch) lb_frames = 3;
    int btn = 0;
    if ((held & KEY_A) || lb_frames > 0) btn |= 1;
    if ((held & KEY_B) || rb_frames > 0) btn |= 2;
    if (lb_frames > 0) lb_frames--;
    if (rb_frames > 0) rb_frames--;
    m->mouse_btn = btn;
    prev_held = held;
}

static void input_release_all(Machine *m) {
    for (unsigned i = 0; i < sizeof KEYS / sizeof KEYS[0]; i++)
        if (key_sent & (1u << i)) machine_key(m, KEYS[i].sc, 0, KEYS[i].ascii);
    key_sent = 0; prev_held = 0; m->mouse_btn = 0; lb_frames = rb_frames = 0; touch_active = 0;
}

/* ------------------------------------------------------------------ rendering */
static uint16_t topimg[400 * 240];      /* last rendered game frame for the top screen (column-major, 400x240) */
static void top_render(Machine *m) {
    if (win_hide && win_state) {                          /* message window up: picture only, the text lives on the bottom screen */
        if (top_zoom) vgascale_render_picture(m, topimg);
        else {
            vgascale_render(m, topimg, 40);
            for (int x = 0; x < 400; x++) memset(topimg + x * 240, 0, 64 * sizeof(uint16_t));      /* picture rows 176..239 = the window strip */
        }
        return;
    }
    if (top_zoom) vgascale_render_zoom(m, topimg);
    else vgascale_render(m, topimg, 40);                  /* 320x240 picture centred in the 400-wide screen */
}
/* once per frame: is the message window up?  The view only switches after the answer has been the same for a few
 * frames, so the picture does not jump while the engine redraws the window. */
static void win_update(Machine *m) {
    int up = win_hide && vgascale_window_up(m);
    if (up == win_cand) { if (win_cnt < 1000) win_cnt++; } else { win_cand = up; win_cnt = 0; }
    if (win_cand != win_state && win_cnt >= 6) { win_state = win_cand; win_force = 1; }
}
static void top_clear(void) { memset(topimg, 0, sizeof topimg); }
static uint8_t botrgb[320 * 240 * 3];
static int last_glyphs;

static void bottom_render(Machine *m) {
    last_glyphs = textcap_render(m, botrgb, 320, 240);
    const uint8_t *p = botrgb;
    for (int y = 0; y < 240; y++)
        for (int x = 0; x < 320; x++, p += 3) fbBot[x * 240 + (239 - y)] = C565(p[0], p[1], p[2]);
    if (!last_glyphs) {
        const uint16_t dim = C565(120, 130, 160);
        gtext(fbBot, 320, 8, 150, 38, "Dialogue appears here in large type.", dim, -1, 1);
        gtext(fbBot, 320, 8, 170, 38, "A:click  B:right-click  X:Enter  Y:Esc\nD-pad:arrows  L:Ctrl  R:Space\nCircle pad / touch drag: mouse\nZL/ZR: slow/fast mouse  START: menu", dim, -1, 1);
    }
}

/* ------------------------------------------------------------------ pause menu */
enum { MI_RESUME, MI_TEXT, MI_TOP, MI_WIN, MI_CPU, MI_MOUSE, MI_QUIT, MI_COUNT };
static double stat_speed = 1.0, stat_mips = 0, stat_busy = 0, stat_skip = 0;

static void menu_draw(Machine *m, int sel) {
    const uint16_t fg = C565(235, 235, 235), hi = C565(255, 220, 80), dim = C565(140, 150, 180);
    fill(fbBot, 320, C565(10, 10, 30));
    gtext(fbBot, 320, 8, 6, 20, "PAUSED", hi, -1, 2);
    char line[64];
    int y = 34;
    for (int i = 0; i < MI_COUNT; i++) {
        switch (i) {
        case MI_RESUME: snprintf(line, sizeof line, "Resume"); break;
        case MI_CPU:
            if (cpu_mode == 0) snprintf(line, sizeof line, "CPU speed: Auto (%.1f MIPS)", m->ips / 1e6);
            else snprintf(line, sizeof line, "CPU speed: %.0f MIPS fixed", CPU_FIXED[cpu_mode] / 1e6);
            break;
        case MI_MOUSE: snprintf(line, sizeof line, "Mouse speed: %d", mouse_level); break;
        case MI_TEXT: snprintf(line, sizeof line, "Bottom text size: %s", TEXT_NAME[text_size]); break;
        case MI_TOP: snprintf(line, sizeof line, "Top screen: %s", top_zoom ? "Zoom (fill)" : "Original 4:3"); break;
        case MI_WIN: snprintf(line, sizeof line, "Top: dialogue window: %s", win_hide ? "Hidden" : "Shown"); break;
        default: snprintf(line, sizeof line, "Quit game"); break;
        }
        char buf[72]; snprintf(buf, sizeof buf, "%c %s", i == sel ? '>' : ' ', line);
        gtext(fbBot, 320, 10, y, 38, buf, i == sel ? hi : fg, -1, 1 + 0);
        y += 14;
    }
    char st[200];
    snprintf(st, sizeof st, "Speed %3.0f%%   CPU %2.0f%%   %.1f MIPS\nunhandled ops: %u   capture: %s\nSaves + log: SD:/3ds/nonomura/",
             stat_speed * 100.0, stat_busy * 100.0, stat_mips, (unsigned)m->stat_unhandled, textcap_active(m) ? "on" : "OFF");
    gtext(fbBot, 320, 8, 176, 38, st, dim, -1, 1);
    gtext(fbBot, 320, 8, 224, 38, "D-pad: select  A: ok  B/START: resume", dim, -1, 1);
}

/* returns 1 to quit */
static int text_changed;
static int pause_menu(Machine *m) {
    int sel = 0;
    input_release_all(m);
    while (aptMainLoop()) {
        fb_get();
        menu_draw(m, sel);
        /* keep showing the last game frame on top */
        memcpy(fbTop, topimg, sizeof topimg);
        present();
        hidScanInput();
        uint32_t d = hidKeysDown();
        if (d & (KEY_START | KEY_B)) { cfg_save(); return 0; }
        if (d & KEY_DDOWN) sel = (sel + 1) % MI_COUNT;
        if (d & KEY_DUP) sel = (sel + MI_COUNT - 1) % MI_COUNT;
        int dir = ((d & KEY_DRIGHT) ? 1 : 0) - ((d & KEY_DLEFT) ? 1 : 0);
        if (sel == MI_CPU && (dir || (d & KEY_A))) {
            cpu_mode = (cpu_mode + (dir ? dir : 1) + N_CPU_MODES) % N_CPU_MODES;
            if (cpu_mode) machine_set_ips(m, CPU_FIXED[cpu_mode]);
        }
        if (sel == MI_MOUSE && (dir || (d & KEY_A))) { mouse_level += dir ? dir : 1; if (mouse_level > 5) mouse_level = 1; if (mouse_level < 1) mouse_level = 5; }
        if (sel == MI_TEXT && (dir || (d & KEY_A))) { text_size = (text_size + (dir ? dir : 1) + 4) % 4; textcap_set_max_scale(TEXT_SCALE[text_size]); text_changed = 1; }
        if (sel == MI_TOP && (dir || (d & KEY_A))) { top_zoom ^= 1; top_clear(); top_render(m); }
        if (sel == MI_WIN && (dir || (d & KEY_A))) { win_hide ^= 1; win_cand = win_state = win_hide && vgascale_window_up(m); win_cnt = 0; top_clear(); top_render(m); }
        if (sel == MI_RESUME && (d & KEY_A)) { cfg_save(); return 0; }
        if (sel == MI_QUIT && (d & KEY_A)) { cfg_save(); return 1; }
    }
    return 1;
}

/* ------------------------------------------------------------------ main */
static int file_exists(const char *dir, const char *name) {
    char p[300]; snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "rb"); if (!f) return 0; fclose(f); return 1;
}

int main(void) {
    gfxInit(GSP_RGB565_OES, GSP_RGB565_OES, false);
    gfxSet3D(false);
    gfxSetDoubleBuffering(GFX_BOTTOM, false);
    osSetSpeedupEnable(true);
    t_start = svcGetSystemTick();

    const char *gdir = NULL;
    for (int i = 0; GAME_DIRS[i]; i++) if (file_exists(GAME_DIRS[i], "SILV1.COM") || file_exists(GAME_DIRS[i], "silv1.com")) { gdir = GAME_DIRS[i]; break; }
    if (!gdir) {
        message_screen("Game files not found",
            "Copy the game files to the SD card:\n(SILV1.COM, SILV.EXE, MUSICV.COM,\nSAGWA.*, FLAG*, SILK*.DAT ...)\n\n  sdmc:/3ds/nonomura/\n\nthen start the app again.");
        gfxExit(); return 0;
    }
    char logpath[300]; snprintf(logpath, sizeof logpath, "%s/nono_log.txt", gdir);
    logfp = fopen(logpath, "w");
    snprintf(cfg_path, sizeof cfg_path, "%s/nono_cfg.txt", gdir);
    cfg_load();
    textcap_set_max_scale(TEXT_SCALE[text_size]);
    bool is_new = false; APT_CheckNew3DS(&is_new);
    LOG("Nonomura 3DS native v%s  new3ds=%d  gamedir=%s", APP_VERSION, (int)is_new, gdir);

    audio_init();
    LOG("audio %s", audio_ok ? "ok" : "unavailable");

    Machine *m = machine_new(gdir);
    if (!m) { message_screen("Out of memory", "Could not allocate the emulated PC."); gfxExit(); return 0; }
    m->verbose = 0;
    m->opl = opl_new(AUD_RATE);
    opl_attach(m->opl, (opl_clock_fn)machine_vtime_ns, m);
    textcap_init(m);
    idle_init(m);
    uint32_t ips = is_new ? 4000000u : 1500000u;
    machine_set_ips(m, ips);

    fb_get(); fill(fbBot, 320, C565(10, 10, 30));
    gtext(fbBot, 320, 8, 100, 38, "Loading...", C565(230, 230, 230), -1, 2);
    fill(fbTop, 400, 0); present(); fb_get(); fill(fbTop, 400, 0); present();

    if (dos_exec(m, "MUSICV.COM", "") < 0) { LOG("MUSICV.COM missing"); message_screen("MUSICV.COM not found", "The music driver MUSICV.COM must be in the game folder."); goto done; }
    { long total = 0; int guard = 0; while (!m->exited && total < 20000000 && ++guard < 400) total += machine_run(m, 100000);
      LOG("MUSICV done: exited=%d tsr=%d instr=%ld", m->exited, m->tsr_kept, total); }
    if (dos_exec(m, "SILV1.COM", "main.mes D D") < 0) { LOG("SILV1.COM launch failed"); message_screen("SILV1.COM failed", "Could not start SILV1.COM / SILV.EXE.\nCheck that all game files were copied."); goto done; }
    LOG("game started  top_zoom=%d text_size=%d cpu_mode=%d", top_zoom, text_size, cpu_mode);
    top_clear();

    u64 win_t0 = svcGetSystemTick(), win_busy = 0, win_v0 = machine_vtime_ns(m), win_i0 = m->icount, win_s0 = m->skipped_total;
    int win_frames = 0, frame = 0, lag_frames = 0, bot_force = 1;
    u64 pace_t0 = svcGetSystemTick(), pace_v0 = machine_vtime_ns(m);
    uint32_t last_dirty = ~0u, last_gen = ~0u, bot_dirty = ~0u; int last_bot_frame = -100;
    int quit = 0;

    while (aptMainLoop() && !quit) {
        u64 f0 = svcGetSystemTick();
        hidScanInput();
        if (hidKeysDown() & KEY_START) {
            if (pause_menu(m)) break;
            bot_force = 1; pace_t0 = svcGetSystemTick(); pace_v0 = machine_vtime_ns(m); win_t0 = svcGetSystemTick(); win_busy = 0; win_v0 = machine_vtime_ns(m); win_i0 = m->icount; win_s0 = m->skipped_total; win_frames = 0;
            continue;
        }
        if (m->exited) break;
        input_frame(m);

        /* Emulate the virtual time that has passed in real life (not a fixed 1/60 s: the screen refreshes at 59.83 Hz and
         * frames sometimes take longer), so game and music run at true speed and the audio never gets ahead or behind.
         * Never spend more than ~75% of a real frame, and forget any backlog beyond 50 ms. */
        double owe = (double)(f0 - pace_t0) / TICKS_PER_SEC - (double)(machine_vtime_ns(m) - pace_v0) / 1e9;
        if (owe > 0.050) { pace_t0 += (u64)((owe - 0.050) * TICKS_PER_SEC); owe = 0.050; }
        if (owe < 0) owe = 0;
        long want = (long)(m->ips * owe), done = 0;
#ifdef STUB_JITTER      /* test builds only: end frames at random points to provoke mid-redraw renders */
        { static unsigned js = 0; if (!js) js = getenv("STUB_SEED") ? (unsigned)atoi(getenv("STUB_SEED")) : 12345u; js = js * 1103515245u + 12345u; want = want * (10 + (js >> 16) % 90) / 100; }
#endif
        u64 deadline = f0 + FRAME_TICKS * 75 / 100;
        while (done < want) {
            long c = want - done; if (c > 12000) c = 12000;
            done += machine_run(m, (int)c);
            if (m->exited) break;
            if (svcGetSystemTick() > deadline) break;
        }
        int lagged = done < want;
        lag_frames = lagged ? lag_frames + 1 : 0;
        audio_pump(m);

        /* top screen: re-render the picture when VRAM / palette changed (every other frame if we are struggling) */
        fb_get();
        win_update(m);
        if (win_force || (m->vga.dirty != last_dirty && (!lagged || (frame & 1) == 0))) {
            top_render(m);
            last_dirty = m->vga.dirty; win_force = 0;
        }
        memcpy(fbTop, topimg, sizeof topimg);
        if (hidKeysHeld() & KEY_SELECT) {               /* hold SELECT: live performance numbers (for bug reports) */
            char t[200];
            snprintf(t, sizeof t, "spd %3.0f%% cpu %2.0f%% exec %.1fM\nips %.1fM skip %.0f%% aq %d\nunder %u drop %u tc %d",
                     stat_speed * 100.0, stat_busy * 100.0, stat_mips, m->ips / 1e6, stat_skip * 100.0, audio_queued(), aud_underruns, aud_dropped, textcap_active(m));
            gtext(fbTop, 400, 2, 2, 46, t, C565(255, 255, 0), C565(0, 0, 0), 1);
        }

        /* bottom screen: large dialogue text (rate limited) */
        uint32_t gen = textcap_gen(m);
        int unsettled = textcap_unsettled(m);
        if (bot_force || ((gen != last_gen || m->vga.dirty != bot_dirty || unsettled) && frame - last_bot_frame >= (gen != last_gen ? 4 : unsettled ? 3 : 8))) {
            bottom_render(m);
            last_gen = gen; bot_dirty = m->vga.dirty; last_bot_frame = frame; bot_force = 0;
        }
        u64 f1 = svcGetSystemTick();
        win_busy += f1 - f0;
        gfxFlushBuffers(); gfxSwapBuffers(); gspWaitForVBlank();
        frame++; win_frames++;

        /* once a second: compare virtual time with real time and adapt the emulated CPU speed */
        if (win_frames >= 60) {
            u64 now = svcGetSystemTick();
            double real = (double)(now - win_t0) / TICKS_PER_SEC;
            double virt = (double)(machine_vtime_ns(m) - win_v0) / 1e9;
            double ratio = real > 0 ? virt / real : 1.0;
            double busy = real > 0 ? (double)win_busy / TICKS_PER_SEC / real : 1.0;
            stat_speed = ratio; stat_mips = (double)((m->icount - win_i0) - (m->skipped_total - win_s0)) / real / 1e6;   /* instructions really executed */
            stat_busy = busy;
            stat_skip = (m->icount - win_i0) ? (double)(m->skipped_total - win_s0) / (double)(m->icount - win_i0) : 0;
            audio_set_speed(ratio);
            if (cpu_mode == 0) {
                uint32_t n = m->ips;
                static int cooldown;
                if (ratio < 0.97) { double k = ratio < 0.5 ? 0.5 : ratio; n = (uint32_t)(m->ips * k * 0.97); cooldown = 6; m->poll_force = 1; }
                else if (cooldown > 0) cooldown--;
                else if (busy < 0.60 && m->ips < IPS_MAX) n = (uint32_t)(m->ips * 1.06);
                if (n < IPS_MIN) n = IPS_MIN;
                if (n > IPS_MAX) n = IPS_MAX;
                if (n != m->ips) machine_set_ips(m, n);
            }
            static int logn;
            if (logn < 40 || (logn % 10) == 0)
                LOG("speed=%.0f%% busy=%.0f%% exec=%.2f MIPS skip=%.0f%% aq=%d under=%u drop=%u ips=%u mouse=%d,%d unhandled=%u glyphs=%d tc=%d pc=%04X:%04X mode=%02X vdirty=%u", ratio * 100, busy * 100, stat_mips, stat_skip * 100, audio_queued(), aud_underruns, aud_dropped, m->ips, m->mouse_x, m->mouse_y, (unsigned)m->stat_unhandled, last_glyphs, textcap_active(m), m->cpu.seg[CS], m->cpu.ip, m->vga.bios_mode, (unsigned)m->vga.dirty);
            if (m->con_len) { static int lastcl; if (m->con_len != lastcl) { LOG("console: %s", m->con); lastcl = m->con_len; } }
            logn++;
            win_t0 = now; win_busy = 0; win_v0 = machine_vtime_ns(m); win_i0 = m->icount; win_s0 = m->skipped_total; win_frames = 0;
        }
    }

    if (m->exited && aptMainLoop()) {
        char body[700];
        snprintf(body, sizeof body, "The game program ended (exit code %d).\n\n%s%s", m->exit_code,
                 m->con_len ? "Last console output:\n" : "", m->con_len ? m->con : "");
        LOG("game exited code=%d  console: %s", m->exit_code, m->con);
        message_screen("Game ended", body);
    }
done:
    LOG("shutdown");
    if (logfp) fclose(logfp);
    if (audio_ok) { ndspChnReset(0); ndspExit(); linearFree(abuf); }
    gfxExit();
    return 0;
}
