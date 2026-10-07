// Credits / references:
//   * sbc09 (L.C. Benschop and the sbc09 team, GPLv2; S. Kono's OS-9 fork github.com/shinji-kono/sbc09):
//     I/O map ($E000 ACIA, $E030 timer/RTC, $E040 disk controller), the pdisk sector protocol and the
//     /v0 "virtual RBF" command protocol (os9/level*/vrbf.asm, src/io.c, src/vdisk.c) are modelled after it.
//   * NitrOS-9 / OS-9 Level 1 (Microware, the NitrOS-9 project) is the guest software; it is not distributed here.
// sbc09.h - the complete v09-style single board computer (MC6809E + RAM/ROM + I/O)
#pragma once
#include <memory>
#include <string>
#include <vector>
#include "../rtl/rtl.h"
#include "../rtl/mc6809.h"
#include "bus.h"
#include "devices.h"
#include "../host/console.h"
#include "../host/vdisk.h"

namespace soc {

struct Config {
  std::string rom;                 // ROM image (loaded at the top of the 64K space unless rom_base is set)
  int32_t rom_base = -1;           // -1: 0x10000 - size
  std::string disk[2];             // raw sector images for drives 0/1
  std::string vdisk_root;          // host directory for /v0 (empty: none)
  bool timer_firq = false;         // wire the timer to FIRQ instead of IRQ
  uint32_t tick_cycles = 1000000;   // E cycles per tick (50 Hz at a 50 MHz E clock). 20000 = 50 Hz at 1 MHz, but this sbc09 OS-9 guest then garbles its output (also seen on the original v09 with a fast timer)
  bool tick_aligned = false;       // test mode: ticks only between instructions
  bool fixed_time = false;         // deterministic RTC
  unsigned rx_poll_shift = 8;      // poll the host every 2^n cycles for a received character
  unsigned reset_cycles = 4;
};

class Sbc09 {
 public:
  Sbc09(const Config& cfg, host::Console& con);

  mc6809::Pins pins;
  mc6809::Cpu cpu;
  Bus bus;
  Ram ram;
  std::unique_ptr<Rom> rom;
  Acia acia;
  Timer timer;
  DiskCtl disk;
  std::unique_ptr<IoPage> io;
  std::unique_ptr<Vdisk> vdisk;
  rtl::Simulator sim;
  uint32_t rom_base = 0x10000;

  bool halt_req = false;           // external HALT (active high request)
  bool nmi_req = false, firq_req = false;
  bool ok() const { return ok_; }

  void step_cycle() { sim.step_cycle(); }
  uint64_t cycles() const { return sim.clk().cycle; }
  const mc6809::Core& core() const { return cpu.q(); }

  bool load_ok = true;
 private:
  void settle();
  bool ok_ = true;
  Config cfg_;
};

}  // namespace soc
