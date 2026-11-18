# Depth for picture variant patches (a rectangle pasted over a base CG, e.g. people on a chair over an empty room):
# depth of the composite picture, matched to the base picture's depth scale on the unchanged area, cropped to the patch.
import os, sys, struct, numpy as np, onnxruntime as ort
from PIL import Image
sess = ort.InferenceSession(sys.argv[1], providers=['CPUExecutionProvider']); iname = sess.get_inputs()[0].name
MEAN = np.array([0.485, 0.456, 0.406], np.float32); STD = np.array([0.229, 0.224, 0.225], np.float32)
def infer(rgb, long):
    h, w = rgb.shape[:2]; s = long / max(h, w); th, tw = round(h * s / 14) * 14, round(w * s / 14) * 14
    x = (np.asarray(Image.fromarray(rgb).resize((tw, th), Image.BICUBIC), np.float32) / 255 - MEAN) / STD
    d = sess.run(None, {iname: x.transpose(2, 0, 1)[None].astype(np.float32)})[0].reshape(th, tw)
    return np.asarray(Image.fromarray(d.astype(np.float32)).resize((w, h), Image.BILINEAR))
def blur(a, r):
    def b1(x, ax):
        p = np.pad(x, [(r, r) if i == ax else (0, 0) for i in range(2)], mode='edge'); c = np.cumsum(p, axis=ax, dtype=np.float64)
        c = np.concatenate([np.zeros_like(c.take([0], axis=ax)), c], axis=ax); n = x.shape[ax]
        return (c.take(range(2 * r + 1, 2 * r + 1 + n), axis=ax) - c.take(range(0, n), axis=ax)) / (2 * r + 1)
    return b1(b1(a, 0), 1)
def depth(rgb):   # same recipe as gen2 (518 shape + 1036 detail), unnormalised scale kept for matching
    def nm(d): lo, hi = np.percentile(d, 1), np.percentile(d, 99); return np.clip((d - lo) / max(hi - lo, 1e-6), 0, 1)
    g = nm(infer(rgb, 518)); fine = nm(infer(rgb, 1036)); return np.clip(g + 0.8 * (fine - blur(fine, 6)), 0, 1)
pos = {l.split()[0]: (int(l.split()[1]), int(l.split()[2])) for l in open('base_pos.txt') if l.strip()}
jobs = [l.split() for l in open('patch_list.txt') if l.strip()]
shard, nsh = int(os.environ.get('SHARD', 0)), int(os.environ.get('NSHARD', 1)); jobs = jobs[shard::nsh]
ents = []
for n, b, px, py in jobs:
    px, py = int(px), int(py); base = np.asarray(Image.open('img/' + b[:-4] + '.png').convert('RGB')); bx, by = pos[b]
    pat = np.asarray(Image.open('img/' + n[:-4] + '.png').convert('RGB')); ph, pw = pat.shape[:2]
    can = np.zeros((480, 640, 3), np.uint8); bh, bw = base.shape[:2]; can[by:by + bh, bx:bx + bw] = base
    comp = can.copy(); comp[py:py + ph, px:px + pw] = pat[:min(ph, 480 - py), :min(pw, 640 - px)]
    db = np.zeros((480, 640)); db[by:by + bh, bx:bx + bw] = depth(base)
    # base map as stored: normalised 1..99 % again (gen2 did nm() on the fused map)
    v = db[by:by + bh, bx:bx + bw]; lo, hi = np.percentile(v, 1), np.percentile(v, 99); db[by:by + bh, bx:bx + bw] = np.clip((v - lo) / max(hi - lo, 1e-6), 0, 1)
    dc = np.zeros((480, 640)); dc[by:by + bh, bx:bx + bw] = depth(comp[by:by + bh, bx:bx + bw])
    m = np.zeros((480, 640), bool); m[by:by + bh, bx:bx + bw] = True; m[max(0, py - 8):py + ph + 8, max(0, px - 8):px + pw + 8] = False
    A = np.vstack([dc[m], np.ones(m.sum())]).T; k, c0 = np.linalg.lstsq(A, db[m], rcond=None)[0]
    crop = np.clip(dc[py:py + ph, px:px + pw] * k + c0, 0, 1)
    mw, mh = (pw + 1) // 2, (ph + 1) // 2
    small = np.asarray(Image.fromarray((crop * 255).astype(np.uint8)).resize((mw, mh), Image.BILINEAR))
    ents.append((n, mw, mh, small.tobytes())); print(n, round(k, 3), round(c0, 3), flush=True)
hdr = b'DPK1' + struct.pack('<I', len(ents)); off = 8 + 24 * len(ents); tab = b''; data = b''
for n, mw, mh, b in ents: tab += n.encode().ljust(16, b'\0')[:16] + struct.pack('<HHI', mw, mh, off + len(data)); data += b
os.makedirs('out', exist_ok=True); open('out/CG_%d.PAK' % shard, 'wb').write(hdr + tab + data)
