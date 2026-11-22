# Edits (.kro, see kro_apply in src/sys.c) for the gallery/ending pictures the Korean patch left in Japanese. Only text and
# shapes drawn here are stored; pieces of pictures are copied at run time from the Korean patch's own pictures.
#   scene_pt.gcc / ed_pt.gcc  "タイトルへ戻る" button -> the patch's "처음 화면으로" button (cg_pt.gcc)
#   ed_pt.gcc                 "NO.9 涼子/美保/梨恵" labels -> "NO.9 료코/미호/리에"
#   cg_bg_last.gcc            last CG page: the button bar of the patch's cg_bg.gcc ("다음" disabled), Korean empty slots
#   cg_pt.gcc                 the empty-slot thumbnail -> Korean
#   staff.gcc                 cast list -> Korean (mask plane; the colour is plain white)
# usage: mkuipatch_kr.py <outdir> [Korean Bg.Arc + gcc2raw, only to check the result: previewdir]
import sys, struct, os
import numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageFilter
outd = sys.argv[1]; os.makedirs(outd, exist_ok=True)
SERIF = '/usr/share/fonts/opentype/noto/NotoSerifCJK-Bold.ttc'; SERIFB = '/usr/share/fonts/opentype/noto/NotoSerifCJK-Black.ttc'
def u16(*v): return struct.pack('<' + 'H' * len(v), *v)
def COPY(src, sx, sy, w, h, dx, dy, fl): return b'\1' + src.encode().ljust(16, b'\0') + u16(sx, sy, w, h, dx, dy) + bytes([fl, 0])
def ERASE(x, y, w, h): return b'\2' + u16(x, y, w, h)
def RGBA(x, y, im): im = im.convert('RGBA'); return b'\3' + u16(x, y, im.width, im.height) + im.tobytes()
def MASK(a): return b'\4' + u16(a.shape[1], a.shape[0]) + a.astype(np.uint8).tobytes()
def save(name, *ops): open(os.path.join(outd, name + '.kro'), 'wb').write(b'KRO1' + b''.join(ops) + b'\0')

# empty-slot thumbnail 112x84 (drawn here: night-blue panel with the title)
th = Image.new('RGBA', (112, 84)); d = ImageDraw.Draw(th)
for y in range(84): c = int(18 + 30 * y / 83); d.line([(0, y), (111, y)], fill=(c // 3, c // 2, c + 20, 255))
big = Image.new('L', (1400, 300), 0); ImageDraw.Draw(big).text((40, 40), '노노무라 병원의 사람들', font=ImageFont.truetype(SERIFB, 64, index=1), fill=255)
gl = big.crop(big.getbbox()).resize((104, 16), Image.LANCZOS); G = Image.new('L', (112, 84), 0); G.paste(gl, (4, 34))
rim = G.filter(ImageFilter.MaxFilter(3))
th.paste(Image.new('RGBA', (112, 84), (255, 255, 255, 255)), mask=rim); th.paste(Image.new('RGBA', (112, 84), (239, 0, 95, 255)), mask=G)
save('cg_pt.gcc', RGBA(0, 180, th))
THUMBS = [(66, 28), (198, 28), (330, 28), (462, 28), (66, 128), (198, 128), (330, 128), (462, 128)]   # empty slots on cg_bg_last
save('cg_bg_last.gcc', COPY('cg_bg.gcc', 0, 420, 146, 60, 0, 420, 0), COPY('cg_bg.gcc', 206, 420, 434, 60, 206, 420, 0),
     COPY('cg_pt.gcc', 80, 72, 80, 24, 208, 432, 2), *[RGBA(x, y, th) for x, y in THUMBS])
BTN = COPY('cg_pt.gcc', 160, 0, 128, 72, 0, 0, 1)
def btn_at(x, y): return BTN[:17] + u16(160, 0, 128, 72, x, y) + bytes([1, 0])
save('scene_pt.gcc', btn_at(320, 360))
# ed_pt labels: text only (highlight + brown), the old text erased at run time
font = ImageFont.truetype(SERIF, 19, index=1); labs = []
for i, nm in enumerate(('료코', '미호', '리에')):
    tx = Image.new('L', (108, 24), 0); dd = ImageDraw.Draw(tx); s = 'NO.9 ' + nm
    dd.text(((108 - dd.textlength(s, font=font)) / 2, 12), s, font=font, fill=255, anchor='lm')
    hi = tx.transform(tx.size, Image.AFFINE, (1, 0, 0, 0, 1, -1))
    L = Image.new('RGBA', (108, 24)); L.paste(Image.new('RGBA', (108, 24), (255, 240, 215, 255)), mask=hi.point(lambda v: v // 2))
    L.paste(Image.new('RGBA', (108, 24), (58, 34, 20, 255)), mask=tx); labs.append(RGBA(108 * (i + 1), 48, L))
save('ed_pt.gcc', btn_at(224, 324), ERASE(108, 48, 324, 24), *labs)
# staff mask: 3 columns x 6 rows of 192x72; the last cell stays as it was (solid)
cast = [('', '출  연'), ('간호사', '마미야 치사토'), ('같은 병실의 청년', '우루시바라 조지'),
        ('천재 탐정', '카이바라 타쿠마로'), ('간호사', '노기와 미호'), ('빌딩 주인', '미코시바 타로'),
        ('원장 부인', '노노무라 아키코'), ('휠체어 소녀', '이츠키 모모코'), ('집사', '후지키 에이사쿠'),
        ('여탐정', '이토 료코'), ('모모코의 친구', '시마키 카나'), ('원장', '노노무라 사쿠지'),
        ('간호사', '마키노 리에'), ('잡지 기자', '가와사키 벤조'), ('제작·저작', '(주)엘프'),
        ('간호사', '마키노 치에'), ('탐정', '사이조 타카후미'), None]
T = Image.new('L', (576, 432), 0); dd = ImageDraw.Draw(T)
fr = ImageFont.truetype(SERIF, 15, index=1); fn = ImageFont.truetype(SERIF, 24, index=1); fb = ImageFont.truetype(SERIF, 28, index=1)
for i, c in enumerate(cast):
    cx, y0 = (i % 3) * 192 + 96, (i // 3) * 72
    if c is None: continue
    role, name = c
    if not role: dd.text((cx, y0 + 38), name, font=fb, fill=255, anchor='mm'); continue
    dd.text((cx, y0 + 18), role, font=fr, fill=255, anchor='mm'); f = fn
    while dd.textlength(name, font=f) > 184: f = ImageFont.truetype(SERIF, f.size - 1, index=1)
    dd.text((cx, y0 + 46), name, font=f, fill=255, anchor='mm')
glow = T.filter(ImageFilter.MaxFilter(3)).filter(ImageFilter.GaussianBlur(2.2))
A = np.maximum(np.asarray(T), (np.asarray(glow).astype(np.float32) * 0.8).astype(np.uint8)); A[360:432, 384:576] = 255
save('staff.gcc', MASK(A))
print('ok', sorted(os.listdir(outd)))
