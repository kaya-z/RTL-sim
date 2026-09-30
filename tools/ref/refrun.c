/* refrun.c - lock-step reference harness around sbc09's engine.c
 *
 * Not part of the RTL simulator.  It links the *unmodified* sbc09 instruction
 * simulator (src/engine.c, and for OS-9 mode also io.c / vdisk.c) and dumps
 * the architectural state before every instruction so that the RTL model can
 * be compared instruction by instruction.
 *
 *   refrun -mode ram -img f.bin -load 0x200 -pc 0x200 -n N -trace out.txt
 *   refrun -mode os9 -rom os9v1.rom [-0 d0.dsk] [-1 d1.dsk] [-v dir] [-in in.txt]
 *          [-sched sched.txt] -n N -trace out.txt
 *
 * sched.txt (produced by the RTL model) has lines:  "T <iter>"  timer tick
 * (sets the status bit at e030) and "I <iter>" IRQ taken before instruction <iter>.
 *
 * trace line:  pc a b x y u s dp cc    (hex)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#define engine extern
#include "v09.h"

long romstart = 0x10000;
int bpskip, stkskip;
FILE *logfile_dummy;

#ifdef RAMMODE
FILE *disk[2];
FILE *infile;
#else
extern FILE *disk[];
extern FILE *infile;
#endif
#ifdef USE_VDISK
extern int setVdisk(int drv, char *name);
#endif

void my_usleep(long us) { (void)us; }
/* deterministic wall clock for io.c (built with -Dtime=my_time): 2023-11-14 22:13:20 UTC, use TZ=UTC */
#include <time.h>
#include <sys/time.h>
#undef time
#undef setitimer
time_t my_time(time_t *t) { if (t) *t = 1700000000; return 1700000000; }
/* io.c starts a real SIGALRM interval timer on $E030=$8F; the harness replays the RTL run's ticks instead */
int my_setitimer(int which, const struct itimerval *nv, struct itimerval *ov) { (void)which; (void)nv; (void)ov; return 0; }

#ifdef RAMMODE
int do_input(int a) { return mem[(IOPAGE & 0xfe00) + a]; }
void do_output(int a, int c) { mem[(IOPAGE & 0xfe00) + a] = c; }
#endif
void do_escape(void) { fprintf(stderr, "refrun: do_escape (illegal op) pc=%04x\n", pcreg); exit(3); }
void do_exit(void) { exit(0); }

static FILE *tf;
static int sidx_dummy;
static const char *in_path;
static const char *dump_path;
static long nmax = 1000000, iter = 0;
void cwai_wait(void);
static int nsched, sched_cap;
static struct ev { long it; char kind; } *sched;
static int sidx = 0;

void do_trace(FILE *f) {
  (void)f;
  if (iter >= nmax) { fclose(tf); if (dump_path) { FILE *d = fopen(dump_path, "wb"); fwrite(mem, 1, 65536, d); fclose(d); } exit(0); }
  while (sidx < nsched && sched[sidx].it == iter) {
    if (sched[sidx].kind == 'T') mem[IOPAGE + 0x30] |= 0x10;
    else if (sched[sidx].kind == 'G' && in_path) infile = fopen(in_path, "r");    /* typed-ahead input becomes available */
    else if (sched[sidx].kind == 'I') { irq = timerirq; }
    ++sidx;
  }
  fprintf(tf, "%04x %02x %02x %04x %04x %04x %04x %02x %02x\n", pcreg, *areg, *breg, xreg, yreg, ureg,
          sreg, dpreg, ccreg);
  ++iter;
}

#ifndef RAMMODE
/* CWAI (OS-9 idle loop): the RTL run recorded when the tick that wakes it arrived */
void cwai_wait(void) {
  int woke = 0;
  while (sidx < nsched && sched[sidx].it == iter) {
    if (sched[sidx].kind == 'T') { mem[IOPAGE + 0x30] |= 0x10; woke = 1; }
    else if (sched[sidx].kind == 'G' && in_path) infile = fopen(in_path, "r");
    else if (sched[sidx].kind == 'I') { woke = 1; }
    ++sidx;
  }
  if (!woke) { fprintf(stderr, "refrun: CWAI without wake-up event at iteration %ld\n", iter); fclose(tf); exit(4); }
  irq = timerirq;
}
#else
void cwai_wait(void) { fprintf(stderr, "refrun: CWAI in RAM mode\n"); exit(4); }
#endif

static long fsize(FILE *f) { struct stat s; fstat(fileno(f), &s); return s.st_size; }

int main(int argc, char **argv) {
  const char *dump = 0;
  const char *mode = "ram", *img = 0, *rom = 0, *trace = "ref.trace", *in = 0, *sch = 0;
  long load = 0, start = 0;
  int i;
  for (i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-mode")) mode = argv[++i];
    else if (!strcmp(argv[i], "-img")) img = argv[++i];
    else if (!strcmp(argv[i], "-load")) load = strtol(argv[++i], 0, 0);
    else if (!strcmp(argv[i], "-pc")) start = strtol(argv[++i], 0, 0);
    else if (!strcmp(argv[i], "-n")) nmax = strtol(argv[++i], 0, 0);
    else if (!strcmp(argv[i], "-trace")) trace = argv[++i];
    else if (!strcmp(argv[i], "-dump")) dump = argv[++i];
    else if (!strcmp(argv[i], "-rom")) rom = argv[++i];
    else if (!strcmp(argv[i], "-in")) in = argv[++i];
    else if (!strcmp(argv[i], "-sched")) sch = argv[++i];
    else if (!strcmp(argv[i], "-0")) disk[0] = fopen(argv[++i], "r+");
    else if (!strcmp(argv[i], "-1")) disk[1] = fopen(argv[++i], "r+");
#ifdef USE_VDISK
    else if (!strcmp(argv[i], "-v")) setVdisk(0, argv[++i]);
#endif
    else { fprintf(stderr, "refrun: bad arg %s\n", argv[i]); return 2; }
  }
  dump_path = dump;
  tf = fopen(trace, "w");
  if (!tf) { perror(trace); return 2; }
  if (sch) {
    FILE *f = fopen(sch, "r"); char k; long it;
    if (!f) { perror(sch); return 2; }
    while (fscanf(f, " %c %ld", &k, &it) == 2) {
      if (nsched == sched_cap) { sched_cap = sched_cap ? sched_cap * 2 : 1024; sched = realloc(sched, sched_cap * sizeof *sched); }
      sched[nsched].kind = k; sched[nsched].it = it; nsched++;
    }
    fclose(f);
  }
  memsize = 65536;
  escchar = 0x1d;
  tracelo = 0; tracehi = 0xffff;
  tracing = 1; attention = 1;
  timerirq = 1; timer = 3;
  ccreg = 0x50;   /* RESET state: I and F masked (as the RTL core does) */
  if (!strcmp(mode, "ram")) {
    FILE *f = fopen(img, "rb");
    if (!f) { perror(img); return 2; }
    long len = fsize(f);
    if (fread(mem + load, 1, len, f) != (size_t)len) return 2;
    fclose(f);
    pcreg = (mem[0xfffe] << 8) | mem[0xffff];
    if (start) pcreg = start;
  } else {
    FILE *f = fopen(rom, "rb");
    if (!f) { perror(rom); return 2; }
    long len = fsize(f);
    romstart = 0x10000 - len;
    if (fread(mem + romstart, 1, len, f) != (size_t)len) return 2;
    fclose(f);
    in_path = in;
    { int g = 0, k; for (k = 0; k < nsched; k++) if (sched[k].kind == 'G') g = 1;
      if (!g && in) infile = fopen(in, "r"); }
    pcreg = (mem[0xfffe] << 8) | mem[0xffff];
  }
  interpr();
  return 0;
}
