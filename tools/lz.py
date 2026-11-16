def lzss(src, start=0, maxout=None):
    win=bytearray(4096); wp=0xFEE; out=bytearray(); i=start; n=len(src)
    while i<n:
        fl=src[i]; i+=1
        for b in range(8):
            if i>=n: break
            if fl>>b&1:
                c=src[i]; i+=1; out.append(c); win[wp]=c; wp=(wp+1)&0xFFF
            else:
                if i+1>=n: i=n; break
                lo,hi=src[i],src[i+1]; i+=2
                off=lo|((hi&0xF0)<<4); ln=(hi&0xF)+3
                for k in range(ln):
                    c=win[(off+k)&0xFFF]; out.append(c); win[wp]=c; wp=(wp+1)&0xFFF
            if maxout and len(out)>=maxout: return bytes(out)
    return bytes(out)
