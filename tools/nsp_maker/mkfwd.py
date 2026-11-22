#!/usr/bin/env python3
"""Installed (NSP) build = nx-hbloader that loads romfs:/nonoxp.nro from the title's own RomFS (the game program itself
is not used as the NSO: a libnx NRO program started directly as the title's NSO closed at once on the Switch).
  mkfwd.py patch <nx-hbloader/source/main.c>          : load romfs:/nonoxp.nro, exit to HOME when it returns
  mkfwd.py npdm <hbl.npdm> <out.npdm> <program id hex>  : program id, application type 1 (application)"""
import sys, struct
if sys.argv[1] == 'patch':
    p = sys.argv[2]; s = open(p).read()
    o = '''    if (g_nextNroPath[0] == '\\0')
    {
        memcpy(g_nextNroPath, DEFAULT_NRO, sizeof(DEFAULT_NRO));
        memcpy(g_nextArgv,    DEFAULT_NRO, sizeof(DEFAULT_NRO));
    }
'''
    n = '''    static int s_loaded;
    if (g_nextNroPath[0] == '\\0')
    {
        if (s_loaded) svcExitProcess();      /* the game returned: back to HOME */
        strcpy(g_nextNroPath, "romfs:/nonoxp.nro");
        strcpy(g_nextArgv, "romfs:/nonoxp.nro");
    }
    s_loaded = 1;
'''
    assert o in s, 'default-path block not found'; s = s.replace(o, n)
    o2 = '    int fd = open(g_nextNroPath, O_RDONLY);'
    assert o2 in s
    s = s.replace(o2, '''    if (!strncmp(g_nextNroPath, "romfs:", 6) && R_FAILED(romfsInit()))
        diagAbortWithResult(MAKERESULT(Module_HomebrewLoader, 405));
''' + o2)
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
