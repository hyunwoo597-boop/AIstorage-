import sys, struct, os, collections
EXPR_OPS={0xe0:'+',0xe1:'-',0xe2:'*',0xe3:'/',0xe4:'%',0xe5:'rand',0xe6:'&&',0xe7:'||',0xe8:'&',0xe9:'|',0xea:'^',0xeb:'<',0xec:'>',0xed:'<=',0xee:'>=',0xef:'==',0xf0:'!='}
class Desync(Exception): pass
class Dis:
    def __init__(s,d):
        s.d=d; n=struct.unpack('<I',d[:4])[0]; s.labels=struct.unpack('<%dI'%n,d[4:4+4*n]); s.base=4+4*n; s.p=s.base
    def u8(s):
        if s.p>=len(s.d): raise Desync('eof')
        b=s.d[s.p]; s.p+=1; return b
    def u16(s): v=struct.unpack_from('<H',s.d,s.p)[0]; s.p+=2; return v
    def u32(s): v=struct.unpack_from('<I',s.d,s.p)[0]; s.p+=4; return v
    def cstr(s):
        e=s.d.index(b'\0',s.p); t=s.d[s.p:e]; s.p=e+1; return t.decode('cp932','replace')
    def expr(s):
        st=[]
        while True:
            b=s.u8()
            if b==0xff: break
            if b==0x80: st.append('v16[%d]'%s.u8())
            elif b==0xa0: i=st.pop(); v=s.u8(); st.append(('sys16[%s]'%i) if v==0 else 'a16:%d[%s]'%(v-1,i))
            elif b==0xc0: i=st.pop(); v=s.u8(); st.append('a8:%d[%s]'%(v,i))
            elif b in EXPR_OPS:
                if b==0xe5: a=st.pop(); st.append('rand(%s)'%a)
                else: r=st.pop(); l=st.pop(); st.append('(%s %s %s)'%(l,EXPR_OPS[b],r))
            elif b==0xf1: st.append(str(s.u16()))
            elif b==0xf2: st.append(str(s.u32()))
            elif b==0xf3: st.append('f[%d]'%s.u16())
            elif b==0xf5: st.append('f[%s]'%st.pop())
            elif b==0xf6: st.append('m32[%d]'%s.u16())
            elif b==0xf7: st.append('m32[%s]'%st.pop())
            elif b==0xf8: st.append('v32[%d]'%s.u8())
            elif b==0xf9: i=st.pop(); v=s.u8(); st.append(('sys32[%s]'%i) if v==0 else 'a32:%d[%s]'%(v-1,i))
            else: st.append(str(b))
        if len(st)!=1: raise Desync('expr stack %r at %x'%(st,s.p))
        return st[0]
    def elist(s):
        r=[s.expr()]
        while s.u8(): r.append(s.expr())
        return r
    def params(s):
        r=[]
        while True:
            t=s.u8()
            if t==0: return r
            if t==1: r.append(repr(s.cstr()))
            elif t==2: r.append(s.expr())
            else: raise Desync('param type %x at %x'%(t,s.p-1))
    def stmt(s):
        a=s.p-s.base; op=s.u8()
        if op==0: return a,op,'END',[]
        if op==1: return a,op,'TXT',[s.cstr()]
        if op==2: return a,op,'STR',[s.cstr()]
        if op==3: return a,op,'SETRBC',[s.u16()]+s.elist()
        if op==4: return a,op,'SETV',[s.u8()]+s.elist()
        if op==5: return a,op,'SETRBE',[s.expr()]+s.elist()
        if op in(6,7,8): return a,op,['SETAC','SETA@','SETAD'][op-6],[s.expr(),s.u8()]+s.elist()
        if op==9: return a,op,'SETM32C',[s.u16()]+s.elist()
        if op==10: return a,op,'SETM32E',[s.expr()]+s.elist()
        if op==0xb: return a,op,'JZ',[s.expr(),('@',s.u32())]
        if op==0xc: return a,op,'JMP',[('@',s.u32())]
        if op==0xd: return a,op,'SYS',[s.expr()]+s.params()
        if op==0xe: return a,op,'GOTO',s.params()
        if op==0xf: return a,op,'CALL',s.params()
        if op==0x10: return a,op,'MENUI',s.params()+[('@',s.u32())]
        if op==0x11: return a,op,'PROC',s.params()
        if op==0x12: return a,op,'UTIL',s.params()
        if op==0x13: return a,op,'LINE',[s.u8()]
        if op==0x14: return a,op,'PROCD',[s.expr(),('@',s.u32())]
        if op==0x15: return a,op,'MENUS',s.params()
        if op==0x16: return a,op,'SETRD',[s.u8()]+s.elist()
        if op==0x17: return a,op,'MSGID',[s.u32()]
        if op==0x18: return a,op,'JMPLBL',[s.expr()]
        if op==0x1d: return a,op,'OP1D',s.params()
        if op==0x1a: return a,op,'OP1A',[]
        if op==0x19: return a,op,'OP19',[]
        if op==0x1b: return a,op,'OP1B',s.params()
        if op==0x1c: return a,op,'LIBDEF',[s.expr(),('@',s.u32())]
        if op==0x1f: return a,op,'LABEL',[s.u32()]
        raise Desync('unknown op %02x at %x (file %x)'%(op,a,s.p-1))
    def run(s):
        out=[]
        while s.p<len(s.d): out.append(s.stmt())
        return out
if __name__=='__main__':
    base=sys.argv[1]; ok=0; bad=0; ops=collections.Counter(); sysc=collections.Counter(); proc=collections.Counter()
    for fn in sorted(os.listdir(base)):
        d=open(base+'/'+fn,'rb').read(); D=Dis(d)
        try:
            o=D.run(); starts={x[0] for x in o}|{len(d)-D.base}
            for a,op,n,args in o:
                ops[n]+=1
                for x in args:
                    if isinstance(x,tuple) and x[1] not in starts: raise Desync('bad target %x from %x'%(x[1],a))
                if n=='SYS': sysc[(args[0],)]+=1
            lb=[l for l in D.labels if l not in starts]
            if lb: raise Desync('bad labels %r'%lb[:5])
            ok+=1
        except Exception as e:
            bad+=1; print(fn,'DESYNC',e)
    print('ok',ok,'bad',bad); print(ops.most_common()); print(sorted(sysc.items(),key=lambda x:-x[1]))

def dump(path,out=None):
    d=open(path,'rb').read(); D=Dis(d); lines=[]
    try:
        while D.p<len(d):
            a,op,n,args=D.stmt()
            lines.append('%05x %-8s %s'%(a,n,', '.join(('@%x'%x[1]) if isinstance(x,tuple) else str(x) for x in args)))
    except Exception as e: lines.append('!! '+str(e))
    return lines
