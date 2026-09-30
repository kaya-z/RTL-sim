// mc6809.cpp - MC6809E datapath / sequencer (one step per bus cycle)
#include "mc6809.h"
#include <cstring>

namespace mc6809 {

namespace {

// ---- small helpers ------------------------------------------------------------
inline int8_t  sx8(uint32_t v)  { return static_cast<int8_t>(v & 0xFF); }
inline int16_t sx16(uint32_t v) { return static_cast<int16_t>(v & 0xFFFF); }

inline uint16_t rd16(const Core& c, uint8_t r) {
  switch (r) {
    case R_A: return c.a;
    case R_B: return c.b;
    case R_D: return static_cast<uint16_t>((c.a << 8) | c.b);
    case R_X: return c.x;
    case R_Y: return c.y;
    case R_U: return c.u;
    case R_S: return c.s;
    case R_PC: return c.pc;
    case R_CC: return c.cc;
    default:  return c.dp;
  }
}
inline void wr16(Core& c, uint8_t r, uint16_t v) {
  switch (r) {
    case R_A: c.a = static_cast<uint8_t>(v); break;
    case R_B: c.b = static_cast<uint8_t>(v); break;
    case R_D: c.a = static_cast<uint8_t>(v >> 8); c.b = static_cast<uint8_t>(v); break;
    case R_X: c.x = v; break;
    case R_Y: c.y = v; break;
    case R_U: c.u = v; break;
    case R_S: c.s = v; break;
    case R_PC: c.pc = v; break;
    case R_CC: c.cc = static_cast<uint8_t>(v); break;
    default: c.dp = static_cast<uint8_t>(v); break;
  }
}
// index register selected by post-byte bits 6..5
inline uint16_t& ireg(Core& c) {
  switch ((c.pb >> 5) & 3) { case 0: return c.x; case 1: return c.y; case 2: return c.u; default: return c.s; }
}

// MAME-style flag helper: r is the full-width result (bit 8 / bit 16 = carry)
inline void set_flags(uint8_t& cc, uint8_t mask, uint32_t a, uint32_t b, uint32_t r, int bits) {
  const uint32_t hi = 1u << (bits - 1);
  cc &= static_cast<uint8_t>(~mask);
  if ((mask & CC_H) && ((a ^ b ^ r) & 0x10)) cc |= CC_H;
  if ((mask & CC_N) && (r & hi)) cc |= CC_N;
  if ((mask & CC_Z) && ((r & (hi * 2 - 1)) == 0)) cc |= CC_Z;
  if ((mask & CC_V) && ((a ^ b ^ r ^ (r >> 1)) & hi)) cc |= CC_V;
  if ((mask & CC_C) && (r & (hi << 1))) cc |= CC_C;
}
inline uint32_t set_nz8(uint8_t& cc, uint8_t r)   { set_flags(cc, CC_N | CC_Z, 0, r, r, 8);  return r; }
inline uint32_t set_nz16(uint8_t& cc, uint16_t r) { set_flags(cc, CC_N | CC_Z, 0, r, r, 16); return r; }

inline bool cond_true(uint8_t fn, uint8_t cc) {
  const bool C = cc & CC_C, V = cc & CC_V, Z = cc & CC_Z, N = cc & CC_N;
  switch (fn & 15) {
    case 0x0: return true;
    case 0x1: return false;
    case 0x2: return !(C || Z);
    case 0x3: return C || Z;
    case 0x4: return !C;
    case 0x5: return C;
    case 0x6: return !Z;
    case 0x7: return Z;
    case 0x8: return !V;
    case 0x9: return V;
    case 0xA: return !N;
    case 0xB: return N;
    case 0xC: return N == V;
    case 0xD: return N != V;
    case 0xE: return !Z && (N == V);
    default:  return Z || (N != V);
  }
}

// PSH/PUL byte-slot tables
constexpr uint8_t kPushBit[12] = {7, 7, 6, 6, 5, 5, 4, 4, 3, 2, 1, 0};   // PCL PCH oL oH YL YH XL XH DP B A CC
constexpr uint8_t kPullBit[12] = {0, 1, 2, 3, 4, 4, 5, 5, 6, 6, 7, 7};   // CC A B DP XH XL YH YL oH oL PCH PCL

inline uint8_t find_slot(const uint8_t* bits, uint8_t mask, uint8_t slot) {
  for (uint8_t s = slot; s < 12; ++s)
    if (mask & (1u << bits[s])) return s;
  return 12;
}

inline uint16_t& sp_of(Core& c, uint8_t reg) { return reg == R_U ? c.u : c.s; }
inline uint16_t sp_val(const Core& c, uint8_t reg) { return reg == R_U ? c.u : c.s; }

// TFR / EXG register access (MAME m6809.cpp semantics, incl. 8<->16 quirks)
uint16_t tfr_read(const Core& c, uint8_t nib, bool alt) {
  switch (nib & 15) {
    case 0: return static_cast<uint16_t>((c.a << 8) | c.b);
    case 1: return c.x;
    case 2: return c.y;
    case 3: return c.u;
    case 4: return c.s;
    case 5: return c.pc;
    case 8: return 0xFF00 | c.a;
    case 9: return 0xFF00 | c.b;
    case 10: return alt ? (0xFF00 | c.cc) : static_cast<uint16_t>((c.cc << 8) | c.cc);
    case 11: return alt ? (0xFF00 | c.dp) : static_cast<uint16_t>((c.dp << 8) | c.dp);
    default: return 0xFFFF;
  }
}
void tfr_write(Core& c, uint8_t nib, uint16_t v) {
  switch (nib & 15) {
    case 0: c.a = v >> 8; c.b = static_cast<uint8_t>(v); break;
    case 1: c.x = v; break;
    case 2: c.y = v; break;
    case 3: c.u = v; break;
    case 4: c.s = v; break;
    case 5: c.pc = v; break;
    case 8: c.a = static_cast<uint8_t>(v); break;
    case 9: c.b = static_cast<uint8_t>(v); break;
    case 10: c.cc = static_cast<uint8_t>(v); break;
    case 11: c.dp = static_cast<uint8_t>(v); break;
    default: break;
  }
}

enum { IRQ_NONE = -1, IRQ_NMI = 0, IRQ_FIRQ = 1, IRQ_IRQ = 2 };
inline int pending_irq(const Core& n, const Pins& p) {
  if (n.nmi_pend) return IRQ_NMI;
  if (!(n.cc & CC_F) && !p.firq_n) return IRQ_FIRQ;
  if (!(n.cc & CC_I) && !p.irq_n) return IRQ_IRQ;
  return IRQ_NONE;
}
inline bool any_line(const Core& n, const Pins& p) { return n.nmi_pend || !p.firq_n || !p.irq_n; }

// pure guard test (no side effects)
inline bool guard_test(const UOp& u, const Core& n, const Pins& p) {
  switch (u.guard) {
    case G_ALWAYS: return true;
    case G_PUSH: return find_slot(kPushBit, n.mask, n.slot) < 12;
    case G_PULL: return find_slot(kPullBit, n.mask, n.slot) < 12;
    case G_TAKEN: return n.br_taken;
    case G_WAIT_CWAI: return pending_irq(n, p) == IRQ_NONE;
    case G_WAIT_SYNC: return !any_line(n, p);
    case G_WAIT_RESET: return !p.reset_n;
    case G_WAIT_HALT: return !p.halt_n;
  }
  return true;
}

// ---- 8-bit ALU on the accumulators -------------------------------------------------
void alu8(Core& n, const UOp& u, uint8_t d) {
  uint8_t* rp = (u.reg == R_A) ? &n.a : &n.b;
  const uint32_t a = *rp;
  uint32_t r = 0;
  switch (u.fn) {
    case F_SUB: r = a - d;                     set_flags(n.cc, CC_N|CC_Z|CC_V|CC_C, a, d, r, 8); *rp = uint8_t(r); break;
    case F_CMP: r = a - d;                     set_flags(n.cc, CC_N|CC_Z|CC_V|CC_C, a, d, r, 8); break;
    case F_SBC: r = a - d - (n.cc & CC_C ? 1 : 0); set_flags(n.cc, CC_N|CC_Z|CC_V|CC_C, a, d, r, 8); *rp = uint8_t(r); break;
    case F_AND: r = a & d; n.cc &= ~CC_V; set_flags(n.cc, CC_N|CC_Z, 0, a, r, 8); *rp = uint8_t(r); break;
    case F_BIT: r = a & d; n.cc &= ~CC_V; set_flags(n.cc, CC_N|CC_Z, 0, a, r, 8); break;
    case F_EOR: r = a ^ d; n.cc &= ~CC_V; set_flags(n.cc, CC_N|CC_Z, 0, a, r, 8); *rp = uint8_t(r); break;
    case F_OR:  r = a | d; n.cc &= ~CC_V; set_flags(n.cc, CC_N|CC_Z, 0, a, r, 8); *rp = uint8_t(r); break;
    case F_ADC: r = a + d + (n.cc & CC_C ? 1 : 0); set_flags(n.cc, CC_H|CC_N|CC_Z|CC_V|CC_C, a, d, r, 8); *rp = uint8_t(r); break;
    case F_ADD: r = a + d;                     set_flags(n.cc, CC_H|CC_N|CC_Z|CC_V|CC_C, a, d, r, 8); *rp = uint8_t(r); break;
    case F_LD:  *rp = d; n.cc &= ~CC_V; set_nz8(n.cc, d); break;
  }
}

// read-modify-write core: returns the result byte, updates flags.  *store=false for TST.
uint8_t rmw8(Core& n, uint8_t fn, uint8_t v, bool* store) {
  uint32_t r;
  *store = true;
  if (fn == F_XNC) fn = (n.cc & CC_C) ? F_COM : F_NEG;
  switch (fn) {
    case F_NEG: r = 0u - v; set_flags(n.cc, CC_N|CC_Z|CC_V|CC_C, 0, v, r, 8); return uint8_t(r);
    case F_COM: n.cc &= ~CC_V; n.cc |= CC_C; r = uint8_t(~v); set_nz8(n.cc, uint8_t(r)); return uint8_t(r);
    case F_LSR: n.cc &= ~CC_C; if (v & 1) n.cc |= CC_C; r = v >> 1; set_nz8(n.cc, uint8_t(r)); return uint8_t(r);
    case F_ROR: { const uint8_t cin = (n.cc & CC_C) ? 0x80 : 0; n.cc &= ~CC_C; if (v & 1) n.cc |= CC_C;
                  r = (v >> 1) | cin; set_nz8(n.cc, uint8_t(r)); return uint8_t(r); }
    // H after ASR is architecturally undefined; sbc09 (v09s.c and engine.c) loads it from operand bit 4
    case F_ASR: n.cc &= ~(CC_C | CC_H); if (v & 1) n.cc |= CC_C; if (v & 0x10) n.cc |= CC_H;
                r = uint8_t(int8_t(v) >> 1); set_nz8(n.cc, uint8_t(r)); return uint8_t(r);
    // H after ASL is architecturally undefined; like sbc09 (v09s.c) it is computed as v+v (ADD semantics)
    case F_ASL: r = uint32_t(v) << 1; set_flags(n.cc, CC_H|CC_N|CC_Z|CC_V|CC_C, v, v, r, 8); return uint8_t(r);
    case F_ROL: { r = (uint32_t(v) << 1) | ((n.cc & CC_C) ? 1 : 0);
                  n.cc &= ~CC_C; if (v & 0x80) n.cc |= CC_C;
                  set_flags(n.cc, CC_N|CC_Z|CC_V, v, v, r, 8); return uint8_t(r); }
    case F_DEC: r = uint32_t(v) - 1; set_flags(n.cc, CC_N|CC_Z|CC_V, v, 1, r, 8); return uint8_t(r);
    case F_INC: r = uint32_t(v) + 1; set_flags(n.cc, CC_N|CC_Z|CC_V, v, 1, r, 8); return uint8_t(r);
    case F_XDEC: n.cc &= ~CC_C; if (v) n.cc |= CC_C; r = uint32_t(v) - 1; set_flags(n.cc, CC_N|CC_Z|CC_V, v, 1, r, 8); return uint8_t(r);
    case F_TST: *store = false; n.cc &= ~CC_V; set_nz8(n.cc, v); return v;
    case F_CLR: n.cc &= ~(CC_N|CC_V|CC_C); n.cc |= CC_Z; return 0;
  }
  return v;
}

}  // namespace

// ---------------------------------------------------------------------------------
Cpu::Cpu(const Pins* pins)
    : rtl::Module("mc6809", rtl::edge_bit(rtl::Edge::EFall)), pins_(pins), mr_(MicroRom::get()) {
  power_on();
}

void Cpu::power_on() {
  Core c;
  c.upc = mr_.r_reset;
  c.addr = 0xFFFF; c.vma = false; c.rw = true; c.dout = 0xFF;
  st.force(c);
  cycles = 0;
}

void Cpu::eval(rtl::Edge e) {
  if (e != rtl::Edge::EFall) return;
  Core n = st.q;
  step(*pins_, n);
  st.nb(n);
  ++cycles;
}

// ---------------------------------------------------------------------------------
// One bus cycle has just finished (E fell).  din is valid.  Perform the actions of
// the micro-op that was on the bus, select the next micro-op and register the next
// bus request.  Everything here is "combinational logic in front of the flops".
void Cpu::step(const Pins& p, Core& n) {
  const Core& q = st.q;
  const std::vector<UOp>& rom = mr_.rom;

  n.ev_inst = n.ev_int = n.ev_ill = false;

  // NMI edge detector (arming after the first load of S: MAME m_lds_encountered)
  const bool nmi_edge = q.nmi_prev_n && !p.nmi_n;
  n.nmi_prev_n = p.nmi_n;
  if (nmi_edge && q.lds_seen) n.nmi_pend = true;

  // ---- helper lambdas (operate on n) ----------------------------------------------
  auto boundary = [&]() -> int {
    if (!p.halt_n) return mr_.r_halt;
    const int irq = pending_irq(n, p);
    if (irq != IRQ_NONE) {
      n.ev_int = true; n.ev_vec = static_cast<uint8_t>(irq); n.after_int = true;
      return irq == IRQ_NMI ? mr_.r_nmi : irq == IRQ_FIRQ ? mr_.r_firq : mr_.r_irq;
    }
    n.page = 0;
    n.ipc = n.pc;
    if (n.after_int) n.after_int = false; else n.ev_inst = true;
    return mr_.r_fetch;
  };

  auto exec = [&](Act a, const UOp& u, uint8_t d) {
    switch (a) {
      case ACT_NOP: break;
      case ACT_IR: n.ir = d; break;
      case ACT_EA_DIR: n.ea = static_cast<uint16_t>((n.dp << 8) | d); break;
      case ACT_EA_H: n.ea = static_cast<uint16_t>((n.ea & 0x00FF) | (d << 8)); break;
      case ACT_EA_L: n.ea = static_cast<uint16_t>((n.ea & 0xFF00) | d); break;
      case ACT_TMP_H: n.tmp = static_cast<uint16_t>((n.tmp & 0x00FF) | (d << 8)); break;
      case ACT_TMP_L: n.tmp = static_cast<uint16_t>((n.tmp & 0xFF00) | d); break;
      case ACT_IDX_PB: {
        n.pb = d;
        uint16_t& r = ireg(n);
        if (!(d & 0x80)) {
          const int off = (d & 0x10) ? (d & 0x1F) - 32 : (d & 0x1F);
          n.tmp = static_cast<uint16_t>(r + off);
        } else {
          switch (d & 0x0F) {
            case 0x0: n.tmp = r; r = static_cast<uint16_t>(r + 1); break;
            case 0x1: n.tmp = r; r = static_cast<uint16_t>(r + 2); break;
            case 0x2: r = static_cast<uint16_t>(r - 1); n.tmp = r; break;
            case 0x3: r = static_cast<uint16_t>(r - 2); n.tmp = r; break;
            case 0x4: n.tmp = r; break;
            case 0x5: n.tmp = static_cast<uint16_t>(r + sx8(n.b)); break;
            case 0x6: n.tmp = static_cast<uint16_t>(r + sx8(n.a)); break;
            case 0xB: n.tmp = static_cast<uint16_t>(r + ((n.a << 8) | n.b)); break;
            default:  n.tmp = 0; break;
          }
        }
        break;
      }
      case ACT_IDX_OFF8:    n.tmp = static_cast<uint16_t>(ireg(n) + sx8(d)); break;
      case ACT_IDX_OFF16:   n.tmp = static_cast<uint16_t>(ireg(n) + static_cast<uint16_t>((n.tmp & 0xFF00) | d)); break;
      case ACT_IDX_PCREL8:  n.tmp = static_cast<uint16_t>(n.pc + sx8(d)); break;
      case ACT_IDX_PCREL16: n.tmp = static_cast<uint16_t>(n.pc + sx16((n.tmp & 0xFF00) | d)); break;
      case ACT_EA_FROM_TMP: n.ea = n.tmp; break;

      case ACT_ALU8: alu8(n, u, d); break;
      case ACT_ST8: {
        const uint8_t v = static_cast<uint8_t>(rd16(n, u.reg));
        n.cc &= ~CC_V; set_nz8(n.cc, v); break;
      }
      case ACT_RMW_RD: {
        bool st; const uint8_t r = rmw8(n, u.fn, d, &st);
        n.tmp = static_cast<uint16_t>((n.tmp & 0xFF00) | r);
        break;
      }
      case ACT_RMW_REG: {
        bool st; uint8_t* rp = (u.reg == R_A) ? &n.a : &n.b;
        const uint8_t r = rmw8(n, u.fn, *rp, &st);
        if (st) *rp = r;
        break;
      }
      case ACT_LD16H: {
        uint16_t v = rd16(n, u.reg);
        v = static_cast<uint16_t>((v & 0x00FF) | (d << 8));
        wr16(n, u.reg, v);
        break;
      }
      case ACT_LD16L: {
        uint16_t v = rd16(n, u.reg);
        v = static_cast<uint16_t>((v & 0xFF00) | d);
        wr16(n, u.reg, v);
        n.cc &= ~CC_V; set_nz16(n.cc, v);
        if (u.reg == R_S) n.lds_seen = true;
        break;
      }
      case ACT_ALU16: {
        const uint32_t t = static_cast<uint16_t>((n.tmp & 0xFF00) | d);
        const uint32_t a = rd16(n, u.reg);
        if (u.fn == F_ADD) {
          const uint32_t r = a + t; set_flags(n.cc, CC_N|CC_Z|CC_V|CC_C, a, t, r, 16); wr16(n, u.reg, uint16_t(r));
        } else {
          const uint32_t r = a - t; set_flags(n.cc, CC_N|CC_Z|CC_V|CC_C, a, t, r, 16);
          if (u.fn == F_SUB) wr16(n, u.reg, uint16_t(r));
        }
        break;
      }
      case ACT_ST16: n.cc &= ~CC_V; set_nz16(n.cc, rd16(n, u.reg)); break;
      case ACT_LEA:
        wr16(n, u.reg, n.ea);
        if (u.reg == R_X || u.reg == R_Y) set_flags(n.cc, CC_Z, 0, n.ea, n.ea, 16);
        if (u.reg == R_S) n.lds_seen = true;
        break;

      case ACT_JMP: n.pc = n.ea; break;
      case ACT_BRANCH: if (cond_true(u.fn, n.cc)) n.pc = static_cast<uint16_t>(n.pc + sx8(n.tmp)); break;
      case ACT_LB_COND: n.br_taken = cond_true(u.fn, n.cc); break;
      case ACT_LB_ADD: if (n.br_taken) n.pc = static_cast<uint16_t>(n.pc + n.tmp); break;
      case ACT_BSR_EA: n.ea = static_cast<uint16_t>(n.pc + sx8(n.tmp)); break;
      case ACT_LBSR_EA: n.ea = static_cast<uint16_t>(n.pc + sx16(n.tmp)); break;

      case ACT_SP_DEC: { uint16_t& sp = sp_of(n, u.reg); sp = static_cast<uint16_t>(sp - 1); break; }
      case ACT_PUSH_STEP: {
        uint16_t& sp = sp_of(n, u.reg); sp = static_cast<uint16_t>(sp - 1);
        n.slot = static_cast<uint8_t>(n.slot + 1);
        break;
      }
      case ACT_PULL_STEP: {
        switch (n.slot) {
          case 0: n.cc = d; break;
          case 1: n.a = d; break;
          case 2: n.b = d; break;
          case 3: n.dp = d; break;
          case 4: n.x = static_cast<uint16_t>((n.x & 0x00FF) | (d << 8)); break;
          case 5: n.x = static_cast<uint16_t>((n.x & 0xFF00) | d); break;
          case 6: n.y = static_cast<uint16_t>((n.y & 0x00FF) | (d << 8)); break;
          case 7: n.y = static_cast<uint16_t>((n.y & 0xFF00) | d); break;
          case 8: { uint16_t& o = (u.reg == R_S) ? n.u : n.s; o = static_cast<uint16_t>((o & 0x00FF) | (d << 8)); break; }
          case 9: { uint16_t& o = (u.reg == R_S) ? n.u : n.s; o = static_cast<uint16_t>((o & 0xFF00) | d); break; }
          case 10: n.pc = static_cast<uint16_t>((n.pc & 0x00FF) | (d << 8)); break;
          case 11: n.pc = static_cast<uint16_t>((n.pc & 0xFF00) | d); break;
        }
        uint16_t& sp = sp_of(n, u.reg); sp = static_cast<uint16_t>(sp + 1);
        n.slot = static_cast<uint8_t>(n.slot + 1);
        break;
      }
      case ACT_SET_MASK: n.mask = d; n.slot = 0; break;
      case ACT_SET_MASK_PC: n.mask = 0x80; n.slot = 0; break;
      case ACT_RTI_CC:
        n.cc = d;
        n.mask = (d & CC_E) ? 0xFE : 0x80;
        n.slot = 1;
        n.s = static_cast<uint16_t>(n.s + 1);
        break;

      case ACT_DAA: {
        uint32_t cf = 0;
        const uint32_t msn = n.a & 0xF0, lsn = n.a & 0x0F;
        if (lsn > 0x09 || (n.cc & CC_H)) cf |= 0x06;
        if (msn > 0x80 && lsn > 0x09) cf |= 0x60;
        if (msn > 0x90 || (n.cc & CC_C)) cf |= 0x60;
        const uint32_t t = n.a + cf;
        n.cc &= ~CC_V;
        if (t & 0x100) n.cc |= CC_C;
        n.a = static_cast<uint8_t>(t);
        set_nz8(n.cc, n.a);
        break;
      }
      case ACT_SEX: {
        const uint16_t v = static_cast<uint16_t>(static_cast<int16_t>(sx8(n.b)));
        n.a = static_cast<uint8_t>(v >> 8);
        set_nz16(n.cc, v);
        break;
      }
      case ACT_ABX: n.x = static_cast<uint16_t>(n.x + n.b); break;
      case ACT_MUL: {
        const uint16_t r = static_cast<uint16_t>(n.a * n.b);
        n.a = static_cast<uint8_t>(r >> 8); n.b = static_cast<uint8_t>(r);
        n.cc &= ~(CC_Z | CC_C);
        if (r == 0) n.cc |= CC_Z;
        if (r & 0x80) n.cc |= CC_C;
        break;
      }
      case ACT_ORCC: n.cc |= d; break;
      case ACT_ANDCC: n.cc &= d; break;
      case ACT_TFR: {
        const uint16_t v = tfr_read(n, d >> 4, false);
        tfr_write(n, d & 15, v);
        if ((d & 15) == 4) n.lds_seen = true;
        break;
      }
      case ACT_EXG: {
        const bool alt = !(d & 0x80);
        const uint16_t r1 = tfr_read(n, d >> 4, alt), r2 = tfr_read(n, d & 15, alt);
        tfr_write(n, d & 15, r1);
        tfr_write(n, d >> 4, r2);
        break;
      }

      case ACT_SWI_ENT:
        n.cc |= CC_E; n.mask = 0xFF; n.slot = 0;
        n.vec = u.fn == 0 ? VEC_SWI : u.fn == 1 ? VEC_SWI2 : VEC_SWI3;
        break;
      case ACT_SWI_FIN: if (u.fn == 0) n.cc |= CC_I | CC_F; break;
      case ACT_INT_ENT:
        n.slot = 0;
        if (u.fn == 0)      { n.nmi_pend = false; n.cc |= CC_E; n.mask = 0xFF; n.vec = VEC_NMI; }
        else if (u.fn == 1) { n.cc &= ~CC_E; n.mask = 0x81; n.vec = VEC_FIRQ; }
        else                { n.cc |= CC_E; n.mask = 0xFF; n.vec = VEC_IRQ; }
        break;
      case ACT_INT_FIN: n.cc |= (u.fn == 2) ? CC_I : (CC_I | CC_F); break;
      case ACT_CWAI_CC: n.cc &= d; break;
      case ACT_CWAI_ENT: n.cc |= CC_E; n.mask = 0xFF; n.slot = 0; break;
      case ACT_CWAI_TAKE: {
        const int irq = pending_irq(n, p);
        if (irq == IRQ_NMI)       { n.nmi_pend = false; n.vec = VEC_NMI;  n.cc |= CC_I | CC_F; }
        else if (irq == IRQ_FIRQ) { n.vec = VEC_FIRQ; n.cc |= CC_I | CC_F; }
        else                      { n.vec = VEC_IRQ;  n.cc |= CC_I; }
        break;
      }
      case ACT_VEC_H: n.pc = static_cast<uint16_t>((n.pc & 0x00FF) | (d << 8)); break;
      case ACT_VEC_L: n.pc = static_cast<uint16_t>((n.pc & 0xFF00) | d); break;
      case ACT_RESET_ENT:
        n.cc |= CC_I | CC_F; n.dp = 0; n.vec = VEC_RESET;
        n.nmi_pend = false; n.lds_seen = false; n.after_int = false;
        break;
    }
  };

  auto resolve = [&](const UOp& u, int cur, uint8_t d) -> int {
    switch (u.next) {
      case NX_SEQ:  return cur + 1;
      case NX_SELF: return cur;
      case NX_JMP:  return u.tgt;
      case NX_RET:  return n.link;
      case NX_END:  return boundary();
      case NX_IDX: {
        const uint8_t pb = n.pb;
        if (!(pb & 0x80)) return mr_.idx_route5;
        return mr_.idx_route[(pb >> 4) & 1][pb & 0x0F];
      }
      case NX_DISPATCH: {
        if (d == 0x10 || d == 0x11) {
          if (n.page) n.ev_ill = true;                            // prefix after prefix
          n.page = (d == 0x10) ? 1 : 2; return mr_.r_fetch2;
        }
        const Entry& en = mr_.entry[n.page][d];
        if (en.undoc) n.ev_ill = true;
        if (!en.valid) {
          n.ev_ill = true;
          if (n.page) { n.page = 0; return mr_.r_fetch2; }        // undefined prefixed op: re-dispatch (MAME)
          return mr_.r_illegal;                                     // illegal op: fetch only
        }
        n.link = static_cast<uint16_t>(en.ret);
        return en.start;
      }
    }
    return cur + 1;
  };

  // ---- 1. finish the bus cycle that just ended -----------------------------------------
  int cand;
  if (!p.reset_n) {
    n.cc |= CC_I | CC_F; n.dp = 0; n.nmi_pend = false; n.lds_seen = false; n.after_int = false; n.page = 0;
    cand = mr_.r_reset;
  } else {
    const UOp& u = rom[q.upc];
    const uint8_t d = p.din;
    if (u.pcinc) n.pc = static_cast<uint16_t>(q.pc + 1);
    exec(u.act[0], u, d);
    exec(u.act[1], u, d);
    cand = resolve(u, q.upc, d);
  }

  // ---- 2. chain zero-length micro-ops / skip guarded ones until a bus cycle is found ---
  for (int guard = 0; guard < 64; ++guard) {
    const UOp& v = rom[cand];
    n.upc = static_cast<uint16_t>(cand);
    if (!guard_test(v, n, p)) {
      cand = (v.next == NX_SELF) ? cand + 1 : resolve(v, cand, 0);
      continue;
    }
    if (v.guard == G_PUSH) n.slot = find_slot(kPushBit, n.mask, n.slot);
    else if (v.guard == G_PULL) n.slot = find_slot(kPullBit, n.mask, n.slot);
    if (v.kind == K_NONE) {
      exec(v.act[0], v, 0);
      exec(v.act[1], v, 0);
      cand = resolve(v, cand, 0);
      continue;
    }
    break;
  }

  // ---- 3. register the next bus request -----------------------------------------------
  const UOp& v = rom[n.upc];
  // LIC: this cycle is the last one of the instruction (look ahead through skipped ops)
  bool last = false;
  {
    int j = -1;
    if (v.next == NX_END) last = true;
    else if (v.next == NX_SEQ) j = n.upc + 1;
    for (int k = 0; j >= 0 && k < 8; ++k) {
      const UOp& w = rom[j];
      const bool run = guard_test(w, n, p);
      if (run && w.kind != K_NONE) break;
      if (w.next == NX_END) { last = true; break; }
      if (w.next == NX_SEQ || (!run && w.next == NX_SELF)) { ++j; continue; }
      break;
    }
  }
  n.lic = last;
  n.bs = (v.bsba >> 1) & 1;
  n.ba = v.bsba & 1;
  switch (v.asel) {
    case A_PC:  n.addr = n.pc; break;
    case A_PC1: n.addr = static_cast<uint16_t>(n.pc + 1); break;
    case A_FFFF: n.addr = 0xFFFF; break;
    case A_EA:  n.addr = n.ea; break;
    case A_EA1: n.addr = static_cast<uint16_t>(n.ea + 1); break;
    case A_SP_DEC: n.addr = static_cast<uint16_t>(sp_val(n, v.reg) - 1); break;
    case A_SP:  n.addr = sp_val(n, v.reg); break;
    case A_VEC: n.addr = n.vec; break;
    case A_VEC1: n.addr = static_cast<uint16_t>(n.vec + 1); break;
  }
  n.rw = (v.kind != K_WR);
  n.vma = (v.kind != K_IDLE);
  n.dout = 0xFF;
  if (v.kind == K_WR) {
    switch (v.wsel) {
      case W_TMPL: n.dout = static_cast<uint8_t>(n.tmp); break;
      case W_REG8: n.dout = static_cast<uint8_t>(rd16(n, v.reg)); break;
      case W_REG16H: n.dout = static_cast<uint8_t>(rd16(n, v.reg) >> 8); break;
      case W_REG16L: n.dout = static_cast<uint8_t>(rd16(n, v.reg)); break;
      case W_PCL: n.dout = static_cast<uint8_t>(n.pc); break;
      case W_PCH: n.dout = static_cast<uint8_t>(n.pc >> 8); break;
      case W_STK: {
        const bool ofs = (v.reg == R_S);    // when pushing on S the "other" stack pointer is U, and vice versa
        const uint16_t other = ofs ? n.u : n.s;
        switch (n.slot) {
          case 0: n.dout = static_cast<uint8_t>(n.pc); break;
          case 1: n.dout = static_cast<uint8_t>(n.pc >> 8); break;
          case 2: n.dout = static_cast<uint8_t>(other); break;
          case 3: n.dout = static_cast<uint8_t>(other >> 8); break;
          case 4: n.dout = static_cast<uint8_t>(n.y); break;
          case 5: n.dout = static_cast<uint8_t>(n.y >> 8); break;
          case 6: n.dout = static_cast<uint8_t>(n.x); break;
          case 7: n.dout = static_cast<uint8_t>(n.x >> 8); break;
          case 8: n.dout = n.dp; break;
          case 9: n.dout = n.b; break;
          case 10: n.dout = n.a; break;
          case 11: n.dout = n.cc; break;
        }
        break;
      }
      case W_NONE: break;
    }
  }
}

}  // namespace mc6809
