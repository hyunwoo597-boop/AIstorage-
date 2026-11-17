# Korean logo for the in-game system menu: mwaku.gcc (G24m, 840x581) holds "野々村病院の人々" at (0,504) 328x77, drawn
# through its mask (SYS 11,7) at the menu's top-left. Replaces that region (colour + mask) with "노노무라 병원의 사람들".
# usage: mkmenulogo_kr.py Bg.Arc lzss_enc gcc2raw out_mwaku.gcc [preview.png]
import sys, struct, subprocess, tempfile, os
import numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageFilter
arc, lz, g2r, out = sys.argv[1:5]
d = open(arc, 'rb').read(); n = struct.unpack_from('<I', d)[0]; orig = None
for i in range(n):
    nm, sz, of = struct.unpack_from('<20sII', d, 4 + i * 28)
    if nm.split(b'\0')[0].decode().lower() == 'mwaku.gcc': orig = d[of ^ 0x32300645:(of ^ 0x32300645) + (sz ^ 0x15056109)]
assert orig[:4] in (b'G24m', b'R24m')
x0, y0, W, H = struct.unpack_from('<4H', orig, 4); ax, ay, AW, AH = struct.unpack_from('<4H', orig, 20)
with tempfile.TemporaryDirectory() as t:
    g = os.path.join(t, 'a.gcc'); open(g, 'wb').write(orig); subprocess.run([g2r, g, g + '.rgb', g + '.a'], check=True, capture_output=True)
    rgb = np.asarray(Image.frombytes('RGB', (W, H), open(g + '.rgb', 'rb').read(), 'raw', 'BGR', 0, -1)).copy()
    alpha = np.frombuffer(open(g + '.a', 'rb').read(), np.uint8).reshape(AH, AW)[::-1].copy()     # top-down
# render the logo
RX, RY, RW, RH = 0, 504, 328, 77
f = ImageFont.truetype('/usr/share/fonts/opentype/noto/NotoSerifCJK-Black.ttc', 64, index=1)
big = Image.new('L', (1400, 300), 0); ImageDraw.Draw(big).text((40, 40), '노노무라 병원의 사람들', font=f, fill=255)
glyph = big.crop(big.getbbox()).resize((RW - 10, RH - 14), Image.LANCZOS)
G = Image.new('L', (RW, RH), 0); G.paste(glyph, (5, 7))
rim = G.filter(ImageFilter.MaxFilter(5))
Gf, Rf = np.asarray(G).astype(np.float32) / 255, np.asarray(rim).astype(np.float32) / 255
col = np.ones((RH, RW, 3), np.float32) * 255; col = col * (1 - Gf[..., None]) + np.array([239, 0, 95], np.float32) * Gf[..., None]
rgb[RY:RY + RH, RX:RX + RW] = col.astype(np.uint8)
alpha[RY:RY + RH, RX:RX + RW] = (Rf * 255).astype(np.uint8)
# encode G24m: header copy, LZSS colour (bottom-up BGR), control bits all 0 (one literal alpha byte each), alpha bottom-up
color = Image.fromarray(rgb).tobytes('raw', 'BGR', 0, -1)
with tempfile.TemporaryDirectory() as t:
    p = os.path.join(t, 'c'); open(p, 'wb').write(color); subprocess.run([lz, p, p + '.lz'], check=True, capture_output=True); packed = open(p + '.lz', 'rb').read()
a_file = alpha[::-1].tobytes(); controls = bytes((len(a_file) + 7) // 8)
hdr = bytearray(orig[:32]); hdr[0] = ord('G')
struct.pack_into('<I', hdr, 12, len(packed)); struct.pack_into('<I', hdr, 16, 0); struct.pack_into('<I', hdr, 28, len(controls))
open(out, 'wb').write(bytes(hdr) + packed + controls + a_file)
if len(sys.argv) > 5:
    v = Image.new('RGB', (RW, RH), (40, 60, 90)); v.paste(Image.fromarray(rgb[RY:RY + RH, RX:RX + RW]), mask=Image.fromarray(alpha[RY:RY + RH, RX:RX + RW])); v.resize((RW * 2, RH * 2)).save(sys.argv[5])
print('ok', len(packed))
