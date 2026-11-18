# add a depth entry for the Korean title picture (patch/titlelogo.gcc = OP.GCC with the logo painted in):
# OP's AI depth, logo area (grown by 3 cells) as one flat plate in front.  usage: add_logo.py DEPTH.PAK op.raw titlelogo.raw out.pak
import sys, struct, numpy as np
from PIL import Image
from scipy.ndimage import binary_dilation
d = open(sys.argv[1], 'rb').read(); n = struct.unpack('<I', d[4:8])[0]; ents = []
for i in range(n):
    e = d[8 + i * 24:32 + i * 24]; nm = e[:16].rstrip(b'\0').decode(); w, h, off = struct.unpack('<HHI', e[16:24])
    if nm != 'TITLELOGO.GCC': ents.append((nm, w, h, d[off:off + w * h]))
op = np.fromfile(sys.argv[2], np.uint8).reshape(480, 640, 3)[::-1].astype(int); tl = np.fromfile(sys.argv[3], np.uint8).reshape(480, 640, 3)[::-1].astype(int)
diff = (np.abs(tl - op).sum(2) > 40).astype(np.uint8) * 255
m = np.asarray(Image.fromarray(diff).resize((160, 120), Image.BOX)) > 20
m = binary_dilation(m, np.ones((7, 7), bool))
dm = np.frombuffer([e for e in ents if e[0] == 'OP.GCC'][0][3], np.uint8).reshape(120, 160).copy(); dm[m] = 255
ents.append(('TITLELOGO.GCC', 160, 120, dm.tobytes()))
hdr = b'DPK1' + struct.pack('<I', len(ents)); off = 8 + 24 * len(ents); tab = b''; data = b''
for nm, w, h, b in ents: tab += nm.encode().ljust(16, b'\0') + struct.pack('<HHI', w, h, off + len(data)); data += b
open(sys.argv[4], 'wb').write(hdr + tab + data)
Image.fromarray(dm).save(sys.argv[4] + '.png')
