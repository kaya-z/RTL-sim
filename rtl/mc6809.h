/* Credits / references:
 *   * MAME m6809 core (src/devices/cpu/m6809/m6809.cpp, m6809.lst, base6x09.lst) -
 *     (c) Nathan Woods, BSD-3-Clause.  The per-instruction bus-cycle sequences, the interrupt/CWAI/SYNC
 *     sequencing, TFR/EXG register semantics, the DAA algorithm and the "NMI armed after LDS" rule were
 *     taken from it as a *specification*; this file is an independent re-implementation (no code copied).
 *   * sbc09 v09s.c / engine.c - (c) 1994 L.C. Benschop and the sbc09 team, GPLv2 (S. Kono's fork
 *     github.com/shinji-kono/sbc09): instruction semantics and the flag behaviour of undefined cases
 *     (H after ASL/ASR) follow v09s.c.  Used as reference only; not included here.
 *   * Motorola MC6809/MC6809E data sheet and Programming Manual: instruction set, cycle tables. */

/* mc6809.h - cycle-accurate, microcoded RTL model of the Motorola MC6809E (C11)
 *
 * Structure (mirrors a real micro-programmed core):
 *
 *   micro-ROM  : one micro-op (McUop) == one bus cycle (or a zero-length
 *                "action only" step).  Built at start-up by mc6809_ucode.c.
 *                The bus-cycle sequences follow MAME's m6809.lst / base6x09.lst
 *                (which in turn follow the MC6809E data-sheet cycle tables).
 *   datapath   : McCore = all flip-flops.  Updated ONCE per bus cycle,
 *                on the falling edge of E, with non-blocking semantics
 *                (RTL_NB(cpu->st, n)).
 *   bus        : registered outputs (address, R/W, data-out, VMA, LIC, BS, BA).
 *                They are produced on the same E-fall edge that finishes the
 *                previous cycle, so they are stable for the whole next cycle
 *                (address valid before Q rises, data sampled at E fall).
 *
 * Instruction *semantics* (flags etc.) were cross-checked against sbc09's
 * engine.c / v09s.c; instruction *timing / bus order* follow MAME m6809.
 */
#ifndef MC6809_H
#define MC6809_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "rtl.h"

/* ---- condition code bits ------------------------------------------------- */
enum { CC_C = 0x01, CC_V = 0x02, CC_Z = 0x04, CC_N = 0x08,
       CC_I = 0x10, CC_H = 0x20, CC_F = 0x40, CC_E = 0x80 };

/* ---- vectors --------------------------------------------------------------- */
enum { VEC_SWI3 = 0xFFF2, VEC_SWI2 = 0xFFF4, VEC_FIRQ = 0xFFF6,
       VEC_IRQ = 0xFFF8, VEC_SWI = 0xFFFA, VEC_NMI = 0xFFFC,
       VEC_RESET = 0xFFFE };

/* ---- micro-op definition --------------------------------------------------- */
enum { K_NONE, K_RD, K_WR, K_IDLE };                    /* McUop.kind */

enum {                                                  /* McUop.asel */
  A_PC,        /* address = PC */
  A_PC1,       /* address = PC+1 */
  A_FFFF,      /* address = $FFFF (VMA low) */
  A_EA,        /* address = EA */
  A_EA1,       /* address = EA+1 */
  A_SP_DEC,    /* address = SP-1  (pre-decrement push)     SP = S or U (uop.reg) */
  A_SP,        /* address = SP    (pull / dummy stack read) */
  A_VEC,       /* address = vector */
  A_VEC1,      /* address = vector+1 */
};

enum { W_NONE, W_TMPL, W_REG8, W_REG16H, W_REG16L, W_PCL, W_PCH, W_STK };   /* McUop.wsel */

enum {                                                  /* McUop.next */
  NX_SEQ,        /* upc+1 */
  NX_SELF,       /* repeat this micro-op (guard decides) */
  NX_JMP,        /* upc = tgt */
  NX_RET,        /* upc = link register */
  NX_END,        /* instruction boundary (interrupt / halt / next fetch) */
  NX_DISPATCH,   /* opcode decode */
  NX_IDX,        /* indexed post-byte routing */
};

/* A guard is evaluated when a micro-op is *entered*.  If it is false the
 * micro-op is skipped (its `next` is followed, or upc+1 for NX_SELF loops).
 * For the WAIT guards "true" means "keep waiting" (execute one more idle cycle). */
enum { G_ALWAYS, G_PUSH, G_PULL, G_TAKEN,
       G_WAIT_CWAI, G_WAIT_SYNC, G_WAIT_RESET, G_WAIT_HALT };

enum {                                                  /* McUop.act[] */
  ACT_NOP,
  ACT_IR, ACT_EA_DIR, ACT_EA_H, ACT_EA_L, ACT_TMP_H, ACT_TMP_L,
  ACT_IDX_PB, ACT_EA_FROM_TMP,
  ACT_ALU8, ACT_ST8, ACT_RMW_RD, ACT_RMW_REG,
  ACT_LD16H, ACT_LD16L, ACT_ALU16, ACT_ST16, ACT_LEA,
  ACT_IDX_OFF8, ACT_IDX_OFF16, ACT_IDX_PCREL8, ACT_IDX_PCREL16,
  ACT_JMP, ACT_BRANCH, ACT_LB_COND, ACT_LB_ADD, ACT_BSR_EA, ACT_LBSR_EA,
  ACT_SP_DEC, ACT_PUSH_STEP, ACT_PULL_STEP,
  ACT_SET_MASK, ACT_SET_MASK_PC, ACT_RTI_CC,
  ACT_DAA, ACT_SEX, ACT_ABX, ACT_MUL, ACT_ORCC, ACT_ANDCC, ACT_TFR, ACT_EXG,
  ACT_SWI_ENT, ACT_SWI_FIN, ACT_INT_ENT, ACT_INT_FIN,
  ACT_CWAI_CC, ACT_CWAI_ENT, ACT_CWAI_TAKE,
  ACT_VEC_H, ACT_VEC_L, ACT_RESET_ENT,
};

/* register selectors (uop.reg) */
enum { R_A, R_B, R_D, R_X, R_Y, R_U, R_S, R_PC, R_CC, R_DP };

/* ALU functions (uop.fn) */
enum {
  F_SUB, F_CMP, F_SBC, F_AND, F_BIT, F_EOR, F_ADC, F_OR, F_ADD, F_LD,   /* 8/16 bit */
  F_NEG, F_COM, F_LSR, F_ROR, F_ASR, F_ASL, F_ROL, F_DEC, F_INC, F_TST, F_CLR,
  F_XNC, F_XDEC,
};

typedef struct {
  uint8_t  kind;                 /* K_*  */
  uint8_t  asel;                 /* A_*  */
  uint8_t  wsel;                 /* W_*  */
  bool     pcinc;                /* PC <= PC+1 at end of cycle */
  uint8_t  act[2];               /* ACT_* executed in order at the end of the cycle */
  uint8_t  guard;                /* G_*  */
  uint8_t  next;                 /* NX_* */
  uint8_t  fn;
  uint8_t  reg;
  uint8_t  bsba;                 /* bit0 = BA, bit1 = BS */
  uint16_t tgt;
} McUop;

typedef struct {
  int16_t start, ret;
  bool    valid;
  bool    undoc;                 /* undocumented alias (MAME behaviour); raises ev_ill */
} McEntry;

typedef struct {
  McUop   *rom;
  size_t   n, cap;
  McEntry  entry[3][256];        /* [page][opcode] */
  uint16_t idx_route[2][16];     /* [indirect][post-byte low nibble]   (bit7=1) */
  uint16_t idx_route5;           /* 5-bit offset */
  uint16_t r_fetch, r_fetch2, r_illegal, r_vec, r_nmi, r_firq, r_irq, r_reset, r_halt;
} McRom;

const McRom *mc_rom_get(void);   /* singleton, built on first use */

/* ---- flip-flops ------------------------------------------------------------- */
typedef struct {
  /* programmer visible */
  uint8_t  a, b, dp, cc;
  uint16_t x, y, u, s, pc;
  /* datapath temporaries */
  uint8_t  ir, page, pb, mask, slot;
  uint16_t tmp, ea, vec, link;
  bool     br_taken;
  /* control */
  uint16_t upc;
  bool lds_seen, nmi_pend, nmi_prev_n, after_int;
  /* registered bus outputs */
  uint16_t addr;
  uint8_t  dout;
  bool rw;                /* 1 = read */
  bool vma;
  bool lic;
  bool bs, ba;
  /* event strobes (one cycle wide, for trace / test-bench) */
  bool ev_inst;           /* a new instruction is about to be fetched */
  bool ev_int;            /* an interrupt sequence is about to start */
  bool ev_ill;            /* illegal / undefined opcode was dispatched (fetch-only NOP, as MAME) */
  uint8_t ev_vec;         /* 0 NMI, 1 FIRQ, 2 IRQ */
  uint16_t ipc;           /* address of the instruction being fetched */
} McCore;

typedef struct {          /* CPU inputs (wires) */
  uint8_t din;
  bool irq_n, firq_n, nmi_n, halt_n, reset_n;
} McPins;

static inline void mc_pins_init(McPins *p) {
  p->din = 0xFF; p->irq_n = p->firq_n = p->nmi_n = p->halt_n = p->reset_n = true;
}

typedef struct McCpu {
  RtlModule mod;
  RTL_REG(McCore) st;
  uint64_t cycles;        /* bus cycles executed (statistics only) */
  const McPins *pins;
  const McRom  *mr;
} McCpu;

void mc_cpu_init(McCpu *c, const McPins *pins);
void mc_cpu_power_on(McCpu *c);          /* registers <= reset values, bus idle */
static inline const McCore *mc_cpu_q(const McCpu *c) { return &c->st.q; }

#endif /* MC6809_H */
