// flatsys.h - MC6809 core + 64 KiB flat RAM, with a cycle-accurate bus recorder.
// Shared by the directed unit tests (test_cpu.cpp) and the lock-step harness (ramtest.cpp).
#pragma once
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "../rtl/rtl.h"
#include "../rtl/mc6809.h"
#include "../soc/bus.h"

namespace tb {

struct BusCycle {
  uint64_t cycle;
  uint16_t addr;
  bool rw;        // true = read
  bool vma;
  uint8_t data;   // read data or write data
  bool lic;
  bool bs, ba;
};

struct FlatSys {
  mc6809::Pins pins;
  mc6809::Cpu cpu{&pins};
  soc::Bus bus{&cpu};
  soc::Ram ram{65536};
  rtl::Simulator sim;
  bool reset_hold = true;            // RESET asserted until release()
  bool record = false;
  std::vector<BusCycle> cycles;      // one entry per bus cycle while record == true
  long insts = 0;                    // instruction boundaries seen (ev_inst)
  long ints = 0;                     // interrupt sequences started (ev_int)
  std::vector<uint16_t> inst_pc;     // pc at every instruction boundary (when record)
  std::vector<uint64_t> inst_cycle;  // cycle number of every boundary (when record)

  FlatSys() {
    bus.map(0, 0xFFFF, &ram);
    sim.add(&cpu); sim.add(&bus); sim.add(&ram);
    sim.on_settle([](void* c) { static_cast<FlatSys*>(c)->settle(); }, this);
    sim.on_edge_end([](void* c, rtl::Edge e) { static_cast<FlatSys*>(c)->edge_end(e); }, this);
    settle();
  }
  void settle() {
    pins.din = bus.rdata.q;
    pins.reset_n = !reset_hold;
  }
  void edge_end(rtl::Edge e) {
    if (e != rtl::Edge::EFall) return;
    const mc6809::Core& c = cpu.q();
    if (c.ev_inst) ++insts;
    if (c.ev_int) ++ints;
    if (record && (c.ev_inst || c.ev_int)) { inst_pc.push_back(c.pc); inst_cycle.push_back(sim.clk().cycle); }
    if (record) {
      // the cycle that just finished had the *previous* address/rw; log what is on the bus now instead
      // (address, R/W and data are stable for the whole next cycle): logged at the next EFall.
    }
  }

  // Run n E-cycles; records (addr, rw, data) of every completed bus cycle if `record`.
  void run(uint64_t n) {
    for (uint64_t i = 0; i < n; ++i) {
      if (record) {
        // sample the bus at the end of the cycle (just before E falls): the CPU is about to latch rdata
        for (int k = 0; k < 3; ++k) sim.step_edge();
        const mc6809::Core& c = cpu.q();
        cycles.push_back({sim.clk().cycle, c.addr, c.rw, c.vma, c.rw ? bus.rdata.q : c.dout, c.lic, c.bs, c.ba});
        sim.step_edge();
      } else {
        sim.step_cycle();
      }
    }
  }
  void release(uint64_t after_cycles = 4) { run(after_cycles); reset_hold = false; settle(); }

  void poke(uint16_t a, uint8_t v) { ram.mem.poke(a, v); }
  void load(uint16_t a, const std::vector<uint8_t>& b) { for (size_t i = 0; i < b.size(); ++i) poke(a + i, b[i]); }
  uint8_t peek(uint16_t a) const { return ram.mem.rd(a); }
  uint16_t peek16(uint16_t a) const { return static_cast<uint16_t>((peek(a) << 8) | peek(a + 1)); }
  void vectors(uint16_t reset, uint16_t irq = 0x2000, uint16_t firq = 0x2100, uint16_t nmi = 0x2200,
               uint16_t swi = 0x2300, uint16_t swi2 = 0x2400, uint16_t swi3 = 0x2500) {
    auto v = [&](uint16_t a, uint16_t h) { poke(a, h >> 8); poke(a + 1, h & 0xFF); };
    v(0xFFFE, reset); v(0xFFF8, irq); v(0xFFF6, firq); v(0xFFFC, nmi); v(0xFFFA, swi); v(0xFFF4, swi2); v(0xFFF2, swi3);
  }
  const mc6809::Core& core() const { return cpu.q(); }

  // run until the core is about to fetch the instruction at `pc` (or give up after max cycles)
  bool run_to(uint16_t pc, uint64_t max) {
    for (uint64_t i = 0; i < max; ++i) {
      sim.step_cycle();
      const mc6809::Core& c = cpu.q();
      if (c.ev_inst && c.pc == pc) return true;
    }
    return false;
  }
};

}  // namespace tb
