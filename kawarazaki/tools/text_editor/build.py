#!/usr/bin/env python3
"""One-file HTML dialogue editor for 河原崎家の一族 (Korean patch work).  python3 build.py <out.html>
No game data goes in: the user opens his own mes.ARC or disc image in the page."""
import os, sys
here = os.path.dirname(os.path.abspath(__file__)); top = os.path.join(here, '..', '..', '..')
core = open(os.path.join(top, 'tools', 'cia_maker_xp', 'core.js'), encoding='utf-8').read()
ked = open(os.path.join(here, 'kedit.js'), encoding='utf-8').read()
assert '</script' not in core and '</script' not in ked
html = open(os.path.join(here, 'template.html'), encoding='utf-8').read().replace('/*__CORE__*/', core).replace('/*__KEDIT__*/', ked)
open(sys.argv[1], 'w', encoding='utf-8').write(html); print(sys.argv[1], len(html.encode()))
