# NACP: the title name / publisher in every language slot, Korean included, and the supported-language flags set to match.
# nacptool fills slots 0-11 only, so a Korean console (slot 12) had no name of its own and DBI listed no KR.
# slots: 0 en-US 1 en-GB 2 ja 3 fr 4 de 5 es-419 6 es 7 it 8 nl 9 fr-CA 10 pt 11 ru 12 ko 13 zh-Hant 14 zh-Hans 15 pt-BR
import sys, struct
p = sys.argv[1]; d = bytearray(open(p, 'rb').read())
src = bytes(d[0:0x300])
flags = 0
for i in range(16):
    if not any(d[i * 0x300:i * 0x300 + 0x200]): d[i * 0x300:(i + 1) * 0x300] = src
    flags |= 1 << i
ko = 12 * 0x300                                              # Korean slot: the Korean title
d[ko:ko + 0x300] = bytes(0x300)
d[ko:ko + 0x200] = (sys.argv[2] if len(sys.argv) > 2 else '노노무라 병원의 사람들 HD').encode('utf-8').ljust(0x200, b'\0')
d[ko + 0x200:ko + 0x300] = '디시인사이드 스위치CFW 갤러리 -루카-'.encode('utf-8').ljust(0x100, b'\0')
struct.pack_into('<I', d, 0x302C, flags)
open(p, 'wb').write(d); print('nacp languages: all 16 slots, flags %04x' % flags)
