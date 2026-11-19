/* Voice clips (Ogg Vorbis inside the Windows version's VOICE.ARC) on DSP channel 1, decoded a piece per frame with Tremor. */
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <tremor/ivorbisfile.h>
#include "voiceplay.h"

#define VCH 1
#define NVB 4
#define VB_FRAMES 4096
typedef struct { char name[20]; uint32_t off, size; } ArcEnt;
static ArcEnt *ents; static int nents;
static FILE *arc;
static ndspWaveBuf vb[NVB];
static int16_t *vbuf;
static uint8_t *clip; static size_t clip_len, clip_pos;
static OggVorbis_File vf; static int playing, eof, channels;
static float volume = 1.0f;

static size_t m_read(void *p, size_t sz, size_t n, void *ds) { (void)ds; size_t want = sz * n, left = clip_len - clip_pos; if (want > left) want = left; memcpy(p, clip + clip_pos, want); clip_pos += want; return sz ? want / sz : 0; }
static int m_seek(void *ds, ogg_int64_t off, int wh) { (void)ds; long np = wh == SEEK_SET ? (long)off : wh == SEEK_CUR ? (long)clip_pos + (long)off : (long)clip_len + (long)off; if (np < 0 || (size_t)np > clip_len) return -1; clip_pos = (size_t)np; return 0; }
static int m_close(void *ds) { (void)ds; return 0; }
static long m_tell(void *ds) { (void)ds; return (long)clip_pos; }

int vp_init(const char *gdir) {
    char p[300]; const char *nm[] = { "VOICE.ARC", "voice.arc", "Voice.arc", NULL };
    for (int i = 0; nm[i] && !arc; i++) { snprintf(p, sizeof p, "%s/%s", gdir, nm[i]); arc = fopen(p, "rb"); }
    if (!arc) return 0;
    uint32_t n = 0; uint8_t *t = NULL;
    if (fread(&n, 4, 1, arc) != 1 || n < 2 || n > 20000) goto bad;
    t = malloc((size_t)n * 28); ents = malloc((size_t)n * sizeof *ents);
    if (!t || !ents || fread(t, 28, n, arc) != n) goto bad;
    /* index entries: name[20], size ^ ks, offset ^ ko; the first file starts right after the index, the files are back to back */
    uint32_t s0, o0, s1, o1; memcpy(&s0, t + 20, 4); memcpy(&o0, t + 24, 4); memcpy(&s1, t + 48, 4); memcpy(&o1, t + 52, 4);
    uint32_t ko = o0 ^ (4 + n * 28), ks = s0 ^ ((o1 ^ ko) - (o0 ^ ko));
    for (uint32_t i = 0; i < n; i++) {
        memcpy(ents[i].name, t + i * 28, 20); ents[i].name[19] = 0;
        uint32_t s, o; memcpy(&s, t + i * 28 + 20, 4); memcpy(&o, t + i * 28 + 24, 4);
        ents[i].size = s ^ ks; ents[i].off = o ^ ko;
    }
    (void)s1; free(t); nents = (int)n;
    vbuf = linearAlloc((size_t)NVB * VB_FRAMES * 2 * 2);
    if (!vbuf) goto bad;
    memset(vb, 0, sizeof vb);
    return nents;
bad:
    free(t); free(ents); ents = NULL; nents = 0; fclose(arc); arc = NULL; return 0;
}

void vp_stop(void) {
    if (!playing) return;
    ndspChnWaveBufClear(VCH);
    ov_clear(&vf); free(clip); clip = NULL; playing = 0;
}

int vp_play(const char *name) {
    if (!nents) return 0;
    vp_stop();
    char want[24]; snprintf(want, sizeof want, "%s.OGG", name);
    int e = -1; for (int i = 0; i < nents; i++) if (!strcasecmp(ents[i].name, want)) { e = i; break; }
    if (e < 0 || ents[e].size > 4u << 20) return 0;
    clip = malloc(ents[e].size); if (!clip) return 0;
    if (fseek(arc, (long)ents[e].off, SEEK_SET) || fread(clip, 1, ents[e].size, arc) != ents[e].size) { free(clip); clip = NULL; return 0; }
    clip_len = ents[e].size; clip_pos = 0;
    ov_callbacks cb = { m_read, m_seek, m_close, m_tell };
    if (ov_open_callbacks(NULL, &vf, NULL, 0, cb) != 0) { free(clip); clip = NULL; return 0; }
    vorbis_info *vi = ov_info(&vf, -1);
    channels = vi->channels; if (channels < 1 || channels > 2) { ov_clear(&vf); free(clip); clip = NULL; return 0; }
    ndspChnReset(VCH);
    ndspChnSetInterp(VCH, NDSP_INTERP_LINEAR);
    ndspChnSetRate(VCH, (float)vi->rate);
    ndspChnSetFormat(VCH, channels == 2 ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);
    float mix[12] = { 0 }; mix[0] = mix[1] = volume; ndspChnSetMix(VCH, mix);
    for (int i = 0; i < NVB; i++) { vb[i].status = NDSP_WBUF_FREE; vb[i].data_pcm16 = vbuf + (size_t)i * VB_FRAMES * 2; }
    playing = 1; eof = 0;
    vp_pump();
    return 1;
}

void vp_pump(void) {
    if (!playing) return;
    int busy = 0;
    for (int i = 0; i < NVB; i++) {
        ndspWaveBuf *w = &vb[i];
        if (w->status == NDSP_WBUF_QUEUED || w->status == NDSP_WBUF_PLAYING) { busy++; continue; }
        if (eof) continue;
        int fr = 0, bs; char *dst = (char *)w->data_pcm16; int bytes = VB_FRAMES * 2 * channels;
        while (fr * 2 * channels < bytes) {
            long r = ov_read(&vf, dst + fr * 2 * channels, bytes - fr * 2 * channels, &bs);
            if (r <= 0) { if (r == OV_HOLE) continue; eof = 1; break; }
            fr += (int)(r / (2 * channels));
        }
        if (fr > 0) { w->nsamples = (u32)fr; DSP_FlushDataCache(w->data_pcm16, (u32)fr * 2 * channels); ndspChnWaveBufAdd(VCH, w); busy++; }
    }
    if (eof && !busy) { ov_clear(&vf); free(clip); clip = NULL; playing = 0; }
}

int vp_playing(void) { return playing; }
void vp_set_volume(float v) { volume = v; if (playing) { float mix[12] = { 0 }; mix[0] = mix[1] = v; ndspChnSetMix(VCH, mix); } }
