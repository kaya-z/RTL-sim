#!/usr/bin/env python3
"""Generate v09s_fixed.c from sbc09's src/v09s.c.

v09s.c is the instruction-set reference for this project, but it has a handful of defects
where it disagrees with the MC6809 data sheet (and with MAME's m6809 core).  To keep the
lock-step comparison meaningful these are corrected in a *generated copy* (the original file
is GPLv2 and stays outside this repository).  Every patch is listed here and the script
fails loudly if a pattern is not found (e.g. because upstream changed).
"""
import sys

src, dst = sys.argv[1], sys.argv[2]
s = open(src).read()

def patch(old, new, what):
    global s
    if old not in s:
        sys.exit(f"patch_v09s: pattern not found: {what}")
    s = s.replace(old, new, 1)

# 1. BLT/BGE/BGT/BLE: N is bit 3, V is bit 1 -> shift V before the XOR
patch("#define NXORV  ((ccreg&0x08)^(ccreg&0x02))",
      "#define NXORV  ((ccreg&0x08)^((ccreg&0x02)<<2))", "NXORV")

# 2. PSHU/PULU bit 6 is S (not U)
patch(" if(b&0x40)PUSHUWORD(ureg)", " if(b&0x40)PUSHUWORD(sreg)", "PSHU S")
patch(" if(b&0x40)PULLUWORD(ureg)", " if(b&0x40)PULLUWORD(sreg)", "PULU S")

# 3. SWI sets E before stacking the entire state (RTI depends on it)
patch("   PUSHBYTE(*areg)\n   PUSHBYTE(ccreg)\n   ccreg|=0xd0;\n   pcreg=GETWORD(0xfffa);",
      "   PUSHBYTE(*areg)\n   ccreg|=0x80;\n   PUSHBYTE(ccreg)\n   ccreg|=0xd0;\n   pcreg=GETWORD(0xfffa);", "SWI E flag")

# 4. DAA: correction factor from the original A, N/Z updated (data-sheet algorithm, same as MAME)
a = s.index("void daa()")
b = s.index("void orcc()")
s = s[:a] + """void daa()
{
 Word cf=0,t;
 Byte msn=*areg&0xf0, lsn=*areg&0x0f;
 da_inst("daa",NULL,2);
 if(lsn>9||(ccreg&0x20))cf|=0x06;
 if(msn>0x80&&lsn>9)cf|=0x60;
 if(msn>0x90||(ccreg&0x01))cf|=0x60;
 t=*areg+cf;
 CLV
 if(t&0x100)SEC
 *areg=t;
 SETNZ8(*areg)
}

""" + s[b:]

# 5. 16-bit accesses wrap around at $FFFF (the macros index mem[] with an int ($10000) instead)
patch("#define GETWORD(a) (mem[a]<<8|mem[(a)+1])", "#define GETWORD(a) (mem[(Word)(a)]<<8|mem[(Word)((a)+1)])", "GETWORD")
patch("#define SETWORD(a,n) {mem[a]=(n)>>8;mem[(a)+1]=n;}", "#define SETWORD(a,n) {mem[(Word)(a)]=(n)>>8;mem[(Word)((a)+1)]=n;}", "SETWORD")

open(dst, "w").write(s)
