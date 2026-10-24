/* Credits / references:
 *   * sbc09 (L.C. Benschop and the sbc09 team, GPLv2; S. Kono's OS-9 fork github.com/shinji-kono/sbc09):
 *     I/O map ($E000 ACIA, $E030 timer/RTC, $E040 disk controller), the pdisk sector protocol and the
 *     /v0 "virtual RBF" command protocol (os9/level1,level2/vrbf.asm, src/io.c, src/vdisk.c) are modelled after it.
 *   * NitrOS-9 / OS-9 Level 1 (Microware, the NitrOS-9 project) is the guest software; it is not distributed here. */

/* devices.h - the peripherals of the sbc09 "v09" board, modelled as clocked RTL (C11)
 *
 *   $E000-$E1FF   I/O page (overlays the ROM)
 *     $E000/$E001   ACIA status / data           (serial console)
 *     $E030-$E036   timer control / date-time    (50 Hz tick, IRQ)
 *     $E040-$E048   disk controller registers    (pdisk sectors, host "vrbf")
 *     others        read as ROM
 *
 * Register map and behaviour follow sbc09 io.c (see README for the citations).
 * Peripherals are cycle-driven: the timer counts E cycles, so simulated time is
 * deterministic and independent of the host speed. */
#ifndef DEVICES_H
#define DEVICES_H

#include <stdint.h>
#include <stdio.h>
#include "../rtl/rtl.h"
#include "../rtl/mc6809.h"
#include "bus.h"
#include "../host/console.h"

/* ---------------------------------------------------------------------------- */
typedef struct Rom {
  Device   dev;
  uint32_t base;
  uint8_t *img; size_t len;
} Rom;

void    rom_init(Rom *r, const uint8_t *img, size_t len, uint32_t base);   /* copies img */
void    rom_free(Rom *r);
uint8_t rom_peek(const Rom *r, uint16_t a);

/* ---------------------------------------------------------------------------- */
/* Serial console.  RX: one-character holding register that is refilled from the
 * host when empty (like a UART receiver FIFO of depth 1). */
typedef struct Acia {
  Device    dev;
  RtlModule mod;
  Console  *con;
  uint32_t  poll_mask, tick;
  RTL_REG(bool)    rx_valid;
  RTL_REG(uint8_t) rx_data;
  RTL_REG(uint8_t) bus_last;     /* data-bus latch: sbc09 returns the last status/data byte when no character is waiting */
  uint64_t tx_count, rx_count;
} Acia;
void acia_init(Acia *a, Console *con, unsigned poll_shift);

/* ---------------------------------------------------------------------------- */
/* 50 Hz timer / real-time clock.
 *   write $8F : (re)start          write $80 : stop        write $04 : latch date/time
 *   read        : last written value with bit4 set by every tick ($E030 powers up as the ROM fill byte)
 *   IRQ (or FIRQ) is a level: set by the tick, cleared by any write to $E030 (acknowledge) */
typedef struct Timer {
  Device    dev;
  RtlModule mod;
  const McCpu *cpu;
  uint32_t  period;
  bool      aligned, fixed;
  RTL_REG(uint32_t) cnt;
  RTL_REG(bool)     run;
  RTL_REG(bool)     flag;        /* interrupt request (level) */
  RTL_REG(uint8_t)  ctl;         /* status/control register as seen by the CPU */
  RtlNbMem  date;
  uint64_t  ticks;
  FILE     *sched;               /* lock-step schedule log ("T <iteration>") */
  const long *iter;
} Timer;
void timer_init(Timer *t, const McCpu *cpu, uint32_t period, bool aligned, bool fixed_time);
static inline bool timer_irq_asserted(const Timer *t) { return t->flag.q; }

/* ---------------------------------------------------------------------------- */
/* Disk controller register block ($E040..$E04F).  The data movers are host
 * functions (not RTL): sector I/O on raw image files, and the "virtual RBF"
 * used by the /v0 device. */
typedef struct Vdisk Vdisk;
typedef struct DiskCtl {
  Device    dev;
  RtlModule mod;
  RtlNbMem  regs;                /* regs[0] = status/command, [1] = drive, [2..4] = LSN, [5,6] = buffer */
  uint64_t  reads, writes, errors;
  RtlNbMem *ram;
  uint32_t  ram_top;
  const Rom *rom;
  FILE     *disk[2];
  Vdisk    *vd;
} DiskCtl;
void    diskctl_init(DiskCtl *d, RtlNbMem *ram);
void    diskctl_free(DiskCtl *d);
bool    diskctl_attach(DiskCtl *d, int drive, const char *path);
/* backdoor view of the 64K space: RAM below the ROM base, ROM above (the reference model keeps both in one array) */
static inline uint8_t diskctl_peek(const DiskCtl *d, uint32_t a) {
  a &= 0xFFFF;
  return (a < d->ram_top || !d->rom) ? rtl_nbmem_rd(d->ram, a) : rom_peek(d->rom, (uint16_t)a);
}
static inline void diskctl_poke(DiskCtl *d, uint32_t a, uint8_t v) {
  if ((a & 0xFFFF) < d->ram_top) rtl_nbmem_nb_wr(d->ram, a & 0xFFFF, v);
}
static inline uint8_t diskctl_reg(const DiskCtl *d, int i) { return rtl_nbmem_rd(&d->regs, i); }
static inline void diskctl_set_status(DiskCtl *d, uint8_t v) { rtl_nbmem_nb_wr(&d->regs, 0, v); }

/* ---------------------------------------------------------------------------- */
/* The I/O page: address decode inside $E000-$E1FF and the ROM-backed remainder. */
typedef struct IoPage {
  Device    dev;
  RtlModule mod;
  RtlNbMem  back;
  Acia *acia; Timer *timer; DiskCtl *disk;
} IoPage;
void iopage_init(IoPage *p, const uint8_t *rom_image, size_t rom_len, uint32_t rom_base, Acia *a, Timer *t, DiskCtl *d);
void iopage_free(IoPage *p);

#endif /* DEVICES_H */
