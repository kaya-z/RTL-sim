#!/usr/bin/env python3
"""Random legal MC6809 program generator for lock-step comparison.

Creates a 64 KiB image:
   0x1000  main block (straight-line + forward branches, ends with LBRA start)
   0x2000  leaf subroutines (random instructions + RTS)
   0x2800  SWI/SWI2/SWI3/IRQ handlers (RTI)
   0x3000  data area (pointers into the data area)
   0x7f00  stack
Only opcodes with unambiguous behaviour are generated (no illegal opcodes,
no SYNC/CWAI).  Used with  tools/ref/refram  vs  build/ramtest.
"""
import random, sys, argparse

def emit_imm8(r): return [r.randrange(256)]
def emit_imm16(r): return [r.randrange(256), r.randrange(256)]

class Gen:
    def __init__(self, seed, allow_ind=True):
        self.r = random.Random(seed)
        self.allow_ind = allow_ind
        self.nsub = 4

    # ---- addressing ------------------------------------------------------------
    def direct(self):  return [self.r.randrange(0x00, 0x100)]          # DP=0x30 -> 0x3000..0x30ff
    def extended(self):
        a = 0x3000 + self.r.randrange(0x0800)
        return [a >> 8, a & 255]

    def indexed(self, allow_ind=None, kind='read'):
        """kind: 'read' (loads/compares), 'write' (stores / RMW), 'lea'.  Writes never use S as base
        or D,R (keeps stack and code intact so runs stay in legal-code territory)."""
        r = self.r
        if allow_ind is None: allow_ind = self.allow_ind
        reg = (r.randrange(3) if (kind != 'read') else r.randrange(4)) << 5
        ind = 0x10 if (allow_ind and r.random() < 0.08) else 0
        k = r.choice(['5', '5', 'p', 'pp', 'm', 'mm', 'z', 'b', 'a', 'd', 'o8', 'o16', 'pc8', 'pc16', 'ext'])
        if kind != 'read' and k == 'd': k = 'z'
        if kind != 'read' and k in ('pc8',): k = 'o8'
        if kind == 'write' and k in ('pc16',): k = 'o8'
        if k == '5' and not ind:
            return [reg | (r.randrange(32))]
        if k == '5': k = 'z'
        if k == 'p' and ind: k = 'pp'
        if k == 'm' and ind: k = 'mm'
        base = 0x80 | reg | ind
        if k == 'p':  return [base | 0x00]
        if k == 'pp': return [base | 0x01]
        if k == 'm':  return [base | 0x02]
        if k == 'mm': return [base | 0x03]
        if k == 'z':  return [base | 0x04]
        if k == 'b':  return [base | 0x05]
        if k == 'a':  return [base | 0x06]
        if k == 'd':  return [base | 0x0B]
        if k == 'o8': return [base | 0x08, r.randrange(256)]
        if k == 'o16':
            v = r.randrange(-0x200, 0x200) & 0xFFFF
            return [base | 0x09, v >> 8, v & 255]
        if k == 'pc8': return [base | 0x0C, r.randrange(256)]
        if k == 'pc16':
            v = r.randrange(0x1000, 0x2f00)   # relative into data-ish area
            return [base | 0x0D, v >> 8, v & 255]
        if k == 'ext':
            a = 0x3000 + r.randrange(0x0800)
            return [0x9F, a >> 8, a & 255]        # [nnnn]
        return [base | 0x04]

    def operand(self, mode, kind='read', ptr16=False):
        if mode == 'imm8': return emit_imm8(self.r)
        if mode == 'imm16':
            if ptr16:      # value loaded into a pointer register: keep it inside the data area
                v = 0x3400 + self.r.randrange(0x600)
                return [v >> 8, v & 255]
            return emit_imm16(self.r)
        if mode == 'dir': return self.direct()
        if mode == 'ext': return self.extended()
        return self.indexed(kind=kind)

    # ---- instruction table ---------------------------------------------------------
    def rmw_ops(self):
        return [0x00, 0x03, 0x04, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0C, 0x0D, 0x0F]

    def one(self, in_sub=False):
        r = self.r
        c = r.random()
        b = []
        if c < 0.12:      # RMW memory
            op = r.choice(self.rmw_ops())
            m = r.choice(['dir', 'idx', 'ext'])
            base = {'dir': 0x00, 'idx': 0x60, 'ext': 0x70}[m]
            b = [base | op] + self.operand(m, 'write')
        elif c < 0.22:    # RMW register
            op = r.choice(self.rmw_ops())
            b = [r.choice([0x40, 0x50]) | op]
        elif c < 0.55:    # 8-bit ALU / loads / stores
            grp = r.choice([0x80, 0xC0])
            m = r.choice(['imm8', 'dir', 'idx', 'ext'])
            lo = r.choice([0x0, 0x1, 0x2, 0x4, 0x5, 0x6, 0x7, 0x8, 0x9, 0xA, 0xB])
            if lo == 0x7 and m == 'imm8': lo = 0x6
            off = {'imm8': 0x00, 'dir': 0x10, 'idx': 0x20, 'ext': 0x30}[m]
            b = [grp | off | lo] + self.operand(m, 'write' if lo == 0x7 else 'read')
        elif c < 0.72:    # 16-bit ops
            m = r.choice(['imm16', 'dir', 'idx', 'ext'])
            off = {'imm16': 0x00, 'dir': 0x10, 'idx': 0x20, 'ext': 0x30}[m]
            kind = r.choice(['subd', 'addd', 'cmpx', 'cmpd', 'cmpy', 'cmpu', 'cmps', 'ldd', 'ldx', 'ldy', 'ldu', 'lds',
                             'std', 'stx', 'sty', 'stu', 'sts'])
            pre = []
            if kind == 'subd': op = 0x80 | off | 0x3
            elif kind == 'addd': op = 0xC0 | off | 0x3
            elif kind == 'cmpx': op = 0x80 | off | 0xC
            elif kind == 'cmpd': pre = [0x10]; op = 0x80 | off | 0x3
            elif kind == 'cmpy': pre = [0x10]; op = 0x80 | off | 0xC
            elif kind == 'cmpu': pre = [0x11]; op = 0x80 | off | 0x3
            elif kind == 'cmps': pre = [0x11]; op = 0x80 | off | 0xC
            elif kind == 'ldd': op = 0xC0 | off | 0xC
            elif kind == 'ldx': op = 0x80 | off | 0xE
            elif kind == 'ldy': pre = [0x10]; op = 0x80 | off | 0xE
            elif kind == 'ldu': op = 0xC0 | off | 0xE
            elif kind == 'lds': pre = [0x10]; op = 0xC0 | off | 0xE
            else:
                if m == 'imm16': m = 'dir'; off = 0x10
                if kind == 'std': op = 0xC0 | off | 0xD
                elif kind == 'stx': op = 0x80 | off | 0xF
                elif kind == 'sty': pre = [0x10]; op = 0x80 | off | 0xF
                elif kind == 'stu': op = 0xC0 | off | 0xF
                else: pre = [0x10]; op = 0xC0 | off | 0xF
            b = pre + [op] + self.operand(m, 'write' if kind.startswith('st') else 'read', ptr16=kind in ('ldx', 'ldy', 'ldu', 'lds'))
        elif c < 0.77:    # LEA
            b = [r.choice([0x30, 0x31, 0x32, 0x33])] + self.indexed(allow_ind=False, kind='lea')
        elif c < 0.82:    # inherent misc
            b = [r.choice([0x12, 0x19, 0x1D, 0x3A, 0x3D])]
        elif c < 0.86:    # ORCC / ANDCC (keep I,F set so no IRQ)
            if r.random() < 0.5: b = [0x1A, r.randrange(256) & 0xAF | 0x50]   # ORCC
            else: b = [0x1C, r.randrange(256) | 0x50]                      # ANDCC (keep I,F)
        elif c < 0.90:    # TFR / EXG (no PC)
            ptr, dat = [1, 2, 3, 4], [0, 8, 9, 10, 11]
            grp = r.choice([ptr, dat])
            a = r.choice(grp); d = r.choice(grp)
            if r.random() < 0.15: a = r.choice(ptr + dat)          # occasional 8<->16 mixing (source only)
            if d == 11: d = 10
            if {a, d} & {0} and {a, d} & {8, 9}: a, d = 1, 2       # D<->A/B overlap: exchange order is implementation defined
            b = [r.choice([0x1E, 0x1F]), (a << 4) | d]
        elif c < 0.94:    # push/pull
            if r.random() < 0.5:
                b = [r.choice([0x34, 0x36]), r.randrange(256)]   # PSHS / PSHU
            else:
                b = [r.choice([0x35, 0x37]), r.randrange(256) & 0x7F]   # PULS / PULU (no PC)
        elif c < 0.97:    # software interrupts
            b = [0x3F]   # SWI (SWI2/SWI3 are I/O traps in v09s -> not generated)
        else:
            b = [0x12]
        return b

    def build(self):
        r = self.r
        img = bytearray(0x10000)
        # data area: pointers into data area
        for a in range(0x3000, 0x4000, 2):
            v = 0x3000 + r.randrange(0x0800)
            img[a] = v >> 8; img[a + 1] = v & 255
        # vectors / handlers
        for v, h in ((0xFFFA, 0x2800), (0xFFF4, 0x2802), (0xFFF2, 0x2804), (0xFFF8, 0x2806), (0xFFF6, 0x2808),
                     (0xFFFC, 0x280a), (0xFFFE, 0x1000)):
            img[v] = h >> 8; img[v + 1] = h & 255
        for h in range(0x2800, 0x2810, 2):
            img[h] = 0x3B     # RTI
        # subroutines
        subs = []
        pos = 0x2000
        for s in range(self.nsub):
            subs.append(pos)
            for _ in range(r.randrange(2, 8)):
                while True:
                    ins = self.one(in_sub=True)
                    if ins[0] in (0x34, 0x35, 0x1E, 0x1F, 0x32, 0x3F): continue      # would disturb S
                    if ins[0] in (0x10, 0x11) and (ins[1] in (0xCE, 0xDE, 0xEE, 0xFE, 0x3F) or ins[1] in (0x21,)): continue
                    break
                img[pos:pos + len(ins)] = bytes(ins); pos += len(ins)
            img[pos] = 0x39; pos += 1
        # main block
        prog = []
        # prologue: set DP, regs
        pro = [0x86, 0x30, 0x1F, 0x8B,                    # LDA #$30 ; TFR A,DP
               0x10, 0xCE, 0x7F, 0x00,                    # LDS #$7f00
               0xCE, 0x7C, 0x00,                          # LDU #$7c00
               0x8E, 0x35, 0x00,                          # LDX #$3500
               0x10, 0x8E, 0x36, 0x00,                    # LDY #$3600
               0x86, r.randrange(256), 0xC6, r.randrange(256),
               0x1C, 0x00 | 0xFF & 0xAF | 0x50 & 0xFF]    # ANDCC #$FF (no change)
        pro = pro[:-2] + [0x1C, 0xFF]
        N = 200
        items = []
        for i in range(N):
            k = r.random()
            if k < 0.08:
                items.append(('bra', r.choice(list(range(0x20, 0x30)))))
            elif k < 0.10:
                items.append(('lbra', r.choice(list(range(0x21, 0x30)))))
            elif k < 0.13:
                items.append(('bsr', None))
            elif k < 0.15:
                items.append(('jsr', None))
            elif k < 0.16:
                items.append(('jmp', None))
            else:
                items.append(('raw', self.one()))
        # layout pass: sizes
        def size(it):
            t = it[0]
            if t == 'raw': return len(it[1])
            if t == 'bra': return 2
            if t == 'lbra': return 4
            if t == 'bsr': return 2
            if t == 'jsr': return 3
            if t == 'jmp': return 3
            return 0
        addr = 0x1000 + len(pro)
        starts = []
        for it in items:
            starts.append(addr); addr += size(it)
        end = addr
        code = bytearray(pro)
        for i, it in enumerate(items):
            t = it[0]
            a = starts[i]
            if t == 'raw':
                code += bytes(it[1])
            elif t in ('bra', 'lbra', 'jmp'):
                cand = [s for s in starts[i + 1:i + 6]] + [end]
                tgt = r.choice(cand)
                if t == 'bra':
                    off = tgt - (a + 2)
                    if off > 127: off = 0
                    code += bytes([it[1], off & 255])
                elif t == 'lbra':
                    off = tgt - (a + 4)
                    code += bytes([0x10, it[1], (off >> 8) & 255, off & 255])
                else:
                    code += bytes([0x7E, tgt >> 8, tgt & 255])
            elif t == 'bsr':
                tgt = subs[r.randrange(len(subs))]
                off = tgt - (a + 2)
                # BSR range is -128..127: use JSR extended if out of range
                if -128 <= off <= 127:
                    code += bytes([0x8D, off & 255])
                else:
                    code += bytes([0x12, 0x12])          # keep size 2 (NOP NOP)
            elif t == 'jsr':
                tgt = subs[r.randrange(len(subs))]
                code += bytes([0xBD, tgt >> 8, tgt & 255])
        # loop back
        back = 0x1000 - (0x1000 + len(code) + 3)
        code += bytes([0x7E, 0x10, 0x00])
        img[0x1000:0x1000 + len(code)] = code
        return bytes(img)

if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('seed', type=int)
    ap.add_argument('out')
    ap.add_argument('--no-indirect', action='store_true')
    a = ap.parse_args()
    open(a.out, 'wb').write(Gen(a.seed, not a.no_indirect).build())
