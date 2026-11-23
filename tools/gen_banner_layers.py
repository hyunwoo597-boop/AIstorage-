#!/usr/bin/env python3
"""홈 메뉴 배너용 조각 모음 만들기 (PC 에서 한 번 실행 — numpy·Pillow·opencv 필요).

    python3 tools/gen_banner_layers.py ctr/banner_parts ctr

ctr/banner_parts/ 의 조각 그림(배경이 투명한 PNG: 무대·병원·인물 넷·로고)을 512x256 조각 모음
(ctr/banner_atlas.png)에 채워 넣고 배치표(ctr/banner_layout.json)를 쓴다. tools/banner_diorama.py 가
이 둘로 배너 모델을 만든다. 배치는 아래 LAYOUT 을 고친다.

좌표: 배너 평면은 가로 26(x -13..13), 세로 13(y -7.5..5.5), z 는 + 가 보는 사람 쪽.
"""
import json, sys
import numpy as np, cv2
from PIL import Image

AW, AH = 512, 256   # pycgfx/홈 메뉴 한도: 세로 256 (512 로 만들면 변환기가 세로를 절반으로 줄여 흐려졌다)
# 마리오 배너처럼: 받침 없이 화면을 가득 채우는 큰 로고(맨 앞)와 크게 나온 인물들, 뒤에 병원. 화면 보이는 범위는 대략
# x -20..20, y -11..13 (홈 메뉴 카메라 (0,1,44.8), 시야각 30°). 흔들림 없음 — 깊이(z)만으로 입체.
# (이름, 가운데 x, 바닥 y, 높이, z, 화소 밀도 배율, 아래쪽 흐림 비율)   — 뒤 → 앞 순서
LAYOUT = [
    ('hospital',   0.0,  -6.5,  9.5, -12.0, 0.35, 0.0),
    ('orange',   -14.0, -12.5, 17.5,  -4.0, 1.0, 0.0),
    ('pink',      14.2, -12.5, 19.0,  -2.0, 1.0, 0.0),
    ('blue',      -6.8, -12.5, 15.5,   3.0, 1.0, 0.0),
    ('scooter',    5.0, -12.5, 14.0,   7.0, 1.0, 0.0),
    ('logo',       0.0,  -0.8,  9.6,  12.0, 1.4, 0.0),
]
CAM = 44.786   # 홈 메뉴 카메라 거리 (0, 1, 44.786): 앞뒤로 옮긴 조각은 화면에서 같은 크기·자리로 보이게 거리에 맞춰 줄이거나 키운다


def load(path, fade):
    a = np.array(Image.open(path).convert('RGBA')).astype(np.float32)
    al = a[..., 3] / 255
    al[al < 0.16] = 0                                         # 배경에 남은 옅은 얼룩 제거
    n, lab, st, _ = cv2.connectedComponentsWithStats((al > 0).astype(np.uint8))
    for i in range(1, n):
        if st[i, cv2.CC_STAT_AREA] < 400: al[lab == i] = 0
    ys, xs = np.where(al > 0); x0, x1, y0, y1 = xs.min(), xs.max() + 1, ys.min(), ys.max() + 1
    a = a[y0:y1, x0:x1]; al = al[y0:y1, x0:x1]
    if fade:                                                  # 잘린 아랫단을 서서히 투명하게
        h = al.shape[0]; k = int(h * fade)
        al[h-k:] *= np.linspace(1, 0, k, dtype=np.float32)[:, None]
    # 투명한 곳의 색을 가장자리 색으로 번지게 (줄였을 때 흰 테두리 방지)
    c = a[..., :3]; hard = (al > 0.5).astype(np.float32); acc = c * hard[..., None]; wgt = hard.copy()
    for _ in range(8):
        acc = cv2.blur(acc, (15, 15)); wgt = cv2.blur(wgt, (15, 15))
        c = np.where(hard[..., None] > 0, c, acc / np.maximum(wgt[..., None], 1e-4))
        known = np.maximum(hard, (wgt > 1e-3).astype(np.float32)); acc = c * known[..., None]; wgt = known
    return c, al


def pack(sizes):
    """MaxRects (가장 짧은 변이 잘 맞는 빈칸) — 조각 사이 1 화소 띄움"""
    free = [(0, 0, AW, AH)]; place = {}
    for n in sorted(sizes, key=lambda k: -max(sizes[k])):
        best = None
        for rot in (False, True):                             # 90° 돌려 넣기도 해 본다 (세로 256 한도)
            w, h = (sizes[n][1], sizes[n][0]) if rot else sizes[n]; w += 1; h += 1
            for fx, fy, fw, fh in free:
                if w <= fw and h <= fh:
                    sc = (min(fw - w, fh - h), max(fw - w, fh - h))
                    if best is None or sc < best[0]: best = (sc, fx, fy, rot, w, h)
        if best is None: return None
        _, x, y, rot, w, h = best; place[n] = (x, y, w - 1, h - 1, rot)
        nf = []
        for fx, fy, fw, fh in free:
            if x >= fx + fw or x + w <= fx or y >= fy + fh or y + h <= fy: nf.append((fx, fy, fw, fh)); continue
            if x > fx: nf.append((fx, fy, x - fx, fh))
            if x + w < fx + fw: nf.append((x + w, fy, fx + fw - x - w, fh))
            if y > fy: nf.append((fx, fy, fw, y - fy))
            if y + h < fy + fh: nf.append((fx, y + h, fw, fy + fh - y - h))
        free = [r for r in nf if not any(r != o and r[0] >= o[0] and r[1] >= o[1] and r[0] + r[2] <= o[0] + o[2] and r[1] + r[3] <= o[1] + o[3] for o in nf)]
    return place


def main():
    src, out = sys.argv[1], sys.argv[2]
    parts = {n: load('%s/%s.png' % (src, n), fade) for (n, _, _, _, _, _, fade) in LAYOUT}
    dens = 22.0                                               # 평면 1 단위당 화소 수 — 안 들어가면 줄인다
    while True:
        sizes = {}
        for (n, cx, by, hgt, z, k, _) in LAYOUT:
            c, al = parts[n]; ph = max(2, round(hgt * dens * k)); pw = max(2, round(ph * al.shape[1] / al.shape[0]))
            sizes[n] = (pw, ph)
        place = pack(sizes)
        if place: break
        dens *= 0.97
    atlas = np.zeros((AH, AW, 4), np.uint8); lay = []
    for (n, cx, by, hgt, z, k, _) in LAYOUT:
        c, al = parts[n]; x, y, w, h, rot = place[n]; ow, oh = (h, w) if rot else (w, h)   # ow/oh: 조각 자체의 크기
        cc = np.clip(cv2.resize(c, (ow, oh), interpolation=cv2.INTER_AREA), 0, 255); aa = np.clip(cv2.resize(al, (ow, oh), interpolation=cv2.INTER_AREA) * 255, 0, 255)
        if rot: cc = np.rot90(cc, -1); aa = np.rot90(aa, -1)      # 시계 방향 90°
        atlas[y:y+h, x:x+w, :3] = cc; atlas[y:y+h, x:x+w, 3] = aa
        f = (CAM - (z if not isinstance(z, list) else z[0])) / CAM
        wid = hgt * al.shape[1] / al.shape[0] * f; cxs = cx * f; bys = 1 + (by - 1) * f; hs = hgt * f
        lay.append(dict(name=n, z=z, quad=[round(cxs - wid / 2, 3), round(bys, 3), round(cxs + wid / 2, 3), round(bys + hs, 3)], uv=[x, y, w, h], rot=bool(rot)))
    Image.fromarray(atlas, 'RGBA').save(out + '/banner_atlas.png')
    json.dump(dict(atlas=[AW, AH], layers=lay), open(out + '/banner_layout.json', 'w'), indent=1)
    print('화소 밀도 %.1f' % dens, place)


if __name__ == '__main__':
    main()
