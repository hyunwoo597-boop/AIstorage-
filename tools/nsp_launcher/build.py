#!/usr/bin/env python3
"""NSP launcher maker (one HTML): python3 tools/nsp_launcher/build.py <fwd_main> <fwd.npdm> <nacp> <icon.jpg> <out.html> [version]
The NSP holds only the loader (nx-hbloader starting sdmc:/switch/NonomuraHospitalHD/NonomuraHospitalHD.nro), the NACP and the icon. No game files, no keys."""
import base64, os, sys
here = os.path.dirname(os.path.abspath(__file__)); top = os.path.join(here, '..', '..')
nso, npdm, nacp, icon, out = sys.argv[1:6]; info = sys.argv[6] if len(sys.argv) > 6 else ''
b64 = lambda p: base64.b64encode(open(p, 'rb').read()).decode()
core = open(os.path.join(top, 'tools', 'cia_maker_xp', 'core.js'), encoding='utf-8').read(); nspc = open(os.path.join(top, 'tools', 'nsp_maker', 'nspcore.js'), encoding='utf-8').read()
h = open(os.path.join(here, 'template.html'), encoding='utf-8').read()
h = (h.replace('/*__CORE__*/', core).replace('/*__NSPCORE__*/', nspc).replace('__BASEINFO__', info)
     .replace('__NSO64__', b64(nso)).replace('__NPDM64__', b64(npdm)).replace('__NACP64__', b64(nacp)).replace('__ICON64__', b64(icon)))
open(out, 'w', encoding='utf-8').write(h); print('%s: %d bytes' % (out, len(h.encode())))
