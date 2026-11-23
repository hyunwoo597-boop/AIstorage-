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

AW, AH = 512, 512
# 마리오 배너처럼: 받침 없이 화면을 가득 채우는 큰 로고(맨 앞)와 크게 나온 인물들, 뒤에 병원. 화면 보이는 범위는 대략
# x -20..20, y -11..13 (홈 메뉴 카메라 (0,1,44.8), 시야각 30°). 흔들림 없음 — 깊이(z)만으로 입체.
# (이름, 가운데 x, 바닥 y, 높이, z, 화소 밀도 배율, 아래쪽 흐림 비율)   — 뒤 → 앞 순서
LAYOUT = [
    ('hospital',   0.0,  -6.5,  9.5, -6.0, 0.55, 0.0),
    ('orange',   -14.0, -12.5, 17.5, -2.0, 0.9, 0.0),
    ('pink',      14.2, -12.5, 19.0, -1.0, 0.9, 0.0),
    ('blue',      -6.8, -12.5, 15.5,  1.5, 0.9, 0.0),
    ('scooter',    5.0, -12.5, 14.0,  3.5, 0.9, 0.0),
    ('logo',       0.0,  -0.8,  9.6,  6.0, 1.0, 0.0),
]


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
    sky = [0] * AW; place = {}
    for n in sorted(sizes, key=lambda k: -sizes[k][0] * sizes[k][1]):
        w, h = sizes[n]; best = None
        for x in range(0, AW - w + 1):
            y = max(sky[x:x+w])
            if y + h <= AH and (best is None or y < best[1]): best = (x, y)
        if best is None: return None
        x, y = best; place[n] = (x, y, w, h)
        for i in range(x, min(AW, x + w + 1)): sky[i] = y + h + 1
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
        c, al = parts[n]; x, y, w, h = place[n]
        atlas[y:y+h, x:x+w, :3] = np.clip(cv2.resize(c, (w, h), interpolation=cv2.INTER_AREA), 0, 255)
        atlas[y:y+h, x:x+w, 3] = np.clip(cv2.resize(al, (w, h), interpolation=cv2.INTER_AREA) * 255, 0, 255)
        wid = hgt * al.shape[1] / al.shape[0]
        lay.append(dict(name=n, z=z, quad=[round(cx - wid / 2, 3), by, round(cx + wid / 2, 3), round(by + hgt, 3)], uv=[x, y, w, h]))
    Image.fromarray(atlas, 'RGBA').save(out + '/banner_atlas.png')
    json.dump(dict(atlas=[AW, AH], layers=lay), open(out + '/banner_layout.json', 'w'), indent=1)
    print('화소 밀도 %.1f' % dens, place)


if __name__ == '__main__':
    main()
