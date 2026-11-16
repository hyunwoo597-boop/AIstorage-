#!/usr/bin/env python3
"""홈 메뉴 배너의 깊이 지도(ctr/banner_depth.png, 256x128 회색조)를 만든다.
밝을수록 앞(128 = 원래 평면). 영역은 ctr/banner.png 를 3배로 본 좌표로 손으로 잡았다.
    python3 tools/gen_banner_depth.py ctr/banner_depth.png [겹쳐 보기.png]
"""
import sys
from PIL import Image, ImageDraw, ImageFilter
S = 3
LAYERS = [  # (깊이, 다각형)  뒤에서 앞 순서
    (95,  [(0, 165), (130, 165), (130, 250), (520, 250), (520, 384), (0, 384)]),                # 병원
    (150, [(165, 0), (215, 0), (225, 70), (250, 130), (215, 135), (120, 140), (60, 160), (35, 120), (100, 60)]),   # 주황 머리
    (175, [(120, 140), (215, 125), (228, 225), (300, 260), (312, 384), (140, 384), (150, 240), (115, 200)]),      # 파란 머리
    (165, [(425, 180), (540, 170), (560, 300), (520, 384), (395, 384), (395, 290), (430, 240)]),                  # 스쿠터
    (205, [(550, 60), (700, 50), (768, 160), (768, 384), (520, 384), (515, 250), (520, 140)]),                    # 분홍 머리
    (230, [(232, 22), (585, 22), (585, 95), (500, 100), (505, 200), (255, 205), (232, 100)]),                     # 제목
]
im = Image.new('L', (256 * S, 128 * S), 60)          # 하늘
d = ImageDraw.Draw(im)
for z, poly in LAYERS: d.polygon(poly, fill=z)
im = im.filter(ImageFilter.GaussianBlur(10)).resize((256, 128), Image.LANCZOS)
im.save(sys.argv[1])
if len(sys.argv) > 2:
    import os
    b = Image.open(os.path.join(os.path.dirname(sys.argv[1]), 'banner.png')).convert('RGB').resize((768, 384))
    Image.blend(b, im.resize((768, 384)).convert('RGB'), 0.55).save(sys.argv[2])
