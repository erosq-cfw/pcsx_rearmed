# layout_diff.py A B -- symbol address/size deltas of binary B vs A (text/data/bss/rodata).
# JITSCHED bar: vs the shipped binary, every pre-existing symbol must show delta 0 and
# "size changed: []" (re-derive JITSCHED_RECOMPILE_PAD in recompiler.cpp if not).
import subprocess, collections, sys
import os
EROSQ_ROOT=os.environ.get('EROSQ_ROOT') or os.path.abspath(os.path.join(os.path.dirname(__file__),'../../../..'))
NM=os.environ.get('NM') or os.path.join(EROSQ_ROOT,'buildroot-2026.08/output/host/bin/mipsel-buildroot-linux-musl-nm')
def syms(f):
    d={}
    for l in subprocess.run([NM,'-S','-n',f],capture_output=True,text=True).stdout.split('\n'):
        p=l.split()
        if len(p)==4 and p[2] in 'tTbBdDrR': d[p[3]]=(int(p[0],16),int(p[1],16),p[2].lower())
    return d
a=syms(sys.argv[1]); b=syms(sys.argv[2])
c=collections.Counter((a[k][2], b[k][0]-a[k][0]) for k in a if k in b)
for t in 'tdbr': print(t, sorted([(d,n) for (tt,d),n in c.items() if tt==t], key=lambda x:-x[1])[:5])
print('moved text symbols:', [k for k in a if k in b and a[k][2]=='t' and a[k][0]!=b[k][0]][:12])
print('size changed:', [(k,a[k][1],b[k][1]) for k in a if k in b and a[k][1]!=b[k][1]])
print('only in B:', [k for k in b if k not in a][:20])
