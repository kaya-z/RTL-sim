/* Credits / references:
 *   * MAME m6809 core (src/devices/cpu/m6809/m6809.cpp, m6809.lst, base6x09.lst) -
 *     (c) Nathan Woods, BSD-3-Clause.  The per-instruction bus-cycle sequences, the interrupt/CWAI/SYNC
 *     sequencing, TFR/EXG register semantics, the DAA algorithm and the "NMI armed after LDS" rule were
 *     taken from it as a *specification*; this file is an independent re-implementation (no code copied).
 *   * sbc09 v09s.c / engine.c - (c) 1994 L.C. Benschop and the sbc09 team, GPLv2 (S. Kono's fork
 *     github.com/shinji-kono/sbc09): instruction semantics and the flag behaviour of undefined cases
 *     (H after ASL/ASR) follow v09s.c.  Used as reference only; not included here.
 *   * Motorola MC6809/MC6809E data sheet and Programming Manual: instruction set, cycle tables. */

/* mc6809_ucode.c - micro-ROM generator for the MC6809 core
 *
 * Every micro-op is one bus cycle.  The cycle sequences below are the
 * MC6809E data-sheet cycle tables, expressed in the same shape as MAME's
 * m6809.lst / base6x09.lst ("@dummy_vma(n)", "@dummy_read_opcode_arg(0)" ...). */
#include "mc6809.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#define ROM_CAP 4096          /* fixed: pointers returned by the emitters stay valid */

typedef struct { McRom *m; McUop *r; int idx_pb; } B;
enum { M_INH, M_IMM, M_DIR, M_IDX, M_EXT, M_A, M_B };

static int here(B *b) { return (int)b->m->n; }
static McUop *last(B *b) { return &b->r[b->m->n - 1]; }

static McUop *add(B *b, uint8_t k, uint8_t a) {
  assert(b->m->n < ROM_CAP);
  McUop *u = &b->r[b->m->n++];
  memset(u, 0, sizeof *u);
  u->kind = k; u->asel = a;
  return u;
}
/* read @PC, PC++   (opcode / operand fetch) */
static McUop *rdpc(B *b, uint8_t a0, uint8_t a1) {
  McUop *u = add(b, K_RD, A_PC); u->pcinc = true; u->act[0] = a0; u->act[1] = a1; return u;
}
/* dummy read @PC (no increment)           == dummy_read_opcode_arg(0) */
static McUop *dpc0(B *b, uint8_t a0) { McUop *u = add(b, K_RD, A_PC); u->act[0] = a0; return u; }
/* dummy read @PC+1                         == dummy_read_opcode_arg(1) */
static McUop *dpc1(B *b) { return add(b, K_RD, A_PC1); }
/* n bus cycles with VMA=0                  == dummy_vma(n) */
static McUop *idle(B *b, int n, uint8_t a0) {
  McUop *l = NULL;
  for (int i = 0; i < n; ++i) { l = add(b, K_IDLE, A_FFFF); if (i == 0) l->act[0] = a0; }
  return l;
}
static McUop *rdea(B *b, uint8_t a0, bool second) {
  McUop *u = add(b, K_RD, second ? A_EA1 : A_EA); u->act[0] = a0; return u;
}
static McUop *wrea(B *b, uint8_t w, bool second, uint8_t a0) {
  McUop *u = add(b, K_WR, second ? A_EA1 : A_EA); u->wsel = w; u->act[0] = a0; return u;
}
static McUop *none(B *b, uint8_t a0, uint8_t a1) {
  McUop *u = add(b, K_NONE, A_PC); u->act[0] = a0; u->act[1] = a1; return u;
}
static McUop *push1(B *b, uint8_t w, uint8_t reg, uint8_t a1) {   /* --SP ; write */
  McUop *u = add(b, K_WR, A_SP_DEC); u->wsel = w; u->reg = reg; u->act[0] = ACT_SP_DEC; u->act[1] = a1; return u;
}
static McUop *push_loop(B *b, uint8_t reg) {
  McUop *u = add(b, K_WR, A_SP_DEC); u->wsel = W_STK; u->reg = reg;
  u->guard = G_PUSH; u->next = NX_SELF; u->act[0] = ACT_PUSH_STEP; return u;
}
static McUop *pull_loop(B *b, uint8_t reg) {
  McUop *u = add(b, K_RD, A_SP); u->reg = reg;
  u->guard = G_PULL; u->next = NX_SELF; u->act[0] = ACT_PULL_STEP; return u;
}
static McUop *rdstk(B *b, uint8_t reg) { McUop *u = add(b, K_RD, A_SP); u->reg = reg; return u; }   /* dummy stack read */
static void end(B *b) { last(b)->next = NX_END; }
static void jmp(B *b, int t) { last(b)->next = NX_JMP; last(b)->tgt = (uint16_t)t; }
static void ret(B *b) { last(b)->next = NX_RET; }

/* ---- indexed addressing ------------------------------------------------------ */
static void idx_tail(B *b, bool ind) {
  if (ind) {
    none(b, ACT_EA_FROM_TMP, ACT_NOP);
    rdea(b, ACT_TMP_H, false);
    rdea(b, ACT_TMP_L, true);
    idle(b, 1, ACT_NOP);
  }
  none(b, ACT_EA_FROM_TMP, ACT_NOP); ret(b);
}

static void build_indexed(B *b) {
  McRom *m = b->m;
  b->idx_pb = here(b);
  { McUop *u = rdpc(b, ACT_IDX_PB, ACT_NOP); u->next = NX_IDX; }

  /* 5-bit offset: post-byte, dummy_read_opcode_arg(0), dummy_vma(1) */
  m->idx_route5 = (uint16_t)here(b);
  dpc0(b, ACT_NOP); idle(b, 1, ACT_NOP); idx_tail(b, false);

  for (int ind = 0; ind < 2; ++ind) {
    for (int cls = 0; cls < 16; ++cls) {
      m->idx_route[ind][cls] = (uint16_t)here(b);
      switch (cls) {
        case 0: dpc0(b, ACT_NOP); idle(b, 2, ACT_NOP); break;                       /* ,R+ */
        case 1: dpc0(b, ACT_NOP); idle(b, 3, ACT_NOP); break;                       /* ,R++ */
        case 2: dpc0(b, ACT_NOP); idle(b, 2, ACT_NOP); break;                       /* ,-R */
        case 3: dpc0(b, ACT_NOP); idle(b, 3, ACT_NOP); break;                       /* ,--R */
        case 4: dpc0(b, ACT_NOP); break;                                            /* ,R */
        case 5: dpc0(b, ACT_NOP); idle(b, 1, ACT_NOP); break;                       /* B,R */
        case 6: dpc0(b, ACT_NOP); idle(b, 1, ACT_NOP); break;                       /* A,R */
        case 8: rdpc(b, ACT_IDX_OFF8, ACT_NOP); dpc0(b, ACT_NOP); break;            /* n8,R */
        case 9: rdpc(b, ACT_TMP_H, ACT_NOP); rdpc(b, ACT_IDX_OFF16, ACT_NOP); idle(b, 3, ACT_NOP); break;   /* n16,R */
        case 11: dpc0(b, ACT_NOP); dpc1(b); idle(b, 3, ACT_NOP); break;             /* D,R */
        case 12: rdpc(b, ACT_IDX_PCREL8, ACT_NOP); idle(b, 1, ACT_NOP); break;      /* n8,PC */
        case 13: rdpc(b, ACT_TMP_H, ACT_NOP); rdpc(b, ACT_IDX_PCREL16, ACT_NOP); idle(b, 4, ACT_NOP); break;  /* n16,PC */
        case 15: rdpc(b, ACT_TMP_H, ACT_NOP); rdpc(b, ACT_TMP_L, ACT_NOP); idle(b, 1, ACT_NOP); break;        /* [n16] */
        default: break;                                                             /* illegal: no cycles, tmp=0 */
      }
      idx_tail(b, ind != 0);
    }
  }
}

static void build_common(B *b) {
  McRom *m = b->m;
  /* opcode fetch, second opcode fetch after a $10/$11 prefix */
  m->r_fetch = (uint16_t)here(b);
  { McUop *u = rdpc(b, ACT_IR, ACT_NOP); u->next = NX_DISPATCH; }
  m->r_fetch2 = (uint16_t)here(b);
  { McUop *u = rdpc(b, ACT_IR, ACT_NOP); u->next = NX_DISPATCH; }

  /* illegal opcode: fetch only (MAME: log_illegal(); return;) */
  m->r_illegal = (uint16_t)here(b);
  none(b, ACT_NOP, ACT_NOP); end(b);

  /* INTERRUPT_VECTOR:  dummy_vma(1); pc.h=[vec]; pc.l=[vec+1]; dummy_vma(1) */
  m->r_vec = (uint16_t)here(b);
  idle(b, 1, ACT_NOP);
  { McUop *u = add(b, K_RD, A_VEC);  u->act[0] = ACT_VEC_H; u->bsba = 2; }
  { McUop *u = add(b, K_RD, A_VEC1); u->act[0] = ACT_VEC_L; u->bsba = 2; }
  idle(b, 1, ACT_NOP); end(b);

  /* NMI / FIRQ / IRQ entry.  (fn = 0/1/2) */
  for (int k = 0; k < 3; ++k) {
    int st = here(b);
    dpc0(b, ACT_NOP); dpc0(b, ACT_NOP);
    { McUop *u = idle(b, 1, ACT_NOP); u->act[0] = ACT_INT_ENT; u->fn = (uint8_t)k; }
    push_loop(b, R_S);
    none(b, ACT_INT_FIN, ACT_NOP); last(b)->fn = (uint8_t)k; jmp(b, m->r_vec);
    if (k == 0) m->r_nmi = (uint16_t)st; else if (k == 1) m->r_firq = (uint16_t)st; else m->r_irq = (uint16_t)st;
  }
  /* RESET : hold while asserted, then fetch vector $FFFE */
  m->r_reset = (uint16_t)here(b);
  { McUop *u = idle(b, 1, ACT_NOP); u->guard = G_WAIT_RESET; u->next = NX_SELF; }
  none(b, ACT_RESET_ENT, ACT_NOP); jmp(b, m->r_vec);
  /* HALT : BA=BS=1 while halted */
  m->r_halt = (uint16_t)here(b);
  { McUop *u = idle(b, 1, ACT_NOP); u->guard = G_WAIT_HALT; u->next = NX_SELF; u->bsba = 3; }
  none(b, ACT_NOP, ACT_NOP); end(b);

  build_indexed(b);
}

/* ---- per-opcode sequences ---------------------------------------------------- */
/* open a sequence for one opcode and emit its addressing-mode prefix */
static void begin(B *b, McEntry *en, int md) {
  en->valid = true; en->ret = -1;
  switch (md) {
    case M_DIR: en->start = (int16_t)here(b); rdpc(b, ACT_EA_DIR, ACT_NOP); idle(b, 1, ACT_NOP); break;
    case M_EXT: en->start = (int16_t)here(b); rdpc(b, ACT_EA_H, ACT_NOP); rdpc(b, ACT_EA_L, ACT_NOP); idle(b, 1, ACT_NOP); break;
    case M_IDX: en->start = (int16_t)b->idx_pb; en->ret = (int16_t)here(b); break;
    default:    en->start = (int16_t)here(b); break;
  }
}

/* operand fetch cycles (mode dependent) */
static McUop *opnd(B *b, int md, uint8_t a0, bool second) {
  if (md == M_IMM) return rdpc(b, a0, ACT_NOP);
  return rdea(b, a0, second);
}

static void op_alu8(B *b, McEntry *en, int md, uint8_t fn, uint8_t reg) {
  begin(b, en, md);
  McUop *u = opnd(b, md, ACT_ALU8, false); u->fn = fn; u->reg = reg; end(b);
}
static void op_st8(B *b, McEntry *en, int md, uint8_t reg) {
  begin(b, en, md);
  McUop *u = wrea(b, W_REG8, false, ACT_ST8); u->reg = reg; end(b);
}
static void op_ld16(B *b, McEntry *en, int md, uint8_t reg) {
  begin(b, en, md);
  { McUop *u = opnd(b, md, ACT_LD16H, false); u->reg = reg; }
  { McUop *u = opnd(b, md, ACT_LD16L, true); u->reg = reg; }
  end(b);
}
static void op_alu16(B *b, McEntry *en, int md, uint8_t fn, uint8_t reg) {
  begin(b, en, md);
  opnd(b, md, ACT_TMP_H, false);
  { McUop *u = opnd(b, md, ACT_ALU16, true); u->fn = fn; u->reg = reg; }
  dpc0(b, ACT_NOP); end(b);
}
static void op_st16(B *b, McEntry *en, int md, uint8_t reg) {
  begin(b, en, md);
  { McUop *u = wrea(b, W_REG16H, false, ACT_ST16); u->reg = reg; }
  { McUop *u = wrea(b, W_REG16L, true, ACT_NOP); u->reg = reg; }
  end(b);
}
static void op_rmw(B *b, McEntry *en, int md, uint8_t fn) {
  if (md == M_A || md == M_B) {
    begin(b, en, M_INH);
    uint8_t reg = (md == M_A) ? R_A : R_B;
    if (fn == F_TST) idle(b, 1, ACT_RMW_REG);
    else dpc0(b, ACT_RMW_REG);
    last(b)->fn = fn; last(b)->reg = reg; end(b);
    return;
  }
  begin(b, en, md);
  { McUop *u = rdea(b, ACT_RMW_RD, false); u->fn = fn; }
  if (fn == F_TST) { idle(b, 2, ACT_NOP); }
  else { dpc0(b, ACT_NOP); wrea(b, W_TMPL, false, ACT_NOP); }
  end(b);
}
static void op_jmp(B *b, McEntry *en, int md) { begin(b, en, md); none(b, ACT_JMP, ACT_NOP); end(b); }
static void op_jsr(B *b, McEntry *en, int md) {
  begin(b, en, md);
  dpc0(b, ACT_NOP); idle(b, 1, ACT_NOP);
  push1(b, W_PCL, R_S, ACT_NOP);
  push1(b, W_PCH, R_S, ACT_JMP);
  end(b);
}
static void op_lea(B *b, McEntry *en, uint8_t reg) {
  begin(b, en, M_IDX);
  { McUop *u = idle(b, 1, ACT_LEA); u->reg = reg; }
  end(b);
}
static void op_inh(B *b, McEntry *en, uint8_t a, int extra_idle) {   /* fetch + dummy read (+idle) */
  begin(b, en, M_INH);
  dpc0(b, a);
  if (extra_idle) idle(b, extra_idle, ACT_NOP);
  end(b);
}
static void op_branch(B *b, McEntry *en, uint8_t cond) {
  begin(b, en, M_INH);
  rdpc(b, ACT_TMP_L, ACT_NOP);
  { McUop *u = idle(b, 1, ACT_BRANCH); u->fn = cond; }
  end(b);
}
static void op_lbranch(B *b, McEntry *en, uint8_t cond) {
  begin(b, en, M_INH);
  rdpc(b, ACT_TMP_H, ACT_NOP);
  { McUop *u = rdpc(b, ACT_TMP_L, ACT_LB_COND); u->fn = cond; }
  idle(b, 1, ACT_LB_ADD);
  { McUop *u = idle(b, 1, ACT_NOP); u->guard = G_TAKEN; }
  end(b);
}
static void op_bsr(B *b, McEntry *en) {
  begin(b, en, M_INH);
  rdpc(b, ACT_TMP_L, ACT_NOP);
  idle(b, 3, ACT_BSR_EA);
  push1(b, W_PCL, R_S, ACT_NOP);
  push1(b, W_PCH, R_S, ACT_JMP);
  end(b);
}
static void op_lbsr(B *b, McEntry *en) {
  begin(b, en, M_INH);
  rdpc(b, ACT_TMP_H, ACT_NOP); rdpc(b, ACT_TMP_L, ACT_NOP);
  idle(b, 4, ACT_LBSR_EA);
  push1(b, W_PCL, R_S, ACT_NOP);
  push1(b, W_PCH, R_S, ACT_JMP);
  end(b);
}
static void op_push(B *b, McEntry *en, uint8_t reg) {          /* PSHS / PSHU */
  begin(b, en, M_INH);
  rdpc(b, ACT_SET_MASK, ACT_NOP); idle(b, 2, ACT_NOP); rdstk(b, reg);
  push_loop(b, reg); none(b, ACT_NOP, ACT_NOP); end(b);
}
static void op_pull(B *b, McEntry *en, uint8_t reg) {          /* PULS / PULU */
  begin(b, en, M_INH);
  rdpc(b, ACT_SET_MASK, ACT_NOP); idle(b, 2, ACT_NOP);
  pull_loop(b, reg); rdstk(b, reg); end(b);
}
static void op_rts(B *b, McEntry *en) {
  begin(b, en, M_INH);
  dpc0(b, ACT_SET_MASK_PC);
  pull_loop(b, R_S); rdstk(b, R_S); end(b);
}
static void op_rti(B *b, McEntry *en) {
  begin(b, en, M_INH);
  dpc0(b, ACT_NOP);
  { McUop *u = add(b, K_RD, A_SP); u->reg = R_S; u->act[0] = ACT_RTI_CC; }
  pull_loop(b, R_S); rdstk(b, R_S); end(b);
}
static void op_swi(B *b, McEntry *en, uint8_t which) {
  begin(b, en, M_INH);
  dpc0(b, ACT_NOP);
  { McUop *u = idle(b, 1, ACT_SWI_ENT); u->fn = which; }
  push_loop(b, R_S);
  none(b, ACT_SWI_FIN, ACT_NOP); last(b)->fn = which; jmp(b, b->m->r_vec);
}
static void op_cwai(B *b, McEntry *en) {
  begin(b, en, M_INH);
  rdpc(b, ACT_CWAI_CC, ACT_NOP);
  dpc0(b, ACT_NOP);
  idle(b, 1, ACT_CWAI_ENT);
  push_loop(b, R_S);
  { McUop *u = idle(b, 1, ACT_NOP); u->guard = G_WAIT_CWAI; u->next = NX_SELF; u->bsba = 1; }
  none(b, ACT_CWAI_TAKE, ACT_NOP); jmp(b, b->m->r_vec);
}
static void op_sync(B *b, McEntry *en) {
  begin(b, en, M_INH);
  dpc0(b, ACT_NOP);
  { McUop *u = idle(b, 1, ACT_NOP); u->guard = G_WAIT_SYNC; u->next = NX_SELF; u->bsba = 1; }
  idle(b, 1, ACT_NOP); end(b);
}
static void op_tfr(B *b, McEntry *en, bool exg) {
  begin(b, en, M_INH);
  rdpc(b, exg ? ACT_EXG : ACT_TFR, ACT_NOP);
  idle(b, exg ? 6 : 4, ACT_NOP); end(b);
}
static void op_orcc(B *b, McEntry *en, uint8_t a, int extra) {
  begin(b, en, M_INH);
  if (extra) idle(b, extra, ACT_NOP);
  rdpc(b, a, ACT_NOP); dpc0(b, ACT_NOP); end(b);
}

/* ---- decode ------------------------------------------------------------------- */
static int mode_of(int opc) {
  switch ((opc >> 4) & 3) { case 0: return M_IMM; case 1: return M_DIR; case 2: return M_IDX; default: return M_EXT; }
}

static void build_page0(B *b, int opc) {
  McEntry *en = &b->m->entry[0][opc];
  static const uint8_t rmw_fn[16] = { F_NEG, F_NEG, F_XNC, F_COM, F_LSR, F_LSR, F_ROR, F_ASR,
                                      F_ASL, F_ROL, F_DEC, F_XDEC, F_INC, F_TST, 0xFF, F_CLR };
  const int hi = opc >> 4, lo = opc & 15;
  switch (hi) {
    case 0x0:
      if (lo == 0xE) op_jmp(b, en, M_DIR); else op_rmw(b, en, M_DIR, rmw_fn[lo]);
      return;
    case 0x1:
      switch (lo) {
        case 0x2: case 0xB: op_inh(b, en, ACT_NOP, 0); return;             /* NOP (0x1B alias) */
        case 0x3: op_sync(b, en); return;
        case 0x6: op_lbranch(b, en, 0); return;                            /* LBRA */
        case 0x7: op_lbsr(b, en); return;
        case 0x9: op_inh(b, en, ACT_DAA, 0); return;
        case 0xA: op_orcc(b, en, ACT_ORCC, 0); return;
        case 0xC: op_orcc(b, en, ACT_ANDCC, 0); return;
        case 0xD: op_inh(b, en, ACT_SEX, 0); return;
        case 0xE: op_tfr(b, en, true); return;
        case 0xF: op_tfr(b, en, false); return;
        default: return;                                                   /* illegal / prefixes */
      }
    case 0x2: op_branch(b, en, (uint8_t)lo); return;
    case 0x3:
      switch (lo) {
        case 0x0: op_lea(b, en, R_X); return;
        case 0x1: op_lea(b, en, R_Y); return;
        case 0x2: op_lea(b, en, R_S); return;
        case 0x3: op_lea(b, en, R_U); return;
        case 0x4: op_push(b, en, R_S); return;
        case 0x5: op_pull(b, en, R_S); return;
        case 0x6: op_push(b, en, R_U); return;
        case 0x7: op_pull(b, en, R_U); return;
        case 0x8: op_orcc(b, en, ACT_ANDCC, 1); return;                    /* undocumented ANDCC (+1 cycle) */
        case 0x9: op_rts(b, en); return;
        case 0xA: op_inh(b, en, ACT_ABX, 1); return;
        case 0xB: op_rti(b, en); return;
        case 0xC: op_cwai(b, en); return;
        case 0xD: op_inh(b, en, ACT_MUL, 9); return;
        case 0xF: op_swi(b, en, 0); return;
        default: return;
      }
    case 0x4: case 0x5: {
      const int md = (hi == 4) ? M_A : M_B;
      if (lo == 0xE) return;                                               /* undocumented XCLR: treated as illegal */
      op_rmw(b, en, md, rmw_fn[lo]);
      return;
    }
    case 0x6: if (lo == 0xE) op_jmp(b, en, M_IDX); else op_rmw(b, en, M_IDX, rmw_fn[lo]); return;
    case 0x7: if (lo == 0xE) op_jmp(b, en, M_EXT); else op_rmw(b, en, M_EXT, rmw_fn[lo]); return;
    default: break;
  }
  /* 0x80..0xFF */
  const bool grpB = (opc & 0x40) != 0;
  const int md = mode_of(opc);
  const uint8_t r8 = grpB ? R_B : R_A;
  switch (lo) {
    case 0x0: op_alu8(b, en, md, F_SUB, r8); return;
    case 0x1: op_alu8(b, en, md, F_CMP, r8); return;
    case 0x2: op_alu8(b, en, md, F_SBC, r8); return;
    case 0x3: op_alu16(b, en, md, grpB ? F_ADD : F_SUB, R_D); return;
    case 0x4: op_alu8(b, en, md, F_AND, r8); return;
    case 0x5: op_alu8(b, en, md, F_BIT, r8); return;
    case 0x6: op_alu8(b, en, md, F_LD, r8); return;
    case 0x7:
      if (md != M_IMM) { op_st8(b, en, md, r8); return; }
      begin(b, en, M_IMM);                       /* undocumented "STA #imm": operand is read, N/Z from the register, V cleared (MAME XST8) */
      { McUop *u = rdpc(b, ACT_ST8, ACT_NOP); u->reg = r8; }
      end(b);
      return;
    case 0x8: op_alu8(b, en, md, F_EOR, r8); return;
    case 0x9: op_alu8(b, en, md, F_ADC, r8); return;
    case 0xA: op_alu8(b, en, md, F_OR, r8); return;
    case 0xB: op_alu8(b, en, md, F_ADD, r8); return;
    case 0xC: if (!grpB) op_alu16(b, en, md, F_CMP, R_X); else op_ld16(b, en, md, R_D); return;
    case 0xD:
      if (!grpB) { if (md == M_IMM) op_bsr(b, en); else op_jsr(b, en, md); }
      else if (md != M_IMM) op_st16(b, en, md, R_D);
      return;
    case 0xE: op_ld16(b, en, md, grpB ? R_U : R_X); return;
    case 0xF: if (md != M_IMM) op_st16(b, en, md, grpB ? R_U : R_X); return;
  }
}

static void build_page1(B *b, int opc) {                      /* $10 prefix */
  McEntry *en = &b->m->entry[1][opc];
  const int hi = opc >> 4, lo = opc & 15;
  if (hi == 2) { op_lbranch(b, en, (uint8_t)lo); return; }    /* $1020 = LBRA (cond 0 = always) */
  if (opc == 0x3F) { op_swi(b, en, 1); return; }
  if (opc < 0x80) return;
  const int md = mode_of(opc);
  switch (lo) {
    case 0x3: if (!(opc & 0x40)) op_alu16(b, en, md, F_CMP, R_D); return;
    case 0xC: if (!(opc & 0x40)) op_alu16(b, en, md, F_CMP, R_Y); return;
    case 0xE: if (!(opc & 0x40)) op_ld16(b, en, md, R_Y); else op_ld16(b, en, md, R_S); return;
    case 0xF: if (md == M_IMM) return; if (!(opc & 0x40)) op_st16(b, en, md, R_Y); else op_st16(b, en, md, R_S); return;
    default: return;
  }
}

static void build_page2(B *b, int opc) {                      /* $11 prefix */
  McEntry *en = &b->m->entry[2][opc];
  if (opc == 0x3F) { op_swi(b, en, 2); return; }
  if (opc < 0x80 || (opc & 0x40)) return;
  const int md = mode_of(opc);
  switch (opc & 15) {
    case 0x3: op_alu16(b, en, md, F_CMP, R_U); return;
    case 0xC: op_alu16(b, en, md, F_CMP, R_S); return;
    default: return;
  }
}

static McRom g_rom;
static bool g_rom_built;

const McRom *mc_rom_get(void) {
  if (g_rom_built) return &g_rom;
  McRom *m = &g_rom;
  memset(m, 0, sizeof *m);
  m->rom = (McUop *)calloc(ROM_CAP, sizeof(McUop));
  m->cap = ROM_CAP;
  for (int p = 0; p < 3; ++p)
    for (int o = 0; o < 256; ++o) { m->entry[p][o].start = -1; m->entry[p][o].ret = -1; }
  B b = { m, m->rom, 0 };
  build_common(&b);
  for (int o = 0; o < 256; ++o) build_page0(&b, o);
  for (int o = 0; o < 256; ++o) build_page1(&b, o);
  for (int o = 0; o < 256; ++o) build_page2(&b, o);
  /* Undocumented aliases that MAME implements (and sbc09 treats as NOP): decoded, but flagged. */
  static const uint8_t undoc0[] = {0x01, 0x02, 0x05, 0x0B, 0x1B, 0x38, 0x41, 0x42, 0x45, 0x4B, 0x51, 0x52, 0x55,
                                   0x5B, 0x61, 0x62, 0x65, 0x6B, 0x71, 0x72, 0x75, 0x7B, 0x87, 0xC7};
  for (size_t i = 0; i < sizeof undoc0; ++i) m->entry[0][undoc0[i]].undoc = true;
  m->entry[1][0x20].undoc = true;                                   /* $1020 LBRA alias */
  g_rom_built = true;
  return m;
}
