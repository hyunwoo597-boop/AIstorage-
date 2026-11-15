#include "3ds.h"
#define main app_main
#include "../../ctr/source/main.c"
#undef main
#include <stdio.h>
static void dump(const char *p, u16 *fb, int w) { FILE *f = fopen(p, "wb"); fprintf(f, "P6\n%d 240\n255\n", w);
    for (int y = 0; y < 240; y++) for (int x = 0; x < w; x++) { u16 c = fb[x * 240 + (239 - y)]; u8 b[3] = {(c >> 11) << 3, ((c >> 5) & 63) << 2, (c & 31) << 3}; fwrite(b, 1, 3, f); } fclose(f); }
static FILE *ink_f;
static void hook(int scan) { char p[64];
    if (!ink_f && (scan < _dump_from || scan >= _dump_to)) return;
    if (ink_f) { u16 *fb = _fb[1][_cur[1]]; int n = 0; for (int i = 0; i < 320 * 240; i++) { u16 c = fb[i]; if (((c >> 11) << 3) + (((c >> 5) & 63) << 2) + ((c & 31) << 3) > 500) n++; } fprintf(ink_f, "%d %d\n", scan, n); fflush(ink_f); if (scan < _dump_from || scan >= _dump_to) return; } snprintf(p, sizeof p, "/tmp/st_top_%04d.ppm", scan); dump(p, _fb[0][_cur[0] ^ 1], 400); snprintf(p, sizeof p, "/tmp/st_bot_%04d.ppm", scan); dump(p, _fb[1][_cur[1]], 320); }
int main(int argc, char **argv) {
    if (getenv("STUB_DUMP")) _dump_hook = hook;
    if (getenv("STUB_FRAMECLOCK")) _frameclock = 1;
    if (getenv("STUB_INK")) ink_f = fopen(getenv("STUB_INK"), "w");
    if (getenv("STUB_DUMPEVERY")) _dump_every = atoi(getenv("STUB_DUMPEVERY"));
    if (getenv("STUB_DUMPFROM")) _dump_from = atoi(getenv("STUB_DUMPFROM"));
    if (getenv("STUB_DUMPTO")) _dump_to = atoi(getenv("STUB_DUMPTO"));
    if (getenv("STUB_SLOW")) _slow = atof(getenv("STUB_SLOW"));
    _script_frames = argc > 1 ? atoi(argv[1]) : 900;
    app_main();
    dump("/tmp/stub_top.ppm", _fb[0][_cur[0] ^ 1], 400); dump("/tmp/stub_bot.ppm", _fb[1][_cur[1]], 320);
    fprintf(stderr, "audio frames queued: %ld (%.1f s)  frames=%d\n", _audio_frames, _audio_frames / 32728.0, _frame);
    return 0;
}
