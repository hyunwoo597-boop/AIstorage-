# 3D only on the characters (user request): backgrounds become one flat plane, characters keep their AI relief with the
# face smoothed (only the basic shape: detailed relief bent faces in the stereo pair) and chest / hips strongly lifted.
# usage: char_only.py DEPTH_preboost.PAK pose.json out.pak [gain=4.0] [preview_dir] [BG.ARC: masked pictures, ...24m]
import os, sys, json, struct, numpy as np
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
KEEPBG = __import__('os').environ.get('KEEPBG') == '1'   # KEEPBG=1: backgrounds keep their AI relief (characters still lifted in front)
maps = rd(sys.argv[1]); pose = json.load(open(sys.argv[2])); gain = float(sys.argv[4]) if len(sys.argv) > 4 else 4.0
prev = sys.argv[5] if len(sys.argv) > 5 and sys.argv[5] != '-' else None
masked = set()
if len(sys.argv) > 6:
    sys.path.insert(0, __import__('os').path.join(__import__('os').path.dirname(__file__), '..')); import arc
    ko, ks, ok, l = arc.solve(sys.argv[6]); f = open(sys.argv[6], 'rb')
    for n, o, sz in l:
        f.seek(o)
        if f.read(4)[3:4] == b'm': masked.add(n.upper())
PATCH = {n + '.GCC' for n in 'G107A G12A G23A G27C G28B G37A G40A G40C G41A G41B G46A G52A G56A G78A G81A G94A G94B G99A'.split()}
INSET = {n + '.GCC' for n in 'G36 G36A G38S G47S G54 G60S G65A G67S G72S G74S G97S OVER3B'.split()}
stat = {'flat': 0, 'chars': 0, 'sprite': 0, 'kept': 0}
for key in list(maps):
    name = key.rstrip(b'\0').decode(); mw, mh, D = maps[key]
    if name == 'TITLELOGO.GCC': stat['kept'] += 1; continue
    info = pose.get(name, {'people': [], 'w': mw * 2, 'h': mh * 2}); people = info['people']
    full = (info['w'] >= 600 and info['h'] >= 440) if name in pose else (mw, mh) in ((320, 240), (160, 120))   # full pictures vs sprites (0 outside the figure)
    if name in INSET: full = True                               # small framed pictures over the room (G38S, G74S, ...): whole pictures too
    # variant patches with people (PATCH, e.g. G23A: two people on the chairs) stay on the plain path - their depth is matched to
    # the base CG, and the figure mask there missed the seated woman's upper body; the head dome below lifts their faces                               # small framed pictures over the room (G38S, G74S, ...): whole pictures too
    if full and name in masked: full = False                    # masked picture (cut-out figures over another picture): like a sprite
    sx, sy = mw / info['w'], mh / info['h']; yy, xx = np.mgrid[0:mh, 0:mw].astype(np.float32)
    if full and not people:
        if not KEEPBG: maps[key][2] = np.full_like(D, float(np.percentile(D, 30)))
        elif name in INSET and name not in ('G65A.GCC', 'OVER3B.GCC'): maps[key][2] = 0.35 + 0.65 * D
        stat['flat'] += 1; continue
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
            if v.size >= 4: ref[y] = np.percentile(v, 35)   # (35: a limb outside the boxes on that row must not count as room)
        if np.isnan(ref).all(): ref[:] = np.percentile(D, 20)
        idx = np.arange(mh); ok_ = ~np.isnan(ref); ref = np.interp(idx, idx[ok_], ref[ok_])
        # a bright thing on a few rows (G05: the desk lamp's arm at the man's chin) raised the reference there and cut a band out
        # of the figure: the reference is the running median over a sixth of the height
        k = max(3, mh // 12); ref = np.array([np.median(ref[max(0, y - k):y + k + 1]) for y in range(mh)])
        for x0, y0, x1, y1 in boxes:
            reg = D[y0:y1, x0:x1]; t = otsu(reg.ravel())
            m = (reg > t) & (reg > ref[y0:y1, None] + 0.05)
            fg[y0:y1, x0:x1] = np.maximum(fg[y0:y1, x0:x1], m.astype(np.float32))
        # a limb that runs out of the person's box was lifted only inside it (hard rectangles across thighs, G27B / G88): the figure
        # mask grows from the box over connected pixels that also stand out from the room
        allowed = (D > ref[:, None] + 0.05)
        for _ in range(int(max(mw, mh) * 0.4)):
            g2 = fg.copy(); g2[1:] = np.maximum(g2[1:], fg[:-1]); g2[:-1] = np.maximum(g2[:-1], fg[1:]); g2[:, 1:] = np.maximum(g2[:, 1:], fg[:, :-1]); g2[:, :-1] = np.maximum(g2[:, :-1], fg[:, 1:])
            g2 = np.where(allowed, g2, fg)
            if (g2 == fg).all(): break
            fg = g2
        # close gaps inside each figure: a column whose figure pixels are interrupted by a short run (< 1/8 height) is filled
        g = max(2, mh // 8)
        for x0, y0, x1, y1 in boxes:
            for x in range(x0, x1):
                col = fg[y0:y1, x]; on = np.where(col > 0)[0]
                if on.size < 2: continue
                gaps = np.where(np.diff(on) > 1)[0]
                for gi in gaps:
                    a, b = on[gi], on[gi + 1]
                    if b - a <= g: col[a:b] = 1
        fg = np.clip((blur(fg, 2) - 0.35) / 0.3, 0, 1)          # clean, soft edge
        if fg.max() < 0.5:
            if not KEEPBG: maps[key][2] = np.full_like(D, float(np.percentile(D, 30)))
            stat['flat'] += 1; continue
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
    # each character is ONE body: the whole silhouette (hair, head, body, arms) sits on one level and gets a rounded volume from
    # its own outline, with the AI map's fine relief on top and the chest / hips lifted. Before, the face, the hair and the body
    # each kept the AI depth or their own lift and moved separately in 3D (hair behind the face, face behind the chest)
    from scipy import ndimage as ndi
    fgb = fg > 0.5
    if full and people:                                         # the head and hair: inside the head circle and standing out from the room
        hd = (F > 0.05) & (D > ref[:, None] + 0.06)
        if fgb.any() and hd.any():                              # only what joins the figure (G05: a ring of wall round the head otherwise)
            lab0, _ = ndi.label(fgb | hd); keep = np.unique(lab0[fgb]); fgb = np.isin(lab0, keep[keep > 0])
            fgb = ndi.binary_opening(fgb, iterations=1) | (fg > 0.5)
    if not fgb.any() or not people:
        out = np.where(fg > 0, np.maximum(D, 0.03), 0) if not full else D; stat['sprite' if not full else 'flat'] += 1
    else:
        lab, nl = ndi.label(fgb)
        dist = ndi.distance_transform_edt(fgb)
        loc = ndi.maximum_filter(dist, size=int(max(5, mh / 5)))
        vol = np.sqrt(np.clip(dist / np.maximum(loc, 1), 0, 1))           # 0 at the outline, 1 in the middle of each part
        relief = np.clip(D - ndi.gaussian_filter(D, max(2, mh / 25)), -0.12, 0.12) * (1 - 0.7 * np.clip(F, 0, 1))   # folds, hair strands; calm on the face
        if full and KEEPBG:
            near = blur(D * (1 - fg), max(4, mh / 10)) / np.maximum(blur(1 - fg, max(4, mh / 10)), 1e-3)
        base = np.zeros_like(D)
        for i in range(1, nl + 1):
            m = lab == i
            if m.sum() < 30: base[m] = np.median(D[m]); continue
            lv = float(np.median(D[m]))
            if full: lv = max(lv, float(np.median(near[m])) + 0.12) if KEEPBG else max(lv, bgv + 0.06)
            base[m] = lv
        Cf = base + 0.35 * vol + relief + W * (1 - np.clip(F, 0, 1)) * gain * 0.06
        if full:
            if os.environ.get('MASKOUT'):                       # figure masks for the inpainted room plates (PLATE.PAK)
                mk = fgb.copy()
                for p in people:                                # the rest of each body inside its box at about the body's depth (lower torso behind a desk...)
                    x0, y0, x1, y1 = p['box']; x0, x1 = int(x0 * sx), int(np.ceil(x1 * sx)); y0, y1 = int(y0 * sy), int(np.ceil(y1 * sy))
                    sl = (slice(max(0, y0), min(mh, y1)), slice(max(0, x0), min(mw, x1))); f0 = fgb[sl]
                    if f0.sum() < 20 or (x1 - x0) * (y1 - y0) < mw * mh * 0.15: continue   # small people far back: their box would come out as a rectangle
                    mk[sl] |= D[sl] >= float(np.median(D[sl][f0])) - 0.12
                lab1, _ = ndi.label(mk); keep = np.unique(lab1[fgb]); mk = np.isin(lab1, keep[keep > 0])
                mk = ndi.binary_fill_holes(ndi.binary_closing(mk, iterations=3))   # whole figures: no holes (a stethoscope, a gap under an arm left in the room layer tore)
                Image.fromarray((mk * 255).astype(np.uint8)).save(os.path.join(os.environ['MASKOUT'], name[:-4] + '.png'))
            fs = np.clip(blur(fgb.astype(np.float32), 1.5) * 1.2, 0, 1)
            out = D * (1 - fs) + np.where(fgb, Cf, D) * fs; stat['chars'] += 1
        else:
            out = np.where(fgb, np.maximum(Cf, 0.03), 0); stat['sprite'] += 1
    if name in INSET and name not in ('G65A.GCC', 'OVER3B.GCC'): out = 0.35 + 0.65 * out   # framed picture: in front of the room behind it (the two patches are matched to their base)
    mx = float(np.percentile(out, 99.7)) if full else float(out.max())
    if mx > 1: out = out / mx                                   # squeeze instead of clipping: lifted figures used to saturate at 255 together with the
                                                                 # near floor, so a face, a chest and the floor all had the same depth (no separation)
    out = np.clip(out, 0, 1); maps[key][2] = out
    if prev: Image.fromarray((np.hstack([D, out, np.maximum(W, F * 0.5)]) * 255).clip(0, 255).astype(np.uint8)).save('%s/%s.png' % (prev, name[:-4]))
hdr = b'DPK1' + struct.pack('<I', len(maps)); off = 8 + 24 * len(maps); tab = b''; data = b''
for nm, (w, h, a) in maps.items():
    b = (a * 255 + 0.5).clip(0, 255).astype(np.uint8).tobytes(); tab += nm + struct.pack('<HHI', w, h, off + len(data)); data += b
open(sys.argv[3], 'wb').write(hdr + tab + data); print(stat)
