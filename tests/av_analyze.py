import sys, statistics as st
def load(fn, first_pts=48600):
    rows=[l.split() for l in open(fn) if l.startswith('AV ')]
    out=[]
    for i,r in enumerate(rows):
        t,pts,heard,drop=int(r[1]),int(r[2]),int(r[3]),int(r[4])
        if pts==-9223372036854775808 or pts<0: pts=first_pts+i*3600
        out.append((t,pts,heard,drop))
    return out
for fn in sys.argv[1:]:
    d=load(fn)
    shown=[x for x in d if not x[3]]
    t0=shown[0][0]
    shown=[x for x in shown if x[0]-t0>1e6]           # skip first second
    iv=[(b[0]-a[0])/1000 for a,b in zip(shown,shown[1:])]
    off=[(x[2]-x[1])/90 for x in shown if x[2]>=0]
    dev=[abs(v-40) for v in iv]
    print(f"{fn}: frames={len(d)} dropped={sum(x[3] for x in d)}")
    print(f"  frame interval ms: mean={st.mean(iv):.1f} sd={st.pstdev(iv):.1f} min={min(iv):.1f} max={max(iv):.1f}  >5ms off: {sum(1 for x in dev if x>5)}/{len(iv)}")
    print(f"  A/V offset ms (+ = video late): mean={st.mean(off):.1f} sd={st.pstdev(off):.1f} min={min(off):.1f} max={max(off):.1f}")
    print("  first 16 intervals:", " ".join(f"{v:.0f}" for v in iv[:16]))
