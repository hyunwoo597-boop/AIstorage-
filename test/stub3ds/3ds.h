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
enum { KEY_A = BIT(0), KEY_B = BIT(1), KEY_START = BIT(3), KEY_DRIGHT = BIT(4), KEY_DLEFT = BIT(5), KEY_DUP = BIT(6), KEY_DDOWN = BIT(7),
       KEY_R = BIT(8), KEY_L = BIT(9), KEY_X = BIT(10), KEY_Y = BIT(11), KEY_ZL = BIT(14), KEY_ZR = BIT(15), KEY_TOUCH = BIT(20) };
typedef struct { u16 px, py; } touchPosition; typedef struct { s16 dx, dy; } circlePosition;
typedef enum { GFX_TOP, GFX_BOTTOM } gfxScreen_t; typedef enum { GFX_LEFT, GFX_RIGHT } gfx3dSide_t;
#define GSP_RGB565_OES 2
typedef int GSPGPU_FramebufferFormat;
static u16 _fb[2][2][400 * 240]; static int _cur[2], _dbl[2] = {1, 1};
static int _frame; static u64 _t0;
static double _slow = 1;
static inline u64 svcGetSystemTick(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (u64)((double)(ts.tv_sec * 1e9 + ts.tv_nsec) * 0.268111856 * _slow); }
/* host is far faster than a 3DS: slow the stub clock-independent work isn't needed, just report real time */
static inline void gfxInit(GSPGPU_FramebufferFormat a, GSPGPU_FramebufferFormat b, bool c) {}
static inline void gfxExit(void) {}
static inline void gfxSet3D(bool e) {}
static inline void gfxSetDoubleBuffering(gfxScreen_t s, bool e) { _dbl[s] = e; }
static inline u8 *gfxGetFramebuffer(gfxScreen_t s, gfx3dSide_t sd, u16 *w, u16 *h) { return (u8 *)_fb[s][_cur[s]]; }
static inline void gfxFlushBuffers(void) {}
static inline void gfxSwapBuffers(void) { for (int s = 0; s < 2; s++) if (_dbl[s]) _cur[s] ^= 1; }
static inline void gspWaitForVBlank(void) { _frame++; if (_slow > 1) { u64 fr = SYSCLOCK_ARM11 / 60, now = svcGetSystemTick(), nb = (now / fr + 1) * fr; while (svcGetSystemTick() < nb) usleep(200); } }
static inline void osSetSpeedupEnable(bool e) {}
static inline Result APT_CheckNew3DS(bool *o) { *o = true; return 0; }
static int _script_frames; static int _scans;
static inline bool aptMainLoop(void) { return _scans < _script_frames; }
static u32 _held, _down_prev; static touchPosition _tp; static circlePosition _cp;
extern void stub_input(int scan, u32 *held, touchPosition *tp, circlePosition *cp);
static void (*_dump_hook)(int);
static inline void hidScanInput(void) { if (_dump_hook && _scans % 300 == 0) _dump_hook(_scans); _down_prev = _held; stub_input(_scans++, &_held, &_tp, &_cp); }
static inline u32 hidKeysHeld(void) { return _held; }
static inline u32 hidKeysDown(void) { return _held & ~_down_prev; }
static inline void hidTouchRead(touchPosition *t) { *t = _tp; }
static inline void hidCircleRead(circlePosition *c) { *c = _cp; }
/* audio */
typedef struct tag_ndspWaveBuf { union { s16 *data_pcm16; const void *data_vaddr; }; u32 nsamples; u8 status; } ndspWaveBuf;
enum { NDSP_WBUF_FREE, NDSP_WBUF_QUEUED, NDSP_WBUF_PLAYING, NDSP_WBUF_DONE };
enum { NDSP_OUTPUT_STEREO, NDSP_INTERP_LINEAR, NDSP_FORMAT_STEREO_PCM16 };
static long _audio_frames; static int _wb_age[16];
static inline Result ndspInit(void) { return 0; } static inline void ndspExit(void) {}
static inline void ndspSetOutputMode(int m) {} static inline void ndspChnReset(int c) {} static inline void ndspChnSetInterp(int c, int t) {}
static inline void ndspChnSetRate(int c, float r) {} static inline void ndspChnSetFormat(int c, u16 f) {} static inline void ndspChnSetMix(int c, float *m) {}
static inline void ndspChnWaveBufAdd(int c, ndspWaveBuf *b) { _audio_frames += b->nsamples; b->status = NDSP_WBUF_DONE; }
static inline void *linearAlloc(size_t n) { return malloc(n); } static inline void linearFree(void *p) { free(p); }
static inline Result DSP_FlushDataCache(const void *a, u32 n) { return 0; }
