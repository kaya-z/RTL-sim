/* flatsys.h - MC6809 core + 64 KiB flat RAM, with a cycle-accurate bus recorder (C11).
 * Shared by the directed unit tests (test_cpu.c) and the lock-step harness. */
#ifndef FLATSYS_H
#define FLATSYS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../rtl/rtl.h"
#include "../rtl/mc6809.h"
#include "../soc/bus.h"

typedef struct {
  uint64_t cycle;
  uint16_t addr;
  bool     rw;        /* true = read */
  bool     vma;
  uint8_t  data;      /* read data or write data */
  bool     lic;
  bool     bs, ba;
} BusCycle;

typedef struct FlatSys {
  McPins   pins;
  McCpu    cpu;
  Bus      bus;
  Ram      ram;
  RtlSim   sim;
  bool     reset_hold;            /* RESET asserted until release() */
  bool     record;
  BusCycle *cycles; size_t ncycles, cap_cycles;   /* one entry per bus cycle while record == true */
  long     insts;                 /* instruction boundaries seen (ev_inst) */
  long     ints;                  /* interrupt sequences started (ev_int) */
} FlatSys;

static inline void flat_settle(void *ctx) {
  FlatSys *s = (FlatSys *)ctx;
  s->pins.din = s->bus.rdata.q;
  s->pins.reset_n = !s->reset_hold;
}
static inline void flat_edge_end(void *ctx, RtlEdge e) {
  FlatSys *s = (FlatSys *)ctx;
  if (e != RTL_EFALL) return;
  const McCore *c = mc_cpu_q(&s->cpu);
  if (c->ev_inst) ++s->insts;
  if (c->ev_int) ++s->ints;
}
/* The object holds pointers to itself: initialise in place, never move. */
static inline void flat_init(FlatSys *s) {
  memset(s, 0, sizeof *s);
  s->reset_hold = true;
  mc_pins_init(&s->pins);
  mc_cpu_init(&s->cpu, &s->pins);
  bus_init(&s->bus, &s->cpu);
  ram_init(&s->ram, 65536);
  rtl_sim_init(&s->sim);
  bus_map(&s->bus, 0, 0xFFFF, &s->ram.dev);
  rtl_sim_add(&s->sim, &s->cpu.mod); rtl_sim_add(&s->sim, &s->bus.mod); rtl_sim_add(&s->sim, &s->ram.mod);
  rtl_sim_on_settle(&s->sim, flat_settle, s);
  rtl_sim_settle_on(&s->sim, RTL_EDGE_BIT(RTL_ERISE) | RTL_EDGE_BIT(RTL_EFALL));
  rtl_sim_on_edge_end_mask(&s->sim, flat_edge_end, s, RTL_EDGE_BIT(RTL_EFALL));
  flat_settle(s);
}
static inline void flat_free(FlatSys *s) {
  rtl_nbmem_free(&s->ram.mem);
  free(s->cycles); s->cycles = NULL;
}

/* Run n E-cycles; records (addr, rw, data) of every completed bus cycle if `record`. */
static inline void flat_run(FlatSys *s, uint64_t n) {
  for (uint64_t i = 0; i < n; ++i) {
    if (s->record) {
      /* sample the bus at the end of the cycle (just before E falls): the CPU is about to latch rdata */
      for (int k = 0; k < 3; ++k) rtl_sim_step_edge(&s->sim);
      const McCore *c = mc_cpu_q(&s->cpu);
      if (s->ncycles == s->cap_cycles) {
        s->cap_cycles = s->cap_cycles ? s->cap_cycles * 2 : 1024;
        s->cycles = (BusCycle *)realloc(s->cycles, s->cap_cycles * sizeof *s->cycles);
      }
      s->cycles[s->ncycles++] = (BusCycle){s->sim.clk.cycle, c->addr, c->rw, c->vma,
                                           c->rw ? s->bus.rdata.q : c->dout, c->lic, c->bs, c->ba};
      rtl_sim_step_edge(&s->sim);
    } else {
      rtl_sim_step_cycle(&s->sim);
    }
  }
}
static inline void flat_release(FlatSys *s, uint64_t after_cycles) { flat_run(s, after_cycles); s->reset_hold = false; flat_settle(s); }

static inline void flat_poke(FlatSys *s, uint16_t a, uint8_t v) { rtl_nbmem_poke(&s->ram.mem, a, v); }
static inline uint8_t flat_peek(const FlatSys *s, uint16_t a) { return rtl_nbmem_rd(&s->ram.mem, a); }
static inline uint16_t flat_peek16(const FlatSys *s, uint16_t a) { return (uint16_t)((flat_peek(s, a) << 8) | flat_peek(s, (uint16_t)(a + 1))); }
static inline void flat_vectors(FlatSys *s, uint16_t reset) {
  const struct { uint16_t a, h; } v[] = { {0xFFFE, reset}, {0xFFF8, 0x2000}, {0xFFF6, 0x2100}, {0xFFFC, 0x2200},
                                           {0xFFFA, 0x2300}, {0xFFF4, 0x2400}, {0xFFF2, 0x2500} };
  for (size_t i = 0; i < sizeof v / sizeof v[0]; ++i) { flat_poke(s, v[i].a, v[i].h >> 8); flat_poke(s, (uint16_t)(v[i].a + 1), v[i].h & 0xFF); }
}
static inline const McCore *flat_core(const FlatSys *s) { return mc_cpu_q(&s->cpu); }

/* run until the core is about to fetch the instruction at `pc` (or give up after max cycles) */
static inline bool flat_run_to(FlatSys *s, uint16_t pc, uint64_t max) {
  for (uint64_t i = 0; i < max; ++i) {
    rtl_sim_step_cycle(&s->sim);
    const McCore *c = mc_cpu_q(&s->cpu);
    if (c->ev_inst && c->pc == pc) return true;
  }
  return false;
}

#endif
