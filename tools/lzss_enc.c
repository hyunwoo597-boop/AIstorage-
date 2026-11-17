/* LZSS encoder matching the engine decoder: 4096-byte ring, writes start at 0xFEE, flag bits LSB first (1 = literal),
   match = 2 bytes: lo = offset & 0xFF, hi = (offset >> 4 & 0xF0) | (len - 3), len 3..18. Only references bytes already
   written (the zero-filled ring start is not relied on). Greedy, hash chains.  usage: lzss_enc in out */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int c, char **v) {
    FILE *f = fopen(v[1], "rb"); fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
    unsigned char *d = malloc(n + 3); fread(d, 1, n, f); fclose(f); memset(d + n, 0, 3);
    unsigned char *o = malloc(n + n / 8 + 16); long op = 0;
    int *head = malloc(sizeof(int) * 65536), *prev = malloc(sizeof(int) * (n + 1));
    for (int i = 0; i < 65536; i++) head[i] = -1;
    long i = 0;
    while (i < n) {
        long fp = op++; unsigned char flags = 0;
        for (int b = 0; b < 8 && i < n; b++) {
            int best = 0; long bpos = 0;
            if (i + 3 <= n) {
                unsigned h = (d[i] << 8 | d[i + 1]) ^ (d[i + 2] * 0x9E37u >> 8 & 0xFFFF); h &= 0xFFFF;
                int tries = 64;
                for (long p = head[h]; p >= 0 && i - p < 4096 - 18 && tries--; p = prev[p]) {
                    int l = 0; while (l < 18 && i + l < n && d[p + l] == d[i + l]) l++;
                    if (l > best) { best = l; bpos = p; if (l == 18) break; }
                }
            }
            int adv = best >= 3 ? best : 1;
            if (best >= 3) { int off = (int)((bpos + 0xFEE) & 0xFFF); o[op++] = off & 0xFF; o[op++] = (off >> 4 & 0xF0) | (best - 3); }
            else { flags |= 1 << b; o[op++] = d[i]; }
            for (int k = 0; k < adv; k++, i++) if (i + 3 <= n) { unsigned h = (d[i] << 8 | d[i + 1]) ^ (d[i + 2] * 0x9E37u >> 8 & 0xFFFF); h &= 0xFFFF; prev[i] = head[h]; head[h] = (int)i; }
        }
        o[fp] = flags;
    }
    f = fopen(v[2], "wb"); fwrite(o, 1, op, f); fclose(f); printf("%ld -> %ld\n", n, op); return 0;
}
