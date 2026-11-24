"""Kawarazaki-ke no Ichizoku (AI.exe) MES disassembler."""
import struct, sys, os, collections
class Desync(Exception): pass
ARGOPS={0x02,0x03,0x04,0x0e,0x0f,0x12,0x14,0x15,0x16,0x20,0x22,0x23,0x24,0x25,0x26,0x27,0x28,0x2b,0x2d,0x2e,0x2f,0x30,0x31,0x32,0x33,0x34}
EOPS={0xe0:'+',0xe1:'-',0xe2:'*',0xe3:'/',0xe4:'%',0xe6:'&&',0xe7:'||',0xe8:'&',0xe9:'|',0xea:'^',0xeb:'<',0xec:'>',0xed:'<=',0xee:'>=',0xef:'==',0xf0:'!='}
class Dis:
    def __init__(s,d):
        s.d=d; n=struct.unpack_from('<I',d)[0]; s.msgs=struct.unpack_from('<%dI'%n,d,4); s.base=4+4*n; s.p=s.base; s.tables=set(); s.conds=set()
    def u8(s):
        if s.p>=len(s.d): raise Desync('eof')
        b=s.d[s.p]; s.p+=1; return b
    def u16(s): v=struct.unpack_from('<H',s.d,s.p)[0]; s.p+=2; return v
    def u32(s): v=struct.unpack_from('<I',s.d,s.p)[0]; s.p+=4; return v
    def expr(s):
        st=[]
        while True:
            b=s.u8()
            if b==0xff: break
            if b<0x80: st.append(str(b))
            elif b<0xa0: st.append('v%d'%(b-0x80))
            elif b<0xe0: st.append('b%d[%s]'%(b-0xa0,st.pop() if st else '?'))
            elif b in EOPS:
                if len(st)<2: raise Desync('stack at %x'%s.p)
                r=st.pop(); l=st.pop(); st.append('(%s%s%s)'%(l,EOPS[b],r))
            elif b==0xe5: st.append('rand(%d)'%s.u16())
            elif b==0xf1: st.append(str(s.u16()))
            elif b==0xf2: st.append(str(s.u32()))
            elif b==0xf3: st.append('F[%d]'%s.u16())
            elif b==0xf4: st.append('F[%s]'%st.pop())
            elif b==0xf5: pass
            elif b==0xf6: st.append('W[%d]'%s.u16())
            elif b==0xf7: st.append('W[%s]'%st.pop())
            elif b==0xf8: st.append('D[%d]'%s.u16())
            elif b==0xf9: st.append('D[%s]'%st.pop())
            else: raise Desync('expr op %x at %x'%(b,s.p-1))
        if len(st)!=1: raise Desync('expr stack %r at %x'%(st,s.p))
        return st[0]
    def args(s):
        r=[]
        while True:
            b=s.d[s.p]
            if b==0xff or b==0xfe: s.p+=1; return r
            if len(r)>=16: raise Desync('too many args at %x'%s.p)
            if b==0xf5:
                s.p+=1; e=s.d.index(b'\xff',s.p); r.append(repr(s.d[s.p:e].decode('cp932','replace'))); s.p=e+1
            else: r.append(s.expr())
    def elist(s):
        r=[]
        while True:
            r.append(s.expr())
            b=s.u8()
            if b in (0xff,0xfe): return r
            s.p-=1
    def stmt(s):
        a=s.p-s.base
        if a in s.conds: return a,-2,'COND',[s.expr()]
        if a in s.tables:
            n=s.u8(); pairs=[(s.u32(),s.u32()) for _ in range(n)]; return a,-1,'TABLE',[n,pairs]
        op=s.u8()
        if op==0x00:
            e=s.d.index(b'\0',s.p); t=s.d[s.p:e]; s.p=e+1; return a,op,'TXT',[t.decode('cp932','replace')]
        if op==0x01: return a,op,'JMP',[('@',s.u32())]
        if op in ARGOPS: return a,op,'OP%02X'%op,s.args()
        if op==0x05: return a,op,'SETF',[s.u16()]+s.elist()
        if op==0x06: return a,op,'SETFE',[s.expr()]+s.elist()
        if op==0x07: return a,op,'SETP',[s.u8(),s.expr()]
        if op==0x08: return a,op,'SETPB',[s.u8(),s.expr()]+s.elist()
        if op==0x09: return a,op,'SETPW',[s.u8(),s.expr()]+s.elist()
        if op==0x0a: return a,op,'SETW',[s.u16()]+s.elist()
        if op==0x0b: return a,op,'SETWE',[s.expr()]+s.elist()
        if op==0x0c: return a,op,'SETD',[s.u16()]+s.elist()
        if op==0x0d: return a,op,'SETDE',[s.expr()]+s.elist()
        if op==0x10: return a,op,'JZ',[s.expr(),('@',s.u32())]
        if op==0x11: return a,op,'PROCDEF',[s.expr(),('@',s.u32())]
        if op==0x13:
            e=s.expr(); t=s.u32(); s.tables.add(t)
            q=s.base+t; n=s.d[q]
            for k in range(n):
                c=struct.unpack_from('<I',s.d,q+1+8*k)[0]
                if c: s.conds.add(c)
            return a,op,'MENUDEF',[e,('@',t)]
        if op==0x21: return a,op,'OP21',[]
        if op==0x35: return a,op,'MSG',[s.u32()]
        if op==0x37: return a,op,'OP37',[s.u32()]
        if op==0xff: return a,op,'END',[]
        if op==0xfe: return a,op,'NOP',[]
        raise Desync('unknown op %02x at %x'%(op,a))
def dump(d):
    D=Dis(d); out=[]
    while D.p<len(d): out.append(D.stmt())
    return D,out
if __name__=='__main__':
    base=sys.argv[1]; ok=bad=0; ops=collections.Counter()
    for fn in sorted(os.listdir(base)):
        d=open(os.path.join(base,fn),'rb').read()
        try:
            D,o=dump(d); starts={x[0] for x in o}|{len(d)-D.base}
            for a,op,n,args in o:
                ops[n]+=1
                for x in args:
                    if isinstance(x,tuple) and x[1] not in starts: raise Desync('bad target %x from %x'%(x[1],a))
            bm=[m for m in D.msgs if m not in starts]
            ok+=1
            if bm: print(fn,'msg offsets not at stmt',bm[:4])
        except Exception as e: bad+=1; print(fn,'DESYNC',e)
    print('ok',ok,'bad',bad); print(ops.most_common())
