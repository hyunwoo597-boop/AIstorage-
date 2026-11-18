# Depth Anything V2 depth maps for the NONOXP 3DS port -> DEPTH.PAK
import os, sys, glob, struct, numpy as np, onnxruntime as ort
from PIL import Image
model = sys.argv[1]
sess = ort.InferenceSession(model, providers=['CPUExecutionProvider'])
iname = sess.get_inputs()[0].name
MEAN = np.array([0.485, 0.456, 0.406], np.float32); STD = np.array([0.229, 0.224, 0.225], np.float32)
def infer(rgb):
    h, w = rgb.shape[:2]; s = 518 / max(h, w)
    th, tw = max(14, round(h * s / 14) * 14), max(14, round(w * s / 14) * 14)
    x = np.asarray(Image.fromarray(rgb).resize((tw, th), Image.BICUBIC), np.float32) / 255
    x = ((x - MEAN) / STD).transpose(2, 0, 1)[None]
    d = sess.run(None, {iname: x.astype(np.float32)})[0].reshape(th, tw)
    return np.asarray(Image.fromarray(d.astype(np.float32)).resize((w, h), Image.BILINEAR))
ents = []; prev = []
for f in sorted(glob.glob('img/*.png')):
    n = os.path.basename(f)[:-4] + '.GCC'; im = Image.open(f)
    if im.mode == 'RGBA':
        a = np.asarray(im)[:, :, 3]; rgb = np.asarray(im)[:, :, :3].astype(np.float32)
        bg = np.full_like(rgb, 128); al = a[:, :, None] / 255.0
        d = infer((rgb * al + bg * (1 - al)).astype(np.uint8)); m = a >= 128
    else:
        d = infer(np.asarray(im.convert('RGB'))); m = np.ones(d.shape, bool)
    if m.sum() < 16: continue
    lo, hi = np.percentile(d[m], 2), np.percentile(d[m], 98)
    v = np.clip((d - lo) / max(hi - lo, 1e-6), 0, 1); v[~m] = 0
    h, w = v.shape; mw, mh = (w + 3) // 4, (h + 3) // 4
    small = np.asarray(Image.fromarray((v * 255).astype(np.uint8)).resize((mw, mh), Image.BILINEAR))
    ents.append((n, mw, mh, small.tobytes())); print(n, w, h, flush=True)
    if len(prev) < 24: prev.append((f, small))
hdr = b'DPK1' + struct.pack('<I', len(ents)); off = 8 + 24 * len(ents); tab = b''; data = b''
for n, mw, mh, b in ents:
    tab += n.encode().ljust(16, b'\0')[:16] + struct.pack('<HHI', mw, mh, off + len(data)); data += b
os.makedirs('out', exist_ok=True); open('out/DEPTH.PAK', 'wb').write(hdr + tab + data)
sheet = Image.new('RGB', (6 * 320, 4 * 120))
for i, (f, s) in enumerate(prev):
    im = Image.open(f).convert('RGB'); im.thumbnail((160, 120)); sheet.paste(im, ((i % 6) * 320, (i // 6) * 120))
    dm = Image.fromarray(s).convert('RGB'); dm.thumbnail((160, 120)); sheet.paste(dm, ((i % 6) * 320 + 160, (i // 6) * 120))
sheet.save('out/preview.png')
