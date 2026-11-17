# Builds SOUND.ARC from the game's MUSIC.ARC: every WAV becomes Ogg Vorbis (same name), loop points from the 'smpl' chunk kept.
# Entry layout: "AI5S" u32 loop_start u32 loop_end(exclusive, 0 = no loop) u32 sample_rate, then the .ogg stream.
# Archive layout = the game's ARC format (u32 count, 28-byte entries name[20] size^0x15056109 off^0x32300645).
# usage: python3 mksound.py MUSIC.ARC SOUND.ARC [quality]
import sys, struct, subprocess, tempfile, os
src, dst = sys.argv[1], sys.argv[2]; q = sys.argv[3] if len(sys.argv) > 3 else '3'
f = open(src, 'rb'); n = struct.unpack('<I', f.read(4))[0]; t = f.read(n * 28)
ents = [(t[i*28:i*28+20].split(b'\0')[0], struct.unpack('<I', t[i*28+20:i*28+24])[0] ^ 0x15056109,
         struct.unpack('<I', t[i*28+24:i*28+28])[0] ^ 0x32300645) for i in range(n)]
out = []
for name, size, off in ents:
    f.seek(off); wav = f.read(size)
    i = 12; rate = 22050; ls = le = 0
    while i + 8 <= len(wav):
        tag = wav[i:i+4]; ln = struct.unpack('<I', wav[i+4:i+8])[0]
        if tag == b'fmt ': rate = struct.unpack('<I', wav[i+12:i+16])[0]
        if tag == b'smpl' and ln >= 36 + 24 and struct.unpack('<I', wav[i+8+28:i+8+32])[0] > 0:
            ls, le = struct.unpack('<II', wav[i+8+36+8:i+8+36+16]); le += 1
        i += 8 + ln + (ln & 1)
    with tempfile.TemporaryDirectory() as td:
        a = os.path.join(td, 'a.wav'); b = os.path.join(td, 'b.ogg'); open(a, 'wb').write(wav)
        subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', a, '-c:a', 'libvorbis', '-q:a', q, b], check=True)
        ogg = open(b, 'rb').read()
    out.append((name, b'AI5S' + struct.pack('<III', ls, le, rate) + ogg))
    print(name.decode(), rate, ls, le, len(wav), '->', len(ogg))
with open(dst, 'wb') as o:
    o.write(struct.pack('<I', len(out))); pos = 4 + 28 * len(out); tab = b''
    for name, data in out:
        tab += name.ljust(20, b'\0') + struct.pack('<II', len(data) ^ 0x15056109, pos ^ 0x32300645); pos += len(data)
    o.write(tab)
    for _, data in out: o.write(data)
