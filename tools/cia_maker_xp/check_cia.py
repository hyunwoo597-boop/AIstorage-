# Independent check of a CIA made by the HTML maker: TMD content hash, NCCH exheader / RomFS superblock hashes, the IVFC
# hash tree and the RomFS file table (lists files, optional compare with local files).  usage: check_cia.py file.cia [dir]
import sys, struct, hashlib, os
f = open(sys.argv[1], 'rb'); d = lambda o, n: (f.seek(o), f.read(n))[1]
u32 = lambda b, o: struct.unpack_from('<I', b, o)[0]
h = d(0, 0x20); hs, cs, ts, ms = struct.unpack_from('<IIII', h, 4)[0], u32(h, 8), u32(h, 12), u32(h, 16)
csz = struct.unpack_from('<Q', h, 24)[0]
al = lambda x: (x + 63) // 64 * 64
cert = al(0x2020); tik = al(cert + cs); tmd = al(tik + ts); con = al(tmd + ms)
tm = d(tmd, ms); sig = {0x10004: 0x140, 0x10003: 0x240}[struct.unpack('>I', tm[:4])[0]]
th = sig; chunk = th + 0xc4 + 0x24 * 64
csize = struct.unpack('>Q', tm[chunk + 8:chunk + 16])[0]; chash = tm[chunk + 16:chunk + 48]
assert csize == csz, 'size mismatch'
sh = hashlib.sha256(); left = csz; o = con
while left: n = min(left, 16 << 20); sh.update(d(o, n)); o += n; left -= n
print('content hash', 'OK' if sh.digest() == chash else 'BAD')
print('chunk record hash', 'OK' if hashlib.sha256(tm[chunk:chunk + 0x30]).digest() == tm[th + 0xc4 + 4:th + 0xc4 + 36] else 'BAD')
print('info hash', 'OK' if hashlib.sha256(tm[th + 0xc4:th + 0xc4 + 0x24 * 64]).digest() == tm[th + 0xa4:th + 0xc4] else 'BAD')
nc = d(con, 0x200)
assert nc[0x100:0x104] == b'NCCH'
ex = d(con + 0x200, 0x400); print('exheader hash', 'OK' if hashlib.sha256(ex).digest() == nc[0x160:0x180] else 'BAD')
ro, rs = u32(nc, 0x1b0) * 0x200, u32(nc, 0x1b4) * 0x200; R = con + ro
sb = d(R, 0x200); print('romfs superblock hash', 'OK' if hashlib.sha256(sb).digest() == nc[0x1e0:0x200] else 'BAD')
msize = u32(sb, 8); lv = [(struct.unpack_from('<Q', sb, 0x0c + i * 0x18)[0], struct.unpack_from('<Q', sb, 0x14 + i * 0x18)[0]) for i in range(3)]
l3pos = (0x60 + msize + 0xfff) // 0x1000 * 0x1000
l1pos = l3pos + (lv[2][1] + 0xfff) // 0x1000 * 0x1000; l2pos = l1pos + (lv[0][1] + 0xfff) // 0x1000 * 0x1000
def level_ok(src, size, dst):
    bad = 0; hashes = d(dst, (size + 0xfff) // 0x1000 * 32)
    for k in range((size + 0xfff) // 0x1000):
        blk = d(src + k * 0x1000, 0x1000)
        if hashlib.sha256(blk).digest() != hashes[k * 32:k * 32 + 32]: bad += 1
    return bad
print('master->L1 bad blocks', level_ok(R + l1pos, lv[0][1], R + 0x60))
print('L1->L2 bad blocks', level_ok(R + l2pos, lv[1][1], R + l1pos))
print('L2->L3 bad blocks', level_ok(R + l3pos, lv[2][1], R + l2pos))
L3 = R + l3pos; hd = d(L3, 0x28); fmo, fms, fdo = u32(hd, 0x1c), u32(hd, 0x20), u32(hd, 0x24)
fm = d(L3 + fmo, fms); p = 0
while p < fms:
    par, sib = u32(fm, p), u32(fm, p + 4); off, size = struct.unpack_from('<QQ', fm, p + 8); nl = u32(fm, p + 28)
    name = fm[p + 32:p + 32 + nl].decode('utf-16le'); line = '%-16s %10d' % (name, size)
    if len(sys.argv) > 2 and os.path.exists(os.path.join(sys.argv[2], name)):
        a = open(os.path.join(sys.argv[2], name), 'rb').read(); b = d(L3 + fdo + off, size); line += '  ' + ('same' if a == b else 'DIFF')
    print(line); p += 0x20 + (nl + 3) // 4 * 4
