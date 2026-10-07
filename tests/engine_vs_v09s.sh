#!/bin/sh
# Sanity check of the reference side: patched engine.c (OS-9 interpreter) vs patched v09s.c on random programs.
SEED=${1:?seed}; N=${2:-50000}
HERE=$(cd "$(dirname "$0")/.." && pwd); REF=${REF_BIN:-/tmp/ref/bin}; T=/tmp/evs.$$; mkdir -p $T
python3 $HERE/tests/gen_rand.py $SEED $T/p.bin
$REF/refs -img $T/p.bin -pc 0x1000 -n $N -trace $T/ref.trace -cycles $T/ref.cyc >/dev/null 2>&1
$REF/refram -img $T/p.bin -load 0 -pc 0x1000 -n $N -trace $T/rtl.trace >/dev/null 2>&1
python3 $HERE/tests/cmp_trace.py $T/ref.trace $T/rtl.trace >$T/cmp.txt && { rm -rf $T; echo "seed $SEED OK"; exit 0; }
echo "seed $SEED: $(python3 $HERE/tests/explain.py $T | sed -n 4,5p | tr "\n" " ")"
