#!/bin/sh
# Build the lock-step reference harnesses from a checkout of shinji-kono/sbc09.
# Usage: tools/ref/build_ref.sh <sbc09-checkout> <outdir>
# The reference sources are GPLv2 and are NOT copied into this repository.
set -e
SBC=${1:?sbc09 checkout}
OUT=${2:?output dir}
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$OUT"
CF="-O1 -w -fcommon -I$SBC/src -Dusleep=my_usleep"
# instruction-set reference: v09s.c stepped one instruction at a time (trace + cycle counts)
python3 "$HERE/patch_v09s.py" "$SBC/src/v09s.c" "$OUT/v09s_fixed.c"
cc $CF -I"$OUT" -o "$OUT/refs" "$HERE/refs.c"
# engine.c (the interpreter behind v09) with its inconsistent undefined-H flags aligned to v09s
python3 "$HERE/patch_engine.py" "$SBC/src/engine.c" "$OUT/engine_fixed.c"
# plain-RAM harness on engine.c (kept for cross checks)
cc $CF -I"$OUT" -DRAMMODE -o "$OUT/refram" "$HERE/refrun.c" "$OUT/engine_fixed.c"
# OS-9 harness (real ACIA / timer / disk register model of v09); deterministic clock via my_time()
cc $CF -DUSE_VDISK -c -o "$OUT/refrun_os9.o" "$HERE/refrun.c"
cc $CF -I"$OUT" -c -o "$OUT/engine.o" "$OUT/engine_fixed.c"
cc $CF -DUSE_VDISK -DTERM_CONTROL -DUSE_TERMIOS -Dtime=my_time -Dsetitimer=my_setitimer -c -o "$OUT/io.o" "$SBC/src/io.c"
# vdisk.c uses the BSD-only d_namlen member of struct dirent -> strlen(d_name)
# (and `struct stat st;` is read uninitialised for fmemopen streams -> zero it so the reference is deterministic)
sed -e 's/dp->d_namlen/strlen(dp->d_name)/g' -e 's/struct stat st;/struct stat st; memset(\&st, 0, sizeof st);/' "$SBC/src/vdisk.c" > "$OUT/vdisk_fixed.c"
cc $CF -ftrivial-auto-var-init=zero -I"$SBC/src" -DUSE_VDISK -c -o "$OUT/vdisk.o" "$OUT/vdisk_fixed.c"
cc -o "$OUT/refos9" "$OUT/refrun_os9.o" "$OUT/engine.o" "$OUT/io.o" "$OUT/vdisk.o"
echo "built $OUT/refs $OUT/refram $OUT/refos9"
