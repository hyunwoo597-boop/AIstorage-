import os,collections
def toks(path, n=None, rawhi=True):
    d=open(path,'rb').read()
    if n is None: n=d[1]<<8|d[0]
    tab=[d[2+2*i:4+2*i] for i in range(n)]; c=d[2+2*n:]
    out=[];cur=[];i=0;st=None
    while i<len(c):
        x=c[i]
        if 0x80<=x<0x80+n: 
            if not cur: st=i
            cur.append(tab[x-0x80]); i+=1; continue
        if (0x60<=x<0x80 or (rawhi and x>=0x80+n and x>=0xE0)) and i+1<len(c) and c[i+1]>=0x40:
            if not cur: st=i
            cur.append(bytes([x+0x20 if x<0x80 else x,c[i+1]])); i+=2; continue
        if len(cur)>=3: out.append((st+2+2*n,i+2+2*n,b''.join(cur),x))
        cur=[]; i+=1
    return out
