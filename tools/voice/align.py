import sys,os,json,re,difflib,collections
sys.path.insert(0,'/home/claude/dosvo'); from krtok import toks
def norm(s): return re.sub(r'[\s　％%]','',s)
# XP voice map
xmap={}
import glob
for p in sorted(glob.glob('/home/claude/work/dis/*.MES.txt')):
    pend=None; cur=None
    for ln in open(p,encoding='utf-8',errors='replace'):
        m=re.match(r'\S+ (\S+)\s+(.*)',ln.rstrip('\n'))
        if not m: continue
        op,a=m.groups()
        if op=='SYS' and a.startswith('7, ') and '.ogg' in a: pend=re.search(r"'([^']+\.ogg)'",a).group(1)
        elif op=='TXT':
            if cur is None: cur=[a,pend]; pend=None
            else: cur[0]+=a
        elif op=='SYS' and a.startswith('19,'):
            if cur and cur[1]: xmap.setdefault(norm(cur[0]),cur[1])
            cur=None
keys=list(xmap); idx=collections.defaultdict(set)
for k in keys:
    for i in range(0,max(1,len(k)-4),2): idx[k[i:i+5]].add(k)
def voice_of(t):
    t=norm(t)
    if t in xmap: return xmap[t],1.0
    c=collections.Counter()
    for i in range(0,max(1,len(t)-4),2):
        for k in idx.get(t[i:i+5],()): c[k]+=1
    best=(None,0)
    for k,_ in c.most_common(3):
        r=difflib.SequenceMatcher(None,t,k).ratio()
        if r>best[1]: best=(xmap[k],r)
    return best if best[1]>=0.8 else (None,best[1])
D='/tmp/dos/'; table=[]; stat=collections.Counter(); miss=collections.Counter()
for f in sorted(os.listdir(D+'kr')):
    if not os.path.exists(D+'x/'+f): continue
    kd=open(D+'kr/'+f,'rb').read(); jd=open(D+'x/'+f,'rb').read()
    K=toks(D+'kr/'+f,70); J=toks(D+'x/'+f)
    sig=lambda d,t: d[max(0,t[0]-8):t[0]]+bytes([t[3]])
    ks=[sig(kd,t) for t in K]; js=[sig(jd,t) for t in J]
    sm=difflib.SequenceMatcher(None,js,ks,autojunk=False)
    pair={}
    if len(K)==len(J) and sum((b'\x81\x77' in a[2])==(b'\x81\x77' in b[2]) for a,b in zip(K,J))>=0.85*len(K):
        pair={q:q for q in range(len(J))}
        opcodes=[]
    else: opcodes=None
    for tag,a1,a2,b1,b2 in (sm.get_opcodes() if opcodes is None else []):
        if tag=='equal' or (tag=='replace' and a2-a1==b2-b1):
            for q in range(a2-a1): pair[a1+q]=b1+q
        elif tag=='replace':
            js2=[(b'\x81\x77' in J[q][2], min(len(J[q][2])//24,6)) for q in range(a1,a2)]
            ks2=[(b'\x81\x77' in K[q][2], min(len(K[q][2])//24,6)) for q in range(b1,b2)]
            s2=difflib.SequenceMatcher(None,js2,ks2,autojunk=False)
            for t2,c1,c2,d1,d2 in s2.get_opcodes():
                if t2=='equal':
                    for q in range(c2-c1): pair[a1+c1+q]=b1+d1+q
    if len(pair)<0.6*len(J):
        pair={}
        qa=[(b'\x81\x77' in t[2], t[2][:8]) for t in J]; qb=[(b'\x81\x77' in t[2], t[2][:8]) for t in K]
        qa=[x[0] for x in qa]; qb=[x[0] for x in qb]
        for t2,c1,c2,d1,d2 in difflib.SequenceMatcher(None,qa,qb,autojunk=False).get_opcodes():
            if t2=='equal' and c2-c1>=4:
                for q in range(c2-c1): pair[c1+q]=d1+q
    anc=sorted(pair.items()); anc=[(-1,-1)]+anc+[(len(J),len(K))]
    for (a0,b0),(a1,b1) in zip(anc,anc[1:]):
        ga=list(range(a0+1,a1)); gb=list(range(b0+1,b1))
        if not ga or not gb: continue
        if len(ga)==len(gb):
            for x,y in zip(ga,gb): pair[x]=y
        else:
            qa=[b'\x81\x77' in J[x][2] for x in ga]; qb=[b'\x81\x77' in K[y][2] for y in gb]
            for t2,c1,c2,d1,d2 in difflib.SequenceMatcher(None,qa,qb,autojunk=False).get_opcodes():
                if t2=='equal' or (t2=='replace' and c2-c1==d2-d1):
                    for q in range(c2-c1): pair[ga[c1+q]]=gb[d1+q]
    for ji,t in enumerate(J):
        txt=t[2].decode('cp932','replace'); v,r=voice_of(txt)
        if not v: continue
        stat['voiced']+=1
        if ji in pair:
            kt=K[pair[ji]]; stat['mapped']+=1
            table.append(dict(mes=f,kr_line=pair[ji],kr_off=kt[0],jp_line=ji,ogg=v,jp=txt))
        else: stat["unmapped"]+=1; miss[f]+=1
print(stat, len({x['ogg'] for x in table}))
json.dump(table,open('/home/claude/dosvo/voice_map.json','w'),ensure_ascii=False,indent=0)
print(miss.most_common())
