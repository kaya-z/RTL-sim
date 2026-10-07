// SPDX-License-Identifier: GPL-2.0-only
// This file is licensed under the GNU General Public License, version 2 (see LICENSES/GPL-2.0.txt),
// NOT under the MIT license of the rest of this project: it re-implements the behaviour of sbc09's
// src/vdisk.c (Shinji Kono, GPL; (c) 1994-2014 L.C. Benschop and the sbc09 team) and is a derived work.
// A program linked with this file (build/rtlsim) is therefore distributed under the GPL v2 as a whole.
// Credits / references:
//   * sbc09 (L.C. Benschop and the sbc09 team, GPLv2; S. Kono's OS-9 fork github.com/shinji-kono/sbc09):
//     I/O map ($E000 ACIA, $E030 timer/RTC, $E040 disk controller), the pdisk sector protocol and the
//     /v0 "virtual RBF" command protocol (os9/level*/vrbf.asm, src/io.c, src/vdisk.c) are modelled after it.
//   * NitrOS-9 / OS-9 Level 1 (Microware, the NitrOS-9 project) is the guest software; it is not distributed here.
//   NOTE: re-implementation of vdisk.c's *behaviour* (including quirks that OS-9 programs such as `dir`
//   observe); licensed as a derived work, see the SPDX header and NOTICE.md.
// vdisk.cpp - host side of the "virtual RBF" file manager (/v0)
//
// Protocol (defined by sbc09's os9/level*/vrbf.asm, which is the OS-9 side):
//
//   VRBF stores          $E041 drive        $E044 current-directory number
//   the request in       $E045/6 caller's register frame (system address, "U")
//   the register block:  $E047/8 path descriptor address
//   and then writes the command code to $E040:
//        $D1 create  $D2 open  $D3 makdir  $D4 chgdir  $D5 delete  $D6 seek
//        $D7 readln  $D8 read  $D9 writln  $DA write   $DB close   $DC getstat  $DD setstat
//   The frame holds the caller's registers as stacked by the OS-9 kernel
//        +0 CC +1 A +2 B +3 DP +4 X(hi,lo) +6 Y +8 U +10 PC
//   The host performs the operation on the host file system, patches the frame
//   (B = error code, X, Y, A as required) and leaves the result in $E040.
//
// This is a behavioural (non-RTL) model: it runs "instantly" when the command
// byte is written, exactly like the DMA-style host call it emulates.  The
// error codes / register conventions are those the OS-9 side expects, including
// a few quirks of the original host implementation (they are visible to OS-9
// programs such as `dir`).
#include "vdisk.h"
#include "../soc/devices.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string>
#include <vector>

namespace soc {

namespace {

constexpr int kMaxPd = 256;
constexpr int kMaxPath = 256;
constexpr int kDirRec = 32, kDirName = 29;
constexpr int kFdAtt = 0, kFdOwn = 1, kFdDat = 3, kFdLnk = 8, kFdSiz = 9, kFdCreat = 13;

struct PathDesc {
  std::string name;
  bool has_name = false;
  FILE* fp = nullptr;
  bool dir = false, use = false;
  int num = 0, drv = 0;
  std::vector<uint8_t> dirbuf;     // simulated OS-9 directory file (backing store of fp)
  bool has_dir = false;
};

}  // namespace

struct Vdisk::Impl {
  std::string root[4];
  PathDesc pdv[kMaxPd];
  std::string cdt[512];
  bool cdt_set[512] = {false};
  int cdtptr = 0;
  DiskCtl* dc = nullptr;

  explicit Impl(const std::string& r) { root[0] = r; root[1] = root[2] = root[3] = "."; }
  ~Impl() { for (auto& p : pdv) if (p.fp) fclose(p.fp); }

  // ---- system-space memory helpers --------------------------------------------------------------
  uint8_t rb(uint32_t a) const { return dc->peek(a); }
  uint16_t rw(uint32_t a) const { return static_cast<uint16_t>((rb(a) << 8) | rb(a + 1)); }
  void wb(uint32_t a, uint8_t v) { dc->poke(a, v); }
  void ww(uint32_t a, uint16_t v) { wb(a, v >> 8); wb(a + 1, v & 0xFF); }
  std::string cstr(uint16_t a) const {
    std::string s;
    for (int i = 0; i < kMaxPath + 2; ++i) {
      uint8_t c = rb(static_cast<uint16_t>(a + i));
      if (c == 0) break;
      s.push_back(static_cast<char>(c));
    }
    return s;
  }

  // ---- current directories (most recent 512 names) ----------------------------------------------
  uint8_t setcd(const std::string& name) {
    for (int i = 0; i < 512; ++i)
      if (cdt_set[i] && cdt[i] == name) return static_cast<uint8_t>(i);
    cdtptr &= 0x1FF;
    cdt[cdtptr] = name;
    cdt_set[cdtptr] = true;
    return static_cast<uint8_t>(cdtptr++);
  }

  // ---- pathlists -------------------------------------------------------------------------------------
  // "/v0/foo" -> <root>/foo ; "foo" -> <cwd>/foo.  False if a relative name has no cwd.
  bool add_curdir(const std::string& in, const PathDesc& pd, int curdir, std::string* out) const {
    const char* name = in.c_str();
    if (name[0] == '/') {
      ++name;
      while (*name != '/' && *name != 0) ++name;             // skip the device name
    } else if (!cdt_set[curdir & 0x1FF]) {
      return false;
    }
    if (name[0] == '/') { *out = root[pd.drv & 3] + "/" + (name + 1); return true; }
    if (name[0] == 0) { *out = root[pd.drv & 3]; return true; }
    *out = cdt[curdir & 0x1FF] + "/" + name;
    return true;
  }

  // Parse an OS-9 pathlist (last character may have bit 7 set; may be followed by blanks).
  // Sets pd.name and returns the number of bytes consumed, or -1.
  int check_name(const std::string& raw, PathDesc& pd, int curdir) const {
    const char* start = raw.c_str();
    const char* p = start;
    int maxlen = kMaxPath;
    while (*p != 0 && (static_cast<uint8_t>(*p) & 0x80) == 0 && (static_cast<uint8_t>(*p) & 0x7F) > ' ' && maxlen-- > 0) ++p;
    if (maxlen == kMaxPath) return -1;
    std::string name;
    if (*p) {
      const bool eighth = (static_cast<uint8_t>(*p) & 0x80) != 0;
      name.assign(start, p - start);
      if (eighth) { name.push_back(static_cast<char>(static_cast<uint8_t>(*p) & 0x7F)); ++p; }
      while (*p == ' ') ++p;
    } else {
      name = raw;
    }
    std::string full;
    if (!add_curdir(name, pd, curdir, &full)) return -1;
    pd.name = full; pd.has_name = true;
    return static_cast<int>(p - start);
  }

  // ---- OS-9 <-> unix conversions -------------------------------------------------------------------------------
  static uint8_t os9_attr(mode_t m) {
    uint8_t r = 0;
    if (m & S_IFDIR) r |= 0x80;
    if (m & S_IRUSR) r |= 0x01;
    if (m & S_IWUSR) r |= 0x02;
    if (m & S_IXUSR) r |= 0x04;
    if (m & S_IROTH) r |= 0x08;
    if (m & S_IWOTH) r |= 0x10;
    if (m & S_IXOTH) r |= 0x20;
    return r | 0x60;                                          // always sharable
  }
  static const char* fmode(uint8_t mode) {
    if ((mode & 1) && (mode & 2)) return "r+";
    if (!(mode & 1) && (mode & 2)) return "w";
    return "r";
  }
  static mode_t unix_perm(uint8_t m) {
    mode_t r = 0;
    if (m & 0x80) r |= S_IFDIR;
    if (m & 0x01) r |= S_IRUSR;
    if (m & 0x02) r |= S_IWUSR;
    if (m & 0x04) r |= S_IXUSR;
    if (m & 0x08) r |= S_IROTH;
    if (m & 0x10) r |= S_IWOTH;
    if (m & 0x20) r |= S_IXOTH;
    return r;
  }
  static void os9_date(uint8_t* d, time_t t) {                // yy mm dd hh mm ss
    struct tm r;
    localtime_r(&t, &r);
    d[0] = static_cast<uint8_t>(r.tm_year - 2048);
    d[1] = static_cast<uint8_t>(r.tm_mon + 1);
    d[2] = static_cast<uint8_t>(r.tm_mday);
    d[3] = static_cast<uint8_t>(r.tm_hour);
    d[4] = static_cast<uint8_t>(r.tm_min);
    d[5] = static_cast<uint8_t>(r.tm_sec);
  }

  void close_pd(PathDesc& pd) {
    if (pd.fp) fclose(pd.fp);
    pd.dir = false; pd.use = false; pd.fp = nullptr;
    pd.dirbuf.clear(); pd.has_dir = false;
    pd.has_name = false; pd.name.clear();
  }

  // Simulated OS-9 directory: 32-byte records (29 name bytes with the last char |= $80, 3-byte LSN = inode).
  int open_dir(PathDesc& pd) {
    if (pd.has_dir) return 0;
    DIR* d = opendir(pd.has_name ? pd.name.c_str() : "");
    if (!d) return -1;
    std::vector<std::pair<std::string, uint32_t>> ents;
    while (dirent* e = readdir(d)) ents.emplace_back(e->d_name, static_cast<uint32_t>(e->d_ino));
    closedir(d);
    if (ents.empty()) return 0;
    const size_t sz = ents.size() * kDirRec;
    pd.dirbuf.assign(sz + 1, 0);
    size_t i = 0;
    for (auto& e : ents) {
      const int namlen = static_cast<int>(e.first.size());
      for (int j = 0; j < kDirName; ++j) {
        uint8_t c = 0;
        if (j < namlen) { c = static_cast<uint8_t>(e.first[j]) & 0x7F; if (j == namlen - 1) c |= 0x80; }
        pd.dirbuf[i + j] = c;
      }
      pd.dirbuf[i + kDirName] = (e.second >> 16) & 0xFF;
      pd.dirbuf[i + kDirName + 1] = (e.second >> 8) & 0xFF;
      pd.dirbuf[i + kDirName + 2] = e.second & 0xFF;
      i += kDirRec;
    }
    pd.has_dir = true;
    pd.fp = fmemopen(pd.dirbuf.data(), sz + 1, "r");
    return 0;
  }

  // ---- file descriptor sector (used by `dir -e`) ----------------------------------------------------------------
  // name: pathlist text (host path or a directory record); buf is filled through poke().
  int file_descriptor(uint16_t bufaddr, int len, const std::string& name, int curdir) {
    int err = 0x255;
    if (len < 13) return -1;
    PathDesc tmp;
    uint8_t b[24] = {0};
    if (check_name(name, tmp, curdir) < 0) return err;
    struct stat st;
    if (stat(tmp.name.c_str(), &st) != 0) return err;
    b[kFdAtt] = os9_attr(st.st_mode);
    b[kFdOwn] = (st.st_uid & 0xFF00) >> 8;
    b[kFdOwn + 1] = st.st_uid & 0xFF;
    os9_date(b + kFdDat, st.st_mtime);
    b[kFdLnk] = st.st_nlink & 0xFF;
    b[kFdSiz + 0] = (st.st_size >> 24) & 0xFF;
    b[kFdSiz + 1] = (st.st_size >> 16) & 0xFF;
    b[kFdSiz + 2] = (st.st_size >> 8) & 0xFF;
    b[kFdSiz + 3] = st.st_size & 0xFF;
    os9_date(b + kFdCreat, st.st_ctime);
    for (int i = 0; i < 19; ++i) wb(static_cast<uint16_t>(bufaddr + i), b[i]);
    return 0;
  }

  // SS.FDInf: descriptor of any file in an open directory, addressed by its "LSN" (= inode)
  int fd_info(uint16_t bufaddr, int len, uint32_t inode, int curdir) {
    for (auto& p : pdv) {
      if (!p.use || !p.dir || !p.has_dir) continue;
      const size_t sz = p.dirbuf.size() - 1;
      for (size_t o = 0; o + kDirRec <= sz; o += kDirRec) {
        const uint8_t* rec = &p.dirbuf[o];
        const uint32_t ino = (rec[kDirName] << 16) | (rec[kDirName + 1] << 8) | rec[kDirName + 2];
        if (ino == inode) {
          std::string nm;
          for (int j = 0; j < kDirName; ++j) { nm.push_back(static_cast<char>(rec[j])); if (rec[j] & 0x80) break; }
          return file_descriptor(bufaddr, len, nm, curdir);
        }
      }
    }
    return 255;
  }

  // ---- the command interpreter ------------------------------------------------------------------------------------------
  void run(DiskCtl& d, uint8_t cmd) {
    dc = &d;
    const int curdir = d.reg(4);
    const uint16_t u = static_cast<uint16_t>((d.reg(5) << 8) | d.reg(6));     // caller's register frame
    uint16_t xreg = rw(u + 4), yreg = rw(u + 6), ureg = 0;
    uint8_t areg = rb(u + 1), breg = rb(u + 2);
    const uint16_t pdaddr = static_cast<uint16_t>((d.reg(7) << 8) | d.reg(8));
    const uint8_t pdnum = rb(pdaddr);
    PathDesc* pd = &pdv[pdnum];
    pd->num = pdnum;
    pd->drv = d.reg(1);

    static const bool dbg = getenv("RTLSIM_VDEBUG") != nullptr;
    if (dbg) fprintf(stderr, "vdisk cmd %02x u=%04x x=%04x y=%04x a=%02x b=%02x pd=%02x curdir=%d path='%s'\n", cmd, u, xreg, yreg, areg, breg, pdnum, curdir, cstr(xreg).c_str());
    switch (cmd) {
      case 0xD1: {                                            // I$Create
        const uint8_t mode = areg, attr = breg;
        pd->fp = nullptr;
        int n = check_name(cstr(xreg), *pd, curdir);
        breg = 0xFF;
        int fd = open(pd->name.c_str(), O_RDWR | O_CREAT, unix_perm(attr));
        if (fd > 0) pd->fp = fdopen(fd, fmode(mode));
        if (n >= 0 && pd->fp) {
          breg = 0; areg = static_cast<uint8_t>(pd->num); wb(u + 1, areg);
          xreg = static_cast<uint16_t>(xreg + n);
          pd->use = true;
        } else {
          pd->use = false;
        }
        break;
      }
      case 0xD2: {                                            // I$Open
        const uint8_t mode = areg;
        pd->fp = nullptr; pd->has_name = false;
        int n = check_name(cstr(xreg), *pd, curdir);
        breg = 0xFF;
        if (n >= 0) {
          struct stat st;
          if (stat(pd->name.c_str(), &st) != 0) break;
          if ((st.st_mode & S_IFMT) == S_IFDIR) { pd->dir = true; open_dir(*pd); }
          else { pd->dir = false; pd->fp = fopen(pd->name.c_str(), fmode(mode)); }
          pd->use = true;
        }
        if (n >= 0 && pd->fp) {
          breg = 0; areg = static_cast<uint8_t>(pd->num); wb(u + 1, areg);
          xreg = static_cast<uint16_t>(xreg + n);
          pd->use = true;
        } else {
          pd->use = false;
        }
        break;
      }
      case 0xD3: {                                            // I$MakDir
        breg = 0xFF;
        const uint8_t attr = breg;                            // (sic) the attribute is always $FF
        int n = check_name(cstr(xreg), *pd, curdir);
        if (n >= 0 && mkdir(pd->name.c_str(), unix_perm(attr)) == 0) { xreg = static_cast<uint16_t>(xreg + n); breg = 0; }
        close_pd(*pd);
        break;
      }
      case 0xD4: {                                            // I$ChgDir : cwd/cxd are kept as small numbers
        PathDesc dm = *pd; dm.fp = nullptr;
        int n = check_name(cstr(xreg), dm, curdir);
        if (n >= 0) {
          struct stat st;
          if (stat(dm.name.c_str(), &st) != 0) break;
          if ((st.st_mode & S_IFMT) != S_IFDIR) break;
          xreg = static_cast<uint16_t>(xreg + n);
          areg = setcd(dm.name); wb(u + 1, areg);
          breg = 0;
          break;
        }
        breg = 0xFF;
        break;
      }
      case 0xD5: {                                            // I$Delete
        breg = 0xFF;
        struct stat st;
        int n = check_name(cstr(xreg), *pd, curdir);
        pd->use = false;
        if (n >= 0 && stat(pd->name.c_str(), &st) != 0) break;
        if (n >= 0 && ((st.st_mode & S_IFDIR) ? rmdir(pd->name.c_str()) : unlink(pd->name.c_str())) == 0) {
          xreg = static_cast<uint16_t>(xreg + n); breg = 0;
        }
        break;
      }
      case 0xD6: {                                            // I$Seek
        breg = 0xFF;
        ureg = rw(u + 8);
        const long seek = (static_cast<long>(xreg) << 16) + ureg;
        breg = (pd->fp && fseek(pd->fp, seek, SEEK_SET) == 0) ? 0 : 0xFF;
        break;
      }
      case 0xD7: {                                            // I$ReadLn
        breg = 0xFF;
        if (!pd->fp) break;
        std::vector<char> buf(static_cast<size_t>(yreg) + 2, 0);
        const long p0 = ftell(pd->fp);
        if (yreg > 0 && fgets(buf.data(), yreg, pd->fp)) {
          const long got = ftell(pd->fp) - p0;
          for (long i = 0; i <= got; ++i) wb(static_cast<uint16_t>(xreg + i), static_cast<uint8_t>(buf[i]));   // incl. the NUL
          int i = 0;
          while (i < yreg && buf[i]) ++i;
          if (i > 0 && buf[i - 1] == '\n') { wb(static_cast<uint16_t>(xreg + i - 1), '\r'); yreg = static_cast<uint16_t>(i); }
          ww(u + 6, yreg);
          breg = 0;
        }
        break;
      }
      case 0xD8: {                                            // I$Read
        breg = 0xFF;
        size_t n = 0;
        std::vector<uint8_t> buf(yreg);
        if (pd->fp) n = fread(buf.data(), 1, yreg, pd->fp);
        for (size_t i = 0; i < n; ++i) wb(static_cast<uint16_t>(xreg + i), buf[i]);
        ww(u + 6, static_cast<uint16_t>(n));
        breg = (n == 0) ? 0xD3 : 0;
        break;
      }
      case 0xD9: {                                            // I$WritLn
        breg = 0xFF;
        if (pd->dir || !pd->fp) break;
        int len = yreg, i = 0;
        while (len > 0 && rb(static_cast<uint16_t>(xreg + i)) != '\r') { fputc(rb(static_cast<uint16_t>(xreg + i)), pd->fp); ++i; --len; }
        if (rb(static_cast<uint16_t>(xreg + i)) == '\r') { fputc('\n', pd->fp); ++i; }
        fflush(pd->fp);
        breg = 0;
        ww(u + 6, static_cast<uint16_t>(i));
        break;
      }
      case 0xDA: {                                            // I$Write
        breg = 0xFF;
        if (!pd->dir && pd->fp) {
          std::vector<uint8_t> buf(yreg);
          for (int i = 0; i < yreg; ++i) buf[i] = rb(static_cast<uint16_t>(xreg + i));
          size_t n = fwrite(buf.data(), 1, yreg, pd->fp);
          fflush(pd->fp);
          breg = n ? 0 : 0xFF;
          ww(u + 6, static_cast<uint16_t>(n));
        }
        break;
      }
      case 0xDB:                                              // I$Close
        close_pd(*pd);
        breg = 0;
        break;
      case 0xDC: {                                            // I$GetStat (function code arrives in B)
        struct stat st;
        memset(&st, 0, sizeof st);
        switch (breg) {
          case 0x01: {                                        // SS.Ready
            breg = 0xFF;
            if (!pd->fp) break;
            if (fileno(pd->fp) >= 0) fstat(fileno(pd->fp), &st);
            long pos = ftell(pd->fp);
            if (pos) { xreg = static_cast<uint16_t>(st.st_size - pos); breg = 0; }
            break;
          }
          case 0x02:                                          // SS.Size
            breg = 0xFF;
            if (!pd->fp) break;
            if (fileno(pd->fp) >= 0) fstat(fileno(pd->fp), &st);
            xreg = static_cast<uint16_t>(st.st_size); breg = 0;
            break;
          case 0x05:                                          // SS.Pos
            breg = 0xFF;
            if (!pd->fp) break;
            xreg = static_cast<uint16_t>(ftell(pd->fp)); breg = 0;
            break;
          case 0x0F:                                          // SS.FD
            breg = 0xFF;
            breg = static_cast<uint8_t>(file_descriptor(xreg, yreg, pd->name, curdir));
            break;
          case 0x20:                                          // SS.FDInf (used by `dir`)
            breg = 0xFF;
            ureg = rw(u + 8);
            breg = static_cast<uint8_t>(fd_info(xreg, yreg & 0xFF, ((yreg & 0xFF00) >> 8) * 0x10000 + ureg, curdir));
            break;
          default: breg = 0xFF;
        }
        break;
      }
      case 0xDD:                                              // I$SetStat : nothing is supported
        breg = 0xFF;
        break;
      default: break;                                         // unknown command: B is returned unchanged
    }
    d.set_status(breg);
    wb(u + 2, breg);
    ww(u + 4, xreg);
  }
};

Vdisk::Vdisk(const std::string& root) : impl_(new Impl(root)) {}
Vdisk::~Vdisk() { delete impl_; }
void Vdisk::command(DiskCtl& dc, uint8_t cmd) { impl_->run(dc, cmd); }

}  // namespace soc
