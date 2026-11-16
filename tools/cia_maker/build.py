#!/usr/bin/env python3
"""게임 포함 CIA 만들기 도구(HTML 한 파일)를 조립한다.

    python3 tools/cia_maker/build.py <게임 파일 없는 nonomura.cia> <출력.html> [버전 표시]

template.html 에 ciacore.js 와 CIA(base64)를 끼워 넣는다. 게임 파일은 들어가지 않는다.
"""
import base64, os, sys

here = os.path.dirname(os.path.abspath(__file__))
cia = open(sys.argv[1], 'rb').read()
info = sys.argv[3] if len(sys.argv) > 3 else '%d KB' % (len(cia) // 1024)
html = open(os.path.join(here, 'template.html'), encoding='utf-8').read()
core = open(os.path.join(here, 'ciacore.js'), encoding='utf-8').read()
assert '</script' not in core
b64 = base64.b64encode(cia).decode()
b64 = '\n'.join(b64[i:i + 120] for i in range(0, len(b64), 120))
html = html.replace('/*__CORE__*/', core).replace('__BASEINFO__', info).replace('__BASE64__', b64)
open(sys.argv[2], 'w', encoding='utf-8').write(html)
print('%s: %d bytes' % (sys.argv[2], len(html.encode())))
