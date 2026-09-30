// mc6809_ucode.cpp - micro-ROM generator for the MC6809 core
//
// Every micro-op is one bus cycle.  The cycle sequences below are the
// MC6809E data-sheet cycle tables, expressed in the same shape as MAME's
// m6809.lst / base6x09.lst ("@dummy_vma(n)", "@dummy_read_opcode_arg(0)" ...).
#include "mc6809.h"

namespace mc6809 {

struct Builder {
  MicroRom& m;
  std::vector<UOp>& r;
  explicit Builder(MicroRom& mr) : m(mr), r(mr.rom) {}

  int here() const { return static_cast<int>(r.size()); }

  // ---- primitive emitters ---------------------------------------------------
  UOp& add(Kind k, ASel a) {
    r.emplace_back();
    UOp& u = r.back();
    u.kind = k; u.asel = a;
    return u;
  }
  // read @PC, PC++   (opcode / operand fetch)
  UOp& rdpc(Act a0 = ACT_NOP, Act a1 = ACT_NOP) {
    UOp& u = add(K_RD, A_PC); u.pcinc = true; u.act[0] = a0; u.act[1] = a1; return u;
  }
  // dummy read @PC (no increment)           == dummy_read_opcode_arg(0)
  UOp& dpc0(Act a0 = ACT_NOP) { UOp& u = add(K_RD, A_PC);  u.act[0] = a0; return u; }
  // dummy read @PC+1                         == dummy_read_opcode_arg(1)
  UOp& dpc1() { return add(K_RD, A_PC1); }
  // n bus cycles with VMA=0                  == dummy_vma(n)
  UOp& idle(int n = 1, Act a0 = ACT_NOP) {
    UOp* last = nullptr;
    for (int i = 0; i < n; ++i) { last = &add(K_IDLE, A_FFFF); if (i == 0) last->act[0] = a0; }
    return *last;
  }
  UOp& rdea(Act a0, bool second = false) {
    UOp& u = add(K_RD, second ? A_EA1 : A_EA); u.act[0] = a0; return u;
  }
  UOp& wrea(WSel w, bool second = false, Act a0 = ACT_NOP) {
    UOp& u = add(K_WR, second ? A_EA1 : A_EA); u.wsel = w; u.act[0] = a0; return u;
  }
  UOp& none(Act a0 = ACT_NOP, Act a1 = ACT_NOP) {
    UOp& u = add(K_NONE, A_PC); u.act[0] = a0; u.act[1] = a1; return u;
  }
  UOp& push1(WSel w, uint8_t reg, Act a1 = ACT_NOP) {   // --SP ; write
    UOp& u = add(K_WR, A_SP_DEC); u.wsel = w; u.reg = reg; u.act[0] = ACT_SP_DEC; u.act[1] = a1; return u;
  }
  UOp& push_loop(uint8_t reg) {
    UOp& u = add(K_WR, A_SP_DEC); u.wsel = W_STK; u.reg = reg;
    u.guard = G_PUSH; u.next = NX_SELF; u.act[0] = ACT_PUSH_STEP; return u;
  }
  UOp& pull_loop(uint8_t reg) {
    UOp& u = add(K_RD, A_SP); u.reg = reg;
    u.guard = G_PULL; u.next = NX_SELF; u.act[0] = ACT_PULL_STEP; return u;
  }
  UOp& rdstk(uint8_t reg) { UOp& u = add(K_RD, A_SP); u.reg = reg; return u; }   // dummy stack read
  void end() { r.back().next = NX_END; }
  void jmp(int t) { r.back().next = NX_JMP; r.back().tgt = static_cast<uint16_t>(t); }
  void ret() { r.back().next = NX_RET; }

  // ---- common routines --------------------------------------------------------
  void build_common() {
    // [0] opcode fetch, [1] second opcode fetch after a $10/$11 prefix
    m.r_fetch = here();
    { UOp& u = rdpc(ACT_IR); u.next = NX_DISPATCH; }
    m.r_fetch2 = here();
    { UOp& u = rdpc(ACT_IR); u.next = NX_DISPATCH; }

    // illegal opcode: fetch only (MAME: log_illegal(); return;)
    m.r_illegal = here();
    none(); end();

    // INTERRUPT_VECTOR:  dummy_vma(1); pc.h=[vec]; pc.l=[vec+1]; dummy_vma(1)
    m.r_vec = here();
    idle(1);
    { UOp& u = add(K_RD, A_VEC);  u.act[0] = ACT_VEC_H; u.bsba = 2; }
    { UOp& u = add(K_RD, A_VEC1); u.act[0] = ACT_VEC_L; u.bsba = 2; }
    idle(1); end();

    // NMI / FIRQ / IRQ entry.  (fn = 0/1/2)
    for (int k = 0; k < 3; ++k) {
      int st = here();
      dpc0(); dpc0();
      { UOp& u = idle(1); u.act[0] = ACT_INT_ENT; u.fn = k; }
      push_loop(R_S);
      none(ACT_INT_FIN); r.back().fn = k; jmp(m.r_vec);
      (k == 0 ? m.r_nmi : k == 1 ? m.r_firq : m.r_irq) = st;
    }
    // RESET : hold while asserted, then fetch vector $FFFE
    m.r_reset = here();
    { UOp& u = idle(1); u.guard = G_WAIT_RESET; u.next = NX_SELF; }
    none(ACT_RESET_ENT); jmp(m.r_vec);
    // HALT : BA=BS=1 while halted
    m.r_halt = here();
    { UOp& u = idle(1); u.guard = G_WAIT_HALT; u.next = NX_SELF; u.bsba = 3; }
    none(); end();

    build_indexed();
  }

  // ---- indexed addressing ------------------------------------------------------
  int idx_pb = 0;        // the (shared) post-byte fetch micro-op

  void idx_tail(bool ind) {
    if (ind) {
      none(ACT_EA_FROM_TMP);
      rdea(ACT_TMP_H);
      rdea(ACT_TMP_L, true);
      idle(1);
    }
    none(ACT_EA_FROM_TMP); ret();
  }

  void build_indexed() {
    idx_pb = here();
    { UOp& u = rdpc(ACT_IDX_PB); u.next = NX_IDX; }

    // 5-bit offset: post-byte, dummy_read_opcode_arg(0), dummy_vma(1)
    m.idx_route5 = here();
    dpc0(); idle(1); idx_tail(false);

    for (int ind = 0; ind < 2; ++ind) {
      for (int cls = 0; cls < 16; ++cls) {
        m.idx_route[ind][cls] = here();
        switch (cls) {
          case 0: dpc0(); idle(2); break;                       // ,R+
          case 1: dpc0(); idle(3); break;                       // ,R++
          case 2: dpc0(); idle(2); break;                       // ,-R
          case 3: dpc0(); idle(3); break;                       // ,--R
          case 4: dpc0(); break;                                // ,R
          case 5: dpc0(); idle(1); break;                       // B,R
          case 6: dpc0(); idle(1); break;                       // A,R
          case 8: rdpc(ACT_IDX_OFF8); dpc0(); break;            // n8,R
          case 9: rdpc(ACT_TMP_H); rdpc(ACT_IDX_OFF16); idle(3); break;   // n16,R
          case 11: dpc0(); dpc1(); idle(3); break;              // D,R
          case 12: rdpc(ACT_IDX_PCREL8); idle(1); break;        // n8,PC
          case 13: rdpc(ACT_TMP_H); rdpc(ACT_IDX_PCREL16); idle(4); break;  // n16,PC
          case 15: rdpc(ACT_TMP_H); rdpc(ACT_TMP_L); idle(1); break;        // [n16]
          default: break;                                       // illegal: no cycles, tmp=0
        }
        idx_tail(ind != 0);
      }
    }
  }

  // ---- per-opcode sequences ---------------------------------------------------
  enum Mode { M_INH, M_IMM, M_DIR, M_IDX, M_EXT, M_A, M_B };

  Entry* e = nullptr;

  // open a sequence for one opcode and emit its addressing-mode prefix
  void begin(Entry& en, Mode md) {
    e = &en; en.valid = true; en.ret = -1;
    switch (md) {
      case M_DIR: en.start = here(); rdpc(ACT_EA_DIR); idle(1); break;
      case M_EXT: en.start = here(); rdpc(ACT_EA_H); rdpc(ACT_EA_L); idle(1); break;
      case M_IDX: en.start = idx_pb; en.ret = here(); break;
      default:    en.start = here(); break;
    }
  }
  void illegal(Entry& en) { en.valid = true; en.start = m.r_illegal; en.ret = -1; }

  // operand fetch cycles (mode dependent)
  UOp& opnd(Mode md, Act a0, bool second = false) {
    if (md == M_IMM) return rdpc(a0);
    return rdea(a0, second);
  }

  // ---- op groups ----------------------------------------------------------------
  void op_alu8(Entry& en, Mode md, uint8_t fn, uint8_t reg) {
    begin(en, md);
    UOp& u = opnd(md, ACT_ALU8); u.fn = fn; u.reg = reg; end();
  }
  void op_st8(Entry& en, Mode md, uint8_t reg) {
    begin(en, md);
    UOp& u = wrea(W_REG8, false, ACT_ST8); u.reg = reg; end();
  }
  void op_ld16(Entry& en, Mode md, uint8_t reg) {
    begin(en, md);
    { UOp& u = opnd(md, ACT_LD16H); u.reg = reg; }
    { UOp& u = opnd(md, ACT_LD16L, true); u.reg = reg; }
    end();
  }
  void op_alu16(Entry& en, Mode md, uint8_t fn, uint8_t reg) {
    begin(en, md);
    opnd(md, ACT_TMP_H);
    { UOp& u = opnd(md, ACT_ALU16, true); u.fn = fn; u.reg = reg; }
    dpc0(); end();
  }
  void op_st16(Entry& en, Mode md, uint8_t reg) {
    begin(en, md);
    { UOp& u = wrea(W_REG16H, false, ACT_ST16); u.reg = reg; }
    { UOp& u = wrea(W_REG16L, true); u.reg = reg; }
    end();
  }
  void op_rmw(Entry& en, Mode md, uint8_t fn) {
    if (md == M_A || md == M_B) {
      begin(en, M_INH);
      uint8_t reg = (md == M_A) ? R_A : R_B;
      if (fn == F_TST) idle(1, ACT_RMW_REG);
      else dpc0(ACT_RMW_REG);
      r.back().fn = fn; r.back().reg = reg; end();
      return;
    }
    begin(en, md);
    { UOp& u = rdea(ACT_RMW_RD); u.fn = fn; }
    if (fn == F_TST) { idle(2); }
    else { dpc0(); wrea(W_TMPL); }
    end();
  }
  void op_jmp(Entry& en, Mode md) { begin(en, md); none(ACT_JMP); end(); }
  void op_jsr(Entry& en, Mode md) {
    begin(en, md);
    dpc0(); idle(1);
    push1(W_PCL, R_S);
    push1(W_PCH, R_S, ACT_JMP);
    end();
  }
  void op_lea(Entry& en, uint8_t reg) {
    begin(en, M_IDX);
    { UOp& u = idle(1, ACT_LEA); u.reg = reg; }
    end();
  }
  void op_inh(Entry& en, Act a, int extra_idle = 0, uint8_t fn = 0) {   // fetch + dummy read (+idle)
    begin(en, M_INH);
    { UOp& u = dpc0(a); u.fn = fn; }
    if (extra_idle) idle(extra_idle);
    end();
  }
  void op_branch(Entry& en, uint8_t cond) {
    begin(en, M_INH);
    rdpc(ACT_TMP_L);
    { UOp& u = idle(1, ACT_BRANCH); u.fn = cond; }
    end();
  }
  void op_lbranch(Entry& en, uint8_t cond) {
    begin(en, M_INH);
    rdpc(ACT_TMP_H);
    { UOp& u = rdpc(ACT_TMP_L, ACT_LB_COND); u.fn = cond; }
    idle(1, ACT_LB_ADD);
    { UOp& u = idle(1); u.guard = G_TAKEN; }
    end();
  }
  void op_bsr(Entry& en) {
    begin(en, M_INH);
    rdpc(ACT_TMP_L);
    idle(3, ACT_BSR_EA);
    push1(W_PCL, R_S);
    push1(W_PCH, R_S, ACT_JMP);
    end();
  }
  void op_lbsr(Entry& en) {
    begin(en, M_INH);
    rdpc(ACT_TMP_H); rdpc(ACT_TMP_L);
    idle(4, ACT_LBSR_EA);
    push1(W_PCL, R_S);
    push1(W_PCH, R_S, ACT_JMP);
    end();
  }
  void op_push(Entry& en, uint8_t reg) {          // PSHS / PSHU
    begin(en, M_INH);
    rdpc(ACT_SET_MASK); idle(2); rdstk(reg);
    push_loop(reg); none(); end();
  }
  void op_pull(Entry& en, uint8_t reg) {          // PULS / PULU
    begin(en, M_INH);
    rdpc(ACT_SET_MASK); idle(2);
    pull_loop(reg); rdstk(reg); end();
  }
  void op_rts(Entry& en) {
    begin(en, M_INH);
    dpc0(ACT_SET_MASK_PC);
    pull_loop(R_S); rdstk(R_S); end();
  }
  void op_rti(Entry& en) {
    begin(en, M_INH);
    dpc0();
    { UOp& u = add(K_RD, A_SP); u.reg = R_S; u.act[0] = ACT_RTI_CC; }
    pull_loop(R_S); rdstk(R_S); end();
  }
  void op_swi(Entry& en, uint8_t which) {
    begin(en, M_INH);
    dpc0();
    { UOp& u = idle(1, ACT_SWI_ENT); u.fn = which; }
    push_loop(R_S);
    none(ACT_SWI_FIN); r.back().fn = which; jmp(m.r_vec);
  }
  void op_cwai(Entry& en) {
    begin(en, M_INH);
    rdpc(ACT_CWAI_CC);
    dpc0();
    idle(1, ACT_CWAI_ENT);
    push_loop(R_S);
    { UOp& u = idle(1); u.guard = G_WAIT_CWAI; u.next = NX_SELF; u.bsba = 1; }
    none(ACT_CWAI_TAKE); jmp(m.r_vec);
  }
  void op_sync(Entry& en) {
    begin(en, M_INH);
    dpc0();
    { UOp& u = idle(1); u.guard = G_WAIT_SYNC; u.next = NX_SELF; u.bsba = 1; }
    idle(1); end();
  }
  void op_tfr(Entry& en, bool exg) {
    begin(en, M_INH);
    rdpc(exg ? ACT_EXG : ACT_TFR);
    idle(exg ? 6 : 4); end();
  }
  void op_orcc(Entry& en, Act a, int extra = 0) {
    begin(en, M_INH);
    if (extra) idle(extra);
    rdpc(a); dpc0(); end();
  }

  // ---- decode -------------------------------------------------------------------
  static Mode mode_of(int opc) {
    switch ((opc >> 4) & 3) { case 0: return M_IMM; case 1: return M_DIR; case 2: return M_IDX; default: return M_EXT; }
  }

  void build_page0(int opc) {
    Entry& en = m.entry[0][opc];
    static const uint8_t rmw_fn[16] = { F_NEG, F_NEG, F_XNC, F_COM, F_LSR, F_LSR, F_ROR, F_ASR,
                                        F_ASL, F_ROL, F_DEC, F_XDEC, F_INC, F_TST, 0xFF, F_CLR };
    const int hi = opc >> 4, lo = opc & 15;
    switch (hi) {
      case 0x0:
        if (lo == 0xE) op_jmp(en, M_DIR); else op_rmw(en, M_DIR, rmw_fn[lo]);
        return;
      case 0x1:
        switch (lo) {
          case 0x2: case 0xB: op_inh(en, ACT_NOP); return;             // NOP (0x1B alias)
          case 0x3: op_sync(en); return;
          case 0x6: op_lbranch(en, 0); return;                         // LBRA
          case 0x7: op_lbsr(en); return;
          case 0x9: op_inh(en, ACT_DAA); return;
          case 0xA: op_orcc(en, ACT_ORCC); return;
          case 0xC: op_orcc(en, ACT_ANDCC); return;
          case 0xD: op_inh(en, ACT_SEX); return;
          case 0xE: op_tfr(en, true); return;
          case 0xF: op_tfr(en, false); return;
          default: return;                                             // illegal / prefixes
        }
      case 0x2: op_branch(en, static_cast<uint8_t>(lo)); return;
      case 0x3:
        switch (lo) {
          case 0x0: op_lea(en, R_X); return;
          case 0x1: op_lea(en, R_Y); return;
          case 0x2: op_lea(en, R_S); return;
          case 0x3: op_lea(en, R_U); return;
          case 0x4: op_push(en, R_S); return;
          case 0x5: op_pull(en, R_S); return;
          case 0x6: op_push(en, R_U); return;
          case 0x7: op_pull(en, R_U); return;
          case 0x8: op_orcc(en, ACT_ANDCC, 1); return;                 // undocumented ANDCC (+1 cycle)
          case 0x9: op_rts(en); return;
          case 0xA: op_inh(en, ACT_ABX, 1); return;
          case 0xB: op_rti(en); return;
          case 0xC: op_cwai(en); return;
          case 0xD: op_inh(en, ACT_MUL, 9); return;
          case 0xF: op_swi(en, 0); return;
          default: return;
        }
      case 0x4: case 0x5: {
        Mode md = (hi == 4) ? M_A : M_B;
        if (lo == 0xE) return;                                          // undocumented XCLR: treated as illegal
        op_rmw(en, md, rmw_fn[lo]);
        return;
      }
      case 0x6: if (lo == 0xE) op_jmp(en, M_IDX); else op_rmw(en, M_IDX, rmw_fn[lo]); return;
      case 0x7: if (lo == 0xE) op_jmp(en, M_EXT); else op_rmw(en, M_EXT, rmw_fn[lo]); return;
      default: break;
    }
    // 0x80..0xFF
    const bool grpB = (opc & 0x40) != 0;
    const Mode md = mode_of(opc);
    const uint8_t r8 = grpB ? R_B : R_A;
    switch (lo) {
      case 0x0: op_alu8(en, md, F_SUB, r8); return;
      case 0x1: op_alu8(en, md, F_CMP, r8); return;
      case 0x2: op_alu8(en, md, F_SBC, r8); return;
      case 0x3: op_alu16(en, md, grpB ? F_ADD : F_SUB, R_D); return;
      case 0x4: op_alu8(en, md, F_AND, r8); return;
      case 0x5: op_alu8(en, md, F_BIT, r8); return;
      case 0x6: op_alu8(en, md, F_LD, r8); return;
      case 0x7: if (md != M_IMM) op_st8(en, md, r8); return;
      case 0x8: op_alu8(en, md, F_EOR, r8); return;
      case 0x9: op_alu8(en, md, F_ADC, r8); return;
      case 0xA: op_alu8(en, md, F_OR, r8); return;
      case 0xB: op_alu8(en, md, F_ADD, r8); return;
      case 0xC: if (!grpB) op_alu16(en, md, F_CMP, R_X); else op_ld16(en, md, R_D); return;
      case 0xD:
        if (!grpB) { if (md == M_IMM) op_bsr(en); else op_jsr(en, md); }
        else if (md != M_IMM) op_st16(en, md, R_D);
        return;
      case 0xE: op_ld16(en, md, grpB ? R_U : R_X); return;
      case 0xF: if (md != M_IMM) op_st16(en, md, grpB ? R_U : R_X); return;
    }
  }

  void build_page1(int opc) {                      // $10 prefix
    Entry& en = m.entry[1][opc];
    const int hi = opc >> 4, lo = opc & 15;
    if (hi == 2) { op_lbranch(en, static_cast<uint8_t>(lo)); return; }   // $1020 = LBRA (cond 0 = always)
    if (opc == 0x3F) { op_swi(en, 1); return; }
    if (opc < 0x80) return;
    const Mode md = mode_of(opc);
    switch (lo) {
      case 0x3: if (!(opc & 0x40)) op_alu16(en, md, F_CMP, R_D); return;
      case 0xC: if (!(opc & 0x40)) op_alu16(en, md, F_CMP, R_Y); return;
      case 0xE: if (!(opc & 0x40)) op_ld16(en, md, R_Y); else op_ld16(en, md, R_S); return;
      case 0xF: if (md == M_IMM) return; if (!(opc & 0x40)) op_st16(en, md, R_Y); else op_st16(en, md, R_S); return;
      default: return;
    }
  }

  void build_page2(int opc) {                      // $11 prefix
    Entry& en = m.entry[2][opc];
    if (opc == 0x3F) { op_swi(en, 2); return; }
    if (opc < 0x80 || (opc & 0x40)) return;
    const Mode md = mode_of(opc);
    switch (opc & 15) {
      case 0x3: op_alu16(en, md, F_CMP, R_U); return;
      case 0xC: op_alu16(en, md, F_CMP, R_S); return;
      default: return;
    }
  }

  void build() {
    build_common();
    for (int o = 0; o < 256; ++o) build_page0(o);
    for (int o = 0; o < 256; ++o) build_page1(o);
    for (int o = 0; o < 256; ++o) build_page2(o);
    // Undocumented aliases that MAME implements (and sbc09 treats as NOP): decoded, but flagged.
    static const uint8_t undoc0[] = {0x01, 0x02, 0x05, 0x0B, 0x1B, 0x38, 0x41, 0x42, 0x45, 0x4B, 0x51, 0x52, 0x55,
                                     0x5B, 0x61, 0x62, 0x65, 0x6B, 0x71, 0x72, 0x75, 0x7B};
    for (uint8_t o : undoc0) m.entry[0][o].undoc = true;
    m.entry[1][0x20].undoc = true;                                   // $1020 LBRA alias
  }
};

MicroRom::MicroRom() {
  for (auto& p : idx_route) for (auto& v : p) v = 0;
  Builder b(*this);
  b.build();
}

const MicroRom& MicroRom::get() {
  static MicroRom inst;
  return inst;
}

}  // namespace mc6809
