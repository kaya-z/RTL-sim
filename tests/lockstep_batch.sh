#!/bin/sh
# usage: tests/lockstep_batch.sh <first-seed> <last-seed> [instructions]   (runs 4 jobs in parallel)
A=${1:?first}; B=${2:?last}; N=${3:-100000}
HERE=$(cd "$(dirname "$0")" && pwd)
seq $A $B | xargs -P 4 -I{} sh -c "$HERE/run_lockstep.sh {} $N > /tmp/lockstep_seed_{}.log 2>&1; echo seed {} rc=\$?"
fail=0
for s in $(seq $A $B); do grep -q "cycles:.*OK" /tmp/lockstep_seed_$s.log || { echo "FAIL seed $s"; fail=1; }; done
[ $fail = 0 ] && echo "ALL $((B-A+1)) SEEDS PASS"
exit $fail
