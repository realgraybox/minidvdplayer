import sys,collections
rows=[l.split() for l in open(sys.argv[1]) if l.startswith('AV ')]
t0=int(rows[0][1]); prev=None; b=collections.defaultdict(list)
for r in rows:
    t,pts,heard,drop=int(r[1]),int(r[2]),int(r[3]),int(r[4])
    b[int((t-t0)/1e6)].append(((t-prev)/1000 if prev else 40,(heard-pts)/90 if heard>=0 else None,drop,pts)); prev=t
for k in sorted(b)[:int(sys.argv[2]) if len(sys.argv)>2 else 99]:
    v=b[k]; dts=[x[0] for x in v]; offs=[x[1] for x in v if x[1] is not None]
    print(f"{k:2d}s frames={len(v):2d} dt={min(dts):3.0f}..{max(dts):3.0f}ms av={'%7.1f..%7.1f'%(min(offs),max(offs)) if offs else '     n/a'}ms drops={sum(x[2] for x in v)} pts={v[0][3]/90000:6.2f}s")
