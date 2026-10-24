/* ramtest.c - MC6809 RTL core on a flat 64 KiB RAM, instruction trace for lock-step checks (C11)
 *
 *   ramtest -img f.bin -load 0x200 -pc 0x200 -n N -trace out.txt [-cycles-log f]
 *
 * Trace format (identical to tools/ref/refrun.c):  pc a b x y u s dp cc */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../rtl/rtl.h"
#include "../rtl/mc6809.h"
#include "../soc/bus.h"

typedef struct Tb {
  McPins pins;
  McCpu  cpu;
  Bus    bus;
  Ram    ram;
  RtlSim sim;
  FILE  *tf;
  long   iter, nmax;
  uint64_t last_cycle;
  bool   done;
  bool   stop_on_ill;
  FILE  *cf;
  char   last_bytes[16];
  uint8_t last_cc;
} Tb;

static void tb_settle(void *ctx) {
  Tb *t = (Tb *)ctx;
  t->pins.din = t->bus.rdata.q;
  t->pins.reset_n = t->sim.clk.cycle >= 4;
}
static void tb_edge_end(void *ctx, RtlEdge e) {
  Tb *t = (Tb *)ctx;
  if (e != RTL_EFALL) return;
  const McCore *c = mc_cpu_q(&t->cpu);
  if (t->stop_on_ill && (c->ev_ill || (c->ba && !c->bs))) {
    fprintf(stderr, "ramtest: stop at iteration %ld (%s)\n", t->iter, c->ev_ill ? "illegal opcode" : "SYNC/CWAI wait");
    t->done = true; return;
  }
  if (c->ev_inst || c->ev_int) {
    if (t->cf) {   /* "<cycles of previous instruction> <its bytes> <cc before it>" */
      fprintf(t->cf, "%llu %s %02x\n", (unsigned long long)(t->sim.clk.cycle - t->last_cycle), t->last_bytes, t->last_cc);
      snprintf(t->last_bytes, sizeof t->last_bytes, "%02x%02x%02x%02x%02x", rtl_nbmem_rd(&t->ram.mem, c->pc),
               rtl_nbmem_rd(&t->ram.mem, (c->pc + 1) & 0xFFFF), rtl_nbmem_rd(&t->ram.mem, (c->pc + 2) & 0xFFFF),
               rtl_nbmem_rd(&t->ram.mem, (c->pc + 3) & 0xFFFF), rtl_nbmem_rd(&t->ram.mem, (c->pc + 4) & 0xFFFF));
      t->last_cc = c->cc;
    }
    t->last_cycle = t->sim.clk.cycle;
    if (t->iter >= t->nmax) { t->done = true; return; }
    fprintf(t->tf, "%04x %02x %02x %04x %04x %04x %04x %02x %02x\n", c->pc, c->a, c->b, c->x, c->y, c->u, c->s, c->dp, c->cc);
    ++t->iter;
  }
}

int main(int argc, char **argv) {
  const char *img = NULL, *trace = "rtl.trace", *clog = NULL, *dump = NULL;
  long load = 0, start = -1, n = 100000;
  bool stop_ill = false;
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "-img") && i + 1 < argc) img = argv[++i];
    else if (!strcmp(argv[i], "-load") && i + 1 < argc) load = strtol(argv[++i], 0, 0);
    else if (!strcmp(argv[i], "-pc") && i + 1 < argc) start = strtol(argv[++i], 0, 0);
    else if (!strcmp(argv[i], "-n") && i + 1 < argc) n = strtol(argv[++i], 0, 0);
    else if (!strcmp(argv[i], "-trace") && i + 1 < argc) trace = argv[++i];
    else if (!strcmp(argv[i], "-cycles-log") && i + 1 < argc) clog = argv[++i];
    else if (!strcmp(argv[i], "-stop-on-ill")) stop_ill = true;
    else if (!strcmp(argv[i], "-dump") && i + 1 < argc) dump = argv[++i];
    else { fprintf(stderr, "bad arg %s\n", argv[i]); return 2; }
  }
  Tb *tb = (Tb *)calloc(1, sizeof *tb);
  strcpy(tb->last_bytes, "0000000000");
  mc_pins_init(&tb->pins);
  mc_cpu_init(&tb->cpu, &tb->pins);
  bus_init(&tb->bus, &tb->cpu);
  ram_init(&tb->ram, 65536);
  rtl_sim_init(&tb->sim);
  bus_map(&tb->bus, 0, 0xFFFF, &tb->ram.dev);
  rtl_sim_add(&tb->sim, &tb->cpu.mod); rtl_sim_add(&tb->sim, &tb->bus.mod); rtl_sim_add(&tb->sim, &tb->ram.mod);
  rtl_sim_on_settle(&tb->sim, tb_settle, tb);
  rtl_sim_settle_on(&tb->sim, RTL_EDGE_BIT(RTL_ERISE) | RTL_EDGE_BIT(RTL_EFALL));
  rtl_sim_on_edge_end_mask(&tb->sim, tb_edge_end, tb, RTL_EDGE_BIT(RTL_EFALL));

  tb->nmax = n;
  tb->stop_on_ill = stop_ill;
  tb->tf = fopen(trace, "w");
  if (clog) tb->cf = fopen(clog, "w");
  if (img) {
    FILE *f = fopen(img, "rb");
    if (!f) { perror(img); return 2; }
    uint8_t *buf = (uint8_t *)malloc(65536);
    size_t len = fread(buf, 1, 65536, f);
    fclose(f);
    for (size_t i = 0; i < len; ++i) rtl_nbmem_poke(&tb->ram.mem, (load + i) & 0xFFFF, buf[i]);
    free(buf);
  }
  if (start >= 0) { rtl_nbmem_poke(&tb->ram.mem, 0xFFFE, start >> 8); rtl_nbmem_poke(&tb->ram.mem, 0xFFFF, start & 0xFF); }
  tb_settle(tb);
  while (!tb->done) rtl_sim_step_edge(&tb->sim);
  fclose(tb->tf);
  if (dump) { FILE *d = fopen(dump, "wb"); fwrite(tb->ram.mem.a, 1, 65536, d); fclose(d); }
  if (tb->cf) fclose(tb->cf);
  rtl_nbmem_free(&tb->ram.mem);
  free(tb);
  return 0;
}
