#!/usr/bin/env python3
"""홈 메뉴 배너를 입체로: bannertool 이 만든 평면 배너(.bnr)의 모델을 깊이 지도대로 굴곡진 격자로 바꾼다.

    python3 tools/banner3d.py <평면 banner.bnr> <깊이 지도.png> <출력.cgfx> [뒤 깊이 앞 깊이]

    그다음: bannertool makebanner -ci <출력.cgfx> -a banner.wav -o banner.bnr

bannertool 의 모델은 사각형 한 장(정점 4개, 26x13 단위)에 256x128 그림 한 장을 입힌 것이다.
여기서는 정점·색인 자료만 새로 만들어 파일 끝(IMAG 블록)에 덧붙이고, 그것을 가리키는 값만 고친다.
그림·재질·나머지 구조는 그대로 둔다. 색인이 1바이트라 정점은 256개 이하(21x11 격자).
깊이 지도: 회색조, 128 = 원래 평면, 밝을수록 앞. 가장자리는 평면에 붙여 윤곽이 네모로 남게 한다.
"""
import struct, sys, zlib

GX, GY = 20, 10                       # 격자 칸 수 → 정점 21x11 = 231


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


def u32(b, o): return struct.unpack_from('<I', b, o)[0]


def read_gray_png(path):
    """8비트 회색조 PNG 를 읽는다 (외부 모듈 없이). (너비, 높이, 행 목록)"""
    d = open(path, 'rb').read()
    assert d[:8] == b'\x89PNG\r\n\x1a\n'
    o = 8; idat = b''; w = h = 0
    while o < len(d):
        n, t = struct.unpack_from('>I4s', d, o)
        c = d[o+8:o+8+n]; o += 12 + n
        if t == b'IHDR':
            w, h, bd, ct, _, _, il = struct.unpack('>IIBBBBB', c)
            assert (bd, ct, il) == (8, 0, 0), '깊이 지도는 8비트 회색조 PNG 여야 합니다'
        elif t == b'IDAT': idat += c
    raw = zlib.decompress(idat); rows = []; prev = bytes(w)
    for y in range(h):
        f = raw[y*(w+1)]; line = bytearray(raw[y*(w+1)+1:(y+1)*(w+1)])
        for x in range(w):
            a = line[x-1] if x else 0; b = prev[x]; c = prev[x-1] if x else 0
            if f == 1: line[x] = (line[x] + a) & 255
            elif f == 2: line[x] = (line[x] + b) & 255
            elif f == 3: line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c; pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(bytes(line)); prev = line
    return w, h, rows


def find_shape(g):
    """정점 4개·색인 6개짜리 평면 셰이프를 찾아 고칠 자리들을 돌려준다. 구조가 예상과 다르면 멈춘다."""
    assert g[:4] == b'CGFX' and u32(g, 0x0c) == len(g), 'CGFX 머리'
    data_end = 0x14 + u32(g, 0x18)
    assert g[data_end:data_end+4] == b'IMAG' and data_end + u32(g, data_end + 4) == len(g), 'IMAG 가 마지막 블록이 아님'
    hits = []
    o = g.find(b'SOBJ')
    while o >= 0:
        if u32(g, o - 4) == 0x10000001: hits.append(o)
        o = g.find(b'SOBJ', o + 4)
    assert len(hits) == 1, '셰이프가 하나가 아님'
    s = hits[0]
    # 색인 기술자: 형식 0x1401(1바이트), 개수 6
    fd = None
    for p in range(s, s + 0x200, 4):
        if u32(g, p) == 0x1401 and u32(g, p + 8) == 6:
            fd = p; break
    assert fd, '색인 기술자'
    idx_cnt, idx_ptr = fd + 8, fd + 12
    idx_off = idx_ptr + u32(g, idx_ptr)
    assert list(g[idx_off:idx_off+6]) == [0, 1, 2, 1, 3, 2], '색인 내용'
    # 끼워 넣은(interleaved) 정점 버퍼: 0x40000002, 길이 0x50, 정점 크기 20
    vb = None
    for p in range(fd, fd + 0x100, 4):
        if u32(g, p) == 0x40000002: vb = p; break
    assert vb, '정점 버퍼'
    vlen, vptr, stride = vb + 0x14, vb + 0x18, vb + 0x24
    assert u32(g, vlen) == 0x50 and u32(g, stride) == 20 and u32(g, vb + 0x28) == 2, '정점 버퍼 형식'
    voff = vptr + u32(g, vptr)
    v = [struct.unpack_from('<5f', g, voff + i * 20) for i in range(4)]
    assert all(p[2] == 0 for p in v), '평면이 아님'
    # 경계 상자: 셰이프 머리의 +0x18 이 가리킴. 끝 12바이트가 크기(x, y, z)
    obb = s + 0x18 + u32(g, s + 0x18)
    assert u32(g, obb) == 0x80000000 and struct.unpack_from('<3f', g, obb + 0x34)[2] == 0, '경계 상자'
    return dict(idx_cnt=idx_cnt, idx_ptr=idx_ptr, vlen=vlen, vptr=vptr, obb_size=obb + 0x34, quad=v, imag=data_end)


def build(cgfx, depth, z_far, z_near):
    g = bytearray(cgfx)
    f = find_shape(g)
    q = f['quad']
    x0, x1 = min(p[0] for p in q), max(p[0] for p in q)
    y0, y1 = min(p[1] for p in q), max(p[1] for p in q)
    # 원래 정점에서 (x→u), (y→v) 방향을 읽는다
    u_at_x0 = next(p[3] for p in q if p[0] == x0); v_at_y0 = next(p[4] for p in q if p[1] == y0)
    w, h, rows = depth
    verts = bytearray(); zmax = 0.0
    for j in range(GY + 1):
        for i in range(GX + 1):
            fx, fy = i / GX, j / GY                         # 0..1, 왼쪽→오른쪽 / 아래→위
            d = rows[min(h - 1, int((1 - fy) * (h - 1) + .5))][min(w - 1, int(fx * (w - 1) + .5))]
            z = (d - 128) / 127.0
            z = z * z_near if z > 0 else z * -z_far
            if i in (0, GX) or j in (0, GY): z = 0.0         # 가장자리는 평면에
            elif i in (1, GX - 1) or j in (1, GY - 1): z *= 0.6   # 그 안쪽 한 줄은 완만하게
            zmax = max(zmax, abs(z))
            u = fx if u_at_x0 == 0 else 1 - fx
            v = fy if v_at_y0 == 0 else 1 - fy
            verts += struct.pack('<5f', x0 + (x1 - x0) * fx, y0 + (y1 - y0) * fy, z, u, v)
    idx = bytearray()
    for j in range(GY):
        for i in range(GX):
            a = j * (GX + 1) + i; b = a + 1; c = a + GX + 1; e = c + 1
            idx += bytes([a, b, c, b, e, c])                 # 원래와 같은 감는 방향 (0,1,2 / 1,3,2)
    assert (GX + 1) * (GY + 1) <= 256
    # 파일 끝에 덧붙임: 색인 → 4바이트 맞춤 → 정점
    while len(g) % 4: g.append(0)
    idx_off = len(g); g += idx
    while len(g) % 4: g.append(0)
    v_off = len(g); g += verts
    struct.pack_into('<I', g, f['idx_cnt'], len(idx)); struct.pack_into('<I', g, f['idx_ptr'], idx_off - f['idx_ptr'])
    struct.pack_into('<I', g, f['vlen'], len(verts)); struct.pack_into('<I', g, f['vptr'], v_off - f['vptr'])
    struct.pack_into('<f', g, f['obb_size'] + 8, zmax * 2)
    struct.pack_into('<I', g, f['imag'] + 4, len(g) - f['imag'])
    struct.pack_into('<I', g, 0x0c, len(g))
    return bytes(g)


def main():
    bnr = open(sys.argv[1], 'rb').read()
    if bnr[:4] == b'CBMD':
        bnr = lz11_decompress(bnr[u32(bnr, 8):u32(bnr, 0x84)])
    depth = read_gray_png(sys.argv[2])
    z_far, z_near = (float(sys.argv[4]), float(sys.argv[5])) if len(sys.argv) > 5 else (-1.5, 2.5)
    out = build(bnr, depth, z_far, z_near)
    open(sys.argv[3], 'wb').write(out)
    print('%s: %d bytes, 정점 %d개' % (sys.argv[3], len(out), (GX + 1) * (GY + 1)))


if __name__ == '__main__':
    main()
