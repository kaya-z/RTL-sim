#include "sbc09.h"
#include <cstdio>
#include <fstream>
#include <iterator>

namespace soc {

static std::vector<uint8_t> slurp(const std::string& path, bool* ok) {
  std::ifstream f(path, std::ios::binary);
  if (!f) { *ok = false; return {}; }
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static uint32_t rom_base_of(const Config& c, size_t len) {
  return c.rom_base >= 0 ? static_cast<uint32_t>(c.rom_base) : static_cast<uint32_t>(0x10000 - len);
}

Sbc09::Sbc09(const Config& cfg, host::Console& con)
    : cpu(&pins),
      bus(&cpu),
      ram(65536),
      acia(con, cfg.rx_poll_shift),
      timer(&cpu, cfg.tick_cycles, cfg.tick_aligned, cfg.fixed_time),
      disk(&ram.mem),
      cfg_(cfg) {
  bool ok = true;
  std::vector<uint8_t> img = slurp(cfg.rom, &ok);
  if (!ok) { std::fprintf(stderr, "sbc09: cannot read ROM image '%s'\n", cfg.rom.c_str()); ok_ = false; return; }
  rom_base = rom_base_of(cfg, img.size());
  rom = std::make_unique<Rom>(img, rom_base);
  io = std::make_unique<IoPage>(img, rom_base, &acia, &timer, &disk);
  disk.set_ram_top(rom_base);                 // RAM ends where the ROM starts
  disk.set_rom(rom.get());
  // registers power up with the ROM bytes that sit under the I/O page (as in the reference model)
  for (uint16_t i = 0; i < 16; ++i) {
    timer.date.poke(i, io->back.rd(0x30 + i));
    disk.regs.poke(i, io->back.rd(0x40 + i));
  }
  timer.ctl.force(io->back.rd(0x30));  for (int d = 0; d < 2; ++d)
    if (!cfg.disk[d].empty() && !disk.attach(d, cfg.disk[d].c_str()))
      std::fprintf(stderr, "sbc09: cannot open disk image '%s'\n", cfg.disk[d].c_str());
  if (!cfg.vdisk_root.empty()) {
    vdisk = std::make_unique<Vdisk>(cfg.vdisk_root);
    disk.set_vdisk(vdisk.get());
  }

  // address map: I/O page overlays the ROM; RAM below the ROM base
  bus.map(0xE000, 0xE1FF, io.get());
  bus.map(rom_base, 0xFFFF, rom.get());
  bus.map(0x0000, rom_base - 1, &ram);

  // registration order matters: bus first (drives the strobes), then the devices
  sim.add(&cpu);
  sim.add(&bus);
  sim.add(&ram);
  sim.add(io.get());
  sim.add(&acia);
  sim.add(&timer);
  sim.add(&disk);
  sim.on_settle([](void* c) { static_cast<Sbc09*>(c)->settle(); }, this);
  settle();
}

void Sbc09::settle() {
  pins.din = bus.rdata.q;
  pins.reset_n = sim.clk().cycle >= cfg_.reset_cycles;
  pins.halt_n = !halt_req;
  const bool tirq = timer.irq_asserted();
  pins.irq_n = !(tirq && !cfg_.timer_firq);
  pins.firq_n = !((tirq && cfg_.timer_firq) || firq_req);
  pins.nmi_n = !nmi_req;
}

}  // namespace soc
