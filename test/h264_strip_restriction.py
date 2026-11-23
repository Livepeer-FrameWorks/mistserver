# Rewrite every H264 SPS in an Annex B stream so VUI ends before bitstream_restriction_flag (set to 0).
import sys
def unescape(b):
    o=bytearray(); z=0
    for x in b:
        if z>=2 and x==3: z=0; continue
        o.append(x); z = z+1 if x==0 else 0
    return bytes(o)
def escape(b):
    o=bytearray(); z=0
    for x in b:
        if z>=2 and x<=3: o.append(3); z=0
        o.append(x); z = z+1 if x==0 else 0
    return bytes(o)
class R:
    def __init__(s,d): s.d=d; s.p=0
    def u(s,n):
        v=0
        for _ in range(n): v=(v<<1)|((s.d[s.p>>3]>>(7-(s.p&7)))&1); s.p+=1
        return v
    def ue(s):
        z=0
        while s.u(1)==0: z+=1
        return (1<<z)-1+s.u(z)
    def se(s): k=s.ue(); return (k+1)//2 if k&1 else -(k//2)
def scaling(r,n):
    last=nxt=8
    for _ in range(n):
        if nxt: nxt=(last+r.se()+256)%256
        last = nxt or last
def hrd(r):
    c=r.ue(); r.u(4); r.u(4)
    for _ in range(c+1): r.ue(); r.ue(); r.u(1)
    r.u(5); r.u(5); r.u(5); r.u(5)
def brpos(rbsp):
    r=R(rbsp); r.u(8); prof=r.u(8); r.u(16); r.ue()
    if prof in (100,110,122,244,44,83,86,118,128,138,139,134,135):
        cf=r.ue()
        if cf==3: r.u(1)
        r.ue(); r.ue(); r.u(1)
        if r.u(1):
            for i in range(8 if cf!=3 else 12):
                if r.u(1): scaling(r,16 if i<6 else 64)
    r.ue(); poc=r.ue()
    if poc==0: r.ue()
    elif poc==1:
        r.u(1); r.se(); r.se()
        for _ in range(r.ue()): r.se()
    r.ue(); r.u(1); r.ue(); r.ue()
    if not r.u(1): r.u(1)
    r.u(1)
    if r.u(1): r.ue(); r.ue(); r.ue(); r.ue()
    if not r.u(1): return None
    if r.u(1):
        if r.u(8)==255: r.u(32)
    if r.u(1): r.u(1)
    if r.u(1):
        r.u(4)
        if r.u(1): r.u(24)
    if r.u(1): r.ue(); r.ue()
    if r.u(1): r.u(32); r.u(32); r.u(1)
    n=r.u(1)
    if n: hrd(r)
    v=r.u(1)
    if v: hrd(r)
    if n or v: r.u(1)
    r.u(1)
    return r.p
def fix(nal):
    rb=unescape(nal); p=brpos(rb)
    if p is None or not ((rb[p>>3]>>(7-(p&7)))&1): return nal
    bits=''.join(format(x,'08b') for x in rb)[:p]+'0'+'1'
    bits+= '0'*((-len(bits))%8)
    return escape(bytes(int(bits[i:i+8],2) for i in range(0,len(bits),8)))
d=open(sys.argv[1],'rb').read()
idx=[]; i=0
while True:
    j=d.find(b'\x00\x00\x01',i)
    if j<0: break
    idx.append(j+3); i=j+3
out=bytearray(); n=0
for k,s in enumerate(idx):
    e=idx[k+1]-3 if k+1<len(idx) else len(d)
    nal=d[s:e]
    while k+1<len(idx) and nal.endswith(b'\x00'): nal=nal[:-1]
    if nal and (nal[0]&0x1f)==7: nal=fix(nal); n+=1
    out+=b'\x00\x00\x00\x01'+nal
open(sys.argv[2],'wb').write(out); print('sps rewritten',n)
