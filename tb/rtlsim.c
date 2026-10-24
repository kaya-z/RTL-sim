/* rtlsim - RTL-style simulator of an MC6809 single board computer that runs OS-9 (C11)
 *
 *   rtlsim -rom os9v1.rom -0 OS9.dsk -1 WORK.dsk -v dir [options]
 *
 * Options (compatible in spirit with sbc09's v09):
 *   -rom f  -l addr  -0 f  -1 f  -v dir  -e escchar
 *   -in f            keystrokes from a script file (LF -> CR)   -indelay N  hold them back for N E-cycles
 *   -clock Hz        E-clock frequency the machine is paced to in real time (default 2000000 = FM-11 EX class 68B09E);
 *                    -clock 0 / -turbo runs unpaced (tests).  A host slower than the clock simply falls behind.
 *   -cycles N        stop after N E-cycles           -drain N   run N more cycles after the script ends
 *   -trace f         instruction trace (pc a b x y u s dp cc) for lock-step comparison
 *   -sched f         write timer/IRQ schedule for the reference harness (implies -tick-aligned)
 *   -n N             stop after N traced instructions
 *   -tick N          timer period in E-cycles        -tick-aligned    deliver ticks between instructions only
 *   -fixed-time      deterministic RTC               -firq            timer on FIRQ
 *   -vcd f [-vcd-from N -vcd-cycles N]  waveform dump
 *   -iolog f         log every bus access to the I/O page      -dumpram f  write the 64K RAM image at exit
 *   -stats           print statistics on exit */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../soc/sbc09.h"
#include "vcd.h"

typedef struct {
  Sbc09Config cfg;
  const char *in, *trace, *sched, *iolog, *dumpram, *vcd;
  uint64_t vcd_from, vcd_cycles;
  long nmax;
  uint64_t cycles;               /* 0 = unlimited */
  uint64_t drain;
  uint64_t indelay;
  double clock_hz;               /* E clock; 0 = run as fast as possible (tests) */
  int esc;
  bool stats, term, novdisk;
} Options;

static void usage(void) {
  fprintf(stderr, "usage: rtlsim -rom image [-l addr] [-0 disk0] [-1 disk1] [-v dir] [-in script] [-cycles N] ...\n");
  exit(2);
}

typedef struct {
  Sbc09 *s; FILE *tf, *sf; long *iter; long nmax;
  FILE *iof; Console *con; uint64_t indelay; bool released, stop;
  bool script_done_seen; uint64_t drain_until;
} Probe;

static void probe_edge_end(void *ctx, RtlEdge e) {
  Probe *p = (Probe *)ctx;
  if (e != RTL_EFALL) return;
  const McCore *k = sbc09_core(p->s);
  if (k->ev_inst || k->ev_int) {
    if (!p->released && sbc09_cycles(p->s) >= p->indelay) {      /* release typed-ahead input between instructions */
      p->released = true; p->con->hold = false;
      if (p->sf) fprintf(p->sf, "G %ld\n", *p->iter);
    }
    if (p->sf && k->ev_int && k->ev_vec == 2) fprintf(p->sf, "I %ld\n", *p->iter);
    if (p->tf) fprintf(p->tf, "%04x %02x %02x %04x %04x %04x %04x %02x %02x\n", k->pc, k->a, k->b, k->x, k->y, k->u, k->s, k->dp, k->cc);
    ++*p->iter;
    if (p->nmax >= 0 && *p->iter >= p->nmax) p->stop = true;
  }
}

typedef struct { Vcd *v; uint64_t edges, from, to; } VcdCtx;
static void vcd_edge_end(void *ctx, RtlEdge e) {
  (void)e;
  VcdCtx *p = (VcdCtx *)ctx;
  ++p->edges;                                    /* one edge = 250 ns at 1 MHz E */
  if (p->edges >= p->from && p->edges < p->to) vcd_sample(p->v, p->edges * 250);
}

#define SIG(fn, expr) static uint64_t fn(void *c) { Sbc09 *s = (Sbc09 *)c; (void)s; return (expr); }
SIG(g_e, s->sim.clk.e)       SIG(g_q, s->sim.clk.q)
SIG(g_a, sbc09_core(s)->addr) SIG(g_rw, sbc09_core(s)->rw)   SIG(g_vma, sbc09_core(s)->vma)
SIG(g_dout, sbc09_core(s)->dout) SIG(g_din, s->pins.din)     SIG(g_lic, sbc09_core(s)->lic)
SIG(g_bs, sbc09_core(s)->bs) SIG(g_ba, sbc09_core(s)->ba)
SIG(g_irq, s->pins.irq_n)    SIG(g_firq, s->pins.firq_n)     SIG(g_nmi, s->pins.nmi_n)  SIG(g_rst, s->pins.reset_n)
SIG(g_upc, sbc09_core(s)->upc) SIG(g_ir, sbc09_core(s)->ir)  SIG(g_pc, sbc09_core(s)->pc)
SIG(g_areg, sbc09_core(s)->a) SIG(g_breg, sbc09_core(s)->b)  SIG(g_x, sbc09_core(s)->x)
SIG(g_y, sbc09_core(s)->y)   SIG(g_u, sbc09_core(s)->u)      SIG(g_s, sbc09_core(s)->s)
SIG(g_dp, sbc09_core(s)->dp) SIG(g_cc, sbc09_core(s)->cc)

static double now_s(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec * 1e-9;
}
static void sleep_until(double t0, double due) {
  const double d = (t0 + due) - now_s();
  if (d <= 0) return;
  struct timespec ts = { (time_t)d, (long)((d - (time_t)d) * 1e9) };
  nanosleep(&ts, NULL);
}

int main(int argc, char **argv) {
  Options o;
  memset(&o, 0, sizeof o);
  o.cfg = sbc09_config_default();
  o.vcd_cycles = 100000; o.nmax = -1; o.clock_hz = 2000000; o.esc = 0x1d; o.term = true;
  for (int i = 1; i < argc; ++i) {
#define ARG() (i + 1 >= argc ? (usage(), (const char *)NULL) : argv[++i])
    if (!strcmp(argv[i], "-rom")) o.cfg.rom = ARG();
    else if (!strcmp(argv[i], "-l")) o.cfg.rom_base = (int32_t)strtol(ARG(), 0, 0);
    else if (!strcmp(argv[i], "-0")) o.cfg.disk[0] = ARG();
    else if (!strcmp(argv[i], "-1")) o.cfg.disk[1] = ARG();
    else if (!strcmp(argv[i], "-v")) o.cfg.vdisk_root = ARG();
    else if (!strcmp(argv[i], "-e")) o.esc = (int)strtol(ARG(), 0, 0);
    else if (!strcmp(argv[i], "-in")) o.in = ARG();
    else if (!strcmp(argv[i], "-cycles")) o.cycles = strtoull(ARG(), 0, 0);
    else if (!strcmp(argv[i], "-drain")) o.drain = strtoull(ARG(), 0, 0);
    else if (!strcmp(argv[i], "-indelay")) o.indelay = strtoull(ARG(), 0, 0);
    else if (!strcmp(argv[i], "-clock")) o.clock_hz = strtod(ARG(), 0);
    else if (!strcmp(argv[i], "-turbo")) o.clock_hz = 0;
    else if (!strcmp(argv[i], "-trace")) o.trace = ARG();
    else if (!strcmp(argv[i], "-sched")) { o.sched = ARG(); o.cfg.tick_aligned = true; }
    else if (!strcmp(argv[i], "-n")) o.nmax = strtol(ARG(), 0, 0);
    else if (!strcmp(argv[i], "-tick")) o.cfg.tick_cycles = (uint32_t)strtoul(ARG(), 0, 0);
    else if (!strcmp(argv[i], "-tick-aligned")) o.cfg.tick_aligned = true;
    else if (!strcmp(argv[i], "-fixed-time")) o.cfg.fixed_time = true;
    else if (!strcmp(argv[i], "-firq")) o.cfg.timer_firq = true;
    else if (!strcmp(argv[i], "-stats")) o.stats = true;
    else if (!strcmp(argv[i], "-noterm")) o.term = false;
    else if (!strcmp(argv[i], "-novdisk")) o.novdisk = true;
    else if (!strcmp(argv[i], "-iolog")) o.iolog = ARG();
    else if (!strcmp(argv[i], "-dumpram")) o.dumpram = ARG();
    else if (!strcmp(argv[i], "-vcd")) o.vcd = ARG();
    else if (!strcmp(argv[i], "-vcd-from")) o.vcd_from = strtoull(ARG(), 0, 0);
    else if (!strcmp(argv[i], "-vcd-cycles")) o.vcd_cycles = strtoull(ARG(), 0, 0);
    else if (!strcmp(argv[i], "-rxshift")) o.cfg.rx_poll_shift = (unsigned)strtoul(ARG(), 0, 0);
    else usage();
#undef ARG
  }
  if (!o.cfg.rom || !o.cfg.rom[0]) usage();
  if ((!o.cfg.vdisk_root || !o.cfg.vdisk_root[0]) && !o.novdisk) o.cfg.vdisk_root = ".";      /* like v09: /v0 = current directory */

  Console con;
  console_init(&con);
  if (o.in && !console_open_script(&con, o.in)) { perror(o.in); return 2; }
  if (o.term && !o.in) { console_use_terminal(&con, true); con.esc = o.esc; }
  else if (o.term) { console_use_terminal(&con, false); }

  Sbc09 *sys = (Sbc09 *)calloc(1, sizeof *sys);
  if (!sbc09_init(sys, &o.cfg, &con)) return 2;

  FILE *tf = o.trace ? fopen(o.trace, "w") : NULL;
  FILE *sf = o.sched ? fopen(o.sched, "w") : NULL;
  FILE *iof = o.iolog ? fopen(o.iolog, "w") : NULL;
  long iter = 0;
  if (sf) { sys->timer.sched = sf; sys->timer.iter = &iter; }

  Probe pr;
  memset(&pr, 0, sizeof pr);
  pr.s = sys; pr.tf = tf; pr.sf = sf; pr.iter = &iter; pr.nmax = o.nmax;
  pr.iof = iof; pr.con = &con; pr.indelay = o.indelay; pr.released = true;
  if (o.indelay) { pr.released = false; con.hold = true; }
  if (iof) { sys->bus.log = iof; sys->bus.log_lo = 0xE000; sys->bus.log_hi = 0xE1FF; }
  rtl_sim_on_edge_end(&sys->sim, probe_edge_end, &pr);

  /* ---- waveform dump ---------------------------------------------------------------------------- */
  Vcd vcd; bool vcd_on = false;
  VcdCtx vc = { &vcd, 0, 0, 0 };
  if (o.vcd) {
    if (!vcd_open(&vcd, o.vcd, "1ns")) { perror(o.vcd); return 2; }
    vcd_on = true;
    vcd_add(&vcd, "E", 1, g_e, sys);       vcd_add(&vcd, "Q", 1, g_q, sys);
    vcd_add(&vcd, "A", 16, g_a, sys);      vcd_add(&vcd, "RW", 1, g_rw, sys);
    vcd_add(&vcd, "VMA", 1, g_vma, sys);   vcd_add(&vcd, "D_out", 8, g_dout, sys);
    vcd_add(&vcd, "D_in", 8, g_din, sys);  vcd_add(&vcd, "LIC", 1, g_lic, sys);
    vcd_add(&vcd, "BS", 1, g_bs, sys);     vcd_add(&vcd, "BA", 1, g_ba, sys);
    vcd_add(&vcd, "IRQ_n", 1, g_irq, sys); vcd_add(&vcd, "FIRQ_n", 1, g_firq, sys);
    vcd_add(&vcd, "NMI_n", 1, g_nmi, sys); vcd_add(&vcd, "RESET_n", 1, g_rst, sys);
    vcd_add(&vcd, "upc", 16, g_upc, sys);  vcd_add(&vcd, "IR", 8, g_ir, sys);
    vcd_add(&vcd, "PC", 16, g_pc, sys);    vcd_add(&vcd, "A_reg", 8, g_areg, sys);
    vcd_add(&vcd, "B_reg", 8, g_breg, sys); vcd_add(&vcd, "X", 16, g_x, sys);
    vcd_add(&vcd, "Y", 16, g_y, sys);      vcd_add(&vcd, "U", 16, g_u, sys);
    vcd_add(&vcd, "S", 16, g_s, sys);      vcd_add(&vcd, "DP", 8, g_dp, sys);
    vcd_add(&vcd, "CC", 8, g_cc, sys);
    vcd_finish_header(&vcd);
    vc.from = o.vcd_from * 4; vc.to = (o.vcd_from + o.vcd_cycles) * 4;
    rtl_sim_on_edge_end(&sys->sim, vcd_edge_end, &vc);
  }

  const double t0 = now_s();
  const uint64_t limit = o.cycles;
  /* real-time pacing: simulated time (cycles / clock) is locked to the host's monotonic clock in 1 ms slices,
   * so the speed does not depend on how fast the host is (as long as it is fast enough) */
  const uint64_t slice = o.clock_hz > 0 ? (uint64_t)(o.clock_hz / 1000) + 1 : 0;
  uint64_t lag_slices = 0, nslices = 0;
  while (!pr.stop && !con.quit) {
    sbc09_step_cycle(sys);
    if (slice && sbc09_cycles(sys) % slice == 0) {
      ++nslices;
      const double due = sbc09_cycles(sys) / o.clock_hz;
      if (now_s() - t0 < due) sleep_until(t0, due); else ++lag_slices;
    }
    if (limit && sbc09_cycles(sys) >= limit) break;
    if (o.in && console_script_done(&con) && o.drain) {
      if (!pr.script_done_seen) { pr.script_done_seen = true; pr.drain_until = sbc09_cycles(sys) + o.drain; }
      else if (sbc09_cycles(sys) >= pr.drain_until) break;
    }
  }
  if (vcd_on) vcd_close(&vcd);
  if (tf) fclose(tf);
  if (sf) fclose(sf);
  if (iof) fclose(iof);
  if (o.dumpram) { FILE *d = fopen(o.dumpram, "wb"); if (d) { fwrite(sys->ram.mem.a, 1, 65536, d); fclose(d); } }
  if (o.stats) {
    double dt = now_s() - t0;
    if (slice) fprintf(stderr, "\n[rtlsim] paced to %.3f MHz; host could not keep up in %llu of %llu ms slices\n", o.clock_hz / 1e6, (unsigned long long)lag_slices, (unsigned long long)nslices);
    fprintf(stderr, "[rtlsim] %llu E-cycles, %ld traced instructions, %.2f s host time (%.2f M cycles/s)\n",
            (unsigned long long)sbc09_cycles(sys), iter, dt, sbc09_cycles(sys) / dt / 1e6);
    fprintf(stderr, "[rtlsim] ticks %llu  disk r/w/err %llu/%llu/%llu  acia rx/tx %llu/%llu\n",
            (unsigned long long)sys->timer.ticks, (unsigned long long)sys->disk.reads, (unsigned long long)sys->disk.writes,
            (unsigned long long)sys->disk.errors, (unsigned long long)sys->acia.rx_count, (unsigned long long)sys->acia.tx_count);
  }
  sbc09_free(sys);
  free(sys);
  console_free(&con);
  return 0;
}
