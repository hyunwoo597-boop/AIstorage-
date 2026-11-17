# Removes "대사창 숨기기" (hide window) and "환경 설정" (settings) from the in-game system menu (LIBLARY.LIB, LIBDEF 6):
# their buttons are not drawn, cannot be highlighted or chosen, and the D-pad skips them. Byte patches on constants of the
# Korean patch's LIBLARY.LIB (same layout as the Japanese one). Output is LZSS-compressed for patch/ override use.
# usage: patch_sysmenu.py LIBLARY.LIB(decompressed) lzss_enc out
import sys, subprocess, tempfile, os
src, lz, out = sys.argv[1:4]
d = bytearray(open(src, 'rb').read()); B = 4                     # code starts after the u32 label count (0 labels)
def patch(addr, off, old, new):
    a = B + addr + off; assert d[a] == old, (hex(addr), off, d[a], old); d[a] = new
NEVER = 0x63                                                    # 99: an item index that does not exist
patch(0x1d7e, 3, 0, NEVER)   # PROC 0 highlight, left column item 0 (hide window)
patch(0x1e16, 3, 1, NEVER)   # PROC 0 highlight, left column item 1 (settings)
patch(0x22ad, 3, 0, NEVER)   # PROC 2 confirm item 0
patch(0x22cc, 3, 1, NEVER)   # PROC 2 confirm item 1
patch(0x23d6, 3, 0, 2)       # PROC 4 up: stop at item 2 ...
patch(0x240b, 3, 0, 2)       # ... and from item 2 jump to the right column like the original did from item 0
patch(0x2580, 2, 0, 2)       # PROC 6 left: land on item 2
patch(0x20f4, 4, 7, NEVER)   # PROC 1: do not draw button 0 (SYS 11 sub 7 -> unused sub)
patch(0x215d, 4, 7, NEVER)   # PROC 1: do not draw button 1
patch(0x212c, 4, 0, NEVER)   # PROC 1: do not draw the window-hidden state mark on button 0
with tempfile.TemporaryDirectory() as t:
    p = os.path.join(t, 'l'); open(p, 'wb').write(d); subprocess.run([lz, p, out], check=True, capture_output=True)
print('ok')
