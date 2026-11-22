#!/usr/bin/env python3
"""Installed (NSP) build = nx-hbloader that starts sdmc:/switch/nonoxp/nonoxp.nro (an SD-card forwarder).
  mkfwd.py patch <nx-hbloader/source/main.c>
  mkfwd.py npdm <hbl.npdm> <out.npdm> <program id hex>  : program id, application type 1"""
import sys, struct
if sys.argv[1] == 'patch':
    # the plain nx-hbloader (an NSP with it starts sdmc:/hbmenu.nro fine) with only two changes:
    # it starts sdmc:/switch/nonoxp/nonoxp.nro instead of hbmenu, and goes back to HOME when the game returns
    p = sys.argv[2]; s = open(p).read()
    o = '#define DEFAULT_NRO "sdmc:/hbmenu.nro"'
    assert o in s; s = s.replace(o, '#define DEFAULT_NRO "sdmc:/switch/nonoxp/nonoxp.nro"')
    o = """    if (g_nextNroPath[0] == '\\0')
    {
"""
    assert o in s; s = s.replace(o, o + """        static int s_started;
        if (s_started++) svcExitProcess();
""", 1)
    open(p, 'w').write(s); print('patched', p)
elif sys.argv[1] == 'npdm':
    n = bytearray(open(sys.argv[2], 'rb').read()); tid = int(sys.argv[4], 16)
    a0, a0s, ad, ads = struct.unpack_from('<IIII', n, 0x70)
    struct.pack_into('<Q', n, a0 + 0x10, tid); struct.pack_into('<QQ', n, ad + 0x210, tid, tid)
    for base, off in ((a0, 0x30), (ad, 0x230)):
        ko, ks = struct.unpack_from('<II', n, base + off)
        for i in range(0, ks, 4):
            v = struct.unpack_from('<I', n, base + ko + i)[0]
            if v & 0x3fff == 0x1fff: struct.pack_into('<I', n, base + ko + i, (1 << 14) | 0x1fff)
    open(sys.argv[3], 'wb').write(n); print('npdm', sys.argv[3])
