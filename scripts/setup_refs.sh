#!/bin/sh
# Credits: operates on sbc09 (L.C. Benschop and the sbc09 team, GPLv2; S. Kono's fork github.com/shinji-kono/sbc09).
# The sources are patched into generated copies at build time; nothing from sbc09 is stored in this repository.
# Fetch and build the external reference material used by the simulator and the tests:
#
#   refs/sbc09        shinji-kono/sbc09 (GPLv2): v09s.c / engine.c (instruction reference), the
#                     a09 assembler, the NitrOS-9 sources and disk images
#   build/os9v1.rom   NitrOS-9 Level 1 ROM image built from those sources   (boot ROM for rtlsim)
#   build/os9v2.rom   NitrOS-9 Level 2 ROM image                              (rtlsim -2)
#   build/disks/      OS9.dsk / WORK.dsk copies (raw 256-byte-sector images)
#   build/os9/cmds/   NitrOS-9 commands + BASIC09 assembled from the sources (host directory used for /v0)
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

# --- boot ROMs (Level 1 and Level 2); build_rom.sh prints the reason if one cannot be built
"$HERE/scripts/build_rom.sh" 1
"$HERE/scripts/build_rom.sh" 2
cp "$SBC/os9/OS9.dsk" "$SBC/os9/WORK.dsk" "$B/disks/"
# --- commands and BASIC09 for /v0/cmds ------------------------------------------------------------
"$HERE/scripts/build_cmds.sh" "$B/os9" </dev/null

# --- lock-step reference harnesses -------------------------------------------------------------
"$HERE/tools/ref/build_ref.sh" "$SBC" "$B/ref"
echo "ready: $B/os9v1.rom  $B/disks/  $B/os9/  $B/ref/"
cat <<MSG

next steps:
  * Level 2 commands and BASIC09:   LEVEL=2 scripts/build_cmds.sh
  * Pascal (pc), the editor jwp, YSD BASIC and Dhrystone need other ROM/disk images:
        scripts/setup_jwp.sh          (see README, "ビルドと起動手順" step 5)
MSG
