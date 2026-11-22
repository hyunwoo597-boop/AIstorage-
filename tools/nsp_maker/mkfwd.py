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
elif sys.argv[1] == 'romfs':
    # all-in-one NSP: nx-hbloader that reads nonoxp.nro out of the title's own RomFS with plain fs-service calls
    # (no heap, no romfs device: the loader keeps only a 16KB heap), logs each step to sdmc:/switch/nonoxp/fwd.txt
    p = sys.argv[2]; s = open(p).read()
    def rep(o, n):
        global s
        assert o in s, o; s = s.replace(o, n, 1)
    rep('void __libnx_initheap(void)', r'''static s64 g_flogOff = -1;
static void flog(const char *msg, u64 v)
{
    char b[96]; int n = 0; while (*msg && n < 70) b[n++] = *msg++;
    b[n++] = ' '; b[n++] = '0'; b[n++] = 'x'; for (int i = 15; i >= 0; i--) { int d = (v >> (i * 4)) & 15; if (d || n > 3 + 0 || i == 0) b[n++] = "0123456789abcdef"[d]; }
    b[n++] = '\n';
    FsFileSystem sd; if (R_FAILED(fsOpenSdCardFileSystem(&sd))) return;
    static const char path[] = "/switch/nonoxp/fwd.txt";
    if (g_flogOff < 0) { fsFsCreateDirectory(&sd, "/switch"); fsFsCreateDirectory(&sd, "/switch/nonoxp"); fsFsDeleteFile(&sd, path); g_flogOff = 0; }
    fsFsCreateFile(&sd, path, 0, 0);
    FsFile f; if (R_SUCCEEDED(fsFsOpenFile(&sd, path, FsOpenMode_Write | FsOpenMode_Append, &f))) {
        if (R_SUCCEEDED(fsFileWrite(&f, g_flogOff, b, n, FsWriteOption_Flush))) g_flogOff += n; fsFileClose(&f); }
    fsFsClose(&sd);
}
static void __attribute__((noreturn)) fwdAbort(Result rc) { flog("ABORT", rc); diagAbortWithResult(rc); }
static int g_fromRomfs;
/* nonoxp.nro from the RomFS of this title -> buf */
static void readRomfsNro(u8 *buf)
{
    FsStorage st; Result rc = fsOpenDataStorageByCurrentProcess(&st); flog("open RomFS", rc);
    if (R_FAILED(rc)) fwdAbort(MAKERESULT(Module_HomebrewLoader, 401));
    u64 h[10]; rc = fsStorageRead(&st, 0, h, sizeof h); flog("RomFS header", h[0]);
    if (R_FAILED(rc) || h[0] != 0x50) fwdAbort(MAKERESULT(Module_HomebrewLoader, 402));
    static u8 meta[0x4000]; u64 ms = h[8]; if (ms > sizeof meta) ms = sizeof meta;
    rc = fsStorageRead(&st, h[7], meta, ms); if (R_FAILED(rc)) fwdAbort(MAKERESULT(Module_HomebrewLoader, 403));
    for (u64 p = 0; p + 0x20 <= ms; ) {
        u32 nl; u64 off, size; memcpy(&off, meta + p + 8, 8); memcpy(&size, meta + p + 16, 8); memcpy(&nl, meta + p + 28, 4);
        if (nl == 10 && p + 0x20 + nl <= ms && !memcmp(meta + p + 0x20, "nonoxp.nro", 10)) {
            flog("nonoxp.nro size", size);
            /* only the program part (NroHeader.size): what follows it in the file (icon, NACP, assets) must not land on the
               .bss after .data, which the game expects to be zero (hbl reads header->size bytes for the same reason) */
            rc = fsStorageRead(&st, h[9] + off, buf, sizeof(NroStart) + sizeof(NroHeader));
            if (R_FAILED(rc)) fwdAbort(MAKERESULT(Module_HomebrewLoader, 405));
            u32 psize = ((NroHeader *)(buf + sizeof(NroStart)))->size; flog("program size", psize);
            if (psize < sizeof(NroStart) + sizeof(NroHeader) || psize > size) fwdAbort(MAKERESULT(Module_HomebrewLoader, 406));
            rc = fsStorageRead(&st, h[9] + off + sizeof(NroStart) + sizeof(NroHeader), buf + sizeof(NroStart) + sizeof(NroHeader), psize - sizeof(NroStart) - sizeof(NroHeader)); flog("read", rc);
            if (R_FAILED(rc)) fwdAbort(MAKERESULT(Module_HomebrewLoader, 405));
            fsStorageClose(&st); return;
        }
        p += 0x20 + ((nl + 3) & ~3u);
    }
    fwdAbort(MAKERESULT(Module_HomebrewLoader, 404));
}

void __libnx_initheap(void)''')
    rep("""    if (g_nextNroPath[0] == '\\0')
    {
        memcpy(g_nextNroPath, DEFAULT_NRO, sizeof(DEFAULT_NRO));
        memcpy(g_nextArgv,    DEFAULT_NRO, sizeof(DEFAULT_NRO));
    }
""", """    if (g_nextNroPath[0] == '\\0')
    {
        static int s_started;
        if (s_started++) { flog("game returned", 0); svcExitProcess(); }
        flog("loader start", 0);
        g_fromRomfs = 1;
        strcpy(g_nextNroPath, "romfs:/nonoxp.nro"); strcpy(g_nextArgv, "romfs:/nonoxp.nro");
    }
""")
    rep("    rc = fsdevMountSdmc();\n", "    if (g_fromRomfs) { g_fromRomfs = 0; g_nextNroPath[0] = '\\0'; readRomfsNro(nrobuf); if (header->magic != NROHEADER_MAGIC) fwdAbort(MAKERESULT(Module_HomebrewLoader, 5)); goto nro_loaded; }\n    rc = fsdevMountSdmc();\n")
    rep("    close(fd);\n    fsdevUnmountAll();\n", "    close(fd);\n    fsdevUnmountAll();\nnro_loaded:\n")
    s = s.replace('diagAbortWithResult(MAKERESULT', 'fwdAbort(MAKERESULT')
    # leave 512MB of the application memory out of the game's heap (the GPU driver and system services take theirs from what is left)
    rep('    Result rc = svcSetHeapSize(&addr, size);', '    if (size > 0x40000000) size -= 0x20000000;\n    Result rc = svcSetHeapSize(&addr, size);')
    rep('    nroEntrypointTrampoline(&entries[0], -1, g_nroAddr);', '    flog("jump to NRO, heap MB", nro_heap_size >> 20);\n    nroEntrypointTrampoline(&entries[0], -1, g_nroAddr);')
    open(p, 'w').write(s); print('patched (romfs)', p)
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
