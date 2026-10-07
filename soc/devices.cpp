// Credits / references:
//   * sbc09 (L.C. Benschop and the sbc09 team, GPLv2; S. Kono's OS-9 fork github.com/shinji-kono/sbc09):
//     I/O map ($E000 ACIA, $E030 timer/RTC, $E040 disk controller), the pdisk sector protocol and the
//     /v0 "virtual RBF" command protocol (os9/level*/vrbf.asm, src/io.c, src/vdisk.c) are modelled after it.
//   * NitrOS-9 / OS-9 Level 1 (Microware, the NitrOS-9 project) is the guest software; it is not distributed here.
#include "devices.h"
#include "../host/vdisk.h"
#include <cstring>
#include <ctime>

namespace soc {

// ---------------------------------------------------------------------------
void Timer::latch_date() {
  time_t t = fixed_ ? 1700000000 : time(nullptr);   // 2023-11-14 22:13:20 UTC when fixed
  struct tm tmv;
  if (fixed_) gmtime_r(&t, &tmv); else localtime_r(&t, &tmv);
  date.nb_wr(1, static_cast<uint8_t>(tmv.tm_year));
  date.nb_wr(2, static_cast<uint8_t>(tmv.tm_mon + 1));
  date.nb_wr(3, static_cast<uint8_t>(tmv.tm_mday));
  date.nb_wr(4, static_cast<uint8_t>(tmv.tm_hour));
  date.nb_wr(5, static_cast<uint8_t>(tmv.tm_min));
  date.nb_wr(6, static_cast<uint8_t>(tmv.tm_sec));
}

void Timer::eval(rtl::Edge) {
  uint8_t nctl = ctl.q;
  bool restart = false, ack = false, tick = false;
  if (wr_en) {
    if (sa == 0) {
      if (wdata == 0x04) latch_date();               // latch host clock into $E031..$E036
      else {
        nctl = wdata; ack = true;
        if (wdata == 0x8F) { run.nb(true); cnt.nb(0); restart = true; }
        else if (wdata == 0x80) { run.nb(false); restart = true; }
      }
    } else {
      date.nb_wr(sa & 15, wdata);
    }
  }
  if (run.q && !restart) {
    if (cnt.q + 1 >= period_) {
      const mc6809::Core& k = cpu_->q();
      if (!aligned_ || k.lic || (k.ba && !k.bs)) {   // test mode: tick only between instructions (or while in CWAI/SYNC)
        cnt.nb(0);
        tick = true;
        ++ticks;
        if (sched && iter) fprintf(sched, "T %ld\n", *iter);
      }
    } else {
      cnt.nb(cnt.q + 1);
    }
  }
  if (tick) nctl |= 0x10;
  ctl.nb(nctl);
  if (tick) flag.nb(true);
  else if (ack) flag.nb(false);
}

// ---------------------------------------------------------------------------
DiskCtl::~DiskCtl() {
  for (FILE*& f : disk_) if (f) fclose(f);
}

bool DiskCtl::attach(int drive, const char* path) {
  if (drive < 0 || drive > 1) return false;
  if (disk_[drive]) fclose(disk_[drive]);
  disk_[drive] = fopen(path, "r+b");
  return disk_[drive] != nullptr;
}

void DiskCtl::sector_io(uint8_t cmd) {
  const int drv = regs.rd(1);
  const long lsn = (regs.rd(2) << 16) | (regs.rd(3) << 8) | regs.rd(4);
  const uint16_t buf = static_cast<uint16_t>((regs.rd(5) << 8) | regs.rd(6));
  bool ok = drv <= 1 && disk_[drv] != nullptr && fseeko(disk_[drv], static_cast<off_t>(lsn) * 256, SEEK_SET) == 0;
  if (ok) {
    uint8_t sec[256];
    if (cmd == 0x81) {
      size_t n = fread(sec, 1, 256, disk_[drv]);
      for (size_t i = 0; i < n; ++i) poke(buf + i, sec[i]);
      ++reads;
    } else {
      for (int i = 0; i < 256; ++i) sec[i] = peek(buf + i);
      ok = fwrite(sec, 1, 256, disk_[drv]) == 256 && fflush(disk_[drv]) == 0;
      ++writes;
    }
  }
  if (!ok) ++errors;
  set_status(ok ? 0x00 : 0xFF);
}

void DiskCtl::eval(rtl::Edge) {
  if (!wr_en) return;
  if (sa != 0) { regs.nb_wr(sa & 15, wdata); return; }
  if (wdata == 0x81 || wdata == 0x55) sector_io(wdata);
  else if (vd_) vd_->command(*this, wdata);
  else set_status(0xFF);
}

}  // namespace soc
