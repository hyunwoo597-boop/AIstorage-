#!/usr/bin/env python3
"""Assembles the one-file HTML CIA maker.

    python3 tools/cia_maker_xp/build.py <nonoxp.cia without game files> <out.html> [version text]

Embeds core.js, the base CIA, the Korean font (assets/kr/font16.bin) and the Korean UI pictures (assets/kr/patch/*).
No game files go in; the user supplies their own disc and Korean patch in the browser.
"""
import base64, json, os, sys
here = os.path.dirname(os.path.abspath(__file__)); top = os.path.join(here, '..', '..')
b64 = lambda b: '\n'.join(base64.b64encode(b).decode()[i:i + 120] for i in range(0, len(base64.b64encode(b)), 120))
cia = open(sys.argv[1], 'rb').read()
info = sys.argv[3] if len(sys.argv) > 3 else ''
core = open(os.path.join(here, 'core.js'), encoding='utf-8').read(); assert '</script' not in core
pdir = os.path.join(top, 'assets', 'kr', 'patch')
patch = {n: base64.b64encode(open(os.path.join(pdir, n), 'rb').read()).decode() for n in sorted(os.listdir(pdir))}
html = open(os.path.join(here, 'template.html'), encoding='utf-8').read()
html = (html.replace('/*__CORE__*/', core).replace('__BASEINFO__', info).replace('__BASE64__', b64(cia))
            .replace('__FONT64__', b64(open(os.path.join(top, 'assets', 'kr', 'font16.bin'), 'rb').read()))
            .replace('__PATCHJSON__', json.dumps(patch)))
open(sys.argv[2], 'w', encoding='utf-8').write(html)
print('%s: %d bytes' % (sys.argv[2], len(html.encode())))
