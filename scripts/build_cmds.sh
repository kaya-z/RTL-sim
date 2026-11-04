#!/bin/sh
# usage: [LEVEL=1|2] scripts/build_cmds.sh [outdir]   (default outdir: build/os9, or build/os9l2 for LEVEL=2)
# Assemble the NitrOS-9 commands (Level 1 or 2 definitions) and BASIC09 from the sources in refs/sbc09 (nitros9-code)
# into build/os9/cmds, which rtlsim exposes as /v0/cmds (SysGo sets the execution directory to /v0/cmds).
# Credits: NitrOS-9 project sources (GPL), a09 assembler by the sbc09 team (GPLv2).  Nothing is committed.
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
SBC=${SBC:-$HERE/refs/sbc09}
LEVEL=${LEVEL:-1}
if [ "$LEVEL" = 2 ]; then OUT=${1:-$HERE/build/os9l2}; else OUT=${1:-$HERE/build/os9}; fi
S=$SBC/os9/nitros9-code
A=$SBC/src/a09
[ -x "$A" ] || { echo "a09 not built: run scripts/setup_refs.sh first" >&2; exit 1; }
# a Level 2 setup needs the Level 2 ROM too: build it here if an older setup_refs.sh run did not
[ "$LEVEL" = 2 ] && [ ! -s "$HERE/build/os9v2.rom" ] && "$HERE/scripts/build_rom.sh" 2
mkdir -p "$OUT/cmds" "${OUT}.tmp/b09"
T=${OUT}.tmp
tried=""
# sbc09's os9/level<N>/defsfile is the definitions file the commands expect: assemble from that directory
# (the common command sources live in nitros9-code/level1/cmds; level2/cmds adds the Level 2 only ones)
cd "$SBC/os9/level$LEVEL"
SRCS="$S/level1/cmds/*.asm"; [ "$LEVEL" = 2 ] && SRCS="$SRCS $S/level2/cmds/*.asm"
for f in $SRCS; do
  n=$(basename "$f" .asm)
  if timeout 30 "$A" -I "$S/defs/" "$f" -o "$T/$n" -l "$T/$n.lst" >"$T/$n.log" 2>&1 </dev/null && grep -q '^0 Pass 2 errors' "$T/$n.log" && [ -s "$T/$n" ]; then
    cp "$T/$n" "$OUT/cmds/$n"
  fi; tried="$tried $n"
done
# BASIC09 and its run-time / helper modules (the sources use upper-case directives and trailing blanks)
P=$S/3rdparty/packages/basic09
for f in "$P"/basic09*.asm; do
  sed -e 's/[ \t\r]*$//' -e 's/^\([ \t]*\)USE\b/\1use/' -e 's/^\([ \t]*\)IFP1\b/\1ifp1/' -e 's/^\([ \t]*\)ENDC\b/\1endc/' "$f" > "$T/b09/$(basename "$f")"
done
for p in basic09 runb inkey gfx; do
  src=$P/$p.asm; [ "$p" = basic09 ] && src=$T/b09/basic09.asm
  if timeout 30 "$A" -I "$S/defs/" -I "$T/b09" "$src" -o "$T/$p" -l "$T/$p.lst" >"$T/$p.log" 2>&1 </dev/null && grep -q '^0 Pass 2 errors' "$T/$p.log"; then
    cp "$T/$p" "$OUT/cmds/$p"
  fi; tried="$tried $p"
done
# the level 1 system modules are handy next to the commands (load, mdir, ...)
for m in shell dir mdir; do [ -f "$SBC/os9/level$LEVEL/$m" ] && cp "$SBC/os9/level$LEVEL/$m" "$OUT/cmds/$m" || true; done
# a name counts as built if any of its sources (Level 1 or Level 2 variant) assembled
bad=""; for n in $(printf '%s\n' $tried | sort -u); do [ -s "$OUT/cmds/$n" ] || bad="$bad $n"; done
ok=$(ls "$OUT/cmds" | wc -l)
echo "built $ok programs into $OUT/cmds"
echo "not assembled (hardware-specific or incompatible sources such as the CoCo graphics driver, SCSI and format tools; not needed here):$bad"
echo "essential programs: basic09 runb dir list copy edit procs ->" $(for n in basic09 runb dir list copy edit procs; do [ -s "$OUT/cmds/$n" ] && echo "$n:ok" || echo "$n:MISSING"; done)
