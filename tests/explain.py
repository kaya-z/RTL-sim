#!/usr/bin/env python3
"""explain.py <dir with p.bin/ref.trace/rtl.trace> : disassemble the instruction before the first mismatch."""
import sys, subprocess
d = sys.argv[1]
a = [l.split() for l in open(d + '/ref.trace') if l.strip()]
b = [l.split() for l in open(d + '/rtl.trace') if l.strip()]
for i in range(min(len(a), len(b))):
    if a[i] != b[i]:
        break
pc = int(a[i - 1][0], 16)
print("before :", ' '.join(a[i - 1]), "  (pc a b x y u s dp cc)")
print("ref    :", ' '.join(a[i]))
print("rtl    :", ' '.join(b[i]))
img = open(d + '/p.bin', 'rb').read()
print("bytes  :", img[pc:pc + 5].hex())
out = subprocess.run(['/tmp/ref/bin/d09', d + '/p.bin', '%x' % pc, '%x' % (pc + 3)], capture_output=True, text=True).stdout
print(out.split('\n')[1])
