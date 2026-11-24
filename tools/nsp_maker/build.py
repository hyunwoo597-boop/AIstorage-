#!/usr/bin/env python3
"""Assembles the one-file HTML NSP maker.
    python3 tools/nsp_maker/build.py <main(NSO)> <main.npdm> <control.nacp> <icon.jpg> <out.html> [version] [--kr-mes mes.arc --kr-bg dir --kr-font font16.bin --dos dir-with-SILK0-4.DAT]
Embeds core.js + nspcore.js, the program (NSO + NPDM), NACP and icon, the Korean/Japanese fonts and Korean UI pictures.
No game files and no keys go in."""
import base64, json, os, sys
here = os.path.dirname(os.path.abspath(__file__)); top = os.path.join(here, '..', '..')
args = sys.argv[1:]; opt = {}
for k in ('--kr-mes', '--kr-bg', '--kr-font', '--dos', '--nro'):
    if k in args: i = args.index(k); opt[k] = args[i + 1]; del args[i:i + 2]
nso, npdm, nacp, icon, out = args[:5]; info = args[5] if len(args) > 5 else ''
def b64(b): s = base64.b64encode(b).decode(); return '\n'.join(s[i:i + 120] for i in range(0, len(s), 120))
rb = lambda p: open(p, 'rb').read()
core = open(os.path.join(top, 'tools', 'cia_maker_xp', 'core.js'), encoding='utf-8').read(); nspc = open(os.path.join(here, 'nspcore.js'), encoding='utf-8').read()
assert '</script' not in core and '</script' not in nspc
pdir = os.path.join(top, 'assets', 'kr', 'patch'); patch = {n: base64.b64encode(rb(os.path.join(pdir, n))).decode() for n in sorted(os.listdir(pdir))}
html = open(os.path.join(here, 'template.html'), encoding='utf-8').read()
html = (html.replace('/*__CORE__*/', core).replace('/*__NSPCORE__*/', nspc).replace('__BASEINFO__', (' · ' + info) if info else '')
        .replace('__NSO64__', b64(rb(nso))).replace('__NPDM64__', b64(rb(npdm))).replace('__NACP64__', b64(rb(nacp))).replace('__ICON64__', b64(rb(icon)))
        .replace('__FONT64__', b64(rb(opt.get('--kr-font') or os.path.join(top, 'assets', 'kr', 'font16.bin')))).replace('__FONTJP64__', b64(rb(os.path.join(top, 'assets', 'font16.bin'))))
        .replace('__PATCHJSON__', json.dumps(patch)).replace('__NRO64__', b64(rb(opt['--nro'])) if '--nro' in opt else '')
        .replace('__KRMES64__', b64(rb(opt['--kr-mes'])) if '--kr-mes' in opt else '')
        .replace('__KRBGJSON__', json.dumps({n: base64.b64encode(rb(os.path.join(opt['--kr-bg'], n))).decode() for n in sorted(os.listdir(opt['--kr-bg']))}) if '--kr-bg' in opt else '{}')
        .replace('__DOSJSON__', json.dumps({n: base64.b64encode(rb(os.path.join(opt['--dos'], n))).decode() for n in ('SILK0.DAT', 'SILK1.DAT', 'SILK2.DAT', 'SILK3.DAT', 'SILK4.DAT')}) if '--dos' in opt else '{}'))
open(out, 'w', encoding='utf-8').write(html); print('%s: %d bytes' % (out, len(html.encode())))
