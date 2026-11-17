# Korean title logo for the 3DS port: replaces the baked-in "野々村病院の人々" in titlelogo.gcc (title still) and
# oplan.gcc (title animation sheet; frames tinted by lightning) with "노노무라 병원의 사람들".
# usage: mklogo_kr.py Bg.Arc out_Bg.Arc lzss_enc_binary gcc2raw_binary [preview.png]
import sys, struct, subprocess, tempfile, os
import numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageFilter
TEXT = '노노무라 병원의 사람들'
SERIF = '/usr/share/fonts/opentype/noto/NotoSerifCJK-Black.ttc'
FILL = (239, 0, 95)
def arc_read(p):
    d = open(p, 'rb').read(); n = struct.unpack_from('<I', d)[0]; ents = []
    for i in range(n):
        nm, sz, of = struct.unpack_from('<20sII', d, 4 + i * 28); ents.append([nm, d[of ^ 0x32300645:(of ^ 0x32300645) + (sz ^ 0x15056109)]])
    return ents
def arc_write(p, ents):
    hdr = 4 + 28 * len(ents); out = bytearray(struct.pack('<I', len(ents))); body = bytearray(); off = hdr
    for nm, data in ents: out += struct.pack('<20sII', nm, len(data) ^ 0x15056109, off ^ 0x32300645); off += len(data)
    for nm, data in ents: body += data
    open(p, 'wb').write(out + body)
def decode(g2r, data):
    with tempfile.TemporaryDirectory() as t:
        a = os.path.join(t, 'a.gcc'); open(a, 'wb').write(data); r = subprocess.run([g2r, a, a + '.raw'], capture_output=True, text=True, check=True).stdout.split()
        w, h = int(r[3]), int(r[4]); return np.asarray(Image.frombytes('RGB', (w, h), open(a + '.raw', 'rb').read(), 'raw', 'BGR', 0, -1)).astype(np.int32)
def encode_g24(lz, rgb):
    h, w = rgb.shape[:2]; raw = Image.fromarray(rgb.astype(np.uint8)).tobytes('raw', 'BGR', 0, -1)
    with tempfile.TemporaryDirectory() as t:
        a = os.path.join(t, 'a'); open(a, 'wb').write(raw); subprocess.run([lz, a, a + '.lz'], check=True, capture_output=True); p = open(a + '.lz', 'rb').read()
    return b'G24n' + struct.pack('<4H2I', 0, 0, w, h, 0, 0) + p
def logo_layer():
    """RGBA 640x480: pink text, white rim, soft dark edge, centred where the original logo sits (x 49..592, y 105..226)"""
    f = ImageFont.truetype(SERIF, 64, index=1)
    big = Image.new('L', (1400, 300), 0); d = ImageDraw.Draw(big); d.text((40, 40), TEXT, font=f, fill=255)
    bb = big.getbbox(); glyph = big.crop(bb)
    tw, th = 540, 108                                   # stretch to the original logo's footprint (tall display lettering)
    glyph = glyph.resize((tw, th), Image.LANCZOS)
    pad = 16; W, H = tw + 2 * pad, th + 2 * pad
    g = Image.new('L', (W, H), 0); g.paste(glyph, (pad, pad))
    rim = g.filter(ImageFilter.MaxFilter(9))            # ~4 px white border
    edge = rim.filter(ImageFilter.MaxFilter(3)).filter(ImageFilter.GaussianBlur(1))
    G, R, E = (np.asarray(x).astype(np.float32) / 255 for x in (g, rim, edge))
    rgb = np.zeros((H, W, 3), np.float32); a = np.zeros((H, W), np.float32)
    rgb[:] = (20, 24, 40); a = E * 0.6                                       # dark outer edge
    rgb = rgb * (1 - R[..., None]) + np.array([255, 255, 255], np.float32) * R[..., None]; a = np.maximum(a, R)
    rgb = rgb * (1 - G[..., None]) + np.array(FILL, np.float32) * G[..., None]
    layer = np.zeros((480, 640, 4), np.float32); x0 = (640 - W) // 2; y0 = 166 - H // 2
    layer[y0:y0 + H, x0:x0 + W, :3] = rgb; layer[y0:y0 + H, x0:x0 + W, 3] = a
    return layer
def main():
    src, dst, lz, g2r = sys.argv[1:5]
    ents = arc_read(src); by = {e[0].split(b'\0')[0].decode().lower(): e for e in ents}
    L = logo_layer(); La = L[..., 3:4]
    op, tl = decode(g2r, by['op.gcc'][1]), decode(g2r, by['titlelogo.gcc'][1])
    M = np.abs(tl - op).sum(2) > 30                     # original logo pixels (screen space)
    ref = tl[M].mean(0)                                  # untinted logo colour average
    def put(frame_clean, frame_logo, dy):
        """frame_*: sheet rows mapped to screen rows dy..dy+h. Returns the clean frame + Korean logo tinted like the original."""
        h = frame_clean.shape[0]; m = M[dy:dy + h]
        tint = (frame_logo[m].mean(0) / ref) if m.sum() > 50 else np.ones(3)
        lay = L[dy:dy + h, :, :3] * tint; a = La[dy:dy + h]
        return np.clip(frame_clean * (1 - a) + lay * a, 0, 255)
    by['titlelogo.gcc'][1] = encode_g24(lz, put(op, tl, 0))
    an, al = decode(g2r, by['opan.gcc'][1]), decode(g2r, by['oplan.gcc'][1])
    out = al.copy()
    for sy, h, dy in [(0, 480, 0), (480, 200, 0), (680, 200, 0), (880, 200, 0), (1080, 200, 0), (1280, 280, 200), (1560, 280, 200)]:   # opl.ax draw records
        out[sy:sy + h] = put(an[sy:sy + h], al[sy:sy + h], dy)
    by['oplan.gcc'][1] = encode_g24(lz, out)
    arc_write(dst, ents)
    if len(sys.argv) > 5:
        Image.fromarray(np.clip(put(op, tl, 0), 0, 255).astype(np.uint8)).save(sys.argv[5])
        Image.fromarray(out.astype(np.uint8)).resize((320, 920)).save(sys.argv[5].replace('.png', '_sheet.png'))
main()
