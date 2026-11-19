#!/usr/bin/env python3
"""Assembles the one-file HTML CIA maker.

    python3 tools/cia_maker_xp/build.py <nonoxp.cia without game files> <out.html> [version text] [--kr-mes mes.arc --kr-bg dir]

--kr-mes / --kr-bg build the edition with the Korean patch inside (the translated script and the 14 changed UI
pictures of Bg.Arc, made by the patch author); without them the user picks the patch files in the page.

Embeds core.js, the base CIA, the Korean font (assets/kr/font16.bin) and the Korean UI pictures (assets/kr/patch/*).
No game files go in; the user supplies their own disc and Korean patch in the browser.
"""
import base64, json, os, sys
here = os.path.dirname(os.path.abspath(__file__)); top = os.path.join(here, '..', '..')
b64 = lambda b: '\n'.join(base64.b64encode(b).decode()[i:i + 120] for i in range(0, len(base64.b64encode(b)), 120))
args = sys.argv[1:]; opt = {}
for k in ('--kr-mes', '--kr-bg'):
    if k in args: i = args.index(k); opt[k] = args[i + 1]; del args[i:i + 2]
sys.argv = [sys.argv[0]] + args
cia = open(sys.argv[1], 'rb').read()
info = sys.argv[3] if len(sys.argv) > 3 else ''
core = open(os.path.join(here, 'core.js'), encoding='utf-8').read(); assert '</script' not in core
pdir = os.path.join(top, 'assets', 'kr', 'patch')
patch = {n: base64.b64encode(open(os.path.join(pdir, n), 'rb').read()).decode() for n in sorted(os.listdir(pdir))}
html = open(os.path.join(here, 'template.html'), encoding='utf-8').read()
html = (html.replace('/*__CORE__*/', core).replace('__BASEINFO__', info).replace('__BASE64__', b64(cia))
            .replace('__FONT64__', b64(open(os.path.join(top, 'assets', 'kr', 'font16.bin'), 'rb').read()))
            .replace('__PATCHJSON__', json.dumps(patch))
            .replace('__FONTJP64__', b64(open(os.path.join(top, 'assets', 'font16.bin'), 'rb').read()))
            .replace('__KRMES64__', b64(open(opt['--kr-mes'], 'rb').read()) if '--kr-mes' in opt else '')
            .replace('__KRBGJSON__', json.dumps({n: base64.b64encode(open(os.path.join(opt['--kr-bg'], n), 'rb').read()).decode() for n in sorted(os.listdir(opt['--kr-bg']))}) if '--kr-bg' in opt else '{}'))
open(sys.argv[2], 'w', encoding='utf-8').write(html)
print('%s: %d bytes' % (sys.argv[2], len(html.encode())))
