# High-detail depth for full-screen pictures (event CGs, backgrounds): Depth Anything V2 Large,
# global shape from a 518 px pass + fine detail from a ~1036 px pass, stored at 1/2 resolution -> CG.PAK
import os, sys, glob, struct, numpy as np, onnxruntime as ort
from PIL import Image, ImageFilter
sess = ort.InferenceSession(sys.argv[1], providers=['CPUExecutionProvider']); iname = sess.get_inputs()[0].name
MEAN = np.array([0.485, 0.456, 0.406], np.float32); STD = np.array([0.229, 0.224, 0.225], np.float32)
def infer(rgb, long):
    h, w = rgb.shape[:2]; s = long / max(h, w); th, tw = round(h * s / 14) * 14, round(w * s / 14) * 14
    x = (np.asarray(Image.fromarray(rgb).resize((tw, th), Image.BICUBIC), np.float32) / 255 - MEAN) / STD
    d = sess.run(None, {iname: x.transpose(2, 0, 1)[None].astype(np.float32)})[0].reshape(th, tw)
    return np.asarray(Image.fromarray(d.astype(np.float32)).resize((w, h), Image.BILINEAR))
def norm(d):
    lo, hi = np.percentile(d, 1), np.percentile(d, 99); return np.clip((d - lo) / max(hi - lo, 1e-6), 0, 1)
def blur(a, r):
    def b1(x, ax):
        p = np.pad(x, [(r, r) if i == ax else (0, 0) for i in range(2)], mode='edge'); c = np.cumsum(p, axis=ax, dtype=np.float64)
        c = np.concatenate([np.zeros_like(c.take([0], axis=ax)), c], axis=ax); n = x.shape[ax]
        return (c.take(range(2 * r + 1, 2 * r + 1 + n), axis=ax) - c.take(range(0, n), axis=ax)) / (2 * r + 1)
    return b1(b1(a, 0), 1)
ents = []; prev = []
names = [l.strip() for l in open('cg_list.txt') if l.strip()]
shard, nsh = int(os.environ.get('SHARD', 0)), int(os.environ.get('NSHARD', 1)); names = names[shard::nsh]
import time; t0 = time.time()
for n in names:
    f = 'img/' + n[:-4] + '.png'
    if not os.path.exists(f): continue
    rgb = np.asarray(Image.open(f).convert('RGB'))
    g = norm(infer(rgb, 518)); fine = norm(infer(rgb, 1036))
    detail = fine - blur(fine, 6)                       # local relief the big pass resolves (folds, faces, fingers)
    v = np.clip(g + 0.8 * detail, 0, 1); v = norm(v)
    h, w = v.shape; mw, mh = (w + 1) // 2, (h + 1) // 2
    small = np.asarray(Image.fromarray((v * 255).astype(np.uint8)).resize((mw, mh), Image.BILINEAR))
    ents.append((n, mw, mh, small.tobytes())); print(n, round(time.time() - t0), flush=True)
    if len(prev) < 12: prev.append((f, small))
hdr = b'DPK1' + struct.pack('<I', len(ents)); off = 8 + 24 * len(ents); tab = b''; data = b''
for n, mw, mh, b in ents: tab += n.encode().ljust(16, b'\0')[:16] + struct.pack('<HHI', mw, mh, off + len(data)); data += b
os.makedirs('out', exist_ok=True); open('out/CG_%d.PAK' % shard, 'wb').write(hdr + tab + data)
sheet = Image.new('RGB', (4 * 640, 3 * 240))
for i, (f, s) in enumerate(prev):
    im = Image.open(f).convert('RGB').resize((320, 240)); sheet.paste(im, ((i % 4) * 640, (i // 4) * 240))
    sheet.paste(Image.fromarray(s).convert('RGB').resize((320, 240)), ((i % 4) * 640 + 320, (i // 4) * 240))
sheet.save('out/preview_cg_%d.png' % shard)
