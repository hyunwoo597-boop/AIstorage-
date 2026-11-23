#!/usr/bin/env python3
"""움직이는 입체 홈 메뉴 배너(ctr/banner.cgfx)를 만든다 (PC 에서 실행 — Pillow·numpy 필요, Python 3.12+).

    python3 tools/gen_banner_cgfx.py ctr/banner_atlas.png ctr/banner_layout.json ctr/banner.cgfx [미리보기.gif]

조각 모음과 배치표(tools/gen_banner_layers.py 가 만든 것)로 장면을 짠다:
  diorama(Y축 빌보드) ─ turntable(좌우 흔들림) ─ 조각마다 뼈 하나(Y축 빌보드) + 사각형 하나.
홈 메뉴는 배너 모델을 스스로 세로축으로 돌리므로, 빌보드로 그 회전을 상쇄해 조각이 옆·뒤를 향해 사라지지 않게 한다.
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
ANIM = {'scooter': dict(bob=0.0, period=4.0)}   # 사용자 요청: 배너는 완전히 고정 (움직임 0 — 변환기가 애니메이션 하나는 필요로 함)


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

    # 모든 조각을 메시 하나(뼈 하나)에 담는다. 홈 메뉴는 배너 모델을 세로축으로 돌리는데, 그 뼈 하나에만 Y축 빌보드를
    # 걸면 무대 전체가 한 덩어리로 늘 정면을 향한다 — 조각마다 빌보드를 걸면 각자 제자리에서 돌고 위치는 빙 돌아
    # 조각끼리 따로 도는 것처럼 보였다. 조각 사이의 깊이(z)는 그대로라 입체는 남는다. 그리는 순서 = 배치표 순서(뒤 → 앞).
    pos = []; uvs = []; idx = []
    for l in layout['layers']:
        x0, y0, x1, y1 = l['quad']; ax, ay, w, h = l['uv']
        zb, zt = l['z'] if isinstance(l['z'], list) else (l['z'], l['z'])
        u0, u1 = (ax + 0.25) / aw, (ax + w - 0.25) / aw; vt, vb = (ay + 0.25) / ah, (ay + h - 0.25) / ah   # glTF: v 는 위에서 아래로
        n0 = len(pos)
        pos += [(x0, y0 + 7.0, zb), (x1, y0 + 7.0, zb), (x0, y1 + 7.0, zt), (x1, y1 + 7.0, zt)]
        if l.get('rot'): uvs += [(u0, vt), (u0, vb), (u1, vt), (u1, vb)]   # 조각 모음에 시계 방향 90° 돌려 넣은 조각: 아래왼→위왼, 아래오→아래왼, 위왼→위오, 위오→아래오
        else: uvs += [(u0, vb), (u1, vb), (u0, vt), (u1, vt)]
        idx += [n0, n0 + 1, n0 + 2, n0 + 1, n0 + 3, n0 + 2]
    p = acc(b''.join(struct.pack('<3f', *v) for v in pos), 5126, 'VEC3', len(pos))
    t = acc(b''.join(struct.pack('<2f', *v) for v in uvs), 5126, 'VEC2', len(uvs))
    i = acc(bytes(idx), 5121, 'SCALAR', len(idx))
    meshes.append(N(name='scene', primitives=[N(attributes=N(POSITION=p, TEXCOORD_0=t), indices=i, material=0)]))
    # 뼈대는 예전에 실기에서 잘 뜨던 모양 그대로: diorama ─ turntable ─ 메시를 단 뼈. 메시와 빌보드를 맨 위 뼈에 바로 달았더니
    # 홈 메뉴가 멈췄다 (배너가 안 나옴)
    nodes.append(N(name='scene', mesh=0, translation=[0.0, -7.0, 0.0])); sc = len(nodes) - 1
    nodes.append(N(name='turntable', children=[sc])); turn = len(nodes) - 1
    nodes.append(N(name='diorama', children=[turn])); root = len(nodes) - 1
    animate(sc, 'translation', [[0.0, -7.0, 0.0] for _ in times])   # 움직임 없음 (변환기가 애니메이션 하나는 필요로 함)

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
    cgfx = pycgfx.convert_gltf(gltf)
    patch(cgfx, [])
    return pycgfx.write(cgfx)


def patch(cgfx, pieces):
    """glTF 로는 적을 수 없는 CGFX 설정.
    홈 메뉴는 배너 모델을 스스로 세로축으로 돌린다. 조각은 앞면만 있는 납작한 판이라 옆·뒤를 향하면 사라지므로
    - 무대 전체(diorama)와 로고를 뺀 조각마다 Y축 빌보드(5): 늘 보는 사람 쪽을 향해 옆·뒤로 돌아 사라지지 않는다
    - 재질은 양면 그리기(면 제거 끔): 혹시 돌아가도 사라지지 않게
    """
    from cgfx.sobj import BillboardMode
    from cgfx.mtob import CullMode
    cmdl = cgfx.data.models['COMMON']
    names = {'scene'}   # 뼈 하나(무대 전체)에만 Y축 빌보드: 홈 메뉴의 회전을 통째로 상쇄해 배너가 멈춰 있다
    hit = set()
    for b in cmdl.skeleton.bones:
        if b in names:
            cmdl.skeleton.bones[b].billboard_mode = BillboardMode.YAxial; hit.add(b)
    assert hit == names, ('빌보드를 걸 뼈가 없음', names - hit)
    for m in cmdl.materials:
        r = cmdl.materials[m].rasterization
        r.cull_mode = CullMode.Never
        r.command.param = int(CullMode.Never)


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
            ax, ay, w, h = l['uv']; piece = at[ay:ay+h, ax:ax+w]
            if l.get('rot'): piece = np.ascontiguousarray(np.rot90(piece, 1)); w, h = h, w
            src = np.float32([[0, h], [w, h], [0, 0], [w, 0]]); dst = np.float32([[p[0], p[1]] for p in pts])
            import cv2
            M = cv2.getPerspectiveTransform(src, dst)
            warped = cv2.warpPerspective(piece, M, (W, H), flags=cv2.INTER_LINEAR)
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
