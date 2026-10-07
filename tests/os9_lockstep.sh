#!/bin/sh
# usage: tests/os9_lockstep.sh <rom> <disk0> <disk1> <input-script> <instructions> [input-delay-cycles]
# Boots the OS-9 ROM on the RTL model and on sbc09's engine.c, and compares the
# architectural state before every instruction.  Timer ticks / IRQ acceptance of the
# RTL run are replayed into the reference (the reference has no notion of E cycles).
ROM=${1:?rom}; D0=${2:?disk0}; D1=${3:?disk1}; IN=${4:?script}; N=${5:-2000000}; DELAY=${6:-0}
HERE=$(cd "$(dirname "$0")/.." && pwd)
REF=${REF_BIN:-/tmp/ref/bin}
T=${TMPDIR:-/tmp}/os9ls.$$; mkdir -p $T
cp $D0 $T/rtl0.dsk; cp $D1 $T/rtl1.dsk; cp $D0 $T/ref0.dsk; cp $D1 $T/ref1.dsk
export TZ=UTC
VDIR=${VDIR:-.}
$HERE/build/rtlsim -rom $ROM -0 $T/rtl0.dsk -1 $T/rtl1.dsk -in $IN -n $N -trace $T/rtl.trace -sched $T/sched.txt \
    -fixed-time -rxshift 0 -noterm -indelay $DELAY -v $VDIR > $T/rtl.out 2>&1
$REF/refos9 -mode os9 -rom $ROM -0 $T/ref0.dsk -1 $T/ref1.dsk -in $IN -n $N -trace $T/ref.trace -sched $T/sched.txt -v $VDIR < /dev/null > $T/ref.out 2>&1
python3 $HERE/tests/cmp_trace.py $T/ref.trace $T/rtl.trace; rc=$?
echo "ticks: $(grep -c '^T' $T/sched.txt)  irqs: $(grep -c '^I' $T/sched.txt)  (files in $T)"
cmp -s $T/rtl0.dsk $T/ref0.dsk && cmp -s $T/rtl1.dsk $T/ref1.dsk && echo "disk images identical" || echo "disk images DIFFER"
exit $rc
