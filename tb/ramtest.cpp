// ramtest.cpp - MC6809 RTL core on a flat 64 KiB RAM, instruction trace for lock-step checks
//
//   ramtest -img f.bin -load 0x200 -pc 0x200 -n N -trace out.txt [-cycles-log f]
//
// Trace format (identical to tools/ref/refrun.c):  pc a b x y u s dp cc
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "../rtl/rtl.h"
#include "../rtl/mc6809.h"
#include "../soc/bus.h"

struct Tb {
  mc6809::Pins pins;
  mc6809::Cpu cpu{&pins};
  soc::Bus bus{&cpu};
  soc::Ram ram{65536};
  rtl::Simulator sim;
  FILE* tf = nullptr;
  long iter = 0, nmax = 0;
  uint64_t inst_cycles = 0, last_cycle = 0;
  bool done = false;
  bool stop_on_ill = false;
  std::vector<int> cyc_hist;    // cycles per instruction (for timing tests)
  FILE* cf = nullptr;
  char last_bytes[16] = "0000000000";
  uint8_t last_cc = 0;

  Tb() {
    bus.map(0, 0xFFFF, &ram);
    sim.add(&cpu); sim.add(&bus); sim.add(&ram);
    sim.on_settle([](void* c) { auto* t = static_cast<Tb*>(c); t->settle(); }, this);
    sim.on_edge_end([](void* c, rtl::Edge e) { auto* t = static_cast<Tb*>(c); t->edge_end(e); }, this);
  }
  void settle() {
    pins.din = bus.rdata.q;
    pins.reset_n = sim.clk().cycle >= 4;
  }
  void edge_end(rtl::Edge e) {
    if (e != rtl::Edge::EFall) return;
    const mc6809::Core& c = cpu.q();
    if (stop_on_ill && (c.ev_ill || (c.ba && !c.bs))) {
      fprintf(stderr, "ramtest: stop at iteration %ld (%s)\n", iter, c.ev_ill ? "illegal opcode" : "SYNC/CWAI wait");
      done = true; return;
    }
    if (c.ev_inst || c.ev_int) {
      if (cf) {   // "<cycles of previous instruction> <its bytes> <cc before it>"
        fprintf(cf, "%llu %s %02x\n", (unsigned long long)(sim.clk().cycle - last_cycle), last_bytes, last_cc);
        snprintf(last_bytes, sizeof last_bytes, "%02x%02x%02x%02x%02x", ram.mem.rd(c.pc), ram.mem.rd((c.pc + 1) & 0xFFFF),
                 ram.mem.rd((c.pc + 2) & 0xFFFF), ram.mem.rd((c.pc + 3) & 0xFFFF), ram.mem.rd((c.pc + 4) & 0xFFFF));
        last_cc = c.cc;
      }
      last_cycle = sim.clk().cycle;
      if (iter >= nmax) { done = true; return; }
      fprintf(tf, "%04x %02x %02x %04x %04x %04x %04x %02x %02x\n", c.pc, c.a, c.b, c.x, c.y, c.u, c.s, c.dp, c.cc);
      ++iter;
    }
  }
};

int main(int argc, char** argv) {
  const char *img = nullptr, *trace = "rtl.trace", *clog = nullptr;
  long load = 0, start = -1, n = 100000;
  bool stop_ill = false;
  const char* dump = nullptr;
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "-img")) img = argv[++i];
    else if (!strcmp(argv[i], "-load")) load = strtol(argv[++i], 0, 0);
    else if (!strcmp(argv[i], "-pc")) start = strtol(argv[++i], 0, 0);
    else if (!strcmp(argv[i], "-n")) n = strtol(argv[++i], 0, 0);
    else if (!strcmp(argv[i], "-trace")) trace = argv[++i];
    else if (!strcmp(argv[i], "-cycles-log")) clog = argv[++i];
    else if (!strcmp(argv[i], "-stop-on-ill")) stop_ill = true;
    else if (!strcmp(argv[i], "-dump")) dump = argv[++i];
    else { fprintf(stderr, "bad arg %s\n", argv[i]); return 2; }
  }
  Tb tb;
  tb.nmax = n;
  tb.stop_on_ill = stop_ill;
  tb.tf = fopen(trace, "w");
  if (clog) tb.cf = fopen(clog, "w");
  if (img) {
    FILE* f = fopen(img, "rb");
    if (!f) { perror(img); return 2; }
    std::vector<uint8_t> buf(65536);
    size_t len = fread(buf.data(), 1, buf.size(), f);
    fclose(f);
    for (size_t i = 0; i < len; ++i) tb.ram.mem.poke((load + i) & 0xFFFF, buf[i]);
  }
  if (start >= 0) { tb.ram.mem.poke(0xFFFE, start >> 8); tb.ram.mem.poke(0xFFFF, start & 0xFF); }
  tb.settle();
  while (!tb.done) tb.sim.step_edge();
  fclose(tb.tf);
  if (dump) { FILE* d = fopen(dump, "wb"); fwrite(tb.ram.mem.raw(), 1, 65536, d); fclose(d); }
  if (tb.cf) fclose(tb.cf);
  return 0;
}
