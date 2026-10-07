#!/bin/sh
# Assemble the NitrOS-9 Level 1 commands and BASIC09 from the sources in refs/sbc09 (nitros9-code)
# into build/os9/cmds, which rtlsim exposes as /v0/cmds (SysGo sets the execution directory to /v0/cmds).
# Credits: NitrOS-9 project sources (GPL), a09 assembler by the sbc09 team (GPLv2).  Nothing is committed.
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
SBC=${SBC:-$HERE/refs/sbc09}
OUT=${1:-$HERE/build/os9}
S=$SBC/os9/nitros9-code
A=$SBC/src/a09
[ -x "$A" ] || { echo "a09 not built: run scripts/setup_refs.sh first" >&2; exit 1; }
mkdir -p "$OUT/cmds" "${OUT}.tmp/b09"
T=${OUT}.tmp
ok=0; bad=""
# sbc09's os9/level1/defsfile is the definitions file the commands expect: assemble from that directory
cd "$SBC/os9/level1"
for f in "$S"/level1/cmds/*.asm; do
  n=$(basename "$f" .asm)
  if timeout 30 "$A" -I "$S/defs/" "$f" -o "$T/$n" -l "$T/$n.lst" >"$T/$n.log" 2>&1 </dev/null && grep -q '^0 Pass 2 errors' "$T/$n.log" && [ -s "$T/$n" ]; then
    cp "$T/$n" "$OUT/cmds/$n"; ok=$((ok+1))
  else bad="$bad $n"; fi
done
# BASIC09 and its run-time / helper modules (the sources use upper-case directives and trailing blanks)
P=$S/3rdparty/packages/basic09
for f in "$P"/basic09*.asm; do
  sed -e 's/[ \t\r]*$//' -e 's/^\([ \t]*\)USE\b/\1use/' -e 's/^\([ \t]*\)IFP1\b/\1ifp1/' -e 's/^\([ \t]*\)ENDC\b/\1endc/' "$f" > "$T/b09/$(basename "$f")"
done
for p in basic09 runb inkey gfx; do
  src=$P/$p.asm; [ "$p" = basic09 ] && src=$T/b09/basic09.asm
  if timeout 30 "$A" -I "$S/defs/" -I "$T/b09" "$src" -o "$T/$p" -l "$T/$p.lst" >"$T/$p.log" 2>&1 </dev/null && grep -q '^0 Pass 2 errors' "$T/$p.log"; then
    cp "$T/$p" "$OUT/cmds/$p"; ok=$((ok+1))
  else bad="$bad $p"; fi
done
# the level 1 system modules are handy next to the commands (load, mdir, ...)
for m in shell dir mdir; do [ -f "$SBC/os9/level1/$m" ] && cp "$SBC/os9/level1/$m" "$OUT/cmds/$m" || true; done
echo "built $ok programs into $OUT/cmds   (not assembled:$bad)"
