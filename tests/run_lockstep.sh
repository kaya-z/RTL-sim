#!/bin/sh
# usage: tests/run_lockstep.sh <seed> [n_instructions] [gen options]
# Compares the RTL core against sbc09 v09s.c on a random legal program.
SEED=${1:?seed}; N=${2:-20000}; shift 2 2>/dev/null
T=${TMPDIR:-/tmp}/lockstep.$$; mkdir -p $T
HERE=$(cd "$(dirname "$0")/.." && pwd)
REF=${REF_BIN:-/tmp/ref/bin}
python3 $HERE/tests/gen_rand.py $SEED $T/p.bin "$@" || exit 2
$REF/refs -img $T/p.bin -pc 0x1000 -n $N -trace $T/ref.trace -cycles $T/ref.cyc >/dev/null 2>&1
$HERE/build/ramtest -img $T/p.bin -n $N -trace $T/rtl.trace -cycles-log $T/rtl.cyc -stop-on-ill
python3 $HERE/tests/cmp_trace.py $T/ref.trace $T/rtl.trace || { echo "(files kept in $T)"; exit 1; }
python3 $HERE/tests/cmp_cycles.py $T/p.bin $T/ref.trace $T/ref.cyc $T/rtl.cyc || { echo "(files kept in $T)"; exit 1; }
rm -rf $T
