/* rtl.h - minimal RTL simulation kernel (C11)
 *
 * Emulates the parts of Verilog/SystemVerilog scheduling semantics that a
 * purely functional ("instruction at a time") simulator does not have:
 *
 *   * clock edges        : every process is triggered by an explicit edge
 *   * non-blocking assign: RTL_NB(r, v) writes the *next* value (d); the
 *                          visible value (q) only changes in the NBA region
 *   * two-region step    : (1) active region  - every triggered process runs
 *                              and sees only pre-edge q values,
 *                          (2) NBA region     - all registers commit q <= d
 *                          (3) settle         - combinational nets are re-derived
 *
 * A process may therefore never observe a value written by another process
 * in the same edge, exactly like `always @(posedge clk) r <= f(r, other)`.
 */
#ifndef RTL_H
#define RTL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

/* ---------------------------------------------------------------------------
 * Register: q = value visible to readers, d = value being assigned.
 * After commit d == q, so an un-assigned register holds (flip-flop w/ enable).
 *
 *     RTL_REG(uint16_t) pc;          // declaration
 *     RTL_NB(pc, pc.q + 1);          // pc <= pc + 1
 *     RTL_COMMIT(pc);                // NBA region
 */
#define RTL_REG(T)        struct { T q; T d; }
#define RTL_NB(r, v)      ((r).d = (v))
#define RTL_COMMIT(r)     ((r).q = (r).d)
#define RTL_FORCE(r, v)   ((r).q = (r).d = (v))     /* initial value / $deposit */

/* ---------------------------------------------------------------------------
 * Memory array whose writes are non-blocking (array[a] <= v). */
typedef struct {
  uint8_t *a;
  size_t   n;
  struct RtlNbW { uint32_t adr; uint8_t v; } *pend;
  size_t   np, cap;
} RtlNbMem;

static inline void rtl_nbmem_init(RtlNbMem *m, size_t n, uint8_t fill) {
  m->a = (uint8_t *)malloc(n ? n : 1);
  for (size_t i = 0; i < n; ++i) m->a[i] = fill;
  m->n = n; m->pend = NULL; m->np = m->cap = 0;
}
static inline void rtl_nbmem_free(RtlNbMem *m) { free(m->a); free(m->pend); m->a = NULL; m->pend = NULL; }
static inline uint8_t rtl_nbmem_rd(const RtlNbMem *m, uint32_t adr) { return m->a[adr]; }
static inline void rtl_nbmem_poke(RtlNbMem *m, uint32_t adr, uint8_t v) { m->a[adr] = v; }  /* initial load only */
static inline void rtl_nbmem_nb_wr(RtlNbMem *m, uint32_t adr, uint8_t v) {
  if (adr >= m->n) return;
  if (m->np == m->cap) {
    m->cap = m->cap ? m->cap * 2 : 64;
    m->pend = (struct RtlNbW *)realloc(m->pend, m->cap * sizeof *m->pend);
  }
  m->pend[m->np].adr = adr; m->pend[m->np].v = v; ++m->np;
}
static inline void rtl_nbmem_commit(RtlNbMem *m) {
  for (size_t i = 0; i < m->np; ++i) m->a[m->pend[i].adr] = m->pend[i].v;
  m->np = 0;
}

/* ---------------------------------------------------------------------------
 * Clock: MC6809E style quadrature clock.  Q leads E by 90 degrees.
 *
 *      phase :   0     1     2     3
 *      Q     : _____|-----------|_____      Q rises at phase 1 boundary
 *      E     : ___________|-----------      E rises at phase 2 boundary
 *
 * One bus cycle = one full E period = 4 edges:  QRise -> ERise -> QFall -> EFall */
typedef enum { RTL_QRISE = 0, RTL_ERISE = 1, RTL_QFALL = 2, RTL_EFALL = 3 } RtlEdge;
#define RTL_EDGE_BIT(e) (1u << (unsigned)(e))

typedef struct {
  bool q, e;
  uint64_t cycle;                   /* completed E cycles */
} RtlClock;

/* ---------------------------------------------------------------------------
 * Module: anything with processes + registers ("C class" = struct + function
 * pointers; concrete devices embed an RtlModule and pass themselves as `self`). */
typedef struct RtlModule {
  const char *name;
  unsigned    sens;                                  /* RTL_EDGE_BIT mask */
  void       *self;
  void      (*eval)(void *self, RtlEdge e);          /* active region: read q, write d / nb_wr */
  void      (*commit)(void *self);                   /* NBA region */
} RtlModule;

#define RTL_MAX_MODULES 32
#define RTL_MAX_HOOKS   8

typedef struct RtlSim {
  RtlModule *mods[RTL_MAX_MODULES];
  int        nmods;
  struct { void (*fn)(void *); void *ctx; } settle[RTL_MAX_HOOKS];
  int        nsettle;
  struct { void (*fn)(void *, RtlEdge); void *ctx; } hooks[RTL_MAX_HOOKS];
  int        nhooks;
  RtlClock   clk;
  RtlEdge    next;
} RtlSim;

static inline void rtl_sim_init(RtlSim *s) {
  s->nmods = s->nsettle = s->nhooks = 0;
  s->clk.q = s->clk.e = false; s->clk.cycle = 0;
  s->next = RTL_QRISE;
}
static inline void rtl_sim_add(RtlSim *s, RtlModule *m) { s->mods[s->nmods++] = m; }
static inline void rtl_sim_on_settle(RtlSim *s, void (*fn)(void *), void *ctx) {
  s->settle[s->nsettle].fn = fn; s->settle[s->nsettle].ctx = ctx; ++s->nsettle;
}
static inline void rtl_sim_on_edge_end(RtlSim *s, void (*fn)(void *, RtlEdge), void *ctx) {
  s->hooks[s->nhooks].fn = fn; s->hooks[s->nhooks].ctx = ctx; ++s->nhooks;
}

/* Advance exactly one clock edge. */
static inline void rtl_sim_step_edge(RtlSim *s) {
  const RtlEdge e = s->next;
  switch (e) {
    case RTL_QRISE: s->clk.q = true;  break;
    case RTL_ERISE: s->clk.e = true;  break;
    case RTL_QFALL: s->clk.q = false; break;
    case RTL_EFALL: s->clk.e = false; break;
  }
  const unsigned bit = RTL_EDGE_BIT(e);
  /* active region: everything sees pre-edge state */
  for (int i = 0; i < s->nmods; ++i)
    if (s->mods[i]->sens & bit) s->mods[i]->eval(s->mods[i]->self, e);
  /* NBA region */
  for (int i = 0; i < s->nmods; ++i) s->mods[i]->commit(s->mods[i]->self);
  /* continuous assignments re-settle */
  for (int i = 0; i < s->nsettle; ++i) s->settle[i].fn(s->settle[i].ctx);
  if (e == RTL_EFALL) ++s->clk.cycle;
  for (int i = 0; i < s->nhooks; ++i) s->hooks[i].fn(s->hooks[i].ctx, e);
  s->next = (RtlEdge)(((unsigned)e + 1) & 3);
}

static inline void rtl_sim_step_cycle(RtlSim *s) { for (int i = 0; i < 4; ++i) rtl_sim_step_edge(s); }

#endif /* RTL_H */
