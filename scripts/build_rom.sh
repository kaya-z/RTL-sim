#!/bin/sh
# usage: scripts/build_rom.sh [1|2]      (default 2)   -> build/os9v<N>.rom
# Credits: NitrOS-9 project sources (GPL) and the sbc09 build files (GPLv2, S. Kono's fork); nothing is committed here.
# Builds the NitrOS-9 modules with a09 and links them into a boot ROM with sbc09's make9rom.  Every step prints its errors,
# so a missing ROM can be diagnosed.  Needs refs/sbc09 (run scripts/setup_refs.sh first, or set SBC=<checkout>).
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
LV=${1:-2}
SBC=${SBC:-$HERE/refs/sbc09}
B=$HERE/build
[ -d "$SBC/os9" ] || { echo "build_rom: $SBC/os9 not found - run scripts/setup_refs.sh first" >&2; exit 1; }
mkdir -p "$B"
# tools: assembler and ROM maker
[ -x "$SBC/src/a09" ] || ( cd "$SBC/src" && cc -w -O1 -DTERM_CONTROL -DUSE_TERMIOS -DBIG_ENDIAN -o a09 a09.c os9crc.c )
[ -x "$SBC/os9/make9rom" ] || ( cd "$SBC/os9" && cc -w -o make9rom makerom.c )
[ -x "$SBC/os9/os9mod" ] || ( cd "$SBC/os9" && cc -w -o os9mod crc.c os9mod.c )

if [ "$LV" = 1 ]; then
  MODS="shell sysgo ioman term pty pdisk d0 d1 vrbf v0 clock scf rbf init os9p2 os9p1"; OPT=""
  ( cd "$SBC/os9" && make -k -s CFLAGS=-w level1/init >/dev/null 2>&1 || true )
  ( cd "$SBC/os9/level1" && make -k -s CFLAGS=-w pdisk init os9p1 os9p2 ioman pipe piper rbf scf term pty d0 d1 vrbf v0 clock shell dir mdir sysgo >/dev/null 2>&1 || true )
else
  MODS="shell dir d1 ioman os9p3_perr os9p4_regdump pipe piper pipeman v0 vrbf scf rbf os9p2 sysgo clock pdisk d0 pty term init boot os9p1"; OPT="-2"
  # the Makefile's last targets (TL1, mc09) cannot be built here: -k keeps going, and its final error is expected
  ( cd "$SBC/os9/level2" && make -k -s CFLAGS=-w >"$B/rom_modules.log" 2>&1 || true )
fi
missing=""
for m in $MODS; do [ -s "$SBC/os9/level$LV/$m" ] || missing="$missing level$LV/$m"; done
if [ -n "$missing" ]; then
  echo "build_rom: these Level $LV modules were not built:$missing" >&2
  echo "          see $B/rom_modules.log (a09 messages); is $SBC/nitros9-code complete?" >&2
  exit 1
fi
L=""; for m in $MODS; do L="$L level$LV/$m"; done
( cd "$SBC/os9" && ./make9rom -o os9v$LV.rom $OPT $L ) >"$B/rom_link.log" 2>&1 || { cat "$B/rom_link.log" >&2; echo "build_rom: make9rom failed" >&2; exit 1; }
cp "$SBC/os9/os9v$LV.rom" "$B/os9v$LV.rom"
echo "built $B/os9v$LV.rom ($(wc -c < "$B/os9v$LV.rom") bytes)"
