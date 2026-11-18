import glob, struct
ents = []
for f in sorted(glob.glob('parts/**/CG_*.PAK', recursive=True)):
    d = open(f, 'rb').read(); n = struct.unpack('<I', d[4:8])[0]
    for i in range(n):
        e = d[8 + i * 24:32 + i * 24]; w, h, o = struct.unpack('<HHI', e[16:24]); ents.append((e[:16], w, h, d[o:o + w * h]))
hdr = b'DPK1' + struct.pack('<I', len(ents)); off = 8 + 24 * len(ents); tab = b''; data = b''
for nm, w, h, b in ents: tab += nm + struct.pack('<HHI', w, h, off + len(data)); data += b
open('out/CG.PAK', 'wb').write(hdr + tab + data); print(len(ents))
