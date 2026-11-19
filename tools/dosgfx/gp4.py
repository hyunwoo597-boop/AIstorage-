"""GP4 (16-colour image of the DOS release, SILK*.DAT) decoder - reimplementation of the engine's own routine.

header (big-endian u16): x, y, w-1, h-1, then 16 palette words (4-bit components at bits 12-15, 7-10, 2-5)
data: the picture is coded in 4-pixel-wide columns, top to bottom, left to right.  Each row of a column is either
  0 + 4 pixels, each an index into a move-to-front list kept per previous colour (unary code: n ones then a zero), or
  1 + a copy of N rows (4 pixels each) from an earlier position: same column (rows -16,-8,-6..-1) or k columns back (rows -8..+7).
"""
import struct, sys


class Bits:
    def __init__(s, d, p): s.d = d; s.p = p; s.left = 0; s.cur = 0
    def bit(s):
        if s.left == 0:
            s.cur = s.d[s.p] if s.p < len(s.d) else 0; s.p += 1; s.left = 8
        s.left -= 1
        return (s.cur >> s.left) & 1
    def bits(s, n):
        v = 0
        for _ in range(n): v = v << 1 | s.bit()
        return v


def decode(d):
    x, y, w1, h1 = struct.unpack('>4H', d[:8])
    w, h = w1 + 1, h1 + 1
    pal = []
    for i in range(16):
        v = struct.unpack('>H', d[8 + 2 * i:10 + 2 * i])[0]
        pal.append(((v >> 2) & 15, (v >> 7) & 15, (v >> 12) & 15))
    ncol = ((w + 7) >> 3) * 2                     # 4-pixel columns
    b = Bits(d, 40)
    mtf = [[(j + i) & 15 for i in range(16)] for j in range(17)]
    prev = 16
    cols = []
    for c in range(ncol):
        col = [None] * h; r = 0; prev = 16          # the colour context restarts in every column
        while r < h:
            if b.bit() == 0:                       # literal row of 4 pixels
                px = []
                for _ in range(4):
                    t = mtf[prev]; k = 0
                    while b.bit(): k += 1
                    v = t[k]
                    if k: del t[k]; t.insert(0, v)
                    px.append(v); prev = v
                col[r] = tuple(px); r += 1
                continue
            # copy
            if b.bit() == 0: back = 1
            elif b.bit() == 0: back = 0
            else:
                back = 2
                while b.bit(): back += 1
            if back == 0:
                a = b.bits(3) - 8
                if a <= -7: a = -16 if a == -8 else -8
            else:
                a = b.bits(4) - 8
            if b.bit() == 0: n = 2 + b.bit()
            elif b.bit() == 0: n = 4 + b.bits(2)
            elif b.bit() == 0: n = 8 + b.bits(3)
            else:
                n = 16 + b.bits(6)
                if n == 79: hi = b.bits(2); n = (hi << 8 | b.bits(8)) + 79
            src = cols[c - back] if back else col
            for i in range(n):
                if r >= h: break
                sr = r + a
                col[r] = src[sr] if 0 <= sr < h and src[sr] is not None else (0, 0, 0, 0)
                r += 1
        cols.append(col)
    img = bytearray(ncol * 4 * h)
    W = ncol * 4
    for c, col in enumerate(cols):
        for r in range(h):
            for k in range(4): img[r * W + c * 4 + k] = col[r][k]
    return dict(x=x, y=y, w=w, h=h, W=W, pal=pal, idx=bytes(img))


def to_png(g, path, order=(1, 2, 0)):
    from PIL import Image
    im = Image.frombytes('P', (g['W'], g['h']), g['idx'])
    p = []
    for c in g['pal']: p += [c[order[0]] * 17, c[order[1]] * 17, c[order[2]] * 17]
    im.putpalette(p + [0] * (768 - len(p)))
    im.crop((0, 0, g['w'], g['h'])).convert('RGB').save(path)


if __name__ == '__main__':
    g = decode(open(sys.argv[1], 'rb').read())
    print(g['x'], g['y'], g['w'], g['h'])
    to_png(g, sys.argv[2])
