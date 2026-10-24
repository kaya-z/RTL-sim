/* bus.h - MC6809E system bus (Q/E timing) and device interface (C11)
 *
 * Cycle timing of one bus cycle (matches the MC6809E data-sheet ordering):
 *
 *   EFall(n-1)  CPU registers its next address / R/W / write data  (NBA commit)
 *   QRise       bus decodes the address into a chip-select register (sel)
 *   ERise       selected device puts read data on the bus (rdata register)
 *   EFall       CPU latches rdata; devices latch write data / see read strobe
 *
 * All registers are non-blocking; a device therefore never observes anything
 * the CPU did in the same edge. */
#ifndef BUS_H
#define BUS_H

#include <stdint.h>
#include <stdio.h>
#include "../rtl/rtl.h"
#include "../rtl/mc6809.h"

/* A memory-mapped slave ("interface" = struct of function pointer + strobes).
 *   rd()      combinational read data (a pure function of the device's q registers)
 *   strobes   wires driven by the bus in the E-fall edge (functions of pre-edge
 *             state only).  A device consumes them in its own eval(EFall) and
 *             updates its registers with RTL_NB / nb_wr; the bus module must be
 *             evaluated before the devices (registration order). */
typedef struct Device {
  uint8_t (*rd)(void *self, uint32_t a);          /* a = bus key: physical address (or CPU address without an MMU) */
  void    *self;
  bool     wr_en;          /* write strobe   (E fall, R/W = 0, selected) */
  bool     rd_en;          /* read  strobe   (E fall, R/W = 1, selected): side effects of a read */
  uint32_t sa;             /* bus key / device-relative offset */
  uint8_t  wdata;          /* write data */
} Device;

#define BUS_MAX_REGIONS 8

typedef struct { uint32_t lo, hi; Device *dev; } BusRegion;

typedef struct Bus {
  RtlModule mod;
  const McCpu *cpu;
  BusRegion regions[BUS_MAX_REGIONS];
  int nregions;
  RTL_REG(int16_t) sel;       /* chip-select (index into regions) */
  RTL_REG(uint32_t) pa;       /* bus key latched at Q rise: xlat(addr) if an MMU is present, else addr */
  uint32_t (*xlat)(void *ctx, uint16_t addr);   /* optional address translation (MMU) */
  void *xlat_ctx;
  RTL_REG(uint8_t) rdata;     /* data bus read value */
  uint64_t rd_cycles, wr_cycles, ncyc;
  FILE *log;                  /* optional bus monitor: "cycle R|W addr data" for [log_lo, log_hi] */
  uint32_t log_lo, log_hi;
} Bus;

/* Devices are searched in insertion order; the first that claims the
 * address wins (so overlays such as the I/O page go first). */
static inline void bus_map(Bus *b, uint32_t lo, uint32_t hi, Device *d) {
  b->regions[b->nregions].lo = lo; b->regions[b->nregions].hi = hi; b->regions[b->nregions].dev = d;
  ++b->nregions;
}

static inline void bus_eval(void *self, RtlEdge e) {
  Bus *b = (Bus *)self;
  const McCore *c = mc_cpu_q(b->cpu);
  switch (e) {
    case RTL_QRISE: {
      int16_t s = -1;
      const uint32_t key = b->xlat ? b->xlat(b->xlat_ctx, c->addr) : c->addr;
      if (c->vma) {
        for (int i = 0; i < b->nregions; ++i)
          if (key >= b->regions[i].lo && key <= b->regions[i].hi) { s = (int16_t)i; break; }
      }
      RTL_NB(b->sel, s);
      RTL_NB(b->pa, key);
      break;
    }
    case RTL_ERISE: {
      uint8_t v = 0xFF;
      if (b->sel.q >= 0 && c->rw) { Device *d = b->regions[b->sel.q].dev; v = d->rd(d->self, b->pa.q); }
      RTL_NB(b->rdata, v);
      break;
    }
    case RTL_EFALL:
      ++b->ncyc;
      for (int i = 0; i < b->nregions; ++i) b->regions[i].dev->wr_en = b->regions[i].dev->rd_en = false;
      if (b->log && c->vma && c->addr >= b->log_lo && c->addr <= b->log_hi)
        fprintf(b->log, "%llu %c %04x %02x\n", (unsigned long long)b->ncyc, c->rw ? 'R' : 'W', c->addr,
                c->rw ? b->rdata.q : c->dout);
      if (b->sel.q >= 0) {
        Device *d = b->regions[b->sel.q].dev;
        d->sa = b->pa.q;
        if (c->rw) { d->rd_en = true; ++b->rd_cycles; }
        else       { d->wr_en = true; d->wdata = c->dout; ++b->wr_cycles; }
      }
      break;
    default: break;
  }
}
static inline void bus_commit(void *self) { Bus *b = (Bus *)self; RTL_COMMIT(b->sel); RTL_COMMIT(b->pa); RTL_COMMIT(b->rdata); }

static inline void bus_init(Bus *b, const McCpu *cpu) {
  *b = (Bus){0};
  b->mod.name = "bus";
  b->mod.sens = RTL_EDGE_BIT(RTL_QRISE) | RTL_EDGE_BIT(RTL_ERISE) | RTL_EDGE_BIT(RTL_EFALL);
  b->mod.self = b; b->mod.eval = bus_eval; b->mod.commit = bus_commit;
  b->cpu = cpu;
  RTL_FORCE(b->sel, (int16_t)-1);
  RTL_FORCE(b->rdata, (uint8_t)0xFF);
  b->log_hi = 0xFFFF;
}

/* plain read/write RAM (writes are non-blocking: mem[a] <= v) */
typedef struct Ram {
  Device    dev;
  RtlModule mod;
  RtlNbMem  mem;
} Ram;

static inline uint8_t ram_rd(void *self, uint32_t a) { return rtl_nbmem_rd(&((Ram *)self)->mem, a); }
static inline void ram_eval(void *self, RtlEdge e) {
  (void)e; Ram *r = (Ram *)self;
  if (r->dev.wr_en) rtl_nbmem_nb_wr(&r->mem, r->dev.sa, r->dev.wdata);
}
static inline void ram_commit(void *self) { rtl_nbmem_commit(&((Ram *)self)->mem); }
static inline void ram_init(Ram *r, size_t n) {
  *r = (Ram){0};
  r->dev.rd = ram_rd; r->dev.self = r;
  r->mod.name = "ram"; r->mod.sens = RTL_EDGE_BIT(RTL_EFALL);
  r->mod.self = r; r->mod.eval = ram_eval; r->mod.commit = ram_commit;
  rtl_nbmem_init(&r->mem, n, 0);
}

#endif /* BUS_H */
