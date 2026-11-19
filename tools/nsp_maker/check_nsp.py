# Independent check of an NSP made by the HTML maker (needs the same prod.keys):
# NCA header (AES-XTS), key area, section AES-CTR, PFS0 hash tables, IVFC levels, Switch RomFS file table, CNMT records.
# usage: check_nsp.py file.nsp prod.keys [dir-to-compare]
import sys, struct, hashlib, os
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
keys = {}
for l in open(sys.argv[2]):
    if '=' in l: k, v = l.split('=', 1); keys[k.strip()] = bytes.fromhex(v.strip())
hk, kak = keys['header_key'], keys['key_area_key_application_00']
f = open(sys.argv[1], 'rb'); R = lambda o, n: (f.seek(o), f.read(n))[1]
u32 = lambda b, o: struct.unpack_from('<I', b, o)[0]; u64 = lambda b, o: struct.unpack_from('<Q', b, o)[0]
def pfs0(base, read):
    h = read(base, 0x10); assert h[:4] == b'PFS0', h[:4]; n, st = u32(h, 4), u32(h, 8); ent = read(base + 0x10, n * 0x18); strs = read(base + 0x10 + n * 0x18, st)
    data = base + 0x10 + n * 0x18 + st; out = []
    for i in range(n): o, s, no = u64(ent, i * 0x18), u64(ent, i * 0x18 + 8), u32(ent, i * 0x18 + 16); nm = strs[no:strs.index(b'\0', no)].decode(); out.append((nm, data + o, s))
    return out
def xts_dec(data):
    out = b''
    for s in range(len(data) // 0x200):
        d = Cipher(algorithms.AES(hk), modes.XTS(s.to_bytes(16, 'big'))).decryptor(); out += d.update(data[s * 0x200:(s + 1) * 0x200]) + d.finalize()
    return out
def check_nca(off, size, label):
    hdr = xts_dec(R(off, 0xC00)); assert hdr[0x200:0x204] == b'NCA3', 'magic'
    print('%s: NCA3 type %d size %d tid %016x' % (label, hdr[0x205], u64(hdr, 0x208), u64(hdr, 0x210)))
    assert u64(hdr, 0x208) == size
    ka = Cipher(algorithms.AES(kak), modes.ECB()).decryptor().update(hdr[0x300:0x340]); key = ka[0x20:0x30]
    secs = []
    for i in range(4):
        st, en = u32(hdr, 0x240 + i * 0x10) * 0x200, u32(hdr, 0x244 + i * 0x10) * 0x200
        if not en: continue
        fh = hdr[0x400 + i * 0x200:0x600 + i * 0x200]; assert hashlib.sha256(fh).digest() == hdr[0x280 + i * 0x20:0x2A0 + i * 0x20], 'fs header hash'
        ctrhi = fh[0x140:0x148][::-1]
        def read(o, n, st=st, ctrhi=ctrhi):
            a = (o // 16) * 16; b = ((o + n + 15) // 16) * 16; c = R(off + st + a, b - a)
            ctr = ctrhi + ((st + a) // 16).to_bytes(8, 'big'); p = Cipher(algorithms.AES(key), modes.CTR(ctr)).decryptor().update(c)
            return p[o - a:o - a + n]
        ht, hty, enc = fh[2], fh[3], fh[4]; hd = fh[8:0x100]
        if hty == 2:
            bs = u32(hd, 0x20); to, tsz, po, psz = u64(hd, 0x28), u64(hd, 0x30), u64(hd, 0x38), u64(hd, 0x40)
            table = read(to, tsz); assert hashlib.sha256(table).digest() == hd[:0x20], 'pfs master'
            bad = sum(1 for k in range(psz // bs) if hashlib.sha256(read(po + k * bs, bs)).digest() != table[k * 32:k * 32 + 32])
            print('  section %d PFS0 bad blocks %d' % (i, bad)); secs.append(('pfs', lambda o, n, po=po, read=read: read(po + o, n)))
        else:
            assert hd[:4] == b'IVFC'; lv = [(u64(hd, 0x10 + j * 0x18), u64(hd, 0x18 + j * 0x18), u32(hd, 0x20 + j * 0x18)) for j in range(6)]
            bs = 1 << lv[0][2]; assert hashlib.sha256(read(lv[0][0], bs)).digest() == hd[0xC0:0xE0], 'ivfc master'
            bad = 0
            for j in range(5):
                hashes = read(lv[j][0], lv[j][1]); nb = lv[j + 1][1] // bs
                for k in range(nb):
                    if hashlib.sha256(read(lv[j + 1][0] + k * bs, bs)).digest() != hashes[k * 32:k * 32 + 32]: bad += 1
            print('  section %d IVFC bad blocks %d' % (i, bad)); secs.append(('rom', lambda o, n, base=lv[5][0], read=read: read(base + o, n)))
    return hdr, secs
def romfs_list(read):
    h = read(0, 0x50); fmo, fms, fdo = u64(h, 0x38), u64(h, 0x40), u64(h, 0x48); fm = read(fmo, fms); p = 0; out = []
    while p < fms:
        off, size, nl = u64(fm, p + 8), u64(fm, p + 16), u32(fm, p + 28); nm = fm[p + 32:p + 32 + nl].decode(); out.append((nm, fdo + off, size)); p += 0x20 + (nl + 3) // 4 * 4
    return out
top = pfs0(0, lambda o, n: R(o, n)); hashes = {}
for nm, o, s in top:
    sh = hashlib.sha256(); left, at = s, o
    while left: k = min(left, 16 << 20); sh.update(R(at, k)); at += k; left -= k
    hashes[nm] = sh.digest(); print(nm, s, 'id-ok' if sh.digest()[:16].hex() == nm.split('.')[0] else 'ID MISMATCH')
for nm, o, s in top:
    hdr, secs = check_nca(o, s, nm)
    if hdr[0x205] == 1:
        rd = secs[0][1]; files = pfs0(0, rd); cn = rd(files[0][1], files[0][2]); print('  cnmt', files[0][0], 'tid %016x type %x' % (u64(cn, 0), cn[0xC]))
        cnt = struct.unpack_from('<H', cn, 0x10)[0]
        for i in range(cnt):
            r = cn[0x30 + i * 0x38:0x68 + i * 0x38]; idn = r[0x20:0x30].hex(); match = [h for k, h in hashes.items() if k.startswith(idn)]
            print('   record type %d id %s %s' % (r[0x36], idn, 'hash-ok' if match and match[0] == r[:0x20] else 'BAD'))
    if hdr[0x205] in (0, 2):
        for kind, rd in secs:
            if kind == 'pfs': print('  exefs', [(n, s) for n, o2, s in pfs0(0, rd)])
            else:
                for n, o2, s2 in romfs_list(rd):
                    line = '   %-24s %10d' % (n, s2)
                    if len(sys.argv) > 3 and os.path.exists(os.path.join(sys.argv[3], n)):
                        a = open(os.path.join(sys.argv[3], n), 'rb').read(); line += '  ' + ('same' if rd(o2, s2) == a else 'DIFF')
                    print(line)
