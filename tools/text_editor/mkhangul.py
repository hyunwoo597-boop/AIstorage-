# Pre-renders all 11172 Hangul syllables as font16.bin cells (same drawing as tools/mkfont.py: Noto Sans CJK KR, 15 px,
# 5 px up), so the text editor can add any syllable to font16.bin in the browser.  usage: mkhangul.py NotoSansCJK.ttc out.bin
import sys
from PIL import Image, ImageFont, ImageDraw
font = ImageFont.truetype(sys.argv[1], 15, index=1)
out = bytearray()
for cp in range(0xAC00, 0xD7A4):
    im = Image.new('L', (16, 16), 0); d = ImageDraw.Draw(im); ch = chr(cp)
    bb = d.textbbox((0, 0), ch, font=font); w = bb[2] - bb[0]; x = (16 - w) // 2 - bb[0]
    d.text((x, -5), ch, font=font, fill=255)
    px = im.tobytes(); b = bytearray(128)
    for i in range(256): b[i >> 1] |= (px[i] >> 4) << (0 if i & 1 else 4)
    out += b
open(sys.argv[2], 'wb').write(out); print(len(out) // 128, 'glyphs')
