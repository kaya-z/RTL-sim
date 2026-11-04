#!/bin/sh
# usage: scripts/setup_jwp.sh      -> build/jwp/{os9v1.rom,os9v2.rom,OS9.dsk,OS9L2.dsk,WORK.dsk[,SKK_utf8.dsk]}
# Fetches kaya-z/sim09 (https://github.com/kaya-z/sim09) into refs/sim09 and takes from it what the Japanese word processor
# `jwp` needs on this simulator.  Nothing from sim09 is stored in this repository.
#   * ROMs with the kanji-aware SCF (PD.PAR = character set, `kmode`) and disks that already contain jwp,
#     for Level 1 (os9v1.rom + OS9.dsk) and Level 2 (os9v2.rom + OS9L2.dsk)
#   * optionally a UTF-8 SKK dictionary disk (needs SKK-JISYO.L: apt install skkdic) for kana-kanji conversion
# Licenses: jwp.c/skk.c are MIT, the prebuilt jwp modules contain CMOC's run-time library (GPL-3.0), the SKK dictionary is
# GPL-2.0-or-later and NitrOS-9 is GPL (see sim09's LICENSES.md).  Credits: sim09 / jwp by YUKARI Semiconductor Devices.
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
REFS=${REFS:-$HERE/refs}
SIM=$REFS/sim09
OUT=$HERE/build/jwp
mkdir -p "$REFS" "$OUT"
if [ ! -d "$SIM/.git" ]; then
  GIT_LFS_SKIP_SMUDGE=1 git clone --depth 1 https://github.com/kaya-z/sim09 "$SIM"
else   # sim09 keeps growing (jwp, dhry, pc ...): follow its master (local changes of the clone are discarded)
  GIT_LFS_SKIP_SMUDGE=1 git -C "$SIM" fetch -q --depth 1 origin master && git -C "$SIM" reset -q --hard FETCH_HEAD
fi
O=$SIM/os9
for f in os9v1.rom os9v2.rom OS9.dsk OS9L2.dsk WORK.dsk; do cp "$O/$f" "$OUT/$f"; done
for f in jwp jwp-l2; do [ -f "$O/jwp/$f" ] && cp "$O/jwp/$f" "$OUT/$f"; done      # the same modules, for use through -v
# do the images contain the programs?  (a stale clone/disk from before Pascal was added to sim09 has no `pc`)
python3 - "$OUT" "$O" <<'PY'
import os, sys
out, o = sys.argv[1], sys.argv[2]
for disk, prog in (("OS9.dsk", "level1/cmds/ysdbasic"), ("OS9L2.dsk", "level2/cmds/ysdbasic"), ("OS9L2.dsk", "pascal/pas1"), ("OS9L2.dsk", "pascal/cg")):
    src = os.path.join(o, prog)
    if not os.path.exists(src):
        print(f"  WARNING: {prog} is not in the sim09 clone - update refs/sim09 and run this script again", file=sys.stderr); continue
    head = open(src, "rb").read()[:64]
    ok = head in open(os.path.join(out, disk), "rb").read()
    print(f"  {disk}: {os.path.basename(prog)} {'ok' if ok else 'MISSING'}")
    if not ok: print(f"  WARNING: {disk} does not contain {os.path.basename(prog)} - update refs/sim09 (git -C refs/sim09 log -1) and run this script again", file=sys.stderr)
PY
JISYO=${SKKJISYO:-/usr/share/skk/SKK-JISYO.L}
if [ -f "$JISYO" ]; then
  make -C "$O/skk" SKKJISYO="$JISYO" SKK_utf8.dsk >/dev/null
  cp "$O/skk/SKK_utf8.dsk" "$OUT/SKK_utf8.dsk"
  echo "SKK dictionary disk: $OUT/SKK_utf8.dsk"
else
  echo "note: no SKK dictionary ($JISYO): kana-kanji conversion needs it (apt install skkdic, then run this script again)"
fi
echo "ready: $OUT"
ls -1 "$OUT"
cat <<MSG

start (Level 1 / Level 2; then type  chx /d0/cmds ):
  build/rtlsim    -rom build/jwp/os9v1.rom -0 build/jwp/OS9.dsk   -1 build/jwp/SKK_utf8.dsk -v build/os9
  build/rtlsim -2 -rom build/jwp/os9v2.rom -0 build/jwp/OS9L2.dsk -1 build/jwp/SKK_utf8.dsk -v build/os9l2
  (without the SKK dictionary use  -1 build/jwp/WORK.dsk ;  Pascal 'pc' is Level 2 only)
MSG
