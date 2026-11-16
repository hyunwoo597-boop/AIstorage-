#!/usr/bin/env python3
"""움직이는 입체 홈 메뉴 배너(ctr/banner.cgfx)를 만든다 (PC 에서 실행 — Pillow·numpy 필요, Python 3.12+).

    python3 tools/gen_banner_cgfx.py ctr/banner_atlas.png ctr/banner_layout.json ctr/banner.cgfx [미리보기.gif]

조각 모음과 배치표(tools/gen_banner_layers.py 가 만든 것)로 장면을 짠다:
  무대 전체(뿌리) ─ 조각마다 뼈 하나 + 사각형 하나.
움직임(8초 반복, 60프레임/초): 무대 전체가 좌우로 천천히 돌고, 로고는 위아래로 떠다니고,
인물은 발끝을 축으로 살짝 흔들리고, 스쿠터는 잘게 떤다. ANIM 을 고쳐 조절한다.
CGFX 쓰기는 tools/pycgfx (skyfloogle/pycgfx, MIT) 가 한다. CI 는 만들어진 banner.cgfx 를 bannertool 로 묶기만 한다.
"""
import io, json, math, os, struct, sys, types

HERE = os.path.dirname(os.path.abspath(__file__))


# ---- gltflib 대신 쓰는 최소한의 객체 ----
class N:
    def __init__(self, **kw): self.__dict__.update(kw)
    def __getattr__(self, k): return None          # 없는 항목은 None (gltflib 의 기본값과 같다)


shim = types.ModuleType('gltflib')
shim.Sampler = shim.Material = shim.PBRMetallicRoughness = shim.Accessor = N
shim.GLTF = N
sys.modules['gltflib'] = shim
sys.path.insert(0, os.path.join(HERE, 'pycgfx'))
import convert as pycgfx                            # noqa: E402

LOOP = 8.0                                          # 초
STEP = 0.1                                          # 키 간격(초) — 직선 보간
# 이름: 움직임.  yaw/roll 은 도(°), bob 은 단위 길이, period 는 초(LOOP 의 약수여야 이음매가 없다)
ANIM = {
    '_root':   dict(yaw=9.0, period=8.0),
    'logo':    dict(bob=0.22, period=4.0),
    'orange':  dict(roll=1.0, period=4.0, phase=0.00),
    'blue':    dict(roll=1.2, period=4.0, phase=0.35),
    'pink':    dict(roll=1.0, period=4.0, phase=0.70),
    'scooter': dict(bob=0.045, period=0.5),
}


def quat(axis, deg):
    h = math.radians(deg) / 2; s = math.sin(h)
    return [s if axis == 'x' else 0.0, s if axis == 'y' else 0.0, s if axis == 'z' else 0.0, math.cos(h)]


def build(atlas_png, layout):
    aw, ah = layout['atlas']
    buf = bytearray(); views = []; accs = []

    def acc(data, ctype, typ, count):
        while len(buf) % 4: buf.append(0)
        views.append(N(buffer=0, byteOffset=len(buf), byteLength=len(data))); buf.extend(data)
        accs.append(N(bufferView=len(views) - 1, componentType=ctype, type=typ, count=count))
        return len(accs) - 1

    meshes = []; nodes = []; channels = []; samplers = []
    times = [i * STEP for i in range(int(round(LOOP / STEP)) + 1)]
    t_acc = acc(struct.pack('<%df' % len(times), *times), 5126, 'SCALAR', len(times))

    def animate(node_id, path, values):
        n = 3 if path == 'translation' else 4
        flat = [c for v in values for c in v]
        samplers.append(N(input=t_acc, output=acc(struct.pack('<%df' % len(flat), *flat), 5126, 'VEC%d' % n, len(values)), interpolation='LINEAR'))
        channels.append(N(sampler=len(samplers) - 1, target=N(node=node_id, path=path)))

    kids = []
    for l in layout['layers']:
        x0, y0, x1, y1 = l['quad']; ax, ay, w, h = l['uv']
        zb, zt = l['z'] if isinstance(l['z'], list) else (l['z'], l['z'])
        cx = (x0 + x1) / 2; hw = (x1 - x0) / 2; hh = y1 - y0
        u0, u1 = (ax + 0.25) / aw, (ax + w - 0.25) / aw; vt, vb = (ay + 0.25) / ah, (ay + h - 0.25) / ah   # glTF: v 는 위에서 아래로
        # 뼈의 원점 = 조각의 발끝 가운데 (z 는 아래쪽 깊이). 바닥처럼 기울어진 판은 위쪽 정점만 z 가 다르다
        pos = [(-hw, 0, 0), (hw, 0, 0), (-hw, hh, zt - zb), (hw, hh, zt - zb)]; uv = [(u0, vb), (u1, vb), (u0, vt), (u1, vt)]
        p = acc(b''.join(struct.pack('<3f', *v) for v in pos), 5126, 'VEC3', 4)
        t = acc(b''.join(struct.pack('<2f', *v) for v in uv), 5126, 'VEC2', 4)
        i = acc(bytes([0, 1, 2, 1, 3, 2]), 5121, 'SCALAR', 6)
        meshes.append(N(name=l['name'], primitives=[N(attributes=N(POSITION=p, TEXCOORD_0=t), indices=i, material=0)]))
        base = [cx, y0, zb]
        nodes.append(N(name=l['name'], mesh=len(meshes) - 1, translation=base))
        nid = len(nodes) - 1; kids.append(nid)
        a = ANIM.get(l['name'])
        if a:
            ph = a.get('phase', 0.0)
            wave = [math.sin(2 * math.pi * (tm / a['period'] + ph)) for tm in times]
            if 'bob' in a: animate(nid, 'translation', [[base[0], base[1] + a['bob'] * s, base[2]] for s in wave])
            if 'roll' in a: animate(nid, 'rotation', [quat('z', a['roll'] * s) for s in wave])
    nodes.append(N(name='diorama', children=kids)); root = len(nodes) - 1
    a = ANIM.get('_root')
    if a: animate(root, 'rotation', [quat('y', a['yaw'] * math.sin(2 * math.pi * tm / a['period'])) for tm in times])

    model = N(
        scene=0, scenes=[N(nodes=[root])], nodes=nodes, meshes=meshes, accessors=accs, bufferViews=views,
        buffers=[N(uri='scene.bin')], images=[N(name='atlas', uri='atlas.png')],
        samplers=[N(magFilter=9729, minFilter=9729, wrapS=33071, wrapT=33071)], textures=[N(source=0, sampler=0)],
        materials=[N(name='mt_banner', alphaMode='BLEND', extensions={'KHR_materials_unlit': {}},
                     pbrMetallicRoughness=N(baseColorTexture=N(index=0)))],
        animations=[N(name='idle', channels=channels, samplers=samplers)],
    )
    res = {'scene.bin': N(data=bytes(buf)), 'atlas.png': N(data=atlas_png)}
    gltf = N(model=model); gltf.get_resource = lambda uri: res[uri]
    return pycgfx.write(pycgfx.convert_gltf(gltf))


def dithered(path):
    """RGBA4444 로 줄 때 띠가 지지 않게 색을 미리 흩뿌려 16단계로 맞춘다 (알파는 그대로 자름)."""
    import numpy as np
    from PIL import Image
    a = np.array(Image.open(path).convert('RGBA')).astype(np.float32)
    bayer = np.array([[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]], np.float32)
    t = (np.tile(bayer, (a.shape[0] // 4 + 1, a.shape[1] // 4 + 1))[:a.shape[0], :a.shape[1]] + 0.5) / 16
    a[..., :3] = np.minimum(15, np.floor(a[..., :3] * 15 / 255 + t[..., None])) * 17
    o = io.BytesIO(); Image.fromarray(a.astype(np.uint8), 'RGBA').save(o, 'PNG')
    return o.getvalue()


def preview(atlas_path, layout, out_gif, frames=48, size=(400, 240)):
    """홈 메뉴 카메라(시야각 30°, (0,1,44.786) 에서 -z 방향)로 그려 본 움직임. 실제 기기 화면이 아니다."""
    import numpy as np
    from PIL import Image
    at = np.array(Image.open(atlas_path).convert('RGBA')).astype(np.float32); aw, ah = layout['atlas']
    W, H = size; f = (H / 2) / math.tan(math.radians(15)); imgs = []
    for k in range(frames):
        tm = LOOP * k / frames
        yaw = math.radians(ANIM['_root']['yaw'] * math.sin(2 * math.pi * tm / ANIM['_root']['period'])) if '_root' in ANIM else 0
        cy, sy = math.cos(yaw), math.sin(yaw)
        can = np.zeros((H, W, 3), np.float32); can[:] = (150, 110, 200)
        items = []
        for l in layout['layers']:
            x0, y0, x1, y1 = l['quad']; zb, zt = l['z'] if isinstance(l['z'], list) else (l['z'], l['z'])
            a = ANIM.get(l['name'], {}); s = math.sin(2 * math.pi * (tm / a.get('period', 1) + a.get('phase', 0)))
            dy = a.get('bob', 0) * s; roll = math.radians(a.get('roll', 0) * s); cx = (x0 + x1) / 2
            pts = []
            for (x, y, z) in ((x0, y0, zb), (x1, y0, zb), (x0, y1, zt), (x1, y1, zt)):
                lx, ly = x - cx, y - y0
                lx, ly = lx * math.cos(roll) - ly * math.sin(roll), lx * math.sin(roll) + ly * math.cos(roll)
                X, Y, Z = cx + lx, y0 + ly + dy, z
                X, Z = X * cy + Z * sy, -X * sy + Z * cy
                d = 44.786 - Z
                pts.append((W / 2 + X * f / d, H / 2 - (Y - 1) * f / d, d))
            items.append((sum(p[2] for p in pts) / 4, l, pts))
        for _, l, pts in sorted(items, key=lambda it: -it[0]):
            ax, ay, w, h = l['uv']
            src = np.float32([[0, h], [w, h], [0, 0], [w, 0]]); dst = np.float32([[p[0], p[1]] for p in pts])
            import cv2
            M = cv2.getPerspectiveTransform(src, dst)
            warped = cv2.warpPerspective(at[ay:ay+h, ax:ax+w], M, (W, H), flags=cv2.INTER_LINEAR)
            al = warped[..., 3:4] / 255; can = can * (1 - al) + warped[..., :3] * al
        imgs.append(Image.fromarray(can.astype(np.uint8)))
    imgs[0].save(out_gif, save_all=True, append_images=imgs[1:], duration=int(1000 * LOOP / frames), loop=0)


def main():
    layout = json.load(open(sys.argv[2]))
    data = build(dithered(sys.argv[1]), layout)
    open(sys.argv[3], 'wb').write(data)
    print('%s: %d bytes' % (sys.argv[3], len(data)))
    if len(sys.argv) > 4: preview(sys.argv[1], layout, sys.argv[4])


if __name__ == '__main__':
    main()
