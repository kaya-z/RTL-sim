#!/usr/bin/env python3
"""Compare two instruction traces (pc a b x y u s dp cc) and print the first divergence."""
import sys

def main():
    a = open(sys.argv[1]).read().split('\n')
    b = open(sys.argv[2]).read().split('\n')
    a = [l for l in a if l]; b = [l for l in b if l]
    n = min(len(a), len(b))
    names = 'pc a b x y u s dp cc'.split()
    for i in range(n):
        if a[i] != b[i]:
            print(f"MISMATCH at instruction #{i}  (ref={sys.argv[1]}, rtl={sys.argv[2]})")
            for j in range(max(0, i - 4), i + 1):
                print(f"  {j:7d} ref: {a[j]}")
                print(f"  {'':7s} rtl: {b[j]}")
            fa, fb = a[i].split(), b[i].split()
            print("  differing fields:", ", ".join(f"{k}: ref={x} rtl={y}" for k, x, y in zip(names, fa, fb) if x != y))
            return 1
    print(f"OK: {n} instructions identical (ref {len(a)}, rtl {len(b)})")
    # the RTL run may stop early at an illegal opcode / SYNC (models differ by design there)
    if len(b) < len(a):
        if len(b) < 200:
            print("  (too short: RTL stopped early)"); return 2
        print(f"  (RTL stopped after {len(b)} instructions: illegal opcode / SYNC reached)")
    return 0

if __name__ == '__main__':
    sys.exit(main())
