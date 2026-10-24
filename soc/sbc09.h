/* Credits / references:
 *   * sbc09 (L.C. Benschop and the sbc09 team, GPLv2; S. Kono's OS-9 fork github.com/shinji-kono/sbc09):
 *     I/O map ($E000 ACIA, $E030 timer/RTC, $E040 disk controller), the pdisk sector protocol and the
 *     /v0 "virtual RBF" command protocol (os9/level1,level2/vrbf.asm, src/io.c, src/vdisk.c) are modelled after it.
 *   * NitrOS-9 / OS-9 Level 1 (Microware, the NitrOS-9 project) is the guest software; it is not distributed here. */

/* sbc09.h - the complete v09-style single board computer (MC6809E + RAM/ROM + I/O) (C11) */
#ifndef SBC09_H
#define SBC09_H

#include <stdbool.h>
#include <stdint.h>
#include "../rtl/rtl.h"
#include "../rtl/mc6809.h"
#include "bus.h"
#include "devices.h"
#include "../host/console.h"
#include "../host/vdisk.h"

typedef struct {
  const char *rom;                 /* ROM image (loaded at the top of the 64K space unless rom_base is set) */
  int32_t     rom_base;            /* -1: 0x10000 - size */
  const char *disk[2];             /* raw sector images for drives 0/1 (NULL: none) */
  const char *vdisk_root;          /* host directory for /v0 (NULL: none) */
  bool        level2;              /* Level 2 board (v09c): MMU, 2 MiB RAM, I/O page at $FF80, ROM image at physical $7E000 */
  bool        timer_firq;          /* wire the timer to FIRQ instead of IRQ */
  uint32_t    tick_cycles;         /* E cycles per tick (default 1000000).  20000 = 50 Hz at 1 MHz, but this sbc09 OS-9
                                      guest then garbles its output (also seen on the original v09 with a fast timer) */
  bool        tick_aligned;        /* test mode: ticks only between instructions */
  bool        fixed_time;          /* deterministic RTC */
  unsigned    rx_poll_shift;       /* poll the host every 2^n cycles for a received character */
  unsigned    reset_cycles;
} Sbc09Config;

static inline Sbc09Config sbc09_config_default(void) {
  Sbc09Config c = {0};
  c.rom_base = -1; c.tick_cycles = 1000000; c.rx_poll_shift = 8; c.reset_cycles = 4;
  return c;
}

typedef struct Sbc09 {
  McPins   pins;
  McCpu    cpu;
  Bus      bus;
  Ram      ram;
  Rom      rom;
  Acia     acia;
  Timer    timer;
  Mmu      mmu;                    /* Level 2 only */
  DiskCtl  disk;
  IoPage   io;
  Vdisk   *vdisk;
  RtlSim   sim;
  uint32_t rom_base;
  bool     halt_req;               /* external HALT (active high request) */
  bool     nmi_req, firq_req;
  bool     ok;
  Sbc09Config cfg;
} Sbc09;

/* The object holds pointers to itself: initialise it in place and never move it. */
bool sbc09_init(Sbc09 *s, const Sbc09Config *cfg, Console *con);
void sbc09_free(Sbc09 *s);
static inline void sbc09_step_cycle(Sbc09 *s) { rtl_sim_step_cycle(&s->sim); }
static inline uint64_t sbc09_cycles(const Sbc09 *s) { return s->sim.clk.cycle; }
static inline const McCore *sbc09_core(const Sbc09 *s) { return mc_cpu_q(&s->cpu); }

#endif
