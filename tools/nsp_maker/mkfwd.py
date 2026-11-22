#!/usr/bin/env python3
"""Installed (NSP) build = nx-hbloader that loads romfs:/nonoxp.nro from the title's own RomFS (the game program itself
is not used as the NSO: a libnx NRO program started directly as the title's NSO closed at once on the Switch).
  mkfwd.py patch <nx-hbloader/source/main.c>          : load romfs:/nonoxp.nro, exit to HOME when it returns
  mkfwd.py npdm <hbl.npdm> <out.npdm> <program id hex>  : program id, application type 1 (application)"""
import sys, struct
if sys.argv[1] == 'patch':
    p = sys.argv[2]; s = open(p).read()
    def rep(o, n):
        global s
        assert o in s, o; s = s.replace(o, n, 1)
    # a log on the SD card (sdmc:/switch/nonoxp/fwd.txt) written straight through the fs service (no heap, no newlib)
    rep('void __libnx_initheap(void)', r'''static s64 g_flogOff = -1;
static void flog(const char *fmt, ...)
{
    char b[200]; va_list ap; va_start(ap, fmt); int n = vsnprintf(b, sizeof b - 1, fmt, ap); va_end(ap);
    if (n < 0) return; if (n > (int)sizeof b - 2) n = sizeof b - 2; b[n++] = '\n';
    FsFileSystem sd; if (R_FAILED(fsOpenSdCardFileSystem(&sd))) return;
    static const char path[] = "/switch/nonoxp/fwd.txt";
    if (g_flogOff < 0) { fsFsCreateDirectory(&sd, "/switch"); fsFsCreateDirectory(&sd, "/switch/nonoxp"); fsFsDeleteFile(&sd, path); g_flogOff = 0; }
    fsFsCreateFile(&sd, path, 0, 0);
    FsFile f; if (R_SUCCEEDED(fsFsOpenFile(&sd, path, FsOpenMode_Write | FsOpenMode_Append, &f))) {
        if (R_SUCCEEDED(fsFileWrite(&f, g_flogOff, b, n, FsWriteOption_Flush))) g_flogOff += n; fsFileClose(&f); }
    fsFsClose(&sd);
}
static void NORETURN fwdAbort(Result rc) { flog("ABORT 2%03u-%04u (0x%x)", R_MODULE(rc), R_DESCRIPTION(rc), rc); diagAbortWithResult(rc); }

void __libnx_initheap(void)''')
    rep('#include <unistd.h>', '#include <unistd.h>\n#include <stdio.h>\n#include <stdarg.h>\n#include <errno.h>')
    rep('''    if (g_nextNroPath[0] == '\\0')
    {
        memcpy(g_nextNroPath, DEFAULT_NRO, sizeof(DEFAULT_NRO));
        memcpy(g_nextArgv,    DEFAULT_NRO, sizeof(DEFAULT_NRO));
    }
''', '''    static int s_loaded;
    if (g_nextNroPath[0] == '\\0')
    {
        if (s_loaded) { flog("game returned: exit"); svcExitProcess(); }      /* the game returned: back to HOME */
        flog("loader start (NSP)");
        Result rr = romfsInit();
        flog("romfsInit 0x%x", rr);
        int ok = 0;
        if (R_SUCCEEDED(rr)) { int t = open("romfs:/nonoxp.nro", O_RDONLY); flog("open romfs:/nonoxp.nro fd %d errno %d", t, t < 0 ? errno : 0); if (t >= 0) { close(t); ok = 1; } }
        const char *path = ok ? "romfs:/nonoxp.nro" : "sdmc:/switch/nonoxp/nonoxp.nro";
        flog("start %s", path);
        strcpy(g_nextNroPath, path); strcpy(g_nextArgv, path);
    }
    s_loaded = 1;
''')
    s = s.replace('diagAbortWithResult(MAKERESULT', 'fwdAbort(MAKERESULT')
    rep('    close(fd);\n    fsdevUnmountAll();', '    close(fd);\n    flog("NRO read: %u bytes", (unsigned)header->size);\n    fsdevUnmountAll();')
    rep('    nroEntrypointTrampoline(&entries[0], -1, g_nroAddr);', '    flog("jump to NRO (heap %u MB)", (unsigned)(nro_heap_size >> 20));\n    nroEntrypointTrampoline(&entries[0], -1, g_nroAddr);')
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
