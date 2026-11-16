#!/usr/bin/env python3
"""홈 메뉴 배너용 조각 만들기 (PC 에서 한 번 실행 — opencv·numpy·Pillow 필요).

    python3 tools/gen_banner_layers.py ctr/banner_src.png ctr

원화(1440x720)에서 로고·인물 넷·달·병원을 오려 내어 512x256 조각 모음(ctr/banner_atlas.png)과
배치표(ctr/banner_layout.json: 조각별 위치·깊이)를 만든다. 인물은 손으로 잡은 대략의 윤곽 안에서
GrabCut 으로, 로고는 색으로, 병원은 지붕선 아래 전부로 잡고 인물에 가려진 곳은 주변으로 메운다.
tools/banner_diorama.py 가 이 둘로 배너 모델을 만든다. 깊이는 Z, 조각 크기는 SC 를 고친다.
"""
import cv2, numpy as np, json, sys
SRC=sys.argv[1] if len(sys.argv)>1 else 'src.png'; OUT=sys.argv[2] if len(sys.argv)>2 else '.'
im=cv2.imread(SRC); H,W=im.shape[:2]
hsv=cv2.cvtColor(im,cv2.COLOR_BGR2HSV).astype(int); Hh,S,V=hsv[...,0],hsv[...,1],hsv[...,2]
def poly(pts):
    m=np.zeros((H,W),np.uint8); cv2.fillPoly(m,[np.array(pts,np.int32)],255); return m
# 하늘색(어두운 남색·청회색 구름): 색상 100~125(파랑), 채도·명도 낮음
sky=((Hh>=98)&(Hh<=128)&(V<120))|(V<40)
P={
 'orange':[(250,60),(330,5),(400,5),(480,60),(475,140),(505,250),(450,330),(430,410),(330,410),(280,320),(262,300),(262,235),(245,180),(255,120)],
 'blue':[(85,190),(140,150),(240,160),(262,235),(262,300),(300,290),(350,330),(365,420),(330,470),(405,620),(405,720),(25,720),(100,560),(130,470),(100,430),(0,450),(0,350),(120,300),(92,250)],
 'scooter':[(858,265),(900,225),(980,190),(1085,198),(1135,230),(1130,330),(1085,395),(1100,440),(1140,470),(1145,600),(1090,695),(760,705),(752,620),(800,520),(835,440),(810,395),(845,335),(900,330),(870,300)],
 'pink':[(1000,185),(1040,150),(1090,90),(1180,35),(1340,15),(1385,100),(1440,200),(1440,720),(1060,720),(1090,695),(1145,600),(1140,470),(1100,440),(1085,395),(1130,330),(1135,230),(1085,198),(1040,200)],
}
out={}
for name,pts in P.items():
    pm=poly(pts)
    gc=np.full((H,W),cv2.GC_BGD,np.uint8); gc[pm>0]=cv2.GC_PR_FGD
    inner=cv2.erode(pm,np.ones((31,31),np.uint8))
    bright=(V>150)&(~sky)
    gc[(inner>0)&bright]=cv2.GC_FGD
    gc[(pm>0)&sky&(V<70)]=cv2.GC_PR_BGD
    if name=='scooter':
        e=np.zeros((H,W),np.uint8); cv2.ellipse(e,(808,655),(46,53),0,0,360,255,-1)
        cv2.fillPoly(e,[np.array([(940,575),(1045,560),(1040,640),(1000,662),(975,655),(960,640),(935,640)],np.int32)],255)
        gc[e>0]=cv2.GC_FGD
        b=poly([(868,668),(985,668),(1000,720),(868,720)]); gc[b>0]=cv2.GC_BGD
    if name=='blue':
        b=poly([(40,300),(125,300),(125,347),(40,347)]); gc[b>0]=cv2.GC_BGD
    x,y,w,h=cv2.boundingRect(np.array(pts,np.int32)); pad=40
    x0,y0,x1,y1=max(0,x-pad),max(0,y-pad),min(W,x+w+pad),min(H,y+h+pad)
    sub=im[y0:y1,x0:x1].copy(); g=gc[y0:y1,x0:x1].copy()
    bg=np.zeros((1,65),np.float64); fg=np.zeros((1,65),np.float64)
    cv2.grabCut(sub,g,None,bg,fg,6,cv2.GC_INIT_WITH_MASK)
    m=np.zeros((H,W),np.uint8); m[y0:y1,x0:x1]=np.where((g==cv2.GC_FGD)|(g==cv2.GC_PR_FGD),255,0)
    # 작은 조각 제거, 구멍 메우기
    n,lab,st,_=cv2.connectedComponentsWithStats(m)
    keep=np.zeros_like(m)
    for i in range(1,n):
        if st[i,cv2.CC_STAT_AREA]>=1500: keep[lab==i]=255
    inv=255-keep; n,lab,st,_=cv2.connectedComponentsWithStats(inv)
    for i in range(1,n):
        if st[i,cv2.CC_STAT_AREA]<400: keep[lab==i]=255
    out[name]=keep
# 로고: 영역 안의 밝거나 분홍·빨강인 화소 + 테두리
lp=poly([(405,120),(965,120),(965,225),(880,310),(775,405),(478,405),(400,345),(445,250)])
lg=((V>135)&(S<90))|(((Hh>=140)|(Hh<=10))&(S>90)&(V>110))
lg=(lg&(lp>0)).astype(np.uint8)*255
lg=cv2.morphologyEx(lg,cv2.MORPH_CLOSE,np.ones((7,7),np.uint8))
n,lab,st,_=cv2.connectedComponentsWithStats(lg)
for i in range(1,n):
    if st[i,cv2.CC_STAT_AREA]<60: lg[lab==i]=0
lg=cv2.dilate(lg,cv2.getStructuringElement(cv2.MORPH_ELLIPSE,(9,9)))
chars=np.zeros((H,W),np.uint8)
for n_ in ['orange','blue','pink','scooter']: chars|=out[n_]
lg[cv2.dilate(chars,np.ones((15,15),np.uint8))>0]=0
n,lab,st,_=cv2.connectedComponentsWithStats(lg)
for i in range(1,n):
    if st[i,cv2.CC_STAT_AREA]<900: lg[lab==i]=0
out['logo']=lg
# 달
mn=np.zeros((H,W),np.uint8); cv2.circle(mn,(996,52),40,255,-1); out['moon']=mn
# 병원+바닥: 지붕선 아래 전부
bd=poly([(0,228),(128,228),(165,300),(165,405),(335,405),(560,420),(560,452),(838,452),(838,470),(1380,470),(1380,372),(1440,372),(1440,720),(0,720)])
out['building']=bd
# ================= 조각 → 아틀라스(512x256 RGBA) + 배치표 =================
from PIL import Image
AW,AH=512,256
ORDER=['moon','building','orange','blue','pink','scooter','logo']          # 뒤 → 앞
Z={'moon':-3.0,'building':-1.6,'orange':-0.4,'blue':0.8,'pink':1.2,'scooter':1.9,'logo':2.7}
SC={'moon':0.30,'building':0.17,'orange':0.30,'blue':0.30,'pink':0.30,'scooter':0.30,'logo':0.33}
rgb=cv2.cvtColor(im,cv2.COLOR_BGR2RGB)
# 병원: 인물에 가려진 곳을 주변으로 메움
front=np.zeros((H,W),np.uint8)
for n_ in ['orange','blue','pink','scooter']: front|=out[n_]
front=cv2.dilate(front,np.ones((9,9),np.uint8))
small=cv2.resize(im,(W//4,H//4),interpolation=cv2.INTER_AREA); fm=cv2.resize(front,(W//4,H//4),interpolation=cv2.INTER_NEAREST)
bfill=cv2.inpaint(small,fm,5,cv2.INPAINT_TELEA); bfill=cv2.cvtColor(cv2.resize(bfill,(W,H),interpolation=cv2.INTER_CUBIC),cv2.COLOR_BGR2RGB)
bsrc=rgb.copy(); bsrc[front>0]=bfill[front>0]
# 주황 머리 간호사: 아래쪽은 서서히 사라지게
fade=np.ones((H,W),np.float32); ys=np.arange(H,dtype=np.float32)[:,None]; fade*=np.clip((400-ys)/70,0,1)
def make_layers(k):
    layers={}
    for n_ in ORDER:
        m=out[n_].astype(np.float32)/255
        if n_=='orange': m=m*fade
        if n_=='building':        # 양옆과 아래를 서서히 투명하게 — 떠 있는 무대처럼
            xs_=np.arange(W,dtype=np.float32)[None,:]
            m=m*np.clip(xs_/70,0,1)*np.clip((W-1-xs_)/70,0,1)*np.clip((H-1-ys)/60,0,1)
        src=bsrc if n_=='building' else rgb
        yy,xx=np.where(m>0.02); x0,x1,y0,y1=xx.min(),xx.max()+1,yy.min(),yy.max()+1
        s=SC[n_]*k; w,h=max(2,int(round((x1-x0)*s))),max(2,int(round((y1-y0)*s)))
        a=cv2.GaussianBlur(m[y0:y1,x0:x1],(0,0),1.2)
        # 투명한 곳의 색은 가장자리 색으로 번지게 (어두운 테두리 방지)
        c=src[y0:y1,x0:x1].astype(np.float32); hard=(m[y0:y1,x0:x1]>0.5).astype(np.float32)
        acc=c*hard[...,None]; wgt=hard.copy()
        for _ in range(6):
            acc=cv2.blur(acc,(9,9)); wgt=cv2.blur(wgt,(9,9))
            fillc=acc/np.maximum(wgt[...,None],1e-4); c=np.where(hard[...,None]>0,c,fillc); acc=c*np.maximum(hard,(wgt>1e-3).astype(np.float32))[...,None]; wgt=np.maximum(hard,(wgt>1e-3).astype(np.float32))
        c=cv2.resize(c,(w,h),interpolation=cv2.INTER_AREA); a=cv2.resize(a,(w,h),interpolation=cv2.INTER_AREA)
        layers[n_]=dict(rgba=np.dstack([np.clip(c,0,255),np.clip(a*255,0,255)]).astype(np.uint8),box=(x0,y0,x1,y1))
    return layers
def pack(layers):
    sky=[0]*AW; place={}
    for n_ in sorted(ORDER,key=lambda k:-layers[k]['rgba'].shape[0]*layers[k]['rgba'].shape[1]):
        h,w=layers[n_]['rgba'].shape[:2]; best=None
        for x in range(0,AW-w+1):
            y=max(sky[x:x+w])
            if y+h<=AH and (best is None or y<best[1]): best=(x,y)
        if best is None: return None
        x,y=best; place[n_]=(x,y,w,h)
        for i in range(x,min(AW,x+w+1)): sky[i]=y+h+1
    return place
k=1.0
while True:
    layers=make_layers(k); place=pack(layers)
    if place: break
    k*=0.97
atlas=np.zeros((AH,AW,4),np.uint8)
for n_,(x,y,w,h) in place.items(): atlas[y:y+h,x:x+w]=layers[n_]['rgba']
print('scale factor',k)
print(place)
Image.fromarray(atlas,'RGBA').save(OUT+'/banner_atlas.png')
# 배치표: 평면 26x13 (x -13..13, y 5.5..-7.5) 에 원래 구도대로
lay=[]
for n_ in ORDER:
    x0,y0,x1,y1=[float(v) for v in layers[n_]['box']]; ax,ay,aw,ah=place[n_]
    lay.append(dict(name=n_,z=Z[n_],quad=[-13+26*x0/W, 5.5-13*y1/H, -13+26*x1/W, 5.5-13*y0/H],uv=[ax,ay,aw,ah]))
json.dump(dict(atlas=[AW,AH],layers=lay),open(OUT+'/banner_layout.json','w'),indent=1)
