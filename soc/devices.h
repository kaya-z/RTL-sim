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

/* A register file that is either private (Level 1) or an alias of bytes inside main memory (Level 2, where
 * v09c keeps the I/O registers in the same mem[] that the MMU maps - the kernel reads them through F$Move). */
typedef struct { RtlNbMem own; RtlNbMem *m; uint32_t base; } RegView;
static inline void rv_init(RegView *v, size_t n) { rtl_nbmem_init(&v->own, n, 0); v->m = &v->own; v->base = 0; }
static inline void rv_alias(RegView *v, RtlNbMem *mem, uint32_t base) { rtl_nbmem_free(&v->own); v->m = mem; v->base = base; }
static inline uint8_t rv_rd(const RegView *v, uint32_t i) { return rtl_nbmem_rd(v->m, v->base + i); }
static inline void rv_poke(RegView *v, uint32_t i, uint8_t x) { rtl_nbmem_poke(v->m, v->base + i, x); }
static inline void rv_nb_wr(RegView *v, uint32_t i, uint8_t x) { rtl_nbmem_nb_wr(v->m, v->base + i, x); }
static inline void rv_commit(RegView *v) { if (v->m == &v->own) rtl_nbmem_commit(&v->own); }   /* an alias is committed by its owner */
static inline void rv_free(RegView *v) { rtl_nbmem_free(&v->own); }

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
  RegView   date;
  uint64_t  ticks;
  FILE     *sched;               /* lock-step schedule log ("T <iteration>") */
  const long *iter;
} Timer;
void timer_init(Timer *t, const McCpu *cpu, uint32_t period, bool aligned, bool fixed_time);
static inline bool timer_irq_asserted(const Timer *t) { return t->flag.q; }

/* ---------------------------------------------------------------------------- */
/* Level 2 memory management (v09c / CoCo-GIME-like): 8 KiB blocks, two maps of 8 DAT bytes.
 *   $FF91        task register: bit 0 = 0 system map ($FFA0-A7), 1 user map ($FFA8-AF)
 *   $FFA0-$FFAF  DAT bytes: physical block number (2 MiB = 256 blocks)
 * Translation is combinational on the bus address; $FE00-$FFFF (I/O page + fixed RAM) is never translated.
 * regs[0..0x1f] mirror $FF90..$FFAF (mem bytes in v09c, so every byte reads back what was written). */
#define MMU_IO_KEY 0x400000u        /* bus key of the untranslated I/O page: MMU_IO_KEY + cpu address */
typedef struct Mmu {
  Device    dev;
  RtlModule mod;
  RegView   regs;
  RTL_REG(uint8_t) task;
} Mmu;
void     mmu_init(Mmu *m);
uint32_t mmu_pa(const uint8_t *map8, uint16_t a);        /* map8: 8 DAT bytes; I/O page -> fixed RAM at 0x70000+a */
static inline uint8_t mmu_dat(const Mmu *m, int task, int blk) { return rv_rd(&m->regs, 0x10 + task * 8 + blk); }
uint32_t mmu_xlat(void *ctx, uint16_t a);                /* bus translation (ctx = Mmu*): key for the bus decoder */

/* ---------------------------------------------------------------------------- */
/* Disk controller register block ($E040..$E04F).  The data movers are host
 * functions (not RTL): sector I/O on raw image files, and the "virtual RBF"
 * used by the /v0 device. */
typedef struct Vdisk Vdisk;
typedef struct DiskCtl {
  Device    dev;
  RtlModule mod;
  RegView   regs;                /* regs[0] = status/command, [1] = drive, [2..4] = LSN, [5,6] = buffer */
  uint64_t  reads, writes, errors;
  RtlNbMem *ram;
  uint32_t  ram_top;
  const Rom *rom;
  FILE     *disk[2];
  Vdisk    *vd;
  const Mmu *mmu;                /* Level 2: DMA goes through the MMU */
  uint8_t   pmmu[8];             /* DAT of the calling process (set by vdisk getDAT) */
} DiskCtl;
enum { SP_SYS, SP_PROC, SP_CUR };    /* address space of a DMA access: system map / caller's DAT / current task map */
void    diskctl_init(DiskCtl *d, RtlNbMem *ram);
void    diskctl_free(DiskCtl *d);
bool    diskctl_attach(DiskCtl *d, int drive, const char *path);
/* backdoor view of memory.  Level 1: RAM below the ROM base, ROM above (the reference model keeps both in one array).
 * Level 2: translated through the system map, the caller's DAT or the current task map. */
uint8_t diskctl_rd_sp(const DiskCtl *d, int space, uint32_t a);
void    diskctl_wr_sp(DiskCtl *d, int space, uint32_t a, uint8_t v);
static inline uint8_t diskctl_peek(const DiskCtl *d, uint32_t a) { return diskctl_rd_sp(d, SP_SYS, a); }
static inline void diskctl_poke(DiskCtl *d, uint32_t a, uint8_t v) { diskctl_wr_sp(d, SP_SYS, a, v); }
static inline uint8_t diskctl_reg(const DiskCtl *d, int i) { return rv_rd(&d->regs, i); }
static inline void diskctl_set_status(DiskCtl *d, uint8_t v) { rv_nb_wr(&d->regs, 0, v); }

/* ---------------------------------------------------------------------------- */
/* The I/O page: address decode inside $E000-$E1FF and the ROM-backed remainder. */
typedef struct IoPage {
  Device    dev;
  RtlModule mod;
  RtlNbMem  own;                 /* Level 1: private 512-byte page */
  RtlNbMem *back;                /* backing bytes of the page: own, or (Level 2) the fixed RAM inside main memory */
  uint32_t  bb;                  /* base of the page inside *back */
  uint32_t  base;                /* register offset of the ACIA: 0 (Level 1, $E000) or 0x180 (Level 2, $FF80) */
  Acia *acia; Timer *timer; DiskCtl *disk; Mmu *mmu;
} IoPage;
void iopage_init(IoPage *p, const uint8_t *rom_image, size_t rom_len, uint32_t rom_base, Acia *a, Timer *t, DiskCtl *d);
/* Level 2: the page lives in `mem` at bb (= physical 0x7FE00); the ROM image has already been loaded there */
void iopage_init_l2(IoPage *p, RtlNbMem *mem, uint32_t bb, Acia *a, Timer *t, Mmu *m, DiskCtl *d);
void iopage_free(IoPage *p);

#endif /* DEVICES_H */
