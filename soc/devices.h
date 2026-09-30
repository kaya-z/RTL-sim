// devices.h - the peripherals of the sbc09 "v09" board, modelled as clocked RTL
//
//   $E000-$E1FF   I/O page (overlays the ROM)
//     $E000/$E001   ACIA status / data           (serial console)
//     $E030-$E036   timer control / date-time    (50 Hz tick, IRQ)
//     $E040-$E048   disk controller registers    (pdisk sectors, host "vrbf")
//     others        read as ROM
//
// Register map and behaviour follow sbc09 io.c (see README for the citations).
// Peripherals are cycle-driven: the timer counts E cycles, so simulated time is
// deterministic and independent of the host speed.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include "../rtl/rtl.h"
#include "../rtl/mc6809.h"
#include "bus.h"
#include "../host/console.h"

namespace soc {

// ---------------------------------------------------------------------------
class Rom : public Device {
 public:
  Rom(const std::vector<uint8_t>& img, uint32_t base) : base_(base), img_(img) {}
  uint8_t rd(uint16_t a) override { return peek(a); }
  uint8_t peek(uint16_t a) const { uint32_t o = a - base_; return o < img_.size() ? img_[o] : 0xFF; }
  const std::vector<uint8_t>& image() const { return img_; }
 private:
  uint32_t base_;
  std::vector<uint8_t> img_;
};

// ---------------------------------------------------------------------------
// Serial console.  RX: one-character holding register that is refilled from the
// host when empty (like a UART receiver FIFO of depth 1).
class Acia : public Device, public rtl::Module {
 public:
  Acia(host::Console& con, unsigned poll_shift)
      : rtl::Module("acia", rtl::edge_bit(rtl::Edge::EFall)), con_(con), poll_mask_((1u << poll_shift) - 1) {}
  rtl::Reg<bool> rx_valid{false};
  rtl::Reg<uint8_t> rx_data{0};
  rtl::Reg<uint8_t> bus_last{0};     // data-bus latch: sbc09 returns the last status/data byte when no character is waiting
  uint64_t tx_count = 0, rx_count = 0;

  uint8_t status() const { return static_cast<uint8_t>(2 | (rx_valid.q ? 1 : 0)); }   // TDRE always set, RDRF = rx_valid
  uint8_t rd(uint16_t off) override { return off == 0 ? status() : (rx_valid.q ? rx_data.q : bus_last.q); }
  void eval(rtl::Edge) override {
    const bool consume = rd_en && sa == 1 && rx_valid.q;
    if (rd_en && sa == 0) bus_last.nb(status());
    if (consume) { rx_valid.nb(false); bus_last.nb(rx_data.q); ++rx_count; }
    else if (!rx_valid.q && ((++tick_ & poll_mask_) == 0)) {
      int c = con_.poll();
      if (c >= 0) { rx_data.nb(static_cast<uint8_t>(c)); rx_valid.nb(true); }
    }
    if (wr_en && sa == 1) { con_.put(wdata); ++tx_count; }
  }
  void commit() override { rx_valid.commit(); rx_data.commit(); bus_last.commit(); }
 private:
  host::Console& con_;
  uint32_t poll_mask_;
  uint32_t tick_ = 0;
};

// ---------------------------------------------------------------------------
// 50 Hz timer / real-time clock.
//   write $8F : (re)start          write $80 : stop        write $04 : latch date/time
//   read        : last written value with bit4 set by every tick ($E030 powers up as the ROM fill byte)
//   IRQ (or FIRQ) is a level: set by the tick, cleared by any write to $E030 (acknowledge)
class Timer : public Device, public rtl::Module {
 public:
  Timer(const mc6809::Cpu* cpu, uint32_t period, bool aligned, bool fixed_time)
      : rtl::Module("timer", rtl::edge_bit(rtl::Edge::EFall)), date(16), cpu_(cpu), period_(period),
        aligned_(aligned), fixed_(fixed_time) {}
  rtl::Reg<uint32_t> cnt{0};
  rtl::Reg<bool> run{false};
  rtl::Reg<bool> flag{false};       // interrupt request (level)
  rtl::Reg<uint8_t> ctl{0xFF};      // status/control register as seen by the CPU
  rtl::NbMem date;
  uint64_t ticks = 0;
  FILE* sched = nullptr;        // lock-step schedule log ("T <iteration>")
  const long* iter = nullptr;

  bool irq_asserted() const { return flag.q; }
  uint8_t rd(uint16_t off) override { return off == 0 ? ctl.q : date.rd(off & 15); }
  void eval(rtl::Edge) override;
  void commit() override { cnt.commit(); run.commit(); flag.commit(); ctl.commit(); date.commit(); }
 private:
  void latch_date();
  const mc6809::Cpu* cpu_;
  uint32_t period_;
  bool aligned_, fixed_;
};

// ---------------------------------------------------------------------------
// Disk controller register block ($E040..$E04F).  The data movers are host
// functions (not RTL): sector I/O on raw image files, and the "virtual RBF"
// used by the /v0 device.
class Vdisk;
class DiskCtl : public Device, public rtl::Module {
 public:
  explicit DiskCtl(rtl::NbMem* ram)
      : rtl::Module("diskctl", rtl::edge_bit(rtl::Edge::EFall)), regs(16), ram_(ram) {}
  void set_ram_top(uint32_t top) { ram_top_ = top; }
  ~DiskCtl();
  rtl::NbMem regs;               // regs[0] = status/command, [1] = drive, [2..4] = LSN, [5,6] = buffer
  uint64_t reads = 0, writes = 0, errors = 0;

  bool attach(int drive, const char* path);
  void set_vdisk(Vdisk* v) { vd_ = v; }
  uint8_t rd(uint16_t off) override { return regs.rd(off & 15); }
  void eval(rtl::Edge) override;
  void commit() override { regs.commit(); }
  // host DMA helpers
  void set_rom(const Rom* r) { rom_ = r; }
  // backdoor view of the 64K space: RAM below the ROM base, ROM above (the reference model keeps both in one array)
  uint8_t peek(uint32_t a) const {
    a &= 0xFFFF;
    return (a < ram_top_ || !rom_) ? ram_->rd(a) : rom_->peek(static_cast<uint16_t>(a));
  }
  void poke(uint32_t a, uint8_t v) { if ((a & 0xFFFF) < ram_top_) ram_->nb_wr(a & 0xFFFF, v); }
  uint8_t reg(int i) const { return regs.rd(i); }
  void set_status(uint8_t v) { regs.nb_wr(0, v); }
 private:
  void sector_io(uint8_t cmd);
  rtl::NbMem* ram_;
  uint32_t ram_top_ = 0x10000;
  const Rom* rom_ = nullptr;
  FILE* disk_[2] = {nullptr, nullptr};
  Vdisk* vd_ = nullptr;
};

// ---------------------------------------------------------------------------
// The I/O page: address decode inside $E000-$E1FF and the ROM-backed remainder.
class IoPage : public Device, public rtl::Module {
 public:
  IoPage(const std::vector<uint8_t>& rom_image, uint32_t rom_base, Acia* a, Timer* t, DiskCtl* d)
      : rtl::Module("iopage", rtl::edge_bit(rtl::Edge::EFall)), back(0x200), acia_(a), timer_(t), disk_(d) {
    for (uint32_t i = 0; i < 0x200; ++i) {
      uint32_t o = 0xE000 + i - rom_base;
      back.poke(i, o < rom_image.size() ? rom_image[o] : 0xFF);
    }
  }
  rtl::NbMem back;
  uint8_t rd(uint16_t a) override {
    const uint16_t o = a & 0x1FF;
    if (o < 2) return acia_->rd(o);
    if (o >= 0x30 && o < 0x40) return timer_->rd(o - 0x30);
    if (o >= 0x40 && o < 0x50) return disk_->rd(o - 0x40);
    return back.rd(o);
  }
  void eval(rtl::Edge) override {
    // forward the strobes to the selected sub-device (only during E fall)
    acia_->wr_en = acia_->rd_en = timer_->wr_en = timer_->rd_en = disk_->wr_en = disk_->rd_en = false;
    if (!(wr_en || rd_en)) return;
    const uint16_t o = sa & 0x1FF;
    Device* d = nullptr; uint16_t off = 0;
    if (o < 2) { d = acia_; off = o; }
    else if (o >= 0x30 && o < 0x40) { d = timer_; off = o - 0x30; }
    else if (o >= 0x40 && o < 0x50) { d = disk_; off = o - 0x40; }
    else if (wr_en && o >= 0x50) back.nb_wr(o, wdata);           // sbc09: writes above $E040 land in the page
    if (d) { d->wr_en = wr_en; d->rd_en = rd_en; d->sa = off; d->wdata = wdata; }
  }
  void commit() override { back.commit(); }
 private:
  Acia* acia_; Timer* timer_; DiskCtl* disk_;
};

}  // namespace soc
