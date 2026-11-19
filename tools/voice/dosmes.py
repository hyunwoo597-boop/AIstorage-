import sys,glob,os,json
def lines(path):
    d=open(path,'rb').read(); n=d[0]|d[1]<<8
    tab=[d[2+i:4+i] for i in range(0,n,2)]
    c=d[2+n:]; out=[]; cur=b''; i=0
    while i<len(c):
        x=c[i]
        if 0x80<=x<0x80+len(tab): cur+=tab[x-0x80]; i+=1; continue
        if 0x60<=x<0x80 and i+1<len(c) and c[i+1]>=0x40:
            cur+=bytes([x+0x20,c[i+1]]); i+=2; continue
        if len(cur)>=6: out.append(cur.decode('cp932','replace'))
        cur=b''; i+=1
    if len(cur)>=6: out.append(cur.decode('cp932','replace'))
    return out
if __name__=='__main__':
    allr={}
    for p in sorted(glob.glob(sys.argv[1]+'/*.MES')):
        allr[os.path.basename(p)]=lines(p)
    json.dump(allr,open(sys.argv[2],'w'),ensure_ascii=False)
    print(sum(len(v) for v in allr.values()))
    print('\n'.join(allr['S1.MES'][:25]))
