#!/usr/bin/env python3
"""홈 메뉴 배너의 깊이 지도(ctr/banner_depth.png, 256x128 8비트 회색조)를 만든다.

    python3 tools/gen_banner_depth.py <원본 깊이 지도.png> ctr/banner_depth.png [평면 값=110]

원본은 '어두울수록 뒤, 밝을수록 앞'인 아무 크기의 회색조 그림. banner3d.py 는 128 을 원래 평면으로
보므로, 원본의 '평면 값'이 128 이 되고 가장 밝은 곳이 255 가 되도록 옮긴다.
"""
import sys
from PIL import Image
src = Image.open(sys.argv[1]).convert('L').resize((256, 128), Image.LANCZOS)
mid = float(sys.argv[3]) if len(sys.argv) > 3 else 110.0
hi = src.getextrema()[1]
k = 127.0 / max(1.0, hi - mid)
src.point(lambda d: max(0, min(255, round(128 + (d - mid) * k)))).save(sys.argv[2])
