#!/usr/bin/env python3
"""3DS fix package without reinstalling: takes the program out of a (plain, unencrypted) CIA as Luma3DS game-patching files.
    python3 cia2luma.py <in.cia> <outdir>   -> outdir/luma/titles/<TID>/code.bin (decompressed) + exheader.bin
Needs Luma3DS with "Enable game patching" on."""
import sys, struct, os
d = open(sys.argv[1], 'rb').read()
al = lambda x: (x + 63) // 64 * 64
hs, ty, ver, cs, ts, ms = struct.unpack_from('<IHHIII', d, 0)
con = al(al(al(al(hs) + cs) + ts) + ms)
n = d[con:]
assert n[0x100:0x104] == b'NCCH'
tid = struct.unpack_from('<Q', n, 0x118)[0]
exh = bytearray(n[0x200:0x200 + 0x800])
eo = struct.unpack_from('<I', n, 0x1A0)[0] * 0x200
ef = n[eo:]
code = None
for i in range(10):
    nm = ef[i * 16:i * 16 + 8].rstrip(b'\0'); off, sz = struct.unpack_from('<II', ef, i * 16 + 8)
    if nm == b'.code': code = ef[0x200 + off:0x200 + off + sz]
assert code is not None
def blz(data):
    size = len(data); tb, add = struct.unpack_from('<II', data, size - 8)
    top, bottom = tb & 0xFFFFFF, tb >> 24
    out = bytearray(size + add); out[:size] = data
    src, dst, end = size - bottom, size + add, size - top
    while src > end:
        src -= 1; flag = out[src]
        for _ in range(8):
            if src <= end: break
            if flag & 0x80:
                src -= 2; v = out[src] | out[src + 1] << 8; ln = (v >> 12) + 3; off = (v & 0xFFF) + 3
                for _ in range(ln): dst -= 1; out[dst] = out[dst + off]
            else:
                src -= 1; dst -= 1; out[dst] = out[src]
            flag = (flag << 1) & 0xFF
    return bytes(out)
compressed = exh[0x0D] & 1
if compressed: code = blz(code); exh[0x0D] &= 0xFE
# sanity: decompressed size = text + ro + data (page aligned)
tx, ro, da = (struct.unpack_from('<I', exh, 0x10 + 0x08)[0], struct.unpack_from('<I', exh, 0x20 + 0x08)[0], struct.unpack_from('<I', exh, 0x30 + 0x08)[0])
pg = lambda x: (x + 0xFFF) & ~0xFFF
print('TID %016X compressed %d code %d (text %d ro %d data %d -> %d)' % (tid, compressed, len(code), tx, ro, da, pg(tx * 0x1000) if False else 0))
out = os.path.join(sys.argv[2], 'luma', 'titles', '%016X' % tid); os.makedirs(out, exist_ok=True)
open(os.path.join(out, 'code.bin'), 'wb').write(code); open(os.path.join(out, 'exheader.bin'), 'wb').write(exh)
print(out)
