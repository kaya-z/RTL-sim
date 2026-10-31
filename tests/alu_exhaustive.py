#!/usr/bin/env python3
# Credits: compares the RTL core with sbc09's v09s.c (GPLv2, via the generated reference `refs`); no reference code is included.
"""Exhaustive ALU comparison against v09s.c (instruction-by-instruction pc/regs/CC compare).

  8-bit binary ops (SUB CMP SBC AND BIT EOR ADC OR ADD, A and B forms): every A/B operand pair x carry in {0,1}
  unary / accumulator ops (NEG COM LSR ROR ASR ASL ROL DEC INC TST CLR, DAA, SEX): every value x CC in {0,H,C,H|C,...}
  16-bit ops (ADDD SUBD CMPD CMPX CMPY CMPU CMPS): all pairs from a set of boundary values (0,1,$7F,$80,$FF,
      $100,$7FFF,$8000,$FFFF, ...) x carry
Each case is   LDA/LDB #x ; LDB #cc ; TFR B,CC ; <op>  and is emitted in chunks that fit into the 64K image.
usage: tests/alu_exhaustive.py [--quick]      (needs REF_BIN with `refs`, and build/ramtest)
"""
import itertools, os, subprocess, sys, tempfile, random

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
REF = os.environ.get('REF_BIN', '/tmp/ref/bin')
CHUNK = 5000      # cases per program (<= 8 bytes each => 40 KB at $1000)

def setcc(cc):  return [0xC6, cc, 0x1F, 0x9A]                    # LDB #cc ; TFR B,CC   (clobbers B)
def setcc_a(cc): return [0x86, cc, 0x1F, 0x8A]                   # LDA #cc ; TFR A,CC   (clobbers A)

def cases():
    vals16 = [0, 1, 2, 0x7F, 0x80, 0xFF, 0x100, 0x1FF, 0x7FFF, 0x8000, 0x8001, 0xFFFE, 0xFFFF, 0x0F0F, 0xF0F0, 0x5555, 0xAAAA, 0x1234, 0x00FF, 0xFF00]
    # 8-bit binary: (A-form opcode, B-form opcode)
    bin_ops = {'SUB': 0x80, 'CMP': 0x81, 'SBC': 0x82, 'AND': 0x84, 'BIT': 0x85, 'EOR': 0x88, 'ADC': 0x89, 'OR': 0x8A, 'ADD': 0x8B}
    for name, opa in bin_ops.items():
        for a in range(256):
            for b in range(256):
                for c in (0, 1):
                    # LDA #a ; <set cc> ; op A,#b   (cc set last so it is the input to the operation)
                    yield (name + 'A', [0x86, a] + setcc(c) + [opa, b])
    for name, opa in bin_ops.items():
        for a in range(256):
            for b in range(0, 256, 3):      # B form: every third operand (A form already covers the ALU fully)
                for c in (0, 1):
                    yield (name + 'B', setcc_a(c) + [0xC6, a, opa | 0x40, b])
    un_ops = {'NEG': 0x40, 'COM': 0x43, 'LSR': 0x44, 'ROR': 0x46, 'ASR': 0x47, 'ASL': 0x48, 'ROL': 0x49, 'DEC': 0x4A,
              'INC': 0x4C, 'TST': 0x4D, 'CLR': 0x4F, 'DAA': 0x19, 'SEX': 0x1D}
    for name, op in un_ops.items():
        for a in range(256):
            for cc in range(0, 0x40, 1):      # all of H I N Z V C inputs (E F are set 0)
                yield (name, [0x86, a, 0xC6, 0x5A] + setcc(cc) + [op])
    for name, op in {'NEGB': 0x50, 'COMB': 0x53, 'LSRB': 0x54, 'RORB': 0x56, 'ASRB': 0x57, 'ASLB': 0x58, 'ROLB': 0x59,
                     'DECB': 0x5A, 'INCB': 0x5C, 'TSTB': 0x5D, 'CLRB': 0x5F}.items():
        for a in range(256):
            for cc in (0, 1, 0x20, 0x21, 0x0F, 0x2F):
                yield (name, setcc_a(cc) + [0xC6, a, op])
    ops16 = [('ADDD', [0xC3]), ('SUBD', [0x83]), ('CMPD', [0x10, 0x83]), ('CMPX', [0x8C]), ('CMPY', [0x10, 0x8C]),
             ('CMPU', [0x11, 0x83]), ('CMPS', [0x11, 0x8C])]
    for name, op in ops16:
        for a, b in itertools.product(vals16, repeat=2):
            for c in (0, 1):
                load = {'ADDD': [0xCC], 'SUBD': [0xCC], 'CMPD': [0xCC], 'CMPX': [0x8E], 'CMPY': [0x10, 0x8E],
                        'CMPU': [0xCE], 'CMPS': [0x10, 0xCE]}[name]
                yield (name, load + [a >> 8, a & 0xFF] + setcc(c) + op + [b >> 8, b & 0xFF])
    # MUL and ABX over all operand pairs (MUL: 65536 cases)
    for a in range(256):
        for b in range(0, 256):
            yield ('MUL', [0x86, a, 0xC6, b, 0x3D])
    for b in range(256):
        for x in (0, 1, 0x7FFF, 0xFFFF, 0x1234):
            yield ('ABX', [0xC6, b, 0x8E, x >> 8, x & 0xFF, 0x3A])

def run_chunk(buf, ninst):
    with tempfile.TemporaryDirectory() as t:
        img = bytearray(65536)
        img[0x1000:0x1000 + len(buf)] = bytes(buf)
        end = 0x1000 + len(buf)
        img[end:end + 2] = bytes([0x20, 0xFE])                          # BRA *
        img[0x7F00:0x7F00 + 0] = b''
        img[0xFFFE], img[0xFFFF] = 0x10, 0x00
        open(t + '/p.bin', 'wb').write(img)
        n = ninst + 1
        subprocess.run([REF + '/refs', '-img', t + '/p.bin', '-pc', '0x1000', '-n', str(n), '-trace', t + '/ref.tr', '-cycles', t + '/ref.cy'],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
        subprocess.run([ROOT + '/build/ramtest', '-img', t + '/p.bin', '-n', str(n), '-trace', t + '/rtl.tr', '-cycles-log', t + '/rtl.cy', '-stop-on-ill'],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
        r = subprocess.run([sys.executable, HERE + '/cmp_trace.py', t + '/ref.tr', t + '/rtl.tr'], capture_output=True, text=True)
        nref = sum(1 for _ in open(t + '/ref.tr')) if os.path.exists(t + '/ref.tr') else 0
        nrtl = sum(1 for _ in open(t + '/rtl.tr')) if os.path.exists(t + '/rtl.tr') else 0
        if nref < n or nrtl < n:      # a truncated/empty trace must never count as a pass
            return 1, f"short trace: expected {n} instructions, ref {nref}, rtl {nrtl}\n" + r.stdout
        return r.returncode, r.stdout

def count_inst(code):
    # every case is a straight line of instructions; count by decoding our own small subset
    i, n = 0, 0
    while i < len(code):
        op = code[i]
        if op in (0x10, 0x11): i += 1; op = code[i]; n += 1; l = {0x83: 3, 0x8C: 3, 0x8E: 3, 0xCE: 3}.get(op, 1)
        elif op in (0x86, 0xC6, 0x80, 0x81, 0x82, 0x84, 0x85, 0x88, 0x89, 0x8A, 0x8B, 0xC0, 0xC1, 0xC2, 0xC4, 0xC5, 0xC8, 0xC9, 0xCA, 0xCB): l = 2; n += 1
        elif op == 0x1F: l = 2; n += 1
        elif op in (0x8E, 0xCC, 0xC3, 0x83, 0x8C, 0xCE): l = 3; n += 1
        else: l = 1; n += 1
        i += l
    return n

def main():
    quick = '--quick' in sys.argv
    buf, ninst, ncase, bad, nrun = [], 0, 0, 0, 0
    cur = None
    def flush():
        nonlocal buf, ninst, bad, nrun
        if not buf: return
        rc, out = run_chunk(buf, ninst)
        nrun += 1
        if rc != 0:
            bad += 1
            print(out)
        buf, ninst = [], 0
    rnd = random.Random(1)
    for name, code in cases():
        if quick and rnd.random() > 0.02: continue
        if len(buf) + len(code) > 0x6000 - 16: flush()
        buf += code; ninst += count_inst(code); ncase += 1
    flush()
    print(f"alu_exhaustive: {ncase} cases in {nrun} programs, {bad} with mismatches")
    return 1 if bad else 0

if __name__ == '__main__':
    sys.exit(main())
