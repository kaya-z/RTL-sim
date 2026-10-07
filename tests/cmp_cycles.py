#!/usr/bin/env python3
"""Check the RTL core's per-instruction bus-cycle counts against the independent
data-sheet model (cycle_model.py).  v09s.c's own counts are printed only as information."""
import sys, collections
sys.path.insert(0, __import__('os').path.dirname(__file__))
from cycle_model import expected_cycles
ref = [int(x) for x in open(sys.argv[3]) if x.strip()]
rl = [l.split() for l in open(sys.argv[4]) if l.strip()][1:]      # first line = reset sequence
n = min(len(ref), len(rl))
bad = collections.OrderedDict(); modelled = 0; v09s_diff = collections.Counter()
for i in range(n):
    rtl_c = int(rl[i][0]); code = bytes.fromhex(rl[i][1]); cc = int(rl[i][2], 16)
    exp = expected_cycles(code, 0, cc)
    if ref[i] != rtl_c: v09s_diff[code[0]] += 1
    if exp is None: continue
    modelled += 1
    if exp != rtl_c:
        bad.setdefault(code.hex(), (exp, rtl_c, i))
for k, (e, r, i) in list(bad.items())[:10]:
    print(f"  CYCLE MISMATCH {k}: data-sheet={e} rtl={r} (first at instruction #{i})")
print(f"cycles: {modelled}/{n} instructions checked against data-sheet model: " + ("OK" if not bad else f"{len(bad)} bad"))
sys.exit(1 if bad else 0)
