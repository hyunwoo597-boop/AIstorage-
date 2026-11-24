/* Switch sound: one SDL audio stream (48 kHz stereo S16), the engine's 8 slots mixed in the callback.
 * Sources: Ogg Vorbis (libvorbisfile; "AI5S" header = loop points), PCM16 WAV (smpl loop), AI5A IMA ADPCM (HTML maker). */
#include <SDL2/SDL.h>
#include <vorbis/vorbisfile.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/kai.h"

#define OUT_RATE 48000
typedef struct {
    int active;
    uint8_t *data; size_t len;
    const uint8_t *src; size_t src_len, src_pos;
    int kind;                         /* 1 vorbis, 2 pcm16, 3 adpcm */
    OggVorbis_File vf;
    const int16_t *pcm; uint32_t frames;
    const uint8_t *ad; uint32_t ad_bs; int32_t ad_blk; int16_t *ad_buf;
    int channels, rate;
    uint32_t loop_start, loop_end;
    uint64_t pos;                     /* next source frame to decode */
    int16_t buf[4096 * 2]; int buf_n, buf_i;   /* decoded source frames waiting */
    int16_t prev[2], cur[2]; uint32_t phase;   /* resampler (16.16) */
    int eof, primed;
    float vol, fade;
} slot_t;
static slot_t g_s[SND_SLOTS];
static SDL_AudioDeviceID g_dev;

static size_t mem_read(void *ptr, size_t size, size_t n, void *ds) { slot_t *s = ds; size_t want = size * n, left = s->src_len - s->src_pos; if (want > left) want = left;
    memcpy(ptr, s->src + s->src_pos, want); s->src_pos += want; return size ? want / size : 0; }
static int mem_seek(void *ds, ogg_int64_t off, int whence) { slot_t *s = ds; int64_t p = whence == SEEK_SET ? off : whence == SEEK_CUR ? (int64_t)s->src_pos + off : (int64_t)s->src_len + off;
    if (p < 0 || p > (int64_t)s->src_len) return -1; s->src_pos = (size_t)p; return 0; }
static int mem_close(void *ds) { (void)ds; return 0; }
static long mem_tell(void *ds) { return (long)((slot_t *)ds)->src_pos; }
static uint32_t rd32le(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static const int16_t ima_step[89] = { 7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,118,130,143,157,173,190,209,230,
    253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,
    5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,27086,29794,32767 };
static const int8_t ima_idx[16] = { -1,-1,-1,-1,2,4,6,8,-1,-1,-1,-1,2,4,6,8 };
static void adpcm_block(slot_t *s, int32_t blk) {
    int ch = s->channels; uint32_t bs = s->ad_bs; const uint8_t *b = s->ad + (size_t)blk * (4 * ch + bs / 2 * ch);
    for (int c = 0; c < ch; c++) { int pred = (int16_t)(b[c * 4] | b[c * 4 + 1] << 8), idx = b[c * 4 + 2]; if (idx > 88) idx = 88;
        const uint8_t *nb = b + 4 * ch + c * (bs / 2);
        for (uint32_t i = 0; i < bs; i++) { int n = (nb[i >> 1] >> ((i & 1) * 4)) & 15, st = ima_step[idx], d = st >> 3;
            if (n & 4) d += st; if (n & 2) d += st >> 1; if (n & 1) d += st >> 2; if (n & 8) pred -= d; else pred += d;
            if (pred > 32767) pred = 32767; if (pred < -32768) pred = -32768; idx += ima_idx[n]; if (idx < 0) idx = 0; if (idx > 88) idx = 88;
            s->ad_buf[i * ch + c] = (int16_t)pred; } }
    s->ad_blk = blk;
}
static void release(slot_t *s) {
    if (s->kind == 1) ov_clear(&s->vf);
    free(s->ad_buf); s->ad_buf = NULL; free(s->data); s->data = NULL; s->active = 0; s->kind = 0;
}
/* refill s->buf with up to 4096 source frames (stereo-expanded); returns frames */
static int refill(slot_t *s) {
    int got = 0, empty = 0; const int CAP = 4096;
    static int16_t tmp[4096 * 2];
    while (got < CAP) {
        int want = CAP - got; if (s->loop_end && s->pos + want > s->loop_end) want = (int)(s->loop_end - s->pos);
        int n = 0;
        if (want > 0) {
            if (s->kind == 1) { int bs; long r = ov_read(&s->vf, (char *)tmp, want * s->channels * 2, 0, 2, 1, &bs); if (r < 0) r = 0; n = (int)(r / (2 * s->channels)); }
            else if (s->kind == 2) { uint32_t left = s->pos < s->frames ? s->frames - (uint32_t)s->pos : 0; n = want < (int)left ? want : (int)left; memcpy(tmp, s->pcm + s->pos * s->channels, (size_t)n * s->channels * 2); }
            else { while (n < want && s->pos + n < s->frames) { uint64_t p = s->pos + n; int32_t blk = (int32_t)(p / s->ad_bs); if (blk != s->ad_blk) adpcm_block(s, blk);
                    uint32_t o = (uint32_t)(p % s->ad_bs), k = s->ad_bs - o; if (k > (uint32_t)(want - n)) k = want - n; if (p + k > s->frames) k = (uint32_t)(s->frames - p);
                    memcpy(tmp + n * s->channels, s->ad_buf + o * s->channels, (size_t)k * s->channels * 2); n += k; } }
        }
        for (int i = 0; i < n; i++) { int16_t l = tmp[i * s->channels], r = s->channels == 2 ? tmp[i * 2 + 1] : l; s->buf[(got + i) * 2] = l; s->buf[(got + i) * 2 + 1] = r; }
        got += n; s->pos += n;
        if (n == 0 || (s->loop_end && s->pos >= s->loop_end)) {
            if (!s->loop_end || (n == 0 && ++empty > 2)) { s->eof = 1; break; }
            s->pos = s->loop_start; if (s->kind == 1) ov_pcm_seek(&s->vf, s->loop_start);
        }
    }
    s->buf_n = got; s->buf_i = 0; return got;
}
static int next_frame(slot_t *s, int16_t *o) {
    if (s->buf_i >= s->buf_n) { if (s->eof || !refill(s)) return 0; }
    o[0] = s->buf[s->buf_i * 2]; o[1] = s->buf[s->buf_i * 2 + 1]; s->buf_i++; return 1;
}
static void callback(void *ud, Uint8 *stream, int len) {
    (void)ud; int frames = len / 4; int16_t *out = (int16_t *)stream; static int32_t mix[8192 * 2];
    if (frames > 8192) frames = 8192;
    memset(mix, 0, (size_t)frames * 2 * sizeof(int32_t));
    for (int i = 0; i < SND_SLOTS; i++) { slot_t *s = &g_s[i]; if (!s->active) continue;
        uint32_t step = (uint32_t)(((uint64_t)s->rate << 16) / OUT_RATE);
        if (!s->primed) { int16_t f[2]; if (!next_frame(s, s->prev)) { release(s); continue; } if (!next_frame(s, s->cur)) memcpy(s->cur, s->prev, 4); (void)f; s->primed = 1; s->phase = 0; }
        int done = 0;
        for (int k = 0; k < frames; k++) {
            if (s->fade > 0) { s->vol -= s->fade; if (s->vol <= 0) { done = 1; break; } }
            int v = (int)(s->vol * 256);
            int l = s->prev[0] + (int)(((int64_t)(s->cur[0] - s->prev[0]) * s->phase) >> 16), r = s->prev[1] + (int)(((int64_t)(s->cur[1] - s->prev[1]) * s->phase) >> 16);
            mix[k * 2] += l * v >> 8; mix[k * 2 + 1] += r * v >> 8;
            s->phase += step;
            while (s->phase >= 65536) { s->phase -= 65536; memcpy(s->prev, s->cur, 4); if (!next_frame(s, s->cur)) { done = 1; break; } }
            if (done) break;
        }
        if (done) release(s);
    }
    for (int k = 0; k < frames * 2; k++) { int v = mix[k]; out[k] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
}
void snd_init(void) {
    SDL_AudioSpec want = { 0 }, have; want.freq = OUT_RATE; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024; want.callback = callback;
    g_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!g_dev) { plat_log("audio: %s", SDL_GetError()); return; }
    SDL_PauseAudioDevice(g_dev, 0);
}
void snd_exit(void) { if (!g_dev) return; SDL_LockAudioDevice(g_dev); for (int i = 0; i < SND_SLOTS; i++) if (g_s[i].active) release(&g_s[i]); SDL_UnlockAudioDevice(g_dev); SDL_CloseAudioDevice(g_dev); g_dev = 0; }
void plat_snd_play(int i, void *data, size_t len, int volume, bool loop) {
    if (!g_dev || i < 0 || i >= SND_SLOTS) { free(data); return; }
    SDL_LockAudioDevice(g_dev);
    slot_t *s = &g_s[i]; if (s->active) release(s);
    memset(s, 0, sizeof *s); s->data = data; s->len = len; s->vol = volume / 255.0f; s->ad_blk = -1;
    const uint8_t *d = data; int ok = 0;
    if (len > 24 && !memcmp(d, "AI5A", 4)) {
        s->loop_start = rd32le(d + 4); s->loop_end = rd32le(d + 8); s->rate = (int)rd32le(d + 12); s->channels = d[16] | d[17] << 8;
        s->ad_bs = d[18] | d[19] << 8; s->frames = rd32le(d + 20); s->ad = d + 24;
        uint64_t need = s->ad_bs ? ((uint64_t)s->frames + s->ad_bs - 1) / s->ad_bs * (4 * s->channels + s->ad_bs / 2 * s->channels) : 0;
        if (s->channels >= 1 && s->channels <= 2 && s->ad_bs >= 2 && !(s->ad_bs & 1) && need <= len - 24 && (s->ad_buf = malloc((size_t)s->ad_bs * s->channels * 2))) { s->kind = 3; ok = 1; }
        if (s->loop_end > s->frames) s->loop_end = s->frames;
    } else {
        if (len > 16 && !memcmp(d, "AI5S", 4)) { s->loop_start = rd32le(d + 4); s->loop_end = rd32le(d + 8); d += 16; len -= 16; }
        if (len > 4 && !memcmp(d, "OggS", 4)) {
            s->src = d; s->src_len = len; s->src_pos = 0; ov_callbacks cb = { mem_read, mem_seek, mem_close, mem_tell };
            if (ov_open_callbacks(s, &s->vf, NULL, 0, cb) == 0) { vorbis_info *vi = ov_info(&s->vf, -1); s->channels = vi->channels; s->rate = (int)vi->rate; s->kind = 1;
                ok = s->channels >= 1 && s->channels <= 2; if (!ok) { ov_clear(&s->vf); s->kind = 0; } }
        } else if (len > 44 && !memcmp(d, "RIFF", 4)) {
            size_t p = 12; int fmt_ok = 0;
            while (p + 8 <= len) { uint32_t n = rd32le(d + p + 4); const uint8_t *c = d + p + 8;
                if (!memcmp(d + p, "fmt ", 4) && n >= 16) { s->channels = c[2] | c[3] << 8; s->rate = (int)rd32le(c + 4); fmt_ok = (c[0] == 1 && c[14] == 16); }
                else if (!memcmp(d + p, "data", 4)) { s->pcm = (const int16_t *)c; s->frames = (uint32_t)((n < len - p - 8 ? n : len - p - 8) / (2 * (s->channels ? s->channels : 1))); }
                else if (!memcmp(d + p, "smpl", 4) && n >= 60 && rd32le(c + 28)) { s->loop_start = rd32le(c + 44); s->loop_end = rd32le(c + 48) + 1; }
                p += 8 + n + (n & 1); }
            ok = fmt_ok && s->pcm && s->channels >= 1 && s->channels <= 2; if (ok) s->kind = 2;
        }
    }
    if (ok && loop && !s->loop_end && s->kind == 2) { s->loop_start = 0; s->loop_end = s->frames; }
    if (!ok || s->rate <= 0) { free(s->data); s->data = NULL; s->kind = 0; plat_log("sound: unsupported data in slot %d", i); }
    else s->active = 1;
    SDL_UnlockAudioDevice(g_dev);
}
void plat_snd_stop(int i) { if (!g_dev || i < 0 || i >= SND_SLOTS) return; SDL_LockAudioDevice(g_dev); if (g_s[i].active) release(&g_s[i]); SDL_UnlockAudioDevice(g_dev); }
void plat_snd_fade(int i, int ms) {
    if (!g_dev || i < 0 || i >= SND_SLOTS) return; SDL_LockAudioDevice(g_dev);
    slot_t *s = &g_s[i]; if (s->active) { int n = ms * OUT_RATE / 1000; s->fade = s->vol / (n > 0 ? n : 1); }
    SDL_UnlockAudioDevice(g_dev);
}
bool plat_snd_playing(int i) { if (!g_dev || i < 0 || i >= SND_SLOTS) return false; return g_s[i].active && g_s[i].fade == 0; }
