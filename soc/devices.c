/* Credits / references:
 *   * sbc09 (L.C. Benschop and the sbc09 team, GPLv2; S. Kono's OS-9 fork github.com/shinji-kono/sbc09):
 *     I/O map ($E000 ACIA, $E030 timer/RTC, $E040 disk controller), the pdisk sector protocol and the
 *     /v0 "virtual RBF" command protocol (os9/level1,level2/vrbf.asm, src/io.c, src/vdisk.c) are modelled after it.
 *   * NitrOS-9 / OS-9 Level 1 (Microware, the NitrOS-9 project) is the guest software; it is not distributed here. */
#include "devices.h"
#include "../host/vdisk.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- ROM ------------------------------------------------------------------------ */
uint8_t rom_peek(const Rom *r, uint16_t a) { uint32_t o = a - r->base; return o < r->len ? r->img[o] : 0xFF; }
static uint8_t rom_rd(void *self, uint16_t a) { return rom_peek((const Rom *)self, a); }
void rom_init(Rom *r, const uint8_t *img, size_t len, uint32_t base) {
  memset(r, 0, sizeof *r);
  r->base = base; r->len = len;
  r->img = (uint8_t *)malloc(len ? len : 1);
  memcpy(r->img, img, len);
  r->dev.rd = rom_rd; r->dev.self = r;
}
void rom_free(Rom *r) { free(r->img); r->img = NULL; }

/* ---- ACIA ----------------------------------------------------------------------- */
static uint8_t acia_status(const Acia *a) { return (uint8_t)(2 | (a->rx_valid.q ? 1 : 0)); }   /* TDRE always set, RDRF = rx_valid */
static uint8_t acia_rd(void *self, uint16_t off) {
  Acia *a = (Acia *)self;
  return off == 0 ? acia_status(a) : (a->rx_valid.q ? a->rx_data.q : a->bus_last.q);
}
static void acia_eval(void *self, RtlEdge e) {
  (void)e;
  Acia *a = (Acia *)self;
  const bool consume = a->dev.rd_en && a->dev.sa == 1 && a->rx_valid.q;
  if (a->dev.rd_en && a->dev.sa == 0) RTL_NB(a->bus_last, acia_status(a));
  if (consume) { RTL_NB(a->rx_valid, false); RTL_NB(a->bus_last, a->rx_data.q); ++a->rx_count; }
  else if (!a->rx_valid.q && ((++a->tick & a->poll_mask) == 0)) {
    int c = console_poll(a->con);
    if (c >= 0) { RTL_NB(a->rx_data, (uint8_t)c); RTL_NB(a->rx_valid, true); }
  }
  if (a->dev.wr_en && a->dev.sa == 1) { console_put(a->con, a->dev.wdata); ++a->tx_count; }
}
static void acia_commit(void *self) {
  Acia *a = (Acia *)self;
  RTL_COMMIT(a->rx_valid); RTL_COMMIT(a->rx_data); RTL_COMMIT(a->bus_last);
}
void acia_init(Acia *a, Console *con, unsigned poll_shift) {
  memset(a, 0, sizeof *a);
  a->con = con; a->poll_mask = (1u << poll_shift) - 1;
  a->dev.rd = acia_rd; a->dev.self = a;
  a->mod.name = "acia"; a->mod.sens = RTL_EDGE_BIT(RTL_EFALL);
  a->mod.self = a; a->mod.eval = acia_eval; a->mod.commit = acia_commit;
}

/* ---- timer ----------------------------------------------------------------------- */
static void timer_latch_date(Timer *t) {
  time_t tt = t->fixed ? 1700000000 : time(NULL);   /* 2023-11-14 22:13:20 UTC when fixed */
  struct tm tmv;
  if (t->fixed) gmtime_r(&tt, &tmv); else localtime_r(&tt, &tmv);
  rtl_nbmem_nb_wr(&t->date, 1, (uint8_t)tmv.tm_year);
  rtl_nbmem_nb_wr(&t->date, 2, (uint8_t)(tmv.tm_mon + 1));
  rtl_nbmem_nb_wr(&t->date, 3, (uint8_t)tmv.tm_mday);
  rtl_nbmem_nb_wr(&t->date, 4, (uint8_t)tmv.tm_hour);
  rtl_nbmem_nb_wr(&t->date, 5, (uint8_t)tmv.tm_min);
  rtl_nbmem_nb_wr(&t->date, 6, (uint8_t)tmv.tm_sec);
}
static uint8_t timer_rd(void *self, uint16_t off) {
  Timer *t = (Timer *)self;
  return off == 0 ? t->ctl.q : rtl_nbmem_rd(&t->date, off & 15);
}
static void timer_eval(void *self, RtlEdge e) {
  (void)e;
  Timer *t = (Timer *)self;
  uint8_t nctl = t->ctl.q;
  bool restart = false, ack = false, tick = false;
  if (t->dev.wr_en) {
    if (t->dev.sa == 0) {
      if (t->dev.wdata == 0x04) timer_latch_date(t);              /* latch host clock into $E031..$E036 */
      else {
        nctl = t->dev.wdata; ack = true;
        if (t->dev.wdata == 0x8F) { RTL_NB(t->run, true); RTL_NB(t->cnt, 0); restart = true; }
        else if (t->dev.wdata == 0x80) { RTL_NB(t->run, false); restart = true; }
      }
    } else {
      rtl_nbmem_nb_wr(&t->date, t->dev.sa & 15, t->dev.wdata);
    }
  }
  if (t->run.q && !restart) {
    if (t->cnt.q + 1 >= t->period) {
      const McCore *k = mc_cpu_q(t->cpu);
      if (!t->aligned || k->lic || (k->ba && !k->bs)) {   /* test mode: tick only between instructions (or while in CWAI/SYNC) */
        RTL_NB(t->cnt, 0);
        tick = true;
        ++t->ticks;
        if (t->sched && t->iter) fprintf(t->sched, "T %ld\n", *t->iter);
      }
    } else {
      RTL_NB(t->cnt, t->cnt.q + 1);
    }
  }
  if (tick) nctl |= 0x10;
  RTL_NB(t->ctl, nctl);
  if (tick) RTL_NB(t->flag, true);
  else if (ack) RTL_NB(t->flag, false);
}
static void timer_commit(void *self) {
  Timer *t = (Timer *)self;
  RTL_COMMIT(t->cnt); RTL_COMMIT(t->run); RTL_COMMIT(t->flag); RTL_COMMIT(t->ctl); rtl_nbmem_commit(&t->date);
}
void timer_init(Timer *t, const McCpu *cpu, uint32_t period, bool aligned, bool fixed_time) {
  memset(t, 0, sizeof *t);
  t->cpu = cpu; t->period = period; t->aligned = aligned; t->fixed = fixed_time;
  RTL_FORCE(t->ctl, (uint8_t)0xFF);
  rtl_nbmem_init(&t->date, 16, 0);
  t->dev.rd = timer_rd; t->dev.self = t;
  t->mod.name = "timer"; t->mod.sens = RTL_EDGE_BIT(RTL_EFALL);
  t->mod.self = t; t->mod.eval = timer_eval; t->mod.commit = timer_commit;
}

/* ---- disk controller ------------------------------------------------------------- */
static void diskctl_sector_io(DiskCtl *d, uint8_t cmd) {
  const int drv = diskctl_reg(d, 1);
  const long lsn = (diskctl_reg(d, 2) << 16) | (diskctl_reg(d, 3) << 8) | diskctl_reg(d, 4);
  const uint16_t buf = (uint16_t)((diskctl_reg(d, 5) << 8) | diskctl_reg(d, 6));
  bool ok = drv <= 1 && d->disk[drv] != NULL && fseeko(d->disk[drv], (off_t)lsn * 256, SEEK_SET) == 0;
  if (ok) {
    uint8_t sec[256];
    if (cmd == 0x81) {
      size_t n = fread(sec, 1, 256, d->disk[drv]);
      for (size_t i = 0; i < n; ++i) diskctl_poke(d, buf + i, sec[i]);
      ++d->reads;
    } else {
      for (int i = 0; i < 256; ++i) sec[i] = diskctl_peek(d, buf + i);
      ok = fwrite(sec, 1, 256, d->disk[drv]) == 256 && fflush(d->disk[drv]) == 0;
      ++d->writes;
    }
  }
  if (!ok) ++d->errors;
  diskctl_set_status(d, ok ? 0x00 : 0xFF);
}
static uint8_t diskctl_rd(void *self, uint16_t off) { return rtl_nbmem_rd(&((DiskCtl *)self)->regs, off & 15); }
static void diskctl_eval(void *self, RtlEdge e) {
  (void)e;
  DiskCtl *d = (DiskCtl *)self;
  if (!d->dev.wr_en) return;
  if (d->dev.sa != 0) { rtl_nbmem_nb_wr(&d->regs, d->dev.sa & 15, d->dev.wdata); return; }
  if (d->dev.wdata == 0x81 || d->dev.wdata == 0x55) diskctl_sector_io(d, d->dev.wdata);
  else if (d->vd) vdisk_command(d->vd, d, d->dev.wdata);
  else diskctl_set_status(d, 0xFF);
}
static void diskctl_commit(void *self) { rtl_nbmem_commit(&((DiskCtl *)self)->regs); }
void diskctl_init(DiskCtl *d, RtlNbMem *ram) {
  memset(d, 0, sizeof *d);
  rtl_nbmem_init(&d->regs, 16, 0);
  d->ram = ram; d->ram_top = 0x10000;
  d->dev.rd = diskctl_rd; d->dev.self = d;
  d->mod.name = "diskctl"; d->mod.sens = RTL_EDGE_BIT(RTL_EFALL);
  d->mod.self = d; d->mod.eval = diskctl_eval; d->mod.commit = diskctl_commit;
}
void diskctl_free(DiskCtl *d) {
  for (int i = 0; i < 2; ++i) if (d->disk[i]) { fclose(d->disk[i]); d->disk[i] = NULL; }
  rtl_nbmem_free(&d->regs);
}
bool diskctl_attach(DiskCtl *d, int drive, const char *path) {
  if (drive < 0 || drive > 1) return false;
  if (d->disk[drive]) fclose(d->disk[drive]);
  d->disk[drive] = fopen(path, "r+b");
  return d->disk[drive] != NULL;
}

/* ---- I/O page ---------------------------------------------------------------------- */
static uint8_t iopage_rd(void *self, uint16_t a) {
  IoPage *p = (IoPage *)self;
  const uint16_t o = a & 0x1FF;
  if (o < 2) return p->acia->dev.rd(p->acia, o);
  if (o >= 0x30 && o < 0x40) return p->timer->dev.rd(p->timer, o - 0x30);
  if (o >= 0x40 && o < 0x50) return p->disk->dev.rd(p->disk, o - 0x40);
  return rtl_nbmem_rd(&p->back, o);
}
static void iopage_eval(void *self, RtlEdge e) {
  (void)e;
  IoPage *p = (IoPage *)self;
  /* forward the strobes to the selected sub-device (only during E fall) */
  p->acia->dev.wr_en = p->acia->dev.rd_en = p->timer->dev.wr_en = p->timer->dev.rd_en =
      p->disk->dev.wr_en = p->disk->dev.rd_en = false;
  if (!(p->dev.wr_en || p->dev.rd_en)) return;
  const uint16_t o = p->dev.sa & 0x1FF;
  Device *d = NULL; uint16_t off = 0;
  if (o < 2) { d = &p->acia->dev; off = o; }
  else if (o >= 0x30 && o < 0x40) { d = &p->timer->dev; off = o - 0x30; }
  else if (o >= 0x40 && o < 0x50) { d = &p->disk->dev; off = o - 0x40; }
  else if (p->dev.wr_en && o >= 0x50) rtl_nbmem_nb_wr(&p->back, o, p->dev.wdata);   /* sbc09: writes above $E040 land in the page */
  if (d) { d->wr_en = p->dev.wr_en; d->rd_en = p->dev.rd_en; d->sa = off; d->wdata = p->dev.wdata; }
}
static void iopage_commit(void *self) { rtl_nbmem_commit(&((IoPage *)self)->back); }
void iopage_init(IoPage *p, const uint8_t *rom_image, size_t rom_len, uint32_t rom_base, Acia *a, Timer *t, DiskCtl *d) {
  memset(p, 0, sizeof *p);
  p->acia = a; p->timer = t; p->disk = d;
  rtl_nbmem_init(&p->back, 0x200, 0);
  for (uint32_t i = 0; i < 0x200; ++i) {
    uint32_t o = 0xE000 + i - rom_base;
    rtl_nbmem_poke(&p->back, i, o < rom_len ? rom_image[o] : 0xFF);
  }
  p->dev.rd = iopage_rd; p->dev.self = p;
  p->mod.name = "iopage"; p->mod.sens = RTL_EDGE_BIT(RTL_EFALL);
  p->mod.self = p; p->mod.eval = iopage_eval; p->mod.commit = iopage_commit;
}
void iopage_free(IoPage *p) { rtl_nbmem_free(&p->back); }
