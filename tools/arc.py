import sys, struct, os
def read_index(p):
    f=open(p,'rb'); n=struct.unpack('<I',f.read(4))[0]; t=f.read(n*28)
    ents=[(t[i*28:i*28+20].split(b'\0')[0].decode('cp932'),)+struct.unpack('<II',t[i*28+20:i*28+28]) for i in range(n)]
    return f,n,ents
def solve(p):
    f,n,e=read_index(p); fs=os.path.getsize(p)
    # offset key: assume first offset = 4+n*28 or try xor s.t. sorted & consistent
    for first in (4+n*28,):
        ko=e[0][2]^first
        offs=[x[2]^ko for x in e]
        ks=e[0][1]^(offs[1]-offs[0])
        sizes=[x[1]^ks for x in e]
        ok=all(offs[i]+sizes[i]==offs[i+1] for i in range(n-1)) and offs[-1]+sizes[-1]==fs
        return ko,ks,ok,[(e[i][0],offs[i],sizes[i]) for i in range(n)]
if __name__=='__main__':
    ko,ks,ok,l=solve(sys.argv[1]); print(sys.argv[1],len(l),hex(ko),hex(ks),ok)
    if len(sys.argv)>2:
        os.makedirs(sys.argv[2],exist_ok=True); f=open(sys.argv[1],'rb')
        for nm,o,s in l:
            f.seek(o); open(os.path.join(sys.argv[2],nm),'wb').write(f.read(s))
    else:
        for x in l[:8]: print(x)
