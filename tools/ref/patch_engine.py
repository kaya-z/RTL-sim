#!/usr/bin/env python3
# Credits: operates on sbc09 (L.C. Benschop and the sbc09 team, GPLv2; S. Kono's fork github.com/shinji-kono/sbc09).
# The sources are patched into generated copies at build time; nothing from sbc09 is stored in this repository.
"""Generate engine_fixed.c from sbc09's src/engine.c (the interpreter that runs OS-9 in v09).

engine.c is used only to cross-check the OS-9 boot.  Its treatment of the *undefined* half-carry
flag is inconsistent between instruction forms; align it with v09s.c (the instruction reference):
  ASL memory forms : H = half carry of v+v   (register forms already do this)
  LSR              : H unchanged
  ADD/ADC A,B      : H is computed (engine.c never updates it)
  ASRB H, TST V, DAA, EXG : corrected to the data-sheet / v09s behaviour
  CWAI             : stacked CC is masked/E-flagged first, wait is driven by the harness, I set on entry
Everything else is untouched; any other difference shows up in the lock-step run.
"""
import re, sys
src, dst = sys.argv[1], sys.argv[2]
s = open(src).read()

n = 0
# ASL direct / indexed / extended: SETSTATUS -> SETSTATUSH
for op in ("direct", "indexed", "ext"):
    pat = re.compile(r"(/\*ASL %s\*/.*?)SETSTATUS\(" % op, re.S)
    s, k = pat.subn(lambda m: m.group(1) + "SETSTATUSH(", s, count=1)
    n += k
if n != 3:
    sys.exit("patch_engine: ASL patterns not found (%d)" % n)
# ADCA/ADCB/ADDA/ADDB (12+4 forms): the half carry is never updated in engine.c
pat = re.compile(r"(/\*AD[CD][AB] [^*]*\*/.*?)SETSTATUS\(", re.S)
s, k = pat.subn(lambda m: m.group(1) + "SETSTATUSH(", s)
if k != 16:
    sys.exit("patch_engine: expected 16 ADD/ADC forms, patched %d" % k)
# LSR: drop the H update
s, k = re.subn(r"if\(tb&0x10\)SEH else CLH tb>>=1;(SETNZ8\(tb\))", r"tb>>=1;\1", s)
# ASRB: H from operand bit 4 (as ASRA and v09s)
old = "/*ASRB*/  tb=ibreg;if(tb&0x01)SEC else CLC\n                             tb>>=1;"
if old not in s: sys.exit("patch_engine: ASRB not found")
s = s.replace(old, "/*ASRB*/  tb=ibreg;if(tb&0x01)SEC else CLC\n                             if(tb&0x10)SEH else CLH tb>>=1;")
# TST: V is cleared (memory and register forms)
for old in ("/*TST direct*/ DIRECT tb=mem(eaddr);SETNZ8(tb)", "/*TSTA*/  SETNZ8(iareg)", "/*TSTB*/  SETNZ8(ibreg)",
            "/*TST indexed*/  tb=mem(eaddr);SETNZ8(tb)", "/*TST ext*/ EXTENDED tb=mem(eaddr);SETNZ8(tb)"):
    if old not in s: sys.exit("patch_engine: TST form not found: " + old)
    s = s.replace(old, old + " CLV")
# DAA: data-sheet algorithm (correction from the original A, N/Z updated)
a = s.index("case 0x19: /* DAA*/")
b = s.index("case 0x1A: /* ORCC*/")
s = s[:a] + """case 0x19: /* DAA*/ { Word cf=0; Byte msn=iareg&0xf0, lsn=iareg&0x0f;
			if(lsn>9||(iccreg&0x20))cf|=0x06;
			if(msn>0x80&&lsn>9)cf|=0x60;
			if(msn>0x90||(iccreg&0x01))cf|=0x60;
			tw=iareg+cf; CLV if(tw&0x100)SEC iareg=tw; SETNZ8(iareg) } break;
   """ + s[b:]
# EXG: MAME/v09s write order (low nibble first) and 0xff00|cc for CC/DP when the 16 bit register comes first
old = "SETREG(t2,tb>>4) SETREG(tw,tb&15)"
if old not in s: sys.exit("patch_engine: EXG not found")
s = s.replace("GETREG(tw,tb>>4) GETREG(t2,tb&15)\n                        " + old,
              "GETREG(tw,tb>>4) GETREG(t2,tb&15)\n"
              "                        if(!(tb&0x80)) { if((tb>>4)==10||(tb>>4)==11) tw=0xff00|(tw&0xff); if((tb&15)==10||(tb&15)==11) t2=0xff00|(t2&0xff); }\n"
              "                        SETREG(tw,tb&15) SETREG(t2,tb>>4)")
# CWAI: mask CC *before* stacking the entire state (E set), wait for the wake-up event scheduled by the
# RTL run (cwai_wait() in the harness), then take the vector with I (and F for FIRQ) set.
a = s.index("case 0x3C: /* CWAI*/")
b = s.index("case 0x3D: /* MUL*/")
s = s[:a] + """case 0x3C: /* CWAI*/ IMMBYTE(tb)
			 iccreg&=tb; iccreg|=0x80;
			 PUSHWORD(ipcreg)
			 PUSHWORD(iureg)
			 PUSHWORD(iyreg)
			 PUSHWORD(ixreg)
			 PUSHBYTE(idpreg)
			 PUSHBYTE(ibreg)
			 PUSHBYTE(iareg)
			 PUSHBYTE(iccreg)
			 cwai_wait();
			 if(irq==1){ ipcreg=GETWORD(0xfff8); iccreg|=0x10; }
			 else { ipcreg=GETWORD(0xfff6); iccreg|=0x50; }
			 irq=0;
			 if(!tracing)attention=0;
			 break;
   """ + s[b:]
s = "extern void cwai_wait(void);   /* harness hook: block until the scheduled wake-up */\n" + s
open(dst, "w").write(s)
