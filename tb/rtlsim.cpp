// rtlsim - RTL-style simulator of an MC6809 single board computer that runs OS-9
//
//   rtlsim -rom os9v1.rom -0 OS9.dsk -1 WORK.dsk -v dir [options]
//
// Options (compatible in spirit with sbc09's v09):
//   -rom f  -l addr  -0 f  -1 f  -v dir  -e escchar
//   -in f            keystrokes from a script file (LF -> CR)   -indelay N  hold them back for N E-cycles
//   -clock Hz        E-clock frequency the machine is paced to in real time (default 2000000 = FM-11 EX class 68B09E);
//                    -clock 0 / -turbo runs unpaced (tests).  A host slower than the clock simply falls behind.
//   -cycles N        stop after N E-cycles           -drain N   run N more cycles after the script ends
//   -trace f         instruction trace (pc a b x y u s dp cc) for lock-step comparison
//   -sched f         write timer/IRQ schedule for the reference harness (implies -tick-aligned)
//   -n N             stop after N traced instructions
//   -tick N          timer period in E-cycles        -tick-aligned    deliver ticks between instructions only
//   -fixed-time      deterministic RTC               -firq            timer on FIRQ
//   -vcd f [-vcd-from N -vcd-cycles N]  waveform dump
//   -iolog f         log every bus access to the I/O page      -dumpram f  write the 64K RAM image at exit
//   -stats           print statistics on exit
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <chrono>
#include <thread>
#include "../soc/sbc09.h"
#include "vcd.h"

struct Options {
  soc::Config cfg;
  const char* in = nullptr;
  const char* trace = nullptr;
  const char* sched = nullptr;
  const char* iolog = nullptr;
  const char* dumpram = nullptr;
  const char* vcd = nullptr;
  uint64_t vcd_from = 0, vcd_cycles = 100000;
  long nmax = -1;
  uint64_t cycles = 0;          // 0 = unlimited
  uint64_t drain = 0;
  uint64_t indelay = 0;
  double clock_hz = 2000000;     // E clock; 0 = run as fast as possible (tests)
  int esc = 0x1d;
  bool stats = false;
  bool term = true;
  bool novdisk = false;
};

static void usage() {
  fprintf(stderr, "usage: rtlsim -rom image [-l addr] [-0 disk0] [-1 disk1] [-v dir] [-in script] [-cycles N] ...\n");
  exit(2);
}

int main(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    auto arg = [&]() -> const char* { if (i + 1 >= argc) usage(); return argv[++i]; };
    if (!strcmp(argv[i], "-rom")) o.cfg.rom = arg();
    else if (!strcmp(argv[i], "-l")) o.cfg.rom_base = static_cast<int32_t>(strtol(arg(), 0, 0));
    else if (!strcmp(argv[i], "-0")) o.cfg.disk[0] = arg();
    else if (!strcmp(argv[i], "-1")) o.cfg.disk[1] = arg();
    else if (!strcmp(argv[i], "-v")) o.cfg.vdisk_root = arg();
    else if (!strcmp(argv[i], "-e")) o.esc = static_cast<int>(strtol(arg(), 0, 0));
    else if (!strcmp(argv[i], "-in")) o.in = arg();
    else if (!strcmp(argv[i], "-cycles")) o.cycles = strtoull(arg(), 0, 0);
    else if (!strcmp(argv[i], "-drain")) o.drain = strtoull(arg(), 0, 0);
    else if (!strcmp(argv[i], "-indelay")) o.indelay = strtoull(arg(), 0, 0);
    else if (!strcmp(argv[i], "-clock")) o.clock_hz = strtod(arg(), 0);
    else if (!strcmp(argv[i], "-turbo")) o.clock_hz = 0;
    else if (!strcmp(argv[i], "-trace")) o.trace = arg();
    else if (!strcmp(argv[i], "-sched")) { o.sched = arg(); o.cfg.tick_aligned = true; }
    else if (!strcmp(argv[i], "-n")) o.nmax = strtol(arg(), 0, 0);
    else if (!strcmp(argv[i], "-tick")) o.cfg.tick_cycles = static_cast<uint32_t>(strtoul(arg(), 0, 0));
    else if (!strcmp(argv[i], "-tick-aligned")) o.cfg.tick_aligned = true;
    else if (!strcmp(argv[i], "-fixed-time")) o.cfg.fixed_time = true;
    else if (!strcmp(argv[i], "-firq")) o.cfg.timer_firq = true;
    else if (!strcmp(argv[i], "-stats")) o.stats = true;
    else if (!strcmp(argv[i], "-noterm")) o.term = false;
    else if (!strcmp(argv[i], "-novdisk")) o.novdisk = true;
    else if (!strcmp(argv[i], "-iolog")) o.iolog = arg();
    else if (!strcmp(argv[i], "-dumpram")) o.dumpram = arg();
    else if (!strcmp(argv[i], "-vcd")) o.vcd = arg();
    else if (!strcmp(argv[i], "-vcd-from")) o.vcd_from = strtoull(arg(), 0, 0);
    else if (!strcmp(argv[i], "-vcd-cycles")) o.vcd_cycles = strtoull(arg(), 0, 0);
    else if (!strcmp(argv[i], "-rxshift")) o.cfg.rx_poll_shift = static_cast<unsigned>(strtoul(arg(), 0, 0));
    else usage();
  }
  if (o.cfg.rom.empty()) usage();
  if (o.cfg.vdisk_root.empty() && !o.novdisk) o.cfg.vdisk_root = ".";      // like v09: /v0 = current directory

  host::Console con;
  if (o.in && !con.open_script(o.in)) { perror(o.in); return 2; }
  if (o.term && !o.in) { con.use_terminal(true); con.set_escape(o.esc); }
  else if (o.term) { con.use_terminal(false); }

  soc::Sbc09 sys(o.cfg, con);
  if (!sys.ok()) return 2;

  FILE* tf = o.trace ? fopen(o.trace, "w") : nullptr;
  FILE* sf = o.sched ? fopen(o.sched, "w") : nullptr;
  FILE* iof = o.iolog ? fopen(o.iolog, "w") : nullptr;
  long iter = 0;
  if (sf) { sys.timer.sched = sf; sys.timer.iter = &iter; }

  struct Probe { soc::Sbc09* s; FILE* tf; FILE* sf; long* iter; long nmax; FILE* iof = nullptr; host::Console* con = nullptr; uint64_t indelay = 0; bool released = true; bool stop = false;
                 bool script_done_seen = false; uint64_t drain_until = 0; } pr{&sys, tf, sf, &iter, o.nmax};
  pr.iof = iof; pr.con = &con; pr.indelay = o.indelay;
  if (o.indelay) { pr.released = false; con.hold(true); }
  if (iof) { sys.bus.log = iof; sys.bus.log_lo = 0xE000; sys.bus.log_hi = 0xE1FF; }
  sys.sim.on_edge_end([](void* c, rtl::Edge e) {
    auto* p = static_cast<Probe*>(c);
    if (e != rtl::Edge::EFall) return;
    const mc6809::Core& k = p->s->core();
    if (k.ev_inst || k.ev_int) {
      if (!p->released && p->s->cycles() >= p->indelay) {      // release typed-ahead input between instructions
        p->released = true; p->con->hold(false);
        if (p->sf) fprintf(p->sf, "G %ld\n", *p->iter);
      }
      if (p->sf && k.ev_int && k.ev_vec == 2) fprintf(p->sf, "I %ld\n", *p->iter);
      if (p->tf) fprintf(p->tf, "%04x %02x %02x %04x %04x %04x %04x %02x %02x\n", k.pc, k.a, k.b, k.x, k.y, k.u, k.s, k.dp, k.cc);
      ++*p->iter;
      if (p->nmax >= 0 && *p->iter >= p->nmax) p->stop = true;
    }
  }, &pr);

  // ---- waveform dump ------------------------------------------------------------------------
  tb::Vcd* vcd = nullptr;
  struct VcdCtx { tb::Vcd* v; soc::Sbc09* s; uint64_t edges = 0, from = 0, to = 0; } vc{nullptr, &sys};
  if (o.vcd) {
    vcd = new tb::Vcd(o.vcd);
    if (!vcd->ok()) { perror(o.vcd); return 2; }
    soc::Sbc09* s = &sys;
    const rtl::Simulator* sm = &sys.sim;
    vcd->add("E", 1, [sm] { return sm->clk().e; });
    vcd->add("Q", 1, [sm] { return sm->clk().q; });
    vcd->add("A", 16, [s] { return s->core().addr; });
    vcd->add("RW", 1, [s] { return s->core().rw; });
    vcd->add("VMA", 1, [s] { return s->core().vma; });
    vcd->add("D_out", 8, [s] { return s->core().dout; });
    vcd->add("D_in", 8, [s] { return s->pins.din; });
    vcd->add("LIC", 1, [s] { return s->core().lic; });
    vcd->add("BS", 1, [s] { return s->core().bs; });
    vcd->add("BA", 1, [s] { return s->core().ba; });
    vcd->add("IRQ_n", 1, [s] { return s->pins.irq_n; });
    vcd->add("FIRQ_n", 1, [s] { return s->pins.firq_n; });
    vcd->add("NMI_n", 1, [s] { return s->pins.nmi_n; });
    vcd->add("RESET_n", 1, [s] { return s->pins.reset_n; });
    vcd->add("upc", 16, [s] { return s->core().upc; });
    vcd->add("IR", 8, [s] { return s->core().ir; });
    vcd->add("PC", 16, [s] { return s->core().pc; });
    vcd->add("A_reg", 8, [s] { return s->core().a; });
    vcd->add("B_reg", 8, [s] { return s->core().b; });
    vcd->add("X", 16, [s] { return s->core().x; });
    vcd->add("Y", 16, [s] { return s->core().y; });
    vcd->add("U", 16, [s] { return s->core().u; });
    vcd->add("S", 16, [s] { return s->core().s; });
    vcd->add("DP", 8, [s] { return s->core().dp; });
    vcd->add("CC", 8, [s] { return s->core().cc; });
    vcd->finish_header();
    vc.v = vcd; vc.from = o.vcd_from * 4; vc.to = (o.vcd_from + o.vcd_cycles) * 4;
    sys.sim.on_edge_end([](void* c, rtl::Edge) {
      auto* p = static_cast<VcdCtx*>(c);
      ++p->edges;                                    // one edge = 250 ns at 1 MHz E
      if (p->edges >= p->from && p->edges < p->to) p->v->sample(p->edges * 250);
    }, &vc);
  }

  auto t0 = std::chrono::steady_clock::now();
  uint64_t limit = o.cycles;
  // real-time pacing: simulated time (cycles / clock) is locked to the host's steady clock in 1 ms slices,
  // so the speed does not depend on how fast the host is (as long as it is fast enough)
  const uint64_t slice = o.clock_hz > 0 ? static_cast<uint64_t>(o.clock_hz / 1000) + 1 : 0;
  uint64_t lag_slices = 0, nslices = 0;
  while (!pr.stop && !con.quit()) {
    sys.step_cycle();
    if (slice && sys.cycles() % slice == 0) {
      ++nslices;
      const auto due = t0 + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                std::chrono::duration<double>(sys.cycles() / o.clock_hz));
      if (std::chrono::steady_clock::now() < due) std::this_thread::sleep_until(due); else ++lag_slices;
    }
    if (limit && sys.cycles() >= limit) break;
    if (o.in && con.script_done() && o.drain) {
      if (!pr.script_done_seen) { pr.script_done_seen = true; pr.drain_until = sys.cycles() + o.drain; }
      else if (sys.cycles() >= pr.drain_until) break;
    }
  }
  delete vcd;
  if (tf) fclose(tf);
  if (sf) fclose(sf);
  if (o.dumpram) { FILE* d = fopen(o.dumpram, "wb"); fwrite(sys.ram.mem.raw(), 1, 65536, d); fclose(d); }
  if (o.stats) {
    double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (slice) fprintf(stderr, "\n[rtlsim] paced to %.3f MHz; host could not keep up in %llu of %llu ms slices\n", o.clock_hz / 1e6, (unsigned long long)lag_slices, (unsigned long long)nslices);
    fprintf(stderr, "[rtlsim] %llu E-cycles, %ld traced instructions, %.2f s host time (%.2f M cycles/s)\n",
            (unsigned long long)sys.cycles(), iter, dt, sys.cycles() / dt / 1e6);
    fprintf(stderr, "[rtlsim] ticks %llu  disk r/w/err %llu/%llu/%llu  acia rx/tx %llu/%llu\n",
            (unsigned long long)sys.timer.ticks, (unsigned long long)sys.disk.reads, (unsigned long long)sys.disk.writes,
            (unsigned long long)sys.disk.errors, (unsigned long long)sys.acia.rx_count, (unsigned long long)sys.acia.tx_count);
  }
  return 0;
}
