# builds font16.bin: 4bpp 16x16 glyphs for ASCII (0x20-0x7E, 8x16 cells stored as 16x16) + Shift-JIS double-byte range
# layout: "AF16" u16 cell(16) u16 count, then glyphs; index = ascii-0x20 (95) or 95 + lead_idx*189 + (trail-0x40)
import sys, struct
from PIL import Image, ImageFont, ImageDraw
# optional 4th/5th args: a JSON list of [sjis_code, unicode] pairs (Korean patch glyph remap, from NonoKR.dll) and the
# face index to draw those characters with (Noto Sans CJK: 0 JP, 1 KR)
import json
ttc = sys.argv[1]; out = sys.argv[2]; face = int(sys.argv[3]) if len(sys.argv) > 3 else 0
remap = {k: v for k, v in json.load(open(sys.argv[4]))} if len(sys.argv) > 4 else {}
font = ImageFont.truetype(ttc, 15, index=face)
font_jp = font
font_kr = ImageFont.truetype(ttc, 15, index=int(sys.argv[5])) if len(sys.argv) > 5 else font
# baseline: drawn 5 px up so the CJK body (Noto at 15 px) fits rows 0..15 without cutting the bottom
def glyph(ch, half, f=None):
    global font
    font = f or font_jp
    im = Image.new('L', (16, 16), 0); d = ImageDraw.Draw(im)
    bb = d.textbbox((0, 0), ch, font=font); w = bb[2] - bb[0]
    x = ((8 if half else 16) - w) // 2 - bb[0]
    d.text((x, -5), ch, font=font, fill=255)
    px = im.tobytes(); b = bytearray(128)
    for i in range(256): b[i >> 1] |= (px[i] >> 4) << (0 if i & 1 else 4)
    return bytes(b)
leads = list(range(0x81, 0xA0)) + list(range(0xE0, 0xEB))
gl = [glyph(chr(c), True) for c in range(0x20, 0x7F)]
blank = bytes(128); n = 0
for l in leads:
    for t in range(0x40, 0xFD):
        if (l << 8 | t) in remap: gl.append(glyph(chr(remap[l << 8 | t]), False, font_kr)); n += 1; continue
        try: ch = bytes([l, t]).decode('cp932')
        except Exception: ch = None
        if ch and t != 0x7F: gl.append(glyph(ch, False)); n += 1
        else: gl.append(blank)
open(out, 'wb').write(b'AF16' + struct.pack('<HH', 16, 1 if remap else 0) + struct.pack('<I', len(gl)) + b''.join(gl))
print(len(gl), 'cells,', n, 'kanji/kana glyphs')
