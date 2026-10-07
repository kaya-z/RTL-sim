#!/usr/bin/env python3
"""Credits: cycle tables from the Motorola MC6809E data sheet (cross-checked against MAME m6809, BSD-3-Clause, N. Woods).

Independent MC6809E cycle-count model (from the data-sheet instruction tables).

expected_cycles(image, pc, cc) -> int | None       (None = not modelled: SYNC/CWAI/RTI/SWI...)
Used by cmp_cycles.py to check the RTL core's per-instruction bus-cycle count.
"""

def cond(op, cc):
    C, V, Z, N = cc & 1, (cc >> 1) & 1, (cc >> 2) & 1, (cc >> 3) & 1
    return [True, False, not (C or Z), C or Z, not C, bool(C), not Z, bool(Z), not V, bool(V),
            not N, bool(N), N == V, N != V, (not Z) and N == V, bool(Z) or N != V][op & 15]

def idx_extra(pb):
    """extra cycles of an indexed operand over the '4+' base (data-sheet table)."""
    if not pb & 0x80:
        return 1                                   # 5-bit offset
    ind = 3 if pb & 0x10 else 0
    t = pb & 0x0F
    base = {0x0: 2, 0x1: 3, 0x2: 2, 0x3: 3, 0x4: 0, 0x5: 1, 0x6: 1, 0x8: 1, 0x9: 4, 0xB: 4, 0xC: 1, 0xD: 5}.get(t)
    if t == 0xF:
        return 5                                    # [n16]
    if base is None:
        return None
    return base + ind

def idx_len(pb):
    if not pb & 0x80: return 1
    t = pb & 0x0F
    return {0x8: 2, 0xC: 2, 0x9: 3, 0xD: 3, 0xF: 3}.get(t, 1)

def pushpull_bytes(mask):
    n = 0
    for bit, sz in ((0, 1), (1, 1), (2, 1), (3, 1), (4, 2), (5, 2), (6, 2), (7, 2)):
        if mask & (1 << bit): n += sz
    return n

def expected_cycles(img, pc, cc):
    try:
        return _expected(img, pc, cc)
    except TypeError:
        return None            # illegal indexed post-byte etc.

def _expected(img, pc, cc):
    op = img[pc]
    page = 0
    if op in (0x10, 0x11):
        page = 1
        pc += 1
        op2 = img[pc]
    else:
        op2 = op
    o = op2
    hi, lo = o >> 4, o & 15
    extra_pre = 1 if page else 0
    # --- inherent / misc ---------------------------------------------------------
    if page == 0:
        if hi == 0:
            return 3 if lo == 0xE else 6
        if hi == 2: return 3
        if hi == 3:
            if lo <= 3: return 4 + idx_extra(img[pc + 1])
            if lo in (4, 5, 6, 7): return 5 + pushpull_bytes(img[pc + 1])
            return {0x9: 5, 0xA: 3, 0xD: 11}.get(lo)
        if hi == 1:
            return {0x2: 2, 0x6: 5, 0x7: 9, 0x9: 2, 0xA: 3, 0xC: 3, 0xD: 2, 0xE: 8, 0xF: 6}.get(lo)
        if hi in (4, 5): return 2
        if hi == 6:
            e = idx_extra(img[pc + 1])
            return (3 if lo == 0xE else 6) + e
        if hi == 7:
            return 4 if lo == 0xE else 7
    else:
        if hi == 2:
            return 6 if cond(lo, cc) else 5
        if o == 0x3F: return None
    # --- 0x80..0xFF ----------------------------------------------------------------
    if o >= 0x80:
        mode = (o >> 4) & 3            # 0 imm, 1 dir, 2 idx, 3 ext
        grpB = bool(o & 0x40)
        is16 = False; st = False; special = None
        if lo == 0x3: is16 = True; kind = 'alu16'
        elif lo == 0xC:
            is16 = True; kind = 'alu16' if (page or not grpB) else 'ld16'
        elif lo == 0xE: is16 = True; kind = 'ld16'
        elif lo == 0xD:
            if not grpB and page == 0:
                if mode == 0: return 7               # BSR
                special = 'jsr'
            else:
                is16 = True; kind = 'st16'
        elif lo == 0xF: is16 = True; kind = 'st16'
        elif lo == 0x7: kind = 'st8'
        else: kind = 'alu8'
        if special == 'jsr':
            return {1: 7, 2: 7, 3: 8}[mode] + (idx_extra(img[pc + 1]) if mode == 2 else 0)
        if not is16:
            base = {0: 2, 1: 4, 2: 4, 3: 5}[mode]
        elif kind == 'alu16':
            base = {0: 4, 1: 6, 2: 6, 3: 7}[mode]
        elif kind == 'ld16':
            base = {0: 3, 1: 5, 2: 5, 3: 6}[mode]
        else:  # st16
            base = {1: 5, 2: 5, 3: 6}[mode]
        if mode == 2: base += idx_extra(img[pc + 1])
        return base + extra_pre
    return None
