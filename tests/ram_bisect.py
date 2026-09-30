#!/usr/bin/env python3
"""Find the first instruction count at which RTL and reference RAM images differ (bisection).
usage: ram_bisect.py <lo> <hi> <sched> <input> <vdir> [delay]"""
import subprocess, sys, os, shutil
lo, hi = int(sys.argv[1]), int(sys.argv[2])
sched, inp, vdir = sys.argv[3], sys.argv[4], sys.argv[5]
delay = sys.argv[6] if len(sys.argv) > 6 else '20000000'
T = '/tmp/bisect'; os.makedirs(T, exist_ok=True)
env = dict(os.environ, TZ='UTC')
def dump(n):
    for k in ('a', 'b', 'c', 'd'):
        shutil.copy('/tmp/ref/OS9.dsk' if k in 'ac' else '/tmp/ref/WORK.dsk', f'{T}/{k}.dsk')
    subprocess.run(['/home/user/RTL-sim/build/rtlsim', '-rom', '/tmp/ref/os9v1.rom', '-0', f'{T}/a.dsk', '-1', f'{T}/b.dsk', '-in', inp,
                    '-n', str(n), '-sched', f'{T}/s.txt', '-fixed-time', '-rxshift', '0', '-noterm', '-indelay', delay, '-v', vdir,
                    '-dumpram', f'{T}/rtl.ram'], env=env, capture_output=True)
    subprocess.run(['/tmp/ref/bin/refos9', '-mode', 'os9', '-rom', '/tmp/ref/os9v1.rom', '-0', f'{T}/c.dsk', '-1', f'{T}/d.dsk', '-in', inp,
                    '-n', str(n), '-trace', f'{T}/ref.trace', '-sched', sched, '-v', vdir, '-dump', f'{T}/ref.ram'],
                   env=env, stdin=subprocess.DEVNULL, capture_output=True)
    a = open(f'{T}/ref.ram', 'rb').read(); b = open(f'{T}/rtl.ram', 'rb').read()
    return [i for i in range(0xc000) if a[i] != b[i]]
d = dump(hi)
print(f"n={hi}: {len(d)} differing RAM bytes", [hex(i) for i in d[:10]])
if not d: sys.exit(0)
while hi - lo > 1:
    mid = (lo + hi) // 2
    d = dump(mid)
    if d: hi = mid
    else: lo = mid
    print(f"lo={lo} hi={hi} ndiff={len(d)}", flush=True)
d = dump(hi)
a = open(f'{T}/ref.ram', 'rb').read(); b = open(f'{T}/rtl.ram', 'rb').read()
print(f"first differing image at n={hi}:", [(hex(i), hex(a[i]), hex(b[i])) for i in d[:16]])
