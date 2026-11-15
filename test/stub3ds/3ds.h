#include <stdio.h>
#include <stdlib.h>
/* minimal host stub of the libctru calls used by main.c - for logic testing only */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64; typedef int32_t Result; typedef int16_t s16; typedef int8_t s8;
#define R_FAILED(x) ((x) < 0)
#define BIT(n) (1u << (n))
#define SYSCLOCK_ARM11 268111856ull
enum { KEY_A = BIT(0), KEY_B = BIT(1), KEY_SELECT = BIT(2), KEY_START = BIT(3), KEY_DRIGHT = BIT(4), KEY_DLEFT = BIT(5), KEY_DUP = BIT(6), KEY_DDOWN = BIT(7),
       KEY_R = BIT(8), KEY_L = BIT(9), KEY_X = BIT(10), KEY_Y = BIT(11), KEY_ZL = BIT(14), KEY_ZR = BIT(15), KEY_TOUCH = BIT(20) };
typedef struct { u16 px, py; } touchPosition; typedef struct { s16 dx, dy; } circlePosition;
typedef enum { GFX_TOP, GFX_BOTTOM } gfxScreen_t; typedef enum { GFX_LEFT, GFX_RIGHT } gfx3dSide_t;
#define GSP_RGB565_OES 2
typedef int GSPGPU_FramebufferFormat;
static u16 _fb[2][2][400 * 240]; static u16 _fbr[2][400 * 240]; static int _cur[2], _dbl[2] = {1, 1}; static int _3d;
static int _frame; static u64 _t0;
static double _slow = 1;
static int _frameclock;
/* optional CPU cost model (STUB_COST=1): the clock advances by modelled 3DS costs reported by the front end through stub_cost(ms) */
static int _costmodel; static double _now, _dsp_t;
static void _dsp_run(double dt);
static inline void _advance(double t) { if (t > _dsp_t) { _dsp_run(t - _dsp_t); _dsp_t = t; } _now = t; }
static inline void stub_cost(double ms) { if (_costmodel) _advance(_now + ms / 1000.0); }
static inline u64 svcGetSystemTick(void) { if (_costmodel) return (u64)(_now * (double)SYSCLOCK_ARM11); if (_frameclock) return (u64)((double)_frame * (double)SYSCLOCK_ARM11 / 59.83); struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (u64)((double)(ts.tv_sec * 1e9 + ts.tv_nsec) * 0.268111856 * _slow); }
/* host is far faster than a 3DS: slow the stub clock-independent work isn't needed, just report real time */
static inline void gfxInit(GSPGPU_FramebufferFormat a, GSPGPU_FramebufferFormat b, bool c) {}
static inline void gfxExit(void) {}
static inline void gfxSet3D(bool e) { _3d = e; }
static inline float osGet3DSliderState(void) { return getenv("STUB_SLIDER") ? (float)atof(getenv("STUB_SLIDER")) : 0.f; }
static inline void gfxSetDoubleBuffering(gfxScreen_t s, bool e) { _dbl[s] = e; }
static inline u8 *gfxGetFramebuffer(gfxScreen_t s, gfx3dSide_t sd, u16 *w, u16 *h) { if (s == GFX_TOP && sd == GFX_RIGHT && _3d) return (u8 *)_fbr[_cur[0]]; return (u8 *)_fb[s][_cur[s]]; }
static inline void gfxFlushBuffers(void) {}
static inline void gfxSwapBuffers(void) { for (int s = 0; s < 2; s++) if (_dbl[s]) _cur[s] ^= 1; }
static void _dsp_run(double dt);
static int _stallp, _stallms, _stall_seeded;
static inline void gspWaitForVBlank(void) { if (_costmodel) { static int seeded; if (_stallp && !seeded) { srand(getenv("STUB_SEED") ? atoi(getenv("STUB_SEED")) : 1); seeded = 1; }
        double nb = ((double)(long)(_now * 59.83) + 1.0) / 59.83; if (_stallp && rand() % 1000 < _stallp) nb += _stallms / 1000.0; _frame = (int)(nb * 59.83); _advance(nb); return; }
    int n = 1; if (_stallp) { if (!_stall_seeded) { srand(getenv("STUB_SEED") ? atoi(getenv("STUB_SEED")) : 1); _stall_seeded = 1; } if (rand() % 1000 < _stallp) n += (int)(_stallms * 59.83 / 1000.0 + 0.5); } _frame += n; _dsp_run(n / 59.83);
    if (_slow > 1) { u64 fr = SYSCLOCK_ARM11 / 60, now = svcGetSystemTick(), nb = (now / fr + 1) * fr; while (svcGetSystemTick() < nb) usleep(200); } }
static inline void osSetSpeedupEnable(bool e) {}
static inline Result APT_CheckNew3DS(bool *o) { *o = true; return 0; }
static int _script_frames; static int _scans;
static inline bool aptMainLoop(void) { return _scans < _script_frames; }
static u32 _held, _down_prev; static touchPosition _tp; static circlePosition _cp;
extern void stub_input(int scan, u32 *held, touchPosition *tp, circlePosition *cp);
static void (*_dump_hook)(int);
static int _dump_every = 300, _dump_from = 0, _dump_to = 1 << 30;
static inline void hidScanInput(void) { if (_dump_hook && _scans % _dump_every == 0) _dump_hook(_scans); _down_prev = _held; stub_input(_scans++, &_held, &_tp, &_cp); }
static inline u32 hidKeysHeld(void) { return _held; }
static inline u32 hidKeysDown(void) { return _held & ~_down_prev; }
static inline void hidTouchRead(touchPosition *t) { *t = _tp; }
static inline void hidCircleRead(circlePosition *c) { *c = _cp; }
/* audio */
typedef struct tag_ndspWaveBuf { union { s16 *data_pcm16; const void *data_vaddr; }; u32 nsamples; u8 status; } ndspWaveBuf;
enum { NDSP_WBUF_FREE, NDSP_WBUF_QUEUED, NDSP_WBUF_PLAYING, NDSP_WBUF_DONE };
enum { NDSP_OUTPUT_STEREO, NDSP_INTERP_LINEAR, NDSP_FORMAT_STEREO_PCM16 };
static long _audio_frames; static int _wb_age[16];
/* DSP model: consumes the queued wave buffers in real (frame-clock) time; gaps are written as silence */
static int _dsp_paused = 1, _dsp_n, _dsp_pos, _dsp_in_under, _dsp_started; static double _dsp_rate = 32728.498, _dsp_acc; static ndspWaveBuf *_dsp_q[256];
static long _dsp_under_events, _dsp_under_samples, _dsp_played; static FILE *_dsp_out;
static void _dsp_run(double dt) {
    if (!_dsp_out && getenv("STUB_AUDIO")) _dsp_out = fopen(getenv("STUB_AUDIO"), "wb");
    if (_dsp_paused) return;
    static double _dsp_skew = -1; if (_dsp_skew < 0) _dsp_skew = getenv("STUB_DSPSKEW") ? atof(getenv("STUB_DSPSKEW")) : 1.0;
    _dsp_acc += dt * _dsp_rate * _dsp_skew; int whole = (int)_dsp_acc; _dsp_acc -= whole;
    while (whole > 0) {
        if (!_dsp_n) {
            if (_dsp_started) { _dsp_under_samples += whole; if (!_dsp_in_under) { _dsp_under_events++; _dsp_in_under = 1; } }
            static int16_t z[2048 * 2]; while (whole > 0) { int c = whole > 2048 ? 2048 : whole; if (_dsp_out) fwrite(z, 4, c, _dsp_out); whole -= c; }
            break; }
        ndspWaveBuf *b = _dsp_q[0]; b->status = 2;
        int left = (int)b->nsamples - _dsp_pos, take = whole < left ? whole : left;
        if (_dsp_out) fwrite(b->data_pcm16 + _dsp_pos * 2, 4, take, _dsp_out);
        _dsp_pos += take; whole -= take; _dsp_played += take; _dsp_in_under = 0; _dsp_started = 1;
        if (_dsp_pos >= (int)b->nsamples) { b->status = 3; for (int i = 1; i < _dsp_n; i++) _dsp_q[i - 1] = _dsp_q[i]; _dsp_n--; _dsp_pos = 0; }
    }
}
static inline Result ndspInit(void) { return 0; } static inline void ndspExit(void) {}
static inline void ndspSetOutputMode(int m) {} static inline void ndspChnReset(int c) {} static inline void ndspChnSetInterp(int c, int t) {}
static inline void ndspChnSetPaused(int c, bool p) { _dsp_paused = p; }
static inline void ndspChnSetRate(int c, float r) { _dsp_rate = r; } static inline void ndspChnSetFormat(int c, u16 f) {} static inline void ndspChnSetMix(int c, float *m) {}
static inline void ndspChnWaveBufAdd(int c, ndspWaveBuf *b) { _audio_frames += b->nsamples; if (b->status == 1 || b->status == 2) return; b->status = 1; if (_dsp_n < 256) _dsp_q[_dsp_n++] = b; }
static inline void *linearAlloc(size_t n) { return malloc(n); } static inline void linearFree(void *p) { free(p); }
static inline Result DSP_FlushDataCache(const void *a, u32 n) { return 0; }
