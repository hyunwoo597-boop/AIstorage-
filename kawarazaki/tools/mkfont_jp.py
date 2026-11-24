#!/usr/bin/env python3
"""16px Japanese font for the port (stands in for the original's MS Gothic 16 bold, drawn with GDI).
    python3 mkfont_jp.py <NotoSansCJK-Bold.ttc> <out kfont16.bin>
Layout: "KFN1", then 256 half-width cells (8x16, 1 byte per pixel coverage 0..255, code = byte value),
then 47 x 189 full-width cells (16x16) for Shift-JIS lead 0x81-0x9F,0xE0-0xEF and trail 0x40-0xFC,
then "KHG1" and 11172 Hangul syllables U+AC00.. (16x16, same drawing with the Korean face) for the Korean patch."""
import sys
from PIL import Image, ImageDraw, ImageFont
font = ImageFont.truetype(sys.argv[1], 16, index=0)   # index 0 = JP
out = bytearray(b'KFN1')
def cell(ch, w):
    im = Image.new('L', (w, 16), 0)
    if ch:
        d = ImageDraw.Draw(im); bb = font.getbbox(ch)
        d.text(((w - (bb[2] - bb[0])) // 2 - bb[0], -2), ch, font=font, fill=255)
    return im.tobytes()
for c in range(256):
    ch = None
    if 0x20 < c < 0x7f: ch = chr(c)
    elif 0xa1 <= c <= 0xdf: ch = bytes([c]).decode('cp932')
    out += cell(ch, 8)
leads = list(range(0x81, 0xa0)) + list(range(0xe0, 0xf0))
for l in leads:
    for t in range(0x40, 0xfd):
        try: ch = bytes([l, t]).decode('cp932')
        except UnicodeDecodeError: ch = None
        if ch == '　': ch = None
        out += cell(ch, 16)
out += b'KHG1'
kr = ImageFont.truetype(sys.argv[1], 15, index=1)   # index 1 = KR; 15 px, 5 px up: the syllables span y 5..21 at this size
for cp in range(0xAC00, 0xD7A4):
    im = Image.new('L', (16, 16), 0); d = ImageDraw.Draw(im); bb = kr.getbbox(chr(cp))
    d.text(((16 - (bb[2] - bb[0])) // 2 - bb[0], -5), chr(cp), font=kr, fill=255); out += im.tobytes()
open(sys.argv[2], 'wb').write(out); print(len(out))
