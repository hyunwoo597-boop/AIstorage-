# Extra depth for chest and hips: pose keypoints (YOLOv8-pose, tools/depth pose.py) place soft blobs on the chest and on
# the hips/buttocks; inside them the local roundness of the AI depth is amplified and the area lifted a little.
# usage: body_boost.py DEPTH.PAK pose.json out.pak [gain]
import sys, json, struct, numpy as np
from PIL import Image
def rd(p):
    d = open(p, 'rb').read(); n = struct.unpack('<I', d[4:8])[0]; r = {}
    for i in range(n):
        e = d[8 + i * 24:32 + i * 24]; nm = e[:16]; w, h, o = struct.unpack('<HHI', e[16:24]); r[nm] = [w, h, np.frombuffer(d[o:o + w * h], np.uint8).reshape(h, w).astype(np.float32) / 255]
    return r
def blur(a, r):
    r = max(1, int(r))
    def b1(x, ax):
        p = np.pad(x, [(r, r) if i == ax else (0, 0) for i in range(2)], mode='edge'); c = np.cumsum(p, axis=ax, dtype=np.float64)
        c = np.concatenate([np.zeros_like(c.take([0], axis=ax)), c], axis=ax); n = x.shape[ax]
        return (c.take(range(2 * r + 1, 2 * r + 1 + n), axis=ax) - c.take(range(0, n), axis=ax)) / (2 * r + 1)
    return b1(b1(a, 0), 1)
maps = rd(sys.argv[1]); pose = json.load(open(sys.argv[2])); gain = float(sys.argv[4]) if len(sys.argv) > 4 else 1.0
done = 0
for name, info in pose.items():
    key = name.encode().ljust(16, b'\0')
    if key not in maps or not info['people']: continue
    mw, mh, D = maps[key]; sx, sy = mw / info['w'], mh / info['h']
    yy, xx = np.mgrid[0:mh, 0:mw].astype(np.float32); W = np.zeros_like(D)
    for p in info['people']:
        k = p['kp']; ok = lambda i: k[i][2] > 0.3; P = lambda i: np.array([k[i][0] * sx, k[i][1] * sy])
        if not (ok(5) and ok(6)): continue
        ls, rs = P(5), P(6); sw = np.linalg.norm(ls - rs); sm = (ls + rs) / 2
        if sw < 4: continue
        if ok(11) and ok(12): hm = (P(11) + P(12)) / 2
        else: hm = sm + np.array([0, sw * 1.6])
        down = hm - sm; side = (ls - rs) / max(sw, 1e-3)
        ch = sm + down * 0.32                                  # chest: two blobs below the shoulders
        for s in (-0.27, 0.27):
            c = ch + side * sw * s; r = sw * 0.30
            W = np.maximum(W, np.exp(-((xx - c[0]) ** 2 + (yy - c[1]) ** 2) / (2 * r * r)))
        if ok(11) and ok(12):                                  # hips / buttocks: around each hip point, a little down
            lh, rh = P(11), P(12); hw = max(np.linalg.norm(lh - rh), sw * 0.6)
            kn = [P(13) if ok(13) else lh + down * 0.8, P(14) if ok(14) else rh + down * 0.8]
            for hp, kp in ((lh, kn[0]), (rh, kn[1])):
                c = hp + (kp - hp) * 0.18 + (hp - hm) * 0.25; r = hw * 0.42
                W = np.maximum(W, np.exp(-((xx - c[0]) ** 2 + (yy - c[1]) ** 2) / (2 * r * r)))
    if W.max() < 0.05: continue
    body = (D > 0.02).astype(np.float32)                       # stay on the figure (sprites are 0 outside)
    r = max(3, mh / 24); bulge = np.clip(D - blur(D, r), 0, None)
    bulge = bulge / max(np.percentile(bulge[W > 0.3], 95) if (W > 0.3).any() else 1, 1e-3)
    D2 = D + W * body * gain * (0.07 + 0.09 * np.clip(bulge, 0, 1.5))
    maps[key][2] = np.clip(D2, 0, 1); done += 1
    if len(sys.argv) > 5: Image.fromarray(np.hstack([D, np.clip(D2, 0, 1), W]).clip(0, 1).__mul__(255).astype(np.uint8)).save('%s/%s.png' % (sys.argv[5], name[:-4]))
hdr = b'DPK1' + struct.pack('<I', len(maps)); off = 8 + 24 * len(maps); tab = b''; data = b''
for nm, (w, h, a) in maps.items(): b = (a * 255 + 0.5).clip(0, 255).astype(np.uint8).tobytes(); tab += nm + struct.pack('<HHI', w, h, off + len(data)); data += b
open(sys.argv[3], 'wb').write(hdr + tab + data); print('boosted', done)
