#!/bin/sh
# Fetch and build the external reference material used by the simulator and the tests:
#
#   refs/sbc09        shinji-kono/sbc09 (GPLv2): v09s.c / engine.c (instruction reference), the
#                     a09 assembler, the NitrOS-9 sources and disk images
#   build/os9v1.rom   NitrOS-9 Level 1 ROM image built from those sources   (boot ROM for rtlsim)
#   build/disks/      OS9.dsk / WORK.dsk copies (raw 256-byte-sector images)
#   build/os9level1/  the level1 module files (host directory used for /v0)
#   build/ref/        lock-step reference harnesses (refs / refos9)
#
# Nothing from sbc09 is committed to this repository.
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
REFS=${REFS:-$HERE/refs}
B=$HERE/build
mkdir -p "$REFS" "$B/disks" "$B/ref"

if [ ! -d "$REFS/sbc09/.git" ]; then
  git clone --depth 1 https://github.com/shinji-kono/sbc09 "$REFS/sbc09"
fi
SBC=$REFS/sbc09

# --- assembler + ROM maker ------------------------------------------------------------
( cd "$SBC/src" && cc -w -O1 -DTERM_CONTROL -DUSE_TERMIOS -DBIG_ENDIAN -o a09 a09.c os9crc.c )
( cd "$SBC/os9" && cc -w -o make9rom makerom.c && cc -w -o os9mod crc.c os9mod.c )

# --- NitrOS-9 level 1 modules (the level1/Makefile also wants `hg` and host cmds; only the modules are needed)
( cd "$SBC/os9" && make -k -s CFLAGS=-w level1/init >/dev/null 2>&1 || true )
( cd "$SBC/os9/level1" && make -k -s CFLAGS=-w pdisk init os9p1 os9p2 ioman pipe piper rbf scf term pty d0 d1 vrbf v0 clock shell dir mdir sysgo >/dev/null 2>&1 || true )
( cd "$SBC/os9" && ./make9rom -o os9v1.rom level1/shell level1/sysgo level1/ioman level1/term level1/pty \
    level1/pdisk level1/d0 level1/d1 level1/vrbf level1/v0 level1/clock level1/scf level1/rbf level1/init \
    level1/os9p2 level1/os9p1 >/dev/null )
cp "$SBC/os9/os9v1.rom" "$B/os9v1.rom"
cp "$SBC/os9/OS9.dsk" "$SBC/os9/WORK.dsk" "$B/disks/"
mkdir -p "$B/os9level1"
for f in shell dir mdir sysgo ioman pdisk rbf scf clock init pty term d0 d1 vrbf v0 pipe piper os9p1 os9p2; do
  [ -f "$SBC/os9/level1/$f" ] && cp "$SBC/os9/level1/$f" "$B/os9level1/$f"
done

# --- lock-step reference harnesses -------------------------------------------------------------
"$HERE/tools/ref/build_ref.sh" "$SBC" "$B/ref"
echo "ready: $B/os9v1.rom  $B/disks/  $B/os9level1/  $B/ref/"
