#!/usr/bin/env python3
"""Assembles the one-file HTML Korean text editor.
    python3 tools/text_editor/build.py <mes.arc (Korean patch)> <out.html> [info]
Embeds editcore.js, the Korean mes.arc, assets/kr/font16.bin, assets/kr/nonokr_map.json and all 11172 pre-rendered
Hangul glyphs (mkhangul.py, Noto Sans CJK KR - same drawing as the game font)."""
import base64, os, sys, subprocess, tempfile
here = os.path.dirname(os.path.abspath(__file__)); top = os.path.join(here, '..', '..')
def b64(b): s = base64.b64encode(b).decode(); return '\n'.join(s[i:i + 120] for i in range(0, len(s), 120))
hb = os.path.join(tempfile.gettempdir(), 'hangul16.bin')
if not os.path.exists(hb): subprocess.run([sys.executable, '-I', os.path.join(here, 'mkhangul.py'), '/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc', hb], check=True)
core = open(os.path.join(here, 'editcore.js'), encoding='utf-8').read(); assert '</script' not in core
html = open(os.path.join(here, 'template.html'), encoding='utf-8').read()
html = (html.replace('/*__CORE__*/', core).replace('__INFO__', sys.argv[3] if len(sys.argv) > 3 else '')
        .replace('__MES64__', b64(open(sys.argv[1], 'rb').read())).replace('__FONT64__', b64(open(os.path.join(top, 'assets', 'kr', 'font16.bin'), 'rb').read()))
        .replace('__MAPJSON__', open(os.path.join(top, 'assets', 'kr', 'nonokr_map.json')).read()).replace('__HANGUL64__', b64(open(hb, 'rb').read())))
open(sys.argv[2], 'w', encoding='utf-8').write(html); print('%s: %d bytes' % (sys.argv[2], len(html.encode())))
