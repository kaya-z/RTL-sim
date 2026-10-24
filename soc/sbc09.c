#include "sbc09.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *slurp(const char *path, size_t *len) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  size_t cap = 65536, n = 0;
  uint8_t *b = (uint8_t *)malloc(cap);
  size_t r;
  while ((r = fread(b + n, 1, cap - n, f)) > 0) {
    n += r;
    if (n == cap) { cap *= 2; b = (uint8_t *)realloc(b, cap); }
  }
  fclose(f);
  *len = n;
  return b;
}

static void sbc09_settle(void *ctx) {
  Sbc09 *s = (Sbc09 *)ctx;
  s->pins.din = s->bus.rdata.q;
  s->pins.reset_n = s->sim.clk.cycle >= s->cfg.reset_cycles;
  s->pins.halt_n = !s->halt_req;
  const bool tirq = timer_irq_asserted(&s->timer);
  s->pins.irq_n = !(tirq && !s->cfg.timer_firq);
  s->pins.firq_n = !((tirq && s->cfg.timer_firq) || s->firq_req);
  s->pins.nmi_n = !s->nmi_req;
}

bool sbc09_init(Sbc09 *s, const Sbc09Config *cfg, Console *con) {
  memset(s, 0, sizeof *s);
  s->cfg = *cfg;
  s->ok = false;
  mc_pins_init(&s->pins);
  mc_cpu_init(&s->cpu, &s->pins);
  bus_init(&s->bus, &s->cpu);
  ram_init(&s->ram, cfg->level2 ? 2u * 1024 * 1024 : 65536);
  acia_init(&s->acia, con, cfg->rx_poll_shift);
  timer_init(&s->timer, &s->cpu, cfg->tick_cycles, cfg->tick_aligned, cfg->fixed_time);
  diskctl_init(&s->disk, &s->ram.mem);
  rtl_sim_init(&s->sim);

  size_t len = 0;
  uint8_t *img = slurp(cfg->rom, &len);
  if (!img) { fprintf(stderr, "sbc09: cannot read ROM image '%s'\n", cfg->rom); return false; }
  if (cfg->level2) {
    /* v09c: the image is loaded at CPU $E000 of the boot map = physical $7E000 and may run on into higher blocks;
     * the top 512 bytes of block $3F are the fixed RAM / I/O page */
    mmu_init(&s->mmu);
    const uint32_t load = 0x7E000, iob = 0x7FE00;
    for (size_t i = 0; i < len && load + i < s->ram.mem.n; ++i) rtl_nbmem_poke(&s->ram.mem, (uint32_t)(load + i), img[i]);
    iopage_init_l2(&s->io, &s->ram.mem, iob, &s->acia, &s->timer, &s->mmu, &s->disk);
    s->disk.mmu = &s->mmu;
    /* registers power up with the memory bytes underneath them, then v09c patches $FF90 and the system map */
    rv_alias(&s->timer.date, &s->ram.mem, iob + 0x1B0);       /* the registers ARE memory bytes (as in v09c) */
    rv_alias(&s->disk.regs, &s->ram.mem, iob + 0x1C0);
    rv_alias(&s->mmu.regs, &s->ram.mem, iob + 0x190);
    rv_poke(&s->mmu.regs, 0, 0);          /* mem[$FF90] = 0 */
    rv_poke(&s->mmu.regs, 0x17, 0x3F);    /* mem[$FFA7] = $3F : block 7 of the system map = the boot ROM block */
    RTL_FORCE(s->timer.ctl, rtl_nbmem_rd(&s->ram.mem, iob + 0x1B0));
    s->bus.xlat = mmu_xlat; s->bus.xlat_ctx = &s->mmu;
    bus_map(&s->bus, MMU_IO_KEY + 0xFE00, MMU_IO_KEY + 0xFFFF, &s->io.dev);
    bus_map(&s->bus, 0, 0x1FFFFF, &s->ram.dev);
    s->rom_base = 0xFE00;
  } else {
  s->rom_base = cfg->rom_base >= 0 ? (uint32_t)cfg->rom_base : (uint32_t)(0x10000 - len);
  rom_init(&s->rom, img, len, s->rom_base);
  iopage_init(&s->io, img, len, s->rom_base, &s->acia, &s->timer, &s->disk);
  s->disk.ram_top = s->rom_base;                 /* RAM ends where the ROM starts */
  s->disk.rom = &s->rom;
  /* registers power up with the ROM bytes that sit under the I/O page (as in the reference model) */
  for (uint16_t i = 0; i < 16; ++i) {
    rv_poke(&s->timer.date, i, rtl_nbmem_rd(&s->io.own, 0x30 + i));
    rv_poke(&s->disk.regs, i, rtl_nbmem_rd(&s->io.own, 0x40 + i));
  }
  RTL_FORCE(s->timer.ctl, rtl_nbmem_rd(&s->io.own, 0x30));
  /* address map: I/O page overlays the ROM; RAM below the ROM base */
  bus_map(&s->bus, 0xE000, 0xE1FF, &s->io.dev);
  bus_map(&s->bus, s->rom_base, 0xFFFF, &s->rom.dev);
  bus_map(&s->bus, 0x0000, s->rom_base - 1, &s->ram.dev);
  }
  free(img);
  for (int d = 0; d < 2; ++d)
    if (cfg->disk[d] && cfg->disk[d][0] && !diskctl_attach(&s->disk, d, cfg->disk[d]))
      fprintf(stderr, "sbc09: cannot open disk image '%s'\n", cfg->disk[d]);
  if (cfg->vdisk_root && cfg->vdisk_root[0]) {
    s->vdisk = vdisk_new(cfg->vdisk_root);
    s->disk.vd = s->vdisk;
  }

  /* registration order matters: bus first (drives the strobes), then the devices */
  rtl_sim_add(&s->sim, &s->cpu.mod);
  rtl_sim_add(&s->sim, &s->bus.mod);
  rtl_sim_add(&s->sim, &s->ram.mod);
  rtl_sim_add(&s->sim, &s->io.mod);
  rtl_sim_add(&s->sim, &s->acia.mod);
  rtl_sim_add(&s->sim, &s->timer.mod);
  if (cfg->level2) rtl_sim_add(&s->sim, &s->mmu.mod);
  rtl_sim_add(&s->sim, &s->disk.mod);
  rtl_sim_on_settle(&s->sim, sbc09_settle, s);
  sbc09_settle(s);
  s->ok = true;
  return true;
}

void sbc09_free(Sbc09 *s) {
  vdisk_free(s->vdisk); s->vdisk = NULL;
  diskctl_free(&s->disk);
  iopage_free(&s->io);
  rom_free(&s->rom);
  rv_free(&s->timer.date); rv_free(&s->disk.regs); if (s->cfg.level2) rv_free(&s->mmu.regs);
  rtl_nbmem_free(&s->ram.mem);
}
