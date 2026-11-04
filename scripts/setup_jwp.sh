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
