// bus.h - MC6809E system bus (Q/E timing) and device interface
//
// Cycle timing of one bus cycle (matches the MC6809E data-sheet ordering):
//
//   EFall(n-1)  CPU registers its next address / R/W / write data  (NBA commit)
//   QRise       bus decodes the address into a chip-select register (sel)
//   ERise       selected device puts read data on the bus (rdata register)
//   EFall       CPU latches rdata; devices latch write data / see read strobe
//
// All registers are non-blocking; a device therefore never observes anything
// the CPU did in the same edge.
#pragma once
#include <cstdint>
#include <cstdio>
#include <vector>
#include "../rtl/rtl.h"
#include "../rtl/mc6809.h"

namespace soc {

// A memory-mapped slave.
//   rd()      combinational read data (a pure function of the device's q registers)
//   strobes   wires driven by the bus in the E-fall edge (functions of pre-edge
//             state only).  A device consumes them in its own eval(EFall) and
//             updates its registers with nb() / nb_wr(); the bus module must be
//             evaluated before the devices (registration order).
struct Device {
  virtual ~Device() = default;
  virtual uint8_t rd(uint16_t a) = 0;
  bool wr_en = false;      // write strobe   (E fall, R/W = 0, selected)
  bool rd_en = false;      // read  strobe   (E fall, R/W = 1, selected): side effects of a read
  uint16_t sa = 0;         // bus address (device-relative offset added by the bus glue if needed)
  uint8_t wdata = 0;       // write data
};

class Bus : public rtl::Module {
 public:
  Bus(const mc6809::Cpu* cpu)
      : rtl::Module("bus", rtl::edge_bit(rtl::Edge::QRise) | rtl::edge_bit(rtl::Edge::ERise) |
                               rtl::edge_bit(rtl::Edge::EFall)),
        cpu_(cpu) {}

  // Devices are searched in insertion order; the first that claims the
  // address wins (so overlays such as the I/O page go first).
  struct Region { uint32_t lo, hi; Device* dev; };
  void map(uint32_t lo, uint32_t hi, Device* d) { regions_.push_back({lo, hi, d}); }

  rtl::Reg<int16_t> sel{-1};        // chip-select (index into regions_)
  rtl::Reg<uint8_t> rdata{0xFF};    // data bus read value
  uint64_t rd_cycles = 0, wr_cycles = 0;
  FILE* log = nullptr;                // optional bus monitor: "cycle R|W addr data" for [log_lo, log_hi]
  uint32_t log_lo = 0, log_hi = 0xFFFF;
  uint64_t ncyc = 0;

  void eval(rtl::Edge e) override {
    const mc6809::Core& c = cpu_->q();
    switch (e) {
      case rtl::Edge::QRise: {
        int16_t s = -1;
        if (c.vma) {
          for (size_t i = 0; i < regions_.size(); ++i)
            if (c.addr >= regions_[i].lo && c.addr <= regions_[i].hi) { s = static_cast<int16_t>(i); break; }
        }
        sel.nb(s);
        break;
      }
      case rtl::Edge::ERise:
        rdata.nb((sel.q >= 0 && c.rw) ? regions_[sel.q].dev->rd(c.addr) : 0xFF);
        break;
      case rtl::Edge::EFall:
        ++ncyc;
        for (auto& r : regions_) r.dev->wr_en = r.dev->rd_en = false;
        if (log && c.vma && c.addr >= log_lo && c.addr <= log_hi)
          fprintf(log, "%llu %c %04x %02x\n", (unsigned long long)ncyc, c.rw ? 'R' : 'W', c.addr, c.rw ? rdata.q : c.dout);
        if (sel.q >= 0) {
          Device* d = regions_[sel.q].dev;
          d->sa = c.addr;
          if (c.rw) { d->rd_en = true; ++rd_cycles; }
          else      { d->wr_en = true; d->wdata = c.dout; ++wr_cycles; }
        }
        break;
      default: break;
    }
  }
  void commit() override { sel.commit(); rdata.commit(); }

 private:
  const mc6809::Cpu* cpu_;
  std::vector<Region> regions_;
};

// plain read/write RAM (writes are non-blocking: mem[a] <= v)
class Ram : public Device, public rtl::Module {
 public:
  explicit Ram(size_t n) : rtl::Module("ram", rtl::edge_bit(rtl::Edge::EFall)), mem(n) {}
  rtl::NbMem mem;
  uint8_t rd(uint16_t a) override { return mem.rd(a); }
  void eval(rtl::Edge) override { if (wr_en) mem.nb_wr(sa, wdata); }
  void commit() override { mem.commit(); }
};

}  // namespace soc
