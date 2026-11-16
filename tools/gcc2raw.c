#include <stdio.h>
#include "gccdec.h"
int main(int c, char **v) {
    FILE *f = fopen(v[1], "rb"); fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
    uint8_t *d = malloc(n + 64); fread(d, 1, n, f); memset(d + n, 0, 64); fclose(f);
    gcc_image im; if (gcc_decode(d, n, &im)) return 1;
    printf("%.4s %d %d %d %d a=%d %d %d %d %d\n", d, im.x, im.y, im.w, im.h, im.has_alpha, im.ax, im.ay, im.aw, im.ah);
    f = fopen(v[2], "wb"); fwrite(im.bgr, 1, im.w * im.h * 3, f); fclose(f);
    if (im.has_alpha && c > 3) { f = fopen(v[3], "wb"); fwrite(im.alpha, 1, im.aw * im.ah, f); fclose(f); }
    return 0;
}
