# PLATE.PAK for the 3DS 3D view of event CGs (platform/3ds/plat_3ds.c cg_match): for each CG with people, the room with
# the people removed (AI inpainting, see the private CI) and the figure mask.
# usage: mkplate.py img_dir mask_dir plate_dir out.pak [patch_list.txt pose.json]
#   img_dir/NAME.png   original 640x480 picture
#   mask_dir/NAME.png  figure mask 320x240 (tools/depth/char_only.py with MASKOUT=dir)
#   plate_dir/NAME.png the same picture with the people inpainted
# "PLT1" | u32 n | n x { char name[16] ("G05.GCC"), u32 offset } | per entry: thumbnail 40x30 RGB (16x16 block means of every
# 2nd pixel - exactly as cg_match computes it from the screen), mask 320x240 (255 = figure), room 320x240 RGB (2x2 means)
import os, sys, glob, struct, numpy as np
from PIL import Image, ImageFilter
img, msk, plt, out = sys.argv[1:5]
from scipy import ndimage as ndi
names = sorted(os.path.basename(f) for f in glob.glob(os.path.join(plt, '*.png')))
ents = []
for n in names:
    o = np.asarray(Image.open(os.path.join(img, n)).convert('RGB'), np.int32)
    if o.shape[:2] != (480, 640): continue
    th = o[::2, ::2].reshape(30, 8, 40, 8, 3).sum(axis=(1, 3)) // 64
    m = Image.open(os.path.join(msk, n)).convert('L').resize((320, 240), Image.NEAREST).filter(ImageFilter.MaxFilter(3))   # 1 px wider: the outline goes with the figure
    if (np.asarray(m) > 127).mean() < 0.02: print('no character found, skipped:', n); continue
    p = np.asarray(Image.open(os.path.join(plt, n)).convert('RGB'), np.int32)
    room = (p[0::2, 0::2] + p[1::2, 0::2] + p[0::2, 1::2] + p[1::2, 1::2]) // 4
    ents.append((n[:-4] + '.GCC', th.astype(np.uint8).tobytes() + np.asarray(m, np.uint8).tobytes() + room.astype(np.uint8).tobytes()))
# people pasted as a patch over a CG without people (G23A on G23: two people on the chairs): the room is the base picture
# itself, the figures are what the patch changes. lines: "PATCH.GCC BASE.GCC x y"
if len(sys.argv) > 5:
    have = {e[0] for e in ents}; import json; pose = json.load(open(sys.argv[6])) if len(sys.argv) > 6 else {}
    for l in open(sys.argv[5]):
        f = l.split()
        if len(f) < 4: continue
        pn, bn, x, y = f[0][:-4], f[1][:-4], int(f[2]), int(f[3])
        if bn + '.GCC' in have or not os.path.exists(os.path.join(img, pn + '.png')): continue   # base with people: its own plate covers variants
        if pose and not pose.get(pn + '.GCC', {}).get('people'): continue                           # a patch without people (G40A: an object)
        base = np.asarray(Image.open(os.path.join(img, bn + '.png')).convert('RGB'), np.int32)
        if base.shape[:2] != (480, 640): continue
        pa = Image.open(os.path.join(img, pn + '.png')).convert('RGB'); pw, ph = pa.size; pa = np.asarray(pa, np.int32)
        pw, ph = min(pw, 640 - x), min(ph, 480 - y); comp = base.copy(); comp[y:y + ph, x:x + pw] = pa[:ph, :pw]
        diff = np.abs(comp - base).sum(axis=2) > 36
        diff = ndi.binary_opening(diff, iterations=2)
        if diff.sum() < 2000: continue
        m = diff.reshape(240, 2, 320, 2).any(axis=(1, 3))
        m = ndi.binary_fill_holes(ndi.binary_closing(m, iterations=3)); m = ndi.binary_dilation(m, iterations=1)
        th = comp[::2, ::2].reshape(30, 8, 40, 8, 3).sum(axis=(1, 3)) // 64
        room = (base[0::2, 0::2] + base[1::2, 0::2] + base[0::2, 1::2] + base[1::2, 1::2]) // 4
        ents.append((pn + '.GCC', th.astype(np.uint8).tobytes() + (m * 255).astype(np.uint8).tobytes() + room.astype(np.uint8).tobytes()))
        print('patch scene', pn, 'over', bn, int(m.sum()), 'figure pixels')
off = 8 + 20 * len(ents); tab = b''; data = b''
for name, blob in ents:
    tab += name.encode().ljust(16, b'\0') + struct.pack('<I', off + len(data)); data += blob
open(out, 'wb').write(b'PLT1' + struct.pack('<I', len(ents)) + tab + data)
print(len(ents), 'pictures,', 8 + len(tab) + len(data), 'bytes')
