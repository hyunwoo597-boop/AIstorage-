/* 3DS sound: ndsp output, Ogg Vorbis (Tremor) or PCM16 WAV decoded from memory on a worker thread.
 * Needs the DSP firmware dump (sdmc:/3ds/dspfirm.cdc); without it the game runs silently. */
#include <3ds.h>
#include <tremor/ivorbisfile.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ai5.h"

#define NBUF 3
#define BUF_FRAMES 4096

typedef struct {
    int active;                    /* 1 while decoding/playing */
    uint8_t *data; size_t len;     /* whole file in memory (owned) */
    const uint8_t *src; size_t src_len, src_pos;   /* vorbis stream inside data */
    int is_vorbis; OggVorbis_File vf;
    int is_adpcm; const uint8_t *ad; uint32_t ad_bs, ad_frames; int32_t ad_blk; int16_t *ad_buf;   /* AI5A (IMA ADPCM) */
    const int16_t *pcm; uint32_t pcm_frames;
    int channels, rate;
    uint32_t loop_start, loop_end; /* frames; loop_end 0 = no loop */
    uint64_t pos;                  /* current frame */
    int eof;
    int16_t *buf[NBUF]; ndspWaveBuf wb[NBUF];
    float vol, fade;               /* fade = volume step per tick (0 = none) */
} slot_t;

static slot_t g_slot[SND_SLOTS];
static LightLock g_lock;
static Thread g_thread;
static volatile int g_run;
static int g_ok;

static size_t mem_read(void *ptr, size_t size, size_t n, void *ds) {
    slot_t *s = ds; size_t want = size * n, left = s->src_len - s->src_pos; if (want > left) want = left;
    memcpy(ptr, s->src + s->src_pos, want); s->src_pos += want; return size ? want / size : 0;
}
static int mem_seek(void *ds, ogg_int64_t off, int whence) {
    slot_t *s = ds; int64_t p = whence == SEEK_SET ? off : whence == SEEK_CUR ? (int64_t)s->src_pos + off : (int64_t)s->src_len + off;
    if (p < 0 || p > (int64_t)s->src_len) return -1; s->src_pos = (size_t)p; return 0;
}
static int mem_close(void *ds) { (void)ds; return 0; }
static long mem_tell(void *ds) { return (long)((slot_t *)ds)->src_pos; }

static uint32_t rd32le(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* AI5A = music/voice packed by the HTML CIA maker (IMA ADPCM, 4 bits/sample):
   "AI5A" u32 loop_start u32 loop_end(excl, 0 = none) u32 rate u16 channels u16 block_frames u32 total_frames, then blocks;
   a block = per channel {i16 predictor, u8 step index, u8 0}, then per channel block_frames/2 bytes (low nibble first). */
static const int16_t ima_step[89] = { 7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,118,130,143,157,173,190,209,230,
    253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,
    5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,27086,29794,32767 };
static const int8_t ima_idx[16] = { -1,-1,-1,-1,2,4,6,8,-1,-1,-1,-1,2,4,6,8 };
static void adpcm_block(slot_t *s, int32_t blk) {
    int ch = s->channels; uint32_t bs = s->ad_bs; const uint8_t *b = s->ad + (size_t)blk * (4 * ch + bs / 2 * ch);
    for (int c = 0; c < ch; c++) {
        int pred = (int16_t)(b[c * 4] | b[c * 4 + 1] << 8), idx = b[c * 4 + 2]; if (idx > 88) idx = 88;
        const uint8_t *nb = b + 4 * ch + c * (bs / 2);
        for (uint32_t i = 0; i < bs; i++) { int n = (nb[i >> 1] >> ((i & 1) * 4)) & 15, st = ima_step[idx], d = st >> 3;
            if (n & 4) d += st; if (n & 2) d += st >> 1; if (n & 1) d += st >> 2; if (n & 8) pred -= d; else pred += d;
            if (pred > 32767) pred = 32767; if (pred < -32768) pred = -32768; idx += ima_idx[n]; if (idx < 0) idx = 0; if (idx > 88) idx = 88;
            s->ad_buf[i * ch + c] = (int16_t)pred; } }
    s->ad_blk = blk;
}
static void slot_release(int i) {          /* caller holds the lock */
    slot_t *s = &g_slot[i];
    ndspChnWaveBufClear(i);
    if (s->is_vorbis && s->active) ov_clear(&s->vf);
    free(s->ad_buf); s->ad_buf = NULL; s->is_adpcm = 0;
    free(s->data); s->data = NULL; s->active = 0; s->is_vorbis = 0; s->eof = 0; s->fade = 0;
    for (int b = 0; b < NBUF; b++) s->wb[b].status = NDSP_WBUF_FREE;
}
/* decode up to BUF_FRAMES frames into out; returns frames written */
static int slot_decode(slot_t *s, int16_t *out) {
    int got = 0, empty = 0;
    while (got < BUF_FRAMES) {
        int want = BUF_FRAMES - got;
        if (s->loop_end && s->pos + want > s->loop_end) want = (int)(s->loop_end - s->pos);
        int n = 0;
        if (want > 0) {
            if (s->is_vorbis) {
                int bs; long r = ov_read(&s->vf, (char *)(out + got * s->channels), want * s->channels * 2, &bs);
                if (r < 0) r = 0;             /* decode error: treated like end of data */
                n = (int)(r / (2 * s->channels));
            } else if (s->is_adpcm) {
                while (n < want && s->pos + n < s->ad_frames) {
                    uint64_t p = s->pos + n; int32_t blk = (int32_t)(p / s->ad_bs); if (blk != s->ad_blk) adpcm_block(s, blk);
                    uint32_t o = (uint32_t)(p % s->ad_bs), k = s->ad_bs - o; if (k > (uint32_t)(want - n)) k = want - n;
                    if (p + k > s->ad_frames) k = (uint32_t)(s->ad_frames - p);
                    memcpy(out + (got + n) * s->channels, s->ad_buf + o * s->channels, (size_t)k * s->channels * 2); n += k; }
            } else {
                uint32_t left = s->pos < s->pcm_frames ? s->pcm_frames - (uint32_t)s->pos : 0; n = want < (int)left ? want : (int)left;
                memcpy(out + got * s->channels, s->pcm + s->pos * s->channels, (size_t)n * s->channels * 2);
            }
        }
        got += n; s->pos += n;
        if (n == 0 || (s->loop_end && s->pos >= s->loop_end)) {
            if (!s->loop_end || (n == 0 && ++empty > 2)) { s->eof = 1; break; }
            s->pos = s->loop_start;           /* loop */
            if (s->is_vorbis) ov_pcm_seek(&s->vf, s->loop_start);
        }
    }
    return got;
}
static void worker(void *arg) {
    (void)arg;
    while (g_run) {
        LightLock_Lock(&g_lock);
        for (int i = 0; i < SND_SLOTS; i++) {
            slot_t *s = &g_slot[i]; if (!s->active) continue;
            if (s->fade > 0) {
                s->vol -= s->fade; if (s->vol <= 0) { slot_release(i); continue; }
                float mix[12] = { s->vol, s->vol }; ndspChnSetMix(i, mix);
            }
            int busy = 0;
            for (int b = 0; b < NBUF; b++) {
                ndspWaveBuf *w = &s->wb[b];
                if (w->status == NDSP_WBUF_QUEUED || w->status == NDSP_WBUF_PLAYING) { busy++; continue; }
                if (s->eof) continue;
                int n = slot_decode(s, s->buf[b]); if (n <= 0) continue;
                memset(w, 0, sizeof *w); w->data_vaddr = s->buf[b]; w->nsamples = n;
                DSP_FlushDataCache(s->buf[b], (u32)n * s->channels * 2);
                ndspChnWaveBufAdd(i, w); busy++;
            }
            if (s->eof && !busy) slot_release(i);
        }
        LightLock_Unlock(&g_lock);
        svcSleepThread(4 * 1000 * 1000);
    }
}
void snd_init(void) {
    if (R_FAILED(ndspInit())) { plat_log("ndspInit failed: put dspfirm.cdc in sdmc:/3ds/ (sound disabled)"); return; }
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    LightLock_Init(&g_lock);
    for (int i = 0; i < SND_SLOTS; i++) for (int b = 0; b < NBUF; b++) g_slot[i].buf[b] = linearAlloc(BUF_FRAMES * 2 * sizeof(int16_t));
    s32 prio = 0x30; svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    g_run = 1; g_thread = threadCreate(worker, NULL, 64 * 1024, prio - 1, -2, false);
    g_ok = g_thread != NULL;
    if (!g_ok) plat_log("sound thread failed");
}
void snd_exit(void) {
    if (!g_ok) return;
    g_run = 0; threadJoin(g_thread, U64_MAX); threadFree(g_thread);
    for (int i = 0; i < SND_SLOTS; i++) { slot_release(i); for (int b = 0; b < NBUF; b++) linearFree(g_slot[i].buf[b]); }
    ndspExit(); g_ok = 0;
}
void plat_snd_play(int i, void *data, size_t len, int volume) {
    if (!g_ok || i < 0 || i >= SND_SLOTS) { free(data); return; }
    LightLock_Lock(&g_lock);
    slot_release(i);
    slot_t *s = &g_slot[i]; const uint8_t *d = data;
    s->data = data; s->len = len; s->loop_start = s->loop_end = 0; s->pos = 0; s->eof = 0; s->fade = 0;
    s->vol = volume / 255.0f;
    int ok = 0;
    if (len > 24 && !memcmp(d, "AI5A", 4)) {
        s->loop_start = rd32le(d + 4); s->loop_end = rd32le(d + 8); s->rate = (int)rd32le(d + 12); s->channels = d[16] | d[17] << 8;
        s->ad_bs = d[18] | d[19] << 8; s->ad_frames = rd32le(d + 20); s->ad = d + 24; s->ad_blk = -1;
        uint64_t need = s->ad_bs ? ((uint64_t)s->ad_frames + s->ad_bs - 1) / s->ad_bs * (4 * s->channels + s->ad_bs / 2 * s->channels) : 0;
        if (s->channels >= 1 && s->channels <= 2 && s->ad_bs >= 2 && !(s->ad_bs & 1) && need <= len - 24 && (s->ad_buf = malloc((size_t)s->ad_bs * s->channels * 2))) { s->is_adpcm = 1; ok = 1; }
        if (s->loop_end > s->ad_frames) s->loop_end = s->ad_frames;
    } else if (len > 16 && !memcmp(d, "AI5S", 4)) { s->loop_start = rd32le(d + 4); s->loop_end = rd32le(d + 8); d += 16; len -= 16; }
    if (ok) { }
    else if (len > 4 && !memcmp(d, "OggS", 4)) {
        s->src = d; s->src_len = len; s->src_pos = 0;
        ov_callbacks cb = { mem_read, mem_seek, mem_close, mem_tell };
        if (ov_open_callbacks(s, &s->vf, NULL, 0, cb) == 0) {
            vorbis_info *vi = ov_info(&s->vf, -1); s->channels = vi->channels; s->rate = vi->rate; s->is_vorbis = 1; ok = s->channels >= 1 && s->channels <= 2;
            if (!ok) ov_clear(&s->vf);
        }
    } else if (len > 44 && !memcmp(d, "RIFF", 4)) {          /* PCM16 WAV straight from MUSIC.ARC */
        size_t p = 12; int fmt_ok = 0;
        while (p + 8 <= len) {
            uint32_t n = rd32le(d + p + 4); const uint8_t *c = d + p + 8;
            if (!memcmp(d + p, "fmt ", 4) && n >= 16) { s->channels = c[2] | c[3] << 8; s->rate = (int)rd32le(c + 4); fmt_ok = (c[0] == 1 && c[14] == 16); }
            else if (!memcmp(d + p, "data", 4)) { s->pcm = (const int16_t *)c; s->pcm_frames = (uint32_t)((n < len - p - 8 ? n : len - p - 8) / (2 * (s->channels ? s->channels : 1))); }
            else if (!memcmp(d + p, "smpl", 4) && n >= 60 && rd32le(c + 28)) { s->loop_start = rd32le(c + 44); s->loop_end = rd32le(c + 48) + 1; }
            p += 8 + n + (n & 1);
        }
        ok = fmt_ok && s->pcm && s->channels >= 1 && s->channels <= 2;
    }
    if (!ok) { free(s->data); s->data = NULL; LightLock_Unlock(&g_lock); plat_log("sound: unsupported data in slot %d", i); return; }
    ndspChnReset(i);
    ndspChnSetInterp(i, NDSP_INTERP_LINEAR);
    ndspChnSetRate(i, (float)s->rate);
    ndspChnSetFormat(i, s->channels == 2 ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);
    float mix[12] = { s->vol, s->vol }; ndspChnSetMix(i, mix);
    s->active = 1;
    LightLock_Unlock(&g_lock);
}
void plat_snd_stop(int i) {
    if (!g_ok || i < 0 || i >= SND_SLOTS) return;
    LightLock_Lock(&g_lock); if (g_slot[i].active) slot_release(i); LightLock_Unlock(&g_lock);
}
void plat_snd_fade(int i, int ms) {
    if (!g_ok || i < 0 || i >= SND_SLOTS) return;
    LightLock_Lock(&g_lock);
    slot_t *s = &g_slot[i]; if (s->active) { int ticks = ms / 4; s->fade = s->vol / (ticks > 0 ? ticks : 1); }
    LightLock_Unlock(&g_lock);
}
bool plat_snd_playing(int i) {
    if (!g_ok || i < 0 || i >= SND_SLOTS) return false;
    return g_slot[i].active && g_slot[i].fade == 0;
}
