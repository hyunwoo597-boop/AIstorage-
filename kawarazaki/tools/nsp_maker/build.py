#!/usr/bin/env python3
"""One-file HTML NSP maker for 河原崎家の一族.
    python3 build.py <main(NSO)> <main.npdm> <control.nacp> <icon.jpg> <kfont16.bin> <out.html> [info]
No game files and no keys go in; the user picks the disc and prod.keys in the page."""
import base64, os, sys
here = os.path.dirname(os.path.abspath(__file__)); top = os.path.join(here, '..', '..', '..')
def b64(b): s = base64.b64encode(b).decode(); return '\n'.join(s[i:i + 120] for i in range(0, len(s), 120))
rb = lambda p: open(p, 'rb').read()
nso, npdm, nacp, icon, font, out = sys.argv[1:7]; info = sys.argv[7] if len(sys.argv) > 7 else ''
core = open(os.path.join(top, 'tools', 'cia_maker_xp', 'core.js'), encoding='utf-8').read(); nspc = open(os.path.join(top, 'tools', 'nsp_maker', 'nspcore.js'), encoding='utf-8').read()
assert '</script' not in core and '</script' not in nspc
html = open(os.path.join(here, 'template.html'), encoding='utf-8').read()
html = (html.replace('/*__CORE__*/', core).replace('/*__NSPCORE__*/', nspc).replace('__BASEINFO__', (' · ' + info) if info else '')
        .replace('__NSO64__', b64(rb(nso))).replace('__NPDM64__', b64(rb(npdm))).replace('__NACP64__', b64(rb(nacp))).replace('__ICON64__', b64(rb(icon)))
        .replace('__FONT64__', b64(rb(font))))
open(out, 'w', encoding='utf-8').write(html); print('%s: %d bytes' % (out, len(html.encode())))
