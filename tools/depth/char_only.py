# 3D only on the characters (user request): backgrounds become one flat plane, characters keep their AI relief with the
# face smoothed (only the basic shape: detailed relief bent faces in the stereo pair) and chest / hips strongly lifted.
# usage: char_only.py DEPTH_preboost.PAK pose.json out.pak [gain=4.0] [preview_dir] [BG.ARC: masked pictures, ...24m]
import sys, json, struct, numpy as np
from PIL import Image
def rd(p):
    d = open(p, 'rb').read(); n = struct.unpack('<I', d[4:8])[0]; r = {}
    for i in range(n):
        e = d[8 + i * 24:32 + i * 24]; nm = e[:16]; w, h, o = struct.unpack('<HHI', e[16:24])
        r[nm] = [w, h, np.frombuffer(d[o:o + w * h], np.uint8).reshape(h, w).astype(np.float32) / 255]
    return r
def blur(a, r):
    r = max(1, int(r))
    def b1(x, ax):
        p = np.pad(x, [(r, r) if i == ax else (0, 0) for i in range(2)], mode='edge'); c = np.cumsum(p, axis=ax, dtype=np.float64)
        c = np.concatenate([np.zeros_like(c.take([0], axis=ax)), c], axis=ax); n = x.shape[ax]
        return (c.take(range(2 * r + 1, 2 * r + 1 + n), axis=ax) - c.take(range(0, n), axis=ax)) / (2 * r + 1)
    return b1(b1(a, 0), 1)
def otsu(v):
    h, e = np.histogram(v, 64, (0, 1)); c = (e[:-1] + e[1:]) / 2; best, t = -1, 0.5
    for i in range(1, 64):
        w0, w1 = h[:i].sum(), h[i:].sum()
        if not w0 or not w1: continue
        m0, m1 = (h[:i] * c[:i]).sum() / w0, (h[i:] * c[i:]).sum() / w1; s = w0 * w1 * (m0 - m1) ** 2
        if s > best: best, t = s, c[i]
    return t
maps = rd(sys.argv[1]); pose = json.load(open(sys.argv[2])); gain = float(sys.argv[4]) if len(sys.argv) > 4 else 4.0
prev = sys.argv[5] if len(sys.argv) > 5 and sys.argv[5] != '-' else None
masked = set()
if len(sys.argv) > 6:
    sys.path.insert(0, __import__('os').path.join(__import__('os').path.dirname(__file__), '..')); import arc
    ko, ks, ok, l = arc.solve(sys.argv[6]); f = open(sys.argv[6], 'rb')
    for n, o, sz in l:
        f.seek(o)
        if f.read(4)[3:4] == b'm': masked.add(n.upper())
stat = {'flat': 0, 'chars': 0, 'sprite': 0, 'kept': 0}
for key in list(maps):
    name = key.rstrip(b'\0').decode(); mw, mh, D = maps[key]
    if name == 'TITLELOGO.GCC': stat['kept'] += 1; continue
    info = pose.get(name, {'people': [], 'w': mw * 2, 'h': mh * 2}); people = info['people']
    full = (info['w'] >= 600 and info['h'] >= 440) if name in pose else (mw, mh) in ((320, 240), (160, 120))   # full pictures vs sprites (0 outside the figure)
    if full and name in masked: full = False                    # masked picture (cut-out figures over another picture): like a sprite
    sx, sy = mw / info['w'], mh / info['h']; yy, xx = np.mgrid[0:mh, 0:mw].astype(np.float32)
    if full and not people:
        maps[key][2] = np.full_like(D, float(np.percentile(D, 30))); stat['flat'] += 1; continue
    # character area
    if full:
        fg = np.zeros_like(D); inbox = np.zeros(D.shape, bool); boxes = []
        for p in people:
            x0, y0, x1, y1 = p['box']; x0, x1 = int(x0 * sx), int(np.ceil(x1 * sx)); y0, y1 = int(y0 * sy), int(np.ceil(y1 * sy))
            x0, y0 = max(0, x0 - 2), max(0, y0 - 2); x1, y1 = min(mw, x1 + 2), min(mh, y1 + 2)
            if x1 - x0 < 4 or y1 - y0 < 4: continue
            boxes.append((x0, y0, x1, y1)); inbox[y0:y1, x0:x1] = True
        # the room's own depth row by row (floor comes nearer towards the bottom) from outside the people's boxes
        ref = np.full(mh, np.nan)
        for y in range(mh):
            v = D[y][~inbox[y]]
            if v.size >= 4: ref[y] = np.percentile(v, 60)
        if np.isnan(ref).all(): ref[:] = np.percentile(D, 20)
        idx = np.arange(mh); ok_ = ~np.isnan(ref); ref = np.interp(idx, idx[ok_], ref[ok_])
        for x0, y0, x1, y1 in boxes:
            reg = D[y0:y1, x0:x1]; t = otsu(reg.ravel())
            m = (reg > t) & (reg > ref[y0:y1, None] + 0.05)
            fg[y0:y1, x0:x1] = np.maximum(fg[y0:y1, x0:x1], m.astype(np.float32))
        fg = np.clip((blur(fg, 2) - 0.35) / 0.3, 0, 1)          # clean, soft edge
        if fg.max() < 0.5: maps[key][2] = np.full_like(D, float(np.percentile(D, 30))); stat['flat'] += 1; continue
        bgv = float(np.percentile(D[fg < 0.1], 50)) if (fg < 0.1).any() else float(D.min())
    else:
        fg = (D > 0.02).astype(np.float32); bgv = 0.0
    C = D.copy()
    # face: basic shape only
    F = np.zeros_like(D); W = np.zeros_like(D)
    for p in people:
        k = p['kp']; ok = lambda i: k[i][2] > 0.3; P = lambda i: np.array([k[i][0] * sx, k[i][1] * sy])
        sw = np.linalg.norm(P(5) - P(6)) if ok(5) and ok(6) else 0
        if ok(0) or (ok(1) and ok(2)):
            c = P(0) if ok(0) else (P(1) + P(2)) / 2
            r = max(np.linalg.norm(P(3) - P(4)) * 0.75 if ok(3) and ok(4) else 0, np.linalg.norm(P(1) - P(2)) * 1.7 if ok(1) and ok(2) else 0, sw * 0.38, 4)
            F = np.maximum(F, np.clip(1.4 - np.sqrt((xx - c[0]) ** 2 + (yy - c[1]) ** 2) / r, 0, 1))
        if not (ok(5) and ok(6)) or sw < 4: continue
        ls, rs = P(5), P(6); sm = (ls + rs) / 2
        hm = (P(11) + P(12)) / 2 if ok(11) and ok(12) else sm + np.array([0, sw * 1.6])
        down = hm - sm; side = (ls - rs) / max(sw, 1e-3); ch = sm + down * 0.32
        for s in (-0.27, 0.27):
            c = ch + side * sw * s; r = sw * 0.32
            W = np.maximum(W, np.exp(-((xx - c[0]) ** 2 + (yy - c[1]) ** 2) / (2 * r * r)))
        if ok(11) and ok(12):
            lh, rh = P(11), P(12); hw = max(np.linalg.norm(lh - rh), sw * 0.6)
            kn = [P(13) if ok(13) else lh + down * 0.8, P(14) if ok(14) else rh + down * 0.8]
            for hp, kp in ((lh, kn[0]), (rh, kn[1])):
                c = hp + (kp - hp) * 0.18 + (hp - hm) * 0.25; r = hw * 0.45
                W = np.maximum(W, np.exp(-((xx - c[0]) ** 2 + (yy - c[1]) ** 2) / (2 * r * r)))
    if F.max() > 0:
        rf = max(3, mh / 16); C = C * (1 - F) + blur(C, rf) * F
    if W.max() > 0.05:
        r = max(3, mh / 24); bulge = np.clip(C - blur(C, r), 0, None)
        bulge = bulge / max(np.percentile(bulge[W > 0.3], 95) if (W > 0.3).any() else 1, 1e-3)
        C = C + W * (1 - F) * gain * (0.07 + 0.09 * np.clip(bulge, 0, 1.5))
    if full:
        C = np.maximum(C, bgv + 0.06)                           # characters stand in front of the flat background
        out = bgv * (1 - fg) + C * fg; stat['chars'] += 1
    else:
        out = np.where(fg > 0, np.maximum(C, 0.03), 0); stat['sprite'] += 1
    out = np.clip(out, 0, 1); maps[key][2] = out
    if prev: Image.fromarray((np.hstack([D, out, np.maximum(W, F * 0.5)]) * 255).clip(0, 255).astype(np.uint8)).save('%s/%s.png' % (prev, name[:-4]))
hdr = b'DPK1' + struct.pack('<I', len(maps)); off = 8 + 24 * len(maps); tab = b''; data = b''
for nm, (w, h, a) in maps.items():
    b = (a * 255 + 0.5).clip(0, 255).astype(np.uint8).tobytes(); tab += nm + struct.pack('<HHI', w, h, off + len(data)); data += b
open(sys.argv[3], 'wb').write(hdr + tab + data); print(stat)
