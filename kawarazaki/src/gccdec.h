/* elf AI5WIN "GCC" image decoder (G24n / G24m / R24n / R24m)
 * Clean reimplementation. Output: bottom-up BGR24 (w*h*3, no row padding) + optional 8-bit alpha (w*h, bottom-up). */
#ifndef GCCDEC_H
#define GCCDEC_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef struct { int x, y, w, h; uint8_t *bgr; int has_alpha; int ax, ay, aw, ah; uint8_t *alpha; } gcc_image;

typedef struct { const uint8_t *p; uint32_t pos; } gcc_bits;
static inline int gcc_bit(gcc_bits *b) {
    uint32_t i = b->pos++; const uint8_t *q = b->p + (i >> 5) * 4;
    uint32_t wv = q[0] | q[1] << 8 | q[2] << 16 | (uint32_t)q[3] << 24;
    return (wv >> (i & 31)) & 1;
}
static inline uint32_t gcc_gamma(gcc_bits *b) {
    int n = 0; uint32_t v = 1;
    while (!gcc_bit(b)) n++;
    while (n--) v = v * 2 + gcc_bit(b);
    return v;
}
static void gcc_lzss(const uint8_t *src, const uint8_t *end, uint8_t *dst, size_t n) {
    uint8_t win[4096]; unsigned wp = 0xFEE; size_t o = 0; memset(win, 0, sizeof win);
    while (o < n && src < end) {
        unsigned fl = *src++;
        for (int k = 0; k < 8 && o < n && src < end; k++) {
            if (fl >> k & 1) { uint8_t c = *src++; dst[o++] = c; win[wp] = c; wp = (wp + 1) & 0xFFF; }
            else { if (src + 1 >= end) return; unsigned lo = src[0], hi = src[1]; src += 2;
                unsigned off = lo | (hi & 0xF0) << 4, len = (hi & 0xF) + 3;
                while (len-- && o < n) { uint8_t c = win[off++ & 0xFFF]; dst[o++] = c; win[wp] = c; wp = (wp + 1) & 0xFFF; } }
        }
    }
}
static inline int gcc_mtf_find(const uint8_t *t, uint8_t c) { for (int i = 0; i < 16; i++) if (t[i] == c) return i; return 0xFF; }
static inline void gcc_mtf_push(uint8_t *t, int idx, uint8_t c) {
    if (!idx) return; idx &= 15; for (int k = idx; k > 0; k--) t[k] = t[k - 1]; t[0] = c;
}
/* MTF/run stage: fills buf[0..count) */
static const uint8_t *gcc_mtf_stage(gcc_bits *b, const uint8_t *bp, uint8_t *buf, uint32_t count) {
    uint8_t ta[16], tb[16], last = 0xFF, c; uint32_t pos = 0; int idx;
    for (int i = 0; i < 16; i++) ta[i] = tb[i] = i;
    while (pos < count) {
        if (gcc_bit(b)) {
            uint32_t len = gcc_gamma(b);
            if (gcc_bit(b)) { c = ta[0]; idx = 0; }
            else if (gcc_bit(b)) { idx = gcc_gamma(b); c = ta[idx & 15]; }
            else if (gcc_bit(b)) { uint32_t v = gcc_gamma(b); c = gcc_bit(b) ? (uint8_t)(last - v) : (uint8_t)(last + v); idx = gcc_mtf_find(ta, c); }
            else { c = *bp++; idx = gcc_mtf_find(ta, c); }
            gcc_mtf_push(ta, idx, c);
            if (len > count - pos) len = count - pos;
            memset(buf + pos, c, len); pos += len;
            idx = gcc_mtf_find(tb, c);
        } else {
            if (gcc_bit(b)) { idx = gcc_gamma(b); c = tb[idx & 15]; }
            else if (gcc_bit(b)) { uint32_t v = gcc_gamma(b); c = gcc_bit(b) ? (uint8_t)(last - v) : (uint8_t)(last + v); idx = gcc_mtf_find(tb, c); }
            else { c = *bp++; idx = gcc_mtf_find(tb, c); }
            buf[pos++] = c;
        }
        gcc_mtf_push(tb, idx, c); last = c;
    }
    return bp;
}
static void gcc_unbwt(const uint8_t *buf, uint32_t n, uint32_t index, uint8_t *out) {
    uint16_t cnt[256], start[256]; static uint16_t T[65536]; uint16_t acc = 0;
    memset(cnt, 0, sizeof cnt);
    for (uint32_t i = 0; i < n; i++) cnt[buf[i]]++;
    for (int c = 0; c < 256; c++) { start[c] = acc; acc += cnt[c]; cnt[c] = 0; }
    for (uint32_t i = 0; i < n; i++) { uint8_t c = buf[i]; T[(uint16_t)(start[c] + cnt[c]++)] = i; }
    uint32_t p = T[index & 0xFFFF];
    for (uint32_t i = 0; i < n; i++) { out[i] = buf[p]; p = T[p]; }
}
static void gcc_r24(const uint8_t *data, uint32_t byteoff, uint8_t *out, uint32_t total) {
    gcc_bits b = { data, 0 }; const uint8_t *bp = data + byteoff; uint32_t done = 0;
    uint8_t *buf = malloc(0x10010);
    while (done < total) {
        uint32_t n = total - done; if (n > 0xFFFF) n = 0xFFFF;
        if (gcc_bit(&b)) {
            bp = gcc_mtf_stage(&b, bp, buf, n + 2);
            gcc_unbwt(buf + 2, n, buf[0] | buf[1] << 8, out + done);
        } else {
            uint32_t c = 0; uint8_t *o = out + done;
            while (c < n) {
                if (gcc_bit(&b)) { uint32_t r = gcc_gamma(&b);
                    while (r-- && c < n) { o[c] = bp[0]; o[c + 1] = bp[1]; o[c + 2] = bp[2]; c += 3; } bp += 3; }
                else { o[c] = bp[0]; o[c + 1] = bp[1]; o[c + 2] = bp[2]; bp += 3; c += 3; }
            }
        }
        done += n;
    }
    free(buf);
}
static void gcc_mask(const uint8_t *bits, const uint8_t *bytes, uint8_t *out, uint32_t total) {
    gcc_bits b = { bits, 0 }; uint32_t c = 0;
    while (c < total) {
        if (gcc_bit(&b)) { uint32_t r = gcc_gamma(&b); uint8_t v = *bytes++; if (r > total - c) r = total - c; memset(out + c, v, r); c += r; }
        else out[c++] = *bytes++;
    }
}
static inline uint32_t gcc_u32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static inline int gcc_u16(const uint8_t *p) { return p[0] | p[1] << 8; }
/* returns 0 on success */
static int gcc_decode(const uint8_t *f, size_t len, gcc_image *im) {
    if (len < 0x14 || f[1] != '2' || f[2] != '4') return -1;
    int rle = f[0] == 'R', m = f[3] == 'm';
    if ((f[0] != 'R' && f[0] != 'G') || (f[3] != 'n' && f[3] != 'm')) return -1;
    memset(im, 0, sizeof *im);
    im->x = gcc_u16(f + 4); im->y = gcc_u16(f + 6); im->w = gcc_u16(f + 8); im->h = gcc_u16(f + 10);
    uint32_t total = (uint32_t)im->w * im->h * 3, stride = (im->w * 3 + 3) & ~3u;
    const uint8_t *data = f + (m ? 0x20 : 0x14); uint32_t off = m ? 0 : gcc_u32(f + 0x10);   /* G24m (this game): colour LZSS right after the 0x20 header */
    im->bgr = malloc(total + 16);
    if (rle) gcc_r24(data, off, im->bgr, total);
    else {
        uint8_t *tmp = malloc((size_t)stride * im->h + 16);
        gcc_lzss(data + off, f + len, tmp, (size_t)stride * im->h);
        for (int y = 0; y < im->h; y++) memcpy(im->bgr + (size_t)y * im->w * 3, tmp + (size_t)y * stride, im->w * 3);
        free(tmp);
    }
    if (m) {
        const uint8_t *mh = f + 0x14;
        im->has_alpha = 1; im->ax = gcc_u16(mh); im->ay = gcc_u16(mh + 2); im->aw = gcc_u16(mh + 4); im->ah = gcc_u16(mh + 6);
        const uint8_t *bits = data + gcc_u32(f + 0xC);
        im->alpha = malloc((size_t)im->aw * im->ah + 16);
        gcc_mask(bits, bits + gcc_u32(mh + 8), im->alpha, (uint32_t)im->aw * im->ah);
    }
    return 0;
}
#endif
