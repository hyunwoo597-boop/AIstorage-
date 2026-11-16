#!/usr/bin/env python3
"""홈 메뉴 배너를 '오려 세운 조각' 입체 무대로 만든다 (외부 모듈 없이 동작 — CI 에서 실행).

    python3 tools/banner_diorama.py <평면 banner.bnr> <조각 모음 그림.png> <배치표.json> <출력.cgfx>
    그다음: bannertool makebanner -ci <출력.cgfx> -a banner.wav -o banner.bnr

bannertool 이 만든 평면 배너의 모델(사각형 한 장 + 256x128 RGBA4444 그림)을 틀로 쓴다.
 - 그림: 조각 모음(아틀라스, RGBA PNG)을 RGBA4444 로 바꿔 파일 끝에 덧붙이고 TXOB 의 크기·위치를 고친다.
 - 모양: 조각마다 사각형 하나를 제 깊이(z)에 세운다. 뒤에서 앞 순서로 그려 반투명 가장자리가 맞게 섞인다.
재질·셰이더 등 나머지는 그대로 둔다. 배치표는 tools/gen_banner_layers.py 가 만든다.
"""
import json, struct, sys, zlib


def u32(b, o): return struct.unpack_from('<I', b, o)[0]


def lz11_decompress(s):
    assert s[0] == 0x11
    n = s[1] | s[2] << 8 | s[3] << 16
    o = bytearray(); i = 4
    while len(o) < n:
        f = s[i]; i += 1
        for b in range(8):
            if len(o) >= n: break
            if f & (0x80 >> b):
                t = s[i] >> 4
                if t == 0: ln = ((s[i] & 15) << 4 | s[i+1] >> 4) + 0x11; disp = (s[i+1] & 15) << 8 | s[i+2]; i += 3
                elif t == 1: ln = ((s[i] & 15) << 12 | s[i+1] << 4 | s[i+2] >> 4) + 0x111; disp = (s[i+2] & 15) << 8 | s[i+3]; i += 4
                else: ln = t + 1; disp = (s[i] & 15) << 8 | s[i+1]; i += 2
                for _ in range(ln): o.append(o[-disp-1])
            else: o.append(s[i]); i += 1
    return bytes(o)


def read_rgba_png(path):
    """8비트 RGBA PNG → (너비, 높이, 행 목록[bytes, 4바이트/화소])"""
    d = open(path, 'rb').read()
    assert d[:8] == b'\x89PNG\r\n\x1a\n'
    o = 8; idat = b''; w = h = 0
    while o < len(d):
        n, t = struct.unpack_from('>I4s', d, o)
        c = d[o+8:o+8+n]; o += 12 + n
        if t == b'IHDR':
            w, h, bd, ct, _, _, il = struct.unpack('>IIBBBBB', c)
            assert (bd, ct, il) == (8, 6, 0), '조각 모음은 8비트 RGBA PNG 여야 합니다'
        elif t == b'IDAT': idat += c
    raw = zlib.decompress(idat); rows = []; prev = bytearray(w * 4); st = w * 4 + 1
    for y in range(h):
        f = raw[y * st]; line = bytearray(raw[y * st + 1:(y + 1) * st])
        for x in range(w * 4):
            a = line[x-4] if x >= 4 else 0; b = prev[x]; c = prev[x-4] if x >= 4 else 0
            if f == 1: line[x] = (line[x] + a) & 255
            elif f == 2: line[x] = (line[x] + b) & 255
            elif f == 3: line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c; pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(bytes(line)); prev = line
    return w, h, rows


BAYER = [0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5]


def encode_rgba4444(w, h, rows, dither=True):
    """bannertool 과 같은 배열: 8x8 타일(Z 순서), 값 = R<<12|G<<8|B<<4|A, 위쪽 행부터. 색은 흩뿌려 띠를 줄인다."""
    out = bytearray(w * h * 2)
    for y in range(h):
        row = rows[y]
        for x in range(w):
            r, g, b, a = row[x*4:x*4+4]
            if dither:
                t = BAYER[(y & 3) * 4 + (x & 3)] + 0.5          # 0.5 .. 15.5
                r, g, b = [min(15, int(v * 15 / 255 + t / 16)) for v in (r, g, b)]
            else:
                r, g, b = r >> 4, g >> 4, b >> 4
            m = 0
            for k in range(3): m |= ((x >> k) & 1) << (2 * k) | ((y >> k) & 1) << (2 * k + 1)
            struct.pack_into('<H', out, (((y >> 3) * (w >> 3) + (x >> 3)) * 64 + m) * 2, r << 12 | g << 8 | b << 4 | (a >> 4))
    return bytes(out)


def find_parts(g):
    """틀에서 고칠 자리들을 찾는다. 구조가 예상과 다르면 멈춘다."""
    assert g[:4] == b'CGFX' and u32(g, 0x0c) == len(g), 'CGFX 머리'
    imag = 0x14 + u32(g, 0x18)
    assert g[imag:imag+4] == b'IMAG' and imag + u32(g, imag + 4) == len(g), 'IMAG 가 마지막 블록이 아님'
    shapes = []; o = g.find(b'SOBJ')
    while o >= 0:
        if u32(g, o - 4) == 0x10000001: shapes.append(o)
        o = g.find(b'SOBJ', o + 4)
    assert len(shapes) == 1, '셰이프가 하나가 아님'
    s = shapes[0]
    fd = next((p for p in range(s, s + 0x200, 4) if u32(g, p) == 0x1401 and u32(g, p + 8) == 6), None)
    assert fd, '색인 기술자'
    io = fd + 12 + u32(g, fd + 12)
    assert list(g[io:io+6]) == [0, 1, 2, 1, 3, 2], '색인 내용'
    vb = next((p for p in range(fd, fd + 0x100, 4) if u32(g, p) == 0x40000002), None)
    assert vb and u32(g, vb + 0x14) == 0x50 and u32(g, vb + 0x24) == 20 and u32(g, vb + 0x28) == 2, '정점 버퍼 형식'
    vo = vb + 0x18 + u32(g, vb + 0x18)
    q = [struct.unpack_from('<5f', g, vo + i * 20) for i in range(4)]
    # 원래 사각형: 왼쪽 아래 (u,v)=(0,0), 위가 v=1 이어야 아래 계산이 맞다
    assert q[0][3:] == (0.0, 0.0) and q[3][3:] == (1.0, 1.0) and q[0][1] < q[3][1], '원래 사각형의 UV 방향'
    obb = s + 0x18 + u32(g, s + 0x18)
    assert u32(g, obb) == 0x80000000, '경계 상자'
    # 그림 객체: 256x128, 0x10000 바이트
    tx = None; o = g.find(b'TXOB')
    while o >= 0:
        if (u32(g, o + 0x14), u32(g, o + 0x18), u32(g, o + 0x38), u32(g, o + 0x3c), u32(g, o + 0x40)) == (128, 256, 128, 256, 0x10000): tx = o
        o = g.find(b'TXOB', o + 4)
    assert tx, '그림 객체(TXOB)'
    assert u32(g, tx + 0x1c) == 0x6752 and u32(g, tx + 0x20) == 0x8033, '그림 형식이 RGBA4444 가 아님'
    return dict(idx_cnt=fd + 8, idx_ptr=fd + 12, vlen=vb + 0x14, vptr=vb + 0x18, obb_size=obb + 0x34, tx=tx, imag=imag)


def build(cgfx, atlas, layout):
    g = bytearray(cgfx); f = find_parts(g)
    w, h, rows = atlas
    assert [w, h] == layout['atlas'] and w & (w - 1) == 0 and h & (h - 1) == 0 and 8 <= w <= 1024 and 8 <= h <= 1024, '조각 모음 크기는 2의 거듭제곱'
    layers = sorted(layout['layers'], key=lambda l: l['z'])           # 뒤 → 앞
    assert len(layers) * 4 <= 256
    verts = bytearray(); idx = bytearray(); zmax = 0.0
    for n, l in enumerate(layers):
        x0, y0, x1, y1 = l['quad']; ax, ay, aw, ah = l['uv']; z = float(l['z']); zmax = max(zmax, abs(z))
        u0, u1 = (ax + 0.25) / w, (ax + aw - 0.25) / w
        vt, vb_ = 1 - (ay + 0.25) / h, 1 - (ay + ah - 0.25) / h        # 그림 위쪽 행이 v=1
        for (x, y, u, v) in ((x0, y0, u0, vb_), (x1, y0, u1, vb_), (x0, y1, u0, vt), (x1, y1, u1, vt)):
            verts += struct.pack('<5f', x, y, z, u, v)
        b = n * 4
        idx += bytes([b, b + 1, b + 2, b + 1, b + 3, b + 2])
    tex = encode_rgba4444(w, h, rows)

    def append(data, align):
        while len(g) % align: g.append(0)
        o = len(g); g.extend(data); return o
    io = append(idx, 4); vo = append(verts, 4); to = append(tex, 0x80)
    struct.pack_into('<I', g, f['idx_cnt'], len(idx)); struct.pack_into('<I', g, f['idx_ptr'], io - f['idx_ptr'])
    struct.pack_into('<I', g, f['vlen'], len(verts)); struct.pack_into('<I', g, f['vptr'], vo - f['vptr'])
    struct.pack_into('<f', g, f['obb_size'] + 8, zmax * 2)
    t = f['tx']
    for o in (0x14, 0x38): struct.pack_into('<I', g, t + o, h)
    for o in (0x18, 0x3c): struct.pack_into('<I', g, t + o, w)
    struct.pack_into('<I', g, t + 0x40, len(tex)); struct.pack_into('<I', g, t + 0x44, to - (t + 0x44))
    struct.pack_into('<I', g, f['imag'] + 4, len(g) - f['imag'])
    struct.pack_into('<I', g, 0x0c, len(g))
    return bytes(g)


def main():
    bnr = open(sys.argv[1], 'rb').read()
    if bnr[:4] == b'CBMD': bnr = lz11_decompress(bnr[u32(bnr, 8):u32(bnr, 0x84)])
    out = build(bnr, read_rgba_png(sys.argv[2]), json.load(open(sys.argv[3])))
    open(sys.argv[4], 'wb').write(out)
    print('%s: %d bytes' % (sys.argv[4], len(out)))


if __name__ == '__main__':
    main()
