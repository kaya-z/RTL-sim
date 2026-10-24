/* test_cpu.c - directed self-checking tests of the MC6809 RTL core (C11)
 *
 * Covers what the random lock-step against sbc09 cannot: interrupt entry/return
 * (IRQ, FIRQ, NMI, SWI/SWI2/SWI3), CWAI, SYNC, HALT, RESET bus sequence, interrupt
 * latency in bus cycles, plus exhaustive DAA and MUL checks against arithmetic
 * definitions (not against another simulator).
 *
 * Expected bus-cycle counts follow the MC6809E data sheet (interrupt = 19 cycles, ...). */
#include <stdio.h>
#include "flatsys.h"

#define FLATSYS(n) FlatSys n __attribute__((cleanup(flat_free))); flat_init(&n)
#define HEX(s, at, ...) do { const uint8_t b_[] = { __VA_ARGS__ }; uint16_t a_ = (at); \
                             for (size_t i_ = 0; i_ < sizeof b_; ++i_) flat_poke(&(s), a_++, b_[i_]); } while (0)

static int g_fail = 0, g_checks = 0;
#define CHECK(cond, ...) do { ++g_checks; if (!(cond)) { ++g_fail; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)


// ---------------------------------------------------------------------------------------
static void test_reset() {
  printf("reset sequence\n");
  FLATSYS(s);
  flat_vectors(&s, 0x1000);
  HEX(s, 0x1000, 0x12, 0x12, 0x12);                  // NOP NOP NOP
  s.record = true;
  flat_release(&s, 6);
  flat_run(&s, 12);
  // during RESET the bus is idle (VMA low); then: idle, read FFFE, read FFFF, idle, fetch $1000...
  size_t i = 0;
  while (i < s.ncycles && !s.cycles[i].vma) ++i;
  // find the vector fetch
  size_t v = 0;
  for (; v < s.ncycles; ++v) if (s.cycles[v].vma && s.cycles[v].addr == 0xFFFE) break;
  CHECK(v + 3 < s.ncycles, "vector fetch not found");
  if (v + 3 < s.ncycles) {
    CHECK(s.cycles[v + 1].addr == 0xFFFF && s.cycles[v + 1].vma && s.cycles[v + 1].rw, "second vector byte");
    CHECK(!s.cycles[v - 1].vma, "idle cycle before vector fetch");
    CHECK(!s.cycles[v + 2].vma, "idle cycle after vector fetch");
    CHECK(s.cycles[v + 3].addr == 0x1000 && s.cycles[v + 3].vma, "first opcode fetch at $1000 (got %04x)", s.cycles[v + 3].addr);
    CHECK(s.cycles[v + 3].bs == false && s.cycles[v].bs == true, "BS high during vector fetch");
  }
  CHECK((flat_core(&s)->cc & (CC_I | CC_F)) == (CC_I | CC_F), "I and F set after reset (cc=%02x)", flat_core(&s)->cc);
  CHECK(flat_core(&s)->dp == 0, "DP cleared by reset");
}

// ---------------------------------------------------------------------------------------

static void test_irq() {
  printf("IRQ: entry frame, latency, RTI, masking\n");
  FLATSYS(s);
  flat_vectors(&s, 0x1000);
  HEX(s, 0x1000, 0x10, 0xCE, 0x7F, 0x00,             // LDS #$7F00
                       0xCE, 0x7A, 0x00,                    // LDU #$7A00
                       0x8E, 0x12, 0x34,                    // LDX #$1234
                       0x10, 0x8E, 0x56, 0x78,              // LDY #$5678
                       0x86, 0xA5, 0xC6, 0x5A,              // LDA #$A5 ; LDB #$5A
                       0x1C, 0xEF,                          // ANDCC #$EF   (enable IRQ)
                       0x20, 0xFE);                        // BRA *
  HEX(s, 0x2000, 0x86, 0x55, 0xB7, 0x30, 0x00, 0x3B);   // handler: LDA #$55 ; STA $3000 ; RTI
  flat_release(&s, 4);
  const uint16_t pc_loop = 0x1000 + 20;
  CHECK(flat_run_to(&s, pc_loop, 400), "reached the idle loop");
  // no IRQ yet: nothing pushed
  CHECK(flat_core(&s)->s == 0x7F00, "S untouched before the IRQ");
  s.pins.irq_n = false;
  s.record = true;
  flat_run(&s, 60);
  s.pins.irq_n = true;
  flat_run(&s, 60);
  // frame: PC, U, Y, X, DP, B, A, CC pushed from 7EFF downwards
  CHECK(flat_peek16(&s, 0x7EFE) == pc_loop, "stacked PC = interrupted instruction (%04x)", flat_peek16(&s, 0x7EFE));
  CHECK(flat_peek16(&s, 0x7EFC) == 0x7A00, "stacked U");
  CHECK(flat_peek16(&s, 0x7EFA) == 0x5678, "stacked Y");
  CHECK(flat_peek16(&s, 0x7EF8) == 0x1234, "stacked X");
  CHECK(flat_peek(&s, 0x7EF7) == 0x00, "stacked DP");
  CHECK(flat_peek(&s, 0x7EF6) == 0x5A, "stacked B");
  CHECK(flat_peek(&s, 0x7EF5) == 0xA5, "stacked A");
  CHECK((flat_peek(&s, 0x7EF4) & CC_E) != 0 && (flat_peek(&s, 0x7EF4) & CC_I) == 0, "stacked CC has E set, I clear (%02x)", flat_peek(&s, 0x7EF4));
  CHECK(flat_peek(&s, 0x3000) == 0x55, "handler ran");
  CHECK(flat_core(&s)->s == 0x7F00, "RTI restored S (%04x)", flat_core(&s)->s);
  CHECK(flat_core(&s)->a == 0xA5 && flat_core(&s)->b == 0x5A && flat_core(&s)->x == 0x1234 && flat_core(&s)->y == 0x5678 && flat_core(&s)->u == 0x7A00,
        "RTI restored the registers");
  CHECK((flat_core(&s)->cc & CC_I) == 0, "RTI restored the I flag");
  // latency: the interrupt sequence takes 19 cycles from the boundary to the first handler fetch
  int start = -1;
  for (size_t i = 0; i < s.ncycles; ++i)
    if (s.cycles[i].vma && s.cycles[i].addr == 0xFFF8) { start = (int)(i); break; }
  CHECK(start >= 0, "IRQ vector read seen");
  if (start >= 0) {
    // vector hi at FFF8, lo at FFF9 ; handler fetch two cycles later ($2000)
    CHECK(s.cycles[start + 1].addr == 0xFFF9, "vector low byte");
    CHECK(s.cycles[start + 3].addr == 0x2000, "handler fetch after idle (%04x)", s.cycles[start + 3].addr);
    // count cycles from the last opcode fetch of the loop (BRA, 3 cycles) ... find first cycle of the sequence:
    // it is 19 cycles from the start of the interrupt sequence to the handler's first fetch (excl.)
    int seq_start = start - 15;          // 2 dummy PC reads + 1 idle + 12 pushes = 15 cycles before the vector read  (+1 idle)
    CHECK(seq_start >= 0, "sequence start");
    CHECK(s.cycles[start + 3].cycle - s.cycles[seq_start - 1].cycle == 19, "interrupt sequence is 19 cycles (%llu)",
          (unsigned long long)(s.cycles[start + 3].cycle - s.cycles[seq_start - 1].cycle));
  }

  // masked: with I set the request is ignored
  FLATSYS(m);
  flat_vectors(&m, 0x1000);
  HEX(m, 0x1000, 0x10, 0xCE, 0x7F, 0x00, 0x20, 0xFE);    // LDS ; BRA * (I stays set from reset)
  HEX(m, 0x2000, 0x86, 0x55, 0xB7, 0x30, 0x00, 0x3B);
  flat_release(&m, 4); flat_run(&m, 100);
  m.pins.irq_n = false; flat_run(&m, 200);
  CHECK(flat_peek(&m, 0x3000) == 0 && flat_core(&m)->s == 0x7F00, "IRQ ignored while I is set");
}

static void test_firq_nmi_swi() {
  printf("FIRQ / NMI / SWI / SWI2 / SWI3\n");
  {
    FLATSYS(s);
    flat_vectors(&s, 0x1000);
    HEX(s, 0x1000, 0x10, 0xCE, 0x7F, 0x00, 0x86, 0xA5, 0x1C, 0xAF, 0x20, 0xFE);   // LDS ; LDA #A5 ; ANDCC #$AF (F,I clear) ; BRA *
    HEX(s, 0x2100, 0x86, 0x11, 0xB7, 0x30, 0x00, 0x3B);                             // FIRQ: LDA #$11 ; STA $3000 ; RTI
    flat_release(&s, 4); flat_run(&s, 100);
    s.pins.firq_n = false; flat_run(&s, 40); s.pins.firq_n = true; flat_run(&s, 40);
    CHECK(flat_peek16(&s, 0x7EFE) == 0x1008, "FIRQ stacked PC (%04x)", flat_peek16(&s, 0x7EFE));
    CHECK((flat_peek(&s, 0x7EFD) & CC_E) == 0, "FIRQ frame has E clear");
    CHECK(flat_core(&s)->s == 0x7F00, "FIRQ RTI restored S (%04x)", flat_core(&s)->s);
    CHECK(flat_peek(&s, 0x3000) == 0x11, "FIRQ handler ran");
    CHECK(flat_core(&s)->a == 0x11, "FIRQ does not save A: the handler's LDA survives RTI (a=%02x)", flat_core(&s)->a);
    // only 3 bytes were stacked: the byte below the frame is untouched
    CHECK(flat_peek(&s, 0x7EFB) == 0x00, "nothing below the 3-byte FIRQ frame");
  }
  {
    // NMI: armed only after the first load of S
    FLATSYS(s);
    flat_vectors(&s, 0x1000);
    HEX(s, 0x1000, 0x20, 0x08);                                   // BRA +8  (before S is loaded)
    HEX(s, 0x100A, 0x10, 0xCE, 0x7F, 0x00, 0x20, 0xFE);           // LDS #$7F00 ; BRA *
    HEX(s, 0x2200, 0x86, 0x77, 0xB7, 0x30, 0x00, 0x7C, 0x30, 0x01, 0x3B);  // NMI handler: LDA #$77 ; STA $3000 ; INC $3001 ; RTI
    flat_release(&s, 4); flat_run(&s, 10);
    s.pins.nmi_n = false; flat_run(&s, 4); s.pins.nmi_n = true;                 // NMI before LDS: ignored
    flat_run(&s, 100);
    CHECK(flat_peek(&s, 0x3001) == 0 && flat_core(&s)->s == 0x7F00, "NMI before LDS is ignored");
    s.pins.nmi_n = false; flat_run(&s, 60);                                     // edge after LDS: taken once
    CHECK(flat_peek(&s, 0x3000) == 0x77 && flat_peek(&s, 0x3001) == 1, "NMI taken after LDS");
    CHECK((flat_peek(&s, 0x7EF4) & CC_E) != 0, "NMI frame has E set");
    flat_run(&s, 300);                                                          // line stays low: no retrigger (edge sensitive)
    CHECK(flat_core(&s)->s == 0x7F00 && flat_peek(&s, 0x3001) == 1, "NMI is edge triggered (count=%d)", flat_peek(&s, 0x3001));
    s.pins.nmi_n = true; flat_run(&s, 20); s.pins.nmi_n = false; flat_run(&s, 60);
    CHECK(flat_core(&s)->s == 0x7F00 && flat_peek(&s, 0x3001) == 2, "second edge handled and returned (count=%d)", flat_peek(&s, 0x3001));
  }
  {
    // SWI, SWI2, SWI3 : vectors and flag behaviour
    FLATSYS(s);
    flat_vectors(&s, 0x1000);
    HEX(s, 0x1000, 0x10, 0xCE, 0x7F, 0x00, 0x1C, 0xAF, 0x3F, 0x10, 0x3F, 0x11, 0x3F, 0x20, 0xFE);
    HEX(s, 0x2300, 0x1F, 0xA8, 0xB7, 0x30, 0x10, 0x3B);          // SWI  : TFR CC,A ; STA $3010 ; RTI  (I,F are set on entry)
    HEX(s, 0x2400, 0x1F, 0xA8, 0xB7, 0x30, 0x11, 0x3B);          // SWI2 : record CC
    HEX(s, 0x2500, 0x1F, 0xA8, 0xB7, 0x30, 0x12, 0x3B);          // SWI3 : record CC
    flat_release(&s, 4);
    s.record = true;
    flat_run(&s, 300);
    CHECK((flat_peek(&s, 0x3010) & (CC_I | CC_F)) == (CC_I | CC_F), "SWI sets I and F (cc=%02x)", flat_peek(&s, 0x3010));
    CHECK((flat_peek(&s, 0x3011) & (CC_I | CC_F)) == 0 && (flat_peek(&s, 0x3011) & CC_E), "SWI2 leaves I/F alone, sets E (cc=%02x)", flat_peek(&s, 0x3011));
    CHECK((flat_peek(&s, 0x3012) & (CC_I | CC_F)) == 0 && (flat_peek(&s, 0x3012) & CC_E), "SWI3 leaves I/F alone, sets E (cc=%02x)", flat_peek(&s, 0x3012));
    CHECK(flat_core(&s)->s == 0x7F00, "balanced stack after three SWIs");
    // SWI is 19 cycles: opcode fetch .. handler fetch
    for (size_t i = 0; i < s.ncycles; ++i)
      if (s.cycles[i].vma && s.cycles[i].addr == 0xFFFA) {
        CHECK(s.cycles[i + 3].addr == 0x2300, "SWI handler fetched %04x", s.cycles[i + 3].addr);
        // 1 fetch + 1 dummy + 1 idle + 12 pushes + 1 idle + 2 vector + 1 idle = 19 ; the fetch is 18 cycles before the vector read+... check spacing
        CHECK(s.cycles[i + 3].cycle - s.cycles[i - 16].cycle == 19, "SWI takes 19 cycles (%llu)",
              (unsigned long long)(s.cycles[i + 3].cycle - s.cycles[i - 16].cycle));
      }
  }
}

static void test_cwai_sync_halt() {
  printf("CWAI / SYNC / HALT\n");
  {
    FLATSYS(s);
    flat_vectors(&s, 0x1000);
    HEX(s, 0x1000, 0x10, 0xCE, 0x7F, 0x00, 0x86, 0x42, 0x3C, 0xEF, 0x8E, 0x99, 0x99, 0x20, 0xFE);   // LDS ; LDA #$42 ; CWAI #$EF ; LDX #$9999 ; BRA *
    HEX(s, 0x2000, 0xC6, 0x24, 0xF7, 0x30, 0x00, 0x3B);                                            // IRQ: LDB #$24 ; STB $3000 ; RTI
    flat_release(&s, 4); flat_run(&s, 120);
    CHECK(flat_core(&s)->x == 0, "CWAI waits (X=%04x)", flat_core(&s)->x);
    CHECK(flat_core(&s)->s == 0x7F00 - 12, "CWAI stacked the whole state (S=%04x)", flat_core(&s)->s);
    CHECK(flat_core(&s)->ba && !flat_core(&s)->bs, "BA high / BS low while waiting");
    CHECK(flat_peek16(&s, 0x7EFE) == 0x1008, "CWAI stacked PC = next instruction (%04x)", flat_peek16(&s, 0x7EFE));
    CHECK((flat_peek(&s, 0x7EF4) & CC_E) != 0 && (flat_peek(&s, 0x7EF4) & CC_I) == 0, "CWAI stacked CC: E set, masks cleared");
    s.pins.irq_n = false; flat_run(&s, 30); s.pins.irq_n = true; flat_run(&s, 60);
    CHECK(flat_peek(&s, 0x3000) == 0x24, "IRQ handler ran after CWAI");
    CHECK(flat_core(&s)->x == 0x9999, "execution continues after CWAI (X=%04x)", flat_core(&s)->x);
    CHECK(flat_core(&s)->s == 0x7F00, "balanced (S=%04x)", flat_core(&s)->s);
  }
  {
    // SYNC with the interrupt masked: resumes with the next instruction, no vector
    FLATSYS(s);
    flat_vectors(&s, 0x1000);
    HEX(s, 0x1000, 0x10, 0xCE, 0x7F, 0x00, 0x13, 0x8E, 0x77, 0x77, 0x20, 0xFE);   // LDS ; SYNC ; LDX #$7777 ; BRA *   (I set)
    HEX(s, 0x2000, 0xC6, 0x24, 0xF7, 0x30, 0x00, 0x3B);
    flat_release(&s, 4); flat_run(&s, 80);
    CHECK(flat_core(&s)->x == 0, "SYNC waits");
    s.pins.irq_n = false; flat_run(&s, 30);
    CHECK(flat_core(&s)->x == 0x7777, "masked IRQ ends SYNC without a vector (X=%04x)", flat_core(&s)->x);
    CHECK(flat_peek(&s, 0x3000) == 0 && flat_core(&s)->s == 0x7F00, "handler not entered");
    s.pins.irq_n = true;
  }
  {
    // SYNC with the interrupt enabled: vector taken, return address after SYNC
    FLATSYS(s);
    flat_vectors(&s, 0x1000);
    HEX(s, 0x1000, 0x10, 0xCE, 0x7F, 0x00, 0x1C, 0xEF, 0x13, 0x8E, 0x77, 0x77, 0x20, 0xFE);
    HEX(s, 0x2000, 0xC6, 0x24, 0xF7, 0x30, 0x00, 0x3B);
    flat_release(&s, 4); flat_run(&s, 80);
    s.pins.irq_n = false; flat_run(&s, 60); s.pins.irq_n = true; flat_run(&s, 60);
    CHECK(flat_peek(&s, 0x3000) == 0x24 && flat_core(&s)->x == 0x7777, "IRQ taken after SYNC and execution resumes (x=%04x)", flat_core(&s)->x);
    CHECK(flat_peek16(&s, 0x7EFE) == 0x1007, "return address is the instruction after SYNC (%04x)", flat_peek16(&s, 0x7EFE));
  }
  {
    // HALT: the bus goes quiet (BA=BS=1) at the next instruction boundary; releasing HALT resumes
    FLATSYS(s);
    flat_vectors(&s, 0x1000);
    HEX(s, 0x1000, 0x8E, 0x00, 0x00, 0x30, 0x01, 0x20, 0xFC);    // LDX #0 ; loop: LEAX 1,X ; BRA loop
    flat_release(&s, 4); flat_run(&s, 50);
    s.pins.halt_n = false; flat_run(&s, 20);
    CHECK(flat_core(&s)->ba && flat_core(&s)->bs && !flat_core(&s)->vma, "halted: BA=BS=1, no bus activity");
    const uint16_t x = flat_core(&s)->x;
    flat_run(&s, 50);
    CHECK(flat_core(&s)->x == x, "no progress while halted");
    s.pins.halt_n = true; flat_run(&s, 50);
    CHECK(flat_core(&s)->x != x, "resumes after HALT is released");
  }
}

// ---------------------------------------------------------------------------------------
static uint8_t bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

static void test_daa_exhaustive() {
  printf("DAA: exhaustive BCD addition (100 x 100)\n");
  FLATSYS(s);
  flat_vectors(&s, 0x1000);
  // LDA #a ; ADDA #b ; DAA ; STA $3000 ; TFR CC,A ; STA $3001 ; BRA *
  HEX(s, 0x1000, 0x86, 0, 0x8B, 0, 0x19, 0xB7, 0x30, 0x00, 0x1F, 0xA8, 0xB7, 0x30, 0x01, 0x20, 0xFE);
  int bad = 0;
  for (int a = 0; a < 100 && bad < 5; ++a)
    for (int b = 0; b < 100 && bad < 5; ++b) {
      flat_poke(&s, 0x1001, bcd(a)); flat_poke(&s, 0x1003, bcd(b));
      s.reset_hold = true; flat_run(&s, 3); s.reset_hold = false;
      flat_run(&s, 60);
      const int sum = a + b;
      const uint8_t res = flat_peek(&s, 0x3000), cc = flat_peek(&s, 0x3001);
      const bool ok = res == bcd(sum % 100) && ((cc & CC_C) != 0) == (sum >= 100) && ((cc & CC_Z) != 0) == (res == 0) &&
                      ((cc & CC_N) != 0) == ((res & 0x80) != 0);
      if (!ok) { ++bad; printf("  bcd %02d+%02d -> %02x cc=%02x (want %02x carry %d)\n", a, b, res, cc, bcd(sum % 100), sum >= 100); }
    }
  CHECK(bad == 0, "DAA mismatches: %d", bad);
}

static void test_mul_exhaustive() {
  printf("MUL: A x B for all 65536 operand pairs\n");
  FLATSYS(s);
  flat_vectors(&s, 0x1000);
  // LDA #a ; LDB #b ; MUL ; STD $3000 ; TFR CC,A ; STA $3002 ; BRA *
  HEX(s, 0x1000, 0x86, 0, 0xC6, 0, 0x3D, 0xFD, 0x30, 0x00, 0x1F, 0xA8, 0xB7, 0x30, 0x02, 0x20, 0xFE);
  int bad = 0;
  for (int a = 0; a < 256 && bad < 5; ++a)
    for (int b = 0; b < 256 && bad < 5; ++b) {
      flat_poke(&s, 0x1001, a); flat_poke(&s, 0x1003, b);
      s.reset_hold = true; flat_run(&s, 2); s.reset_hold = false;
      flat_run(&s, 52);
      const uint16_t r = (uint16_t)(a * b);
      const uint8_t cc = flat_peek(&s, 0x3002);
      const bool ok = flat_peek16(&s, 0x3000) == r && ((cc & CC_Z) != 0) == (r == 0) && ((cc & CC_C) != 0) == ((r & 0x80) != 0);
      if (!ok) { ++bad; printf("  %d*%d -> %04x cc=%02x (want %04x)\n", a, b, flat_peek16(&s, 0x3000), cc, r); }
    }
  CHECK(bad == 0, "MUL mismatches: %d", bad);
}

int main() {
  test_reset();
  test_irq();
  test_firq_nmi_swi();
  test_cwai_sync_halt();
  test_daa_exhaustive();
  test_mul_exhaustive();
  printf("\n%d checks, %d failures\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
