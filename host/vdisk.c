/* SPDX-License-Identifier: GPL-2.0-only */
/* This file is licensed under the GNU General Public License, version 2 (see LICENSES/GPL-2.0.txt),
 * NOT under the MIT license of the rest of this project: it re-implements the behaviour of sbc09's
 * src/vdisk.c (Shinji Kono, GPL; (c) 1994-2014 L.C. Benschop and the sbc09 team) and is a derived work.
 * A program linked with this file (build/rtlsim) is therefore distributed under the GPL v2 as a whole.
 *
 * Credits / references:
 *   * sbc09 (L.C. Benschop and the sbc09 team, GPLv2; S. Kono's OS-9 fork github.com/shinji-kono/sbc09):
 *     I/O map ($E000 ACIA, $E030 timer/RTC, $E040 disk controller), the pdisk sector protocol and the
 *     /v0 "virtual RBF" command protocol (os9/level1,level2/vrbf.asm, src/io.c, src/vdisk.c) are modelled after it.
 *   * NitrOS-9 / OS-9 Level 1 (Microware, the NitrOS-9 project) is the guest software; it is not distributed here.
 *   NOTE: re-implementation of vdisk.c's *behaviour* (including quirks that OS-9 programs such as `dir`
 *   observe); licensed as a derived work, see the SPDX header and NOTICE.md.
 *
 * vdisk.c - host side of the "virtual RBF" file manager (/v0)
 *
 * Protocol (defined by sbc09's os9/level1,level2/vrbf.asm, which is the OS-9 side):
 *
 *   VRBF stores          $E041 drive        $E044 current-directory number
 *   the request in       $E045/6 caller's register frame (system address, "U")
 *   the register block:  $E047/8 path descriptor address
 *   and then writes the command code to $E040:
 *        $D1 create  $D2 open  $D3 makdir  $D4 chgdir  $D5 delete  $D6 seek
 *        $D7 readln  $D8 read  $D9 writln  $DA write   $DB close   $DC getstat  $DD setstat
 *   The frame holds the caller's registers as stacked by the OS-9 kernel
 *        +0 CC +1 A +2 B +3 DP +4 X(hi,lo) +6 Y +8 U +10 PC
 *   The host performs the operation on the host file system, patches the frame
 *   (B = error code, X, Y, A as required) and leaves the result in $E040.
 *
 * This is a behavioural (non-RTL) model: it runs "instantly" when the command
 * byte is written, exactly like the DMA-style host call it emulates.  The
 * error codes / register conventions are those the OS-9 side expects, including
 * a few quirks of the original host implementation (they are visible to OS-9
 * programs such as `dir`). */
#include "vdisk.h"
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

enum { kMaxPd = 256, kMaxPath = 256, kDirRec = 32, kDirName = 29, kNameBuf = 4096, kCdt = 512 };
enum { kFdAtt = 0, kFdOwn = 1, kFdDat = 3, kFdLnk = 8, kFdSiz = 9, kFdCreat = 13 };

typedef struct {
  char     name[kNameBuf];
  bool     has_name;
  FILE    *fp;
  bool     dir, use;
  int      num, drv;
  uint8_t *dirbuf;       /* simulated OS-9 directory file (backing store of fp), dirlen+1 bytes */
  size_t   dirlen;
  bool     has_dir;
} PathDesc;

struct Vdisk {
  char     root[4][kNameBuf];
  PathDesc pdv[kMaxPd];
  char    *cdt[kCdt];
  int      cdtptr;
  DiskCtl *dc;
};

/* ---- system-space memory helpers ------------------------------------------------------------ */
static uint8_t rb(const Vdisk *v, uint32_t a) { return diskctl_peek(v->dc, a); }
static uint16_t rw(const Vdisk *v, uint32_t a) { return (uint16_t)((rb(v, a) << 8) | rb(v, a + 1)); }
static void wb(Vdisk *v, uint32_t a, uint8_t x) { diskctl_poke(v->dc, a, x); }
static void ww(Vdisk *v, uint32_t a, uint16_t x) { wb(v, a, x >> 8); wb(v, a + 1, x & 0xFF); }
static void cstr(const Vdisk *v, uint16_t a, char *out) {
  int n = 0;
  for (int i = 0; i < kMaxPath + 2; ++i) {
    uint8_t c = rb(v, (uint16_t)(a + i));
    if (c == 0) break;
    out[n++] = (char)c;
  }
  out[n] = 0;
}

/* ---- current directories (most recent 512 names) -------------------------------------------- */
static uint8_t setcd(Vdisk *v, const char *name) {
  for (int i = 0; i < kCdt; ++i)
    if (v->cdt[i] && strcmp(v->cdt[i], name) == 0) return (uint8_t)i;
  v->cdtptr &= 0x1FF;
  free(v->cdt[v->cdtptr]);
  v->cdt[v->cdtptr] = strdup(name);
  return (uint8_t)v->cdtptr++;
}

/* ---- pathlists ---------------------------------------------------------------------------------- */
/* "/v0/foo" -> <root>/foo ; "foo" -> <cwd>/foo.  False if a relative name has no cwd. */
static bool add_curdir(const Vdisk *v, const char *in, const PathDesc *pd, int curdir, char *out) {
  const char *name = in;
  if (name[0] == '/') {
    ++name;
    while (*name != '/' && *name != 0) ++name;             /* skip the device name */
  } else if (!v->cdt[curdir & 0x1FF]) {
    return false;
  }
  if (name[0] == '/') { snprintf(out, kNameBuf, "%s/%s", v->root[pd->drv & 3], name + 1); return true; }
  if (name[0] == 0)   { snprintf(out, kNameBuf, "%s", v->root[pd->drv & 3]); return true; }
  snprintf(out, kNameBuf, "%s/%s", v->cdt[curdir & 0x1FF], name);
  return true;
}

/* Parse an OS-9 pathlist (last character may have bit 7 set; may be followed by blanks).
 * Sets pd->name and returns the number of bytes consumed, or -1. */
static int check_name(const Vdisk *v, const char *raw, PathDesc *pd, int curdir) {
  const char *start = raw;
  const char *p = start;
  int maxlen = kMaxPath;
  while (*p != 0 && ((uint8_t)*p & 0x80) == 0 && ((uint8_t)*p & 0x7F) > ' ' && maxlen-- > 0) ++p;
  if (maxlen == kMaxPath) return -1;
  char name[kNameBuf];
  if (*p) {
    const bool eighth = ((uint8_t)*p & 0x80) != 0;
    size_t n = (size_t)(p - start);
    memcpy(name, start, n);
    if (eighth) { name[n++] = (char)((uint8_t)*p & 0x7F); ++p; }
    name[n] = 0;
    while (*p == ' ') ++p;
  } else {
    snprintf(name, sizeof name, "%s", raw);
  }
  char full[kNameBuf];
  if (!add_curdir(v, name, pd, curdir, full)) return -1;
  snprintf(pd->name, kNameBuf, "%s", full); pd->has_name = true;
  return (int)(p - start);
}

/* ---- OS-9 <-> unix conversions ---------------------------------------------------------------------- */
static uint8_t os9_attr(mode_t m) {
  uint8_t r = 0;
  if (m & S_IFDIR) r |= 0x80;
  if (m & S_IRUSR) r |= 0x01;
  if (m & S_IWUSR) r |= 0x02;
  if (m & S_IXUSR) r |= 0x04;
  if (m & S_IROTH) r |= 0x08;
  if (m & S_IWOTH) r |= 0x10;
  if (m & S_IXOTH) r |= 0x20;
  return r | 0x60;                                          /* always sharable */
}
static const char *fmode(uint8_t mode) {
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
static void os9_date(uint8_t *d, time_t t) {                /* yy mm dd hh mm ss */
  struct tm r;
  localtime_r(&t, &r);
  d[0] = (uint8_t)(r.tm_year - 2048);
  d[1] = (uint8_t)(r.tm_mon + 1);
  d[2] = (uint8_t)r.tm_mday;
  d[3] = (uint8_t)r.tm_hour;
  d[4] = (uint8_t)r.tm_min;
  d[5] = (uint8_t)r.tm_sec;
}

static void close_pd(PathDesc *pd) {
  if (pd->fp) fclose(pd->fp);
  pd->dir = false; pd->use = false; pd->fp = NULL;
  free(pd->dirbuf); pd->dirbuf = NULL; pd->dirlen = 0; pd->has_dir = false;
  pd->has_name = false; pd->name[0] = 0;
}

/* Simulated OS-9 directory: 32-byte records (29 name bytes with the last char |= $80, 3-byte LSN = inode). */
typedef struct { char name[256]; uint32_t ino; } DirEnt;

static int open_dir(PathDesc *pd) {
  if (pd->has_dir) return 0;
  DIR *d = opendir(pd->has_name ? pd->name : "");
  if (!d) return -1;
  DirEnt *ents = NULL; size_t n = 0, cap = 0;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (n == cap) { cap = cap ? cap * 2 : 64; ents = (DirEnt *)realloc(ents, cap * sizeof *ents); }
    snprintf(ents[n].name, sizeof ents[n].name, "%s", e->d_name);
    ents[n].ino = (uint32_t)e->d_ino;
    ++n;
  }
  closedir(d);
  if (n == 0) { free(ents); return 0; }
  const size_t sz = n * kDirRec;
  free(pd->dirbuf);
  pd->dirbuf = (uint8_t *)calloc(sz + 1, 1);
  pd->dirlen = sz;
  size_t i = 0;
  for (size_t k = 0; k < n; ++k) {
    const int namlen = (int)strlen(ents[k].name);
    for (int j = 0; j < kDirName; ++j) {
      uint8_t c = 0;
      if (j < namlen) { c = (uint8_t)ents[k].name[j] & 0x7F; if (j == namlen - 1) c |= 0x80; }
      pd->dirbuf[i + j] = c;
    }
    pd->dirbuf[i + kDirName] = (ents[k].ino >> 16) & 0xFF;
    pd->dirbuf[i + kDirName + 1] = (ents[k].ino >> 8) & 0xFF;
    pd->dirbuf[i + kDirName + 2] = ents[k].ino & 0xFF;
    i += kDirRec;
  }
  free(ents);
  pd->has_dir = true;
  pd->fp = fmemopen(pd->dirbuf, sz + 1, "r");
  return 0;
}

/* ---- file descriptor sector (used by `dir -e`) ------------------------------------------------------ */
/* name: pathlist text (host path or a directory record); buf is filled through poke(). */
static int file_descriptor(Vdisk *v, uint16_t bufaddr, int len, const char *name, int curdir) {
  int err = 0x255;
  if (len < 13) return -1;
  PathDesc *tmp = (PathDesc *)calloc(1, sizeof *tmp);
  uint8_t b[24] = {0};
  if (check_name(v, name, tmp, curdir) < 0) { free(tmp); return err; }
  struct stat st;
  if (stat(tmp->name, &st) != 0) { free(tmp); return err; }
  free(tmp);
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
  for (int i = 0; i < 19; ++i) wb(v, (uint16_t)(bufaddr + i), b[i]);
  return 0;
}

/* SS.FDInf: descriptor of any file in an open directory, addressed by its "LSN" (= inode) */
static int fd_info(Vdisk *v, uint16_t bufaddr, int len, uint32_t inode, int curdir) {
  for (int k = 0; k < kMaxPd; ++k) {
    PathDesc *p = &v->pdv[k];
    if (!p->use || !p->dir || !p->has_dir) continue;
    const size_t sz = p->dirlen;
    for (size_t o = 0; o + kDirRec <= sz; o += kDirRec) {
      const uint8_t *rec = &p->dirbuf[o];
      const uint32_t ino = ((uint32_t)rec[kDirName] << 16) | (rec[kDirName + 1] << 8) | rec[kDirName + 2];
      if (ino == inode) {
        char nm[kDirName + 1];
        int n = 0;
        for (int j = 0; j < kDirName; ++j) { nm[n++] = (char)rec[j]; if (rec[j] & 0x80) break; }
        nm[n] = 0;
        return file_descriptor(v, bufaddr, len, nm, curdir);
      }
    }
  }
  return 255;
}

/* ---- the command interpreter -------------------------------------------------------------------------- */
static void run(Vdisk *v, DiskCtl *d, uint8_t cmd) {
  v->dc = d;
  const int curdir = diskctl_reg(d, 4);
  const uint16_t u = (uint16_t)((diskctl_reg(d, 5) << 8) | diskctl_reg(d, 6));     /* caller's register frame */
  uint16_t xreg = rw(v, u + 4), yreg = rw(v, u + 6), ureg = 0;
  uint8_t areg = rb(v, u + 1), breg = rb(v, u + 2);
  const uint16_t pdaddr = (uint16_t)((diskctl_reg(d, 7) << 8) | diskctl_reg(d, 8));
  const uint8_t pdnum = rb(v, pdaddr);
  PathDesc *pd = &v->pdv[pdnum];
  pd->num = pdnum;
  pd->drv = diskctl_reg(d, 1);

  static int dbg = -1;
  if (dbg < 0) dbg = getenv("RTLSIM_VDEBUG") != NULL;
  if (dbg) {
    char s[kMaxPath + 4]; cstr(v, xreg, s);
    fprintf(stderr, "vdisk cmd %02x u=%04x x=%04x y=%04x a=%02x b=%02x pd=%02x curdir=%d path='%s'\n", cmd, u, xreg, yreg, areg, breg, pdnum, curdir, s);
  }
  char path[kMaxPath + 4];
  switch (cmd) {
    case 0xD1: {                                            /* I$Create */
      const uint8_t mode = areg, attr = breg;
      pd->fp = NULL;
      cstr(v, xreg, path);
      int n = check_name(v, path, pd, curdir);
      breg = 0xFF;
      int fd = open(pd->name, O_RDWR | O_CREAT, unix_perm(attr));
      if (fd > 0) pd->fp = fdopen(fd, fmode(mode));
      if (n >= 0 && pd->fp) {
        breg = 0; areg = (uint8_t)pd->num; wb(v, u + 1, areg);
        xreg = (uint16_t)(xreg + n);
        pd->use = true;
      } else {
        pd->use = false;
      }
      break;
    }
    case 0xD2: {                                            /* I$Open */
      const uint8_t mode = areg;
      pd->fp = NULL; pd->has_name = false;
      cstr(v, xreg, path);
      int n = check_name(v, path, pd, curdir);
      breg = 0xFF;
      if (n >= 0) {
        struct stat st;
        if (stat(pd->name, &st) != 0) break;
        if ((st.st_mode & S_IFMT) == S_IFDIR) { pd->dir = true; open_dir(pd); }
        else { pd->dir = false; pd->fp = fopen(pd->name, fmode(mode)); }
        pd->use = true;
      }
      if (n >= 0 && pd->fp) {
        breg = 0; areg = (uint8_t)pd->num; wb(v, u + 1, areg);
        xreg = (uint16_t)(xreg + n);
        pd->use = true;
      } else {
        pd->use = false;
      }
      break;
    }
    case 0xD3: {                                            /* I$MakDir */
      breg = 0xFF;
      const uint8_t attr = breg;                            /* (sic) the attribute is always $FF */
      cstr(v, xreg, path);
      int n = check_name(v, path, pd, curdir);
      if (n >= 0 && mkdir(pd->name, unix_perm(attr)) == 0) { xreg = (uint16_t)(xreg + n); breg = 0; }
      close_pd(pd);
      break;
    }
    case 0xD4: {                                            /* I$ChgDir : cwd/cxd are kept as small numbers */
      PathDesc *dm = (PathDesc *)malloc(sizeof *dm);
      *dm = *pd; dm->fp = NULL;
      cstr(v, xreg, path);
      int n = check_name(v, path, dm, curdir);
      if (n >= 0) {
        struct stat st;
        if (stat(dm->name, &st) != 0) { free(dm); break; }
        if ((st.st_mode & S_IFMT) != S_IFDIR) { free(dm); break; }
        xreg = (uint16_t)(xreg + n);
        areg = setcd(v, dm->name); wb(v, u + 1, areg);
        breg = 0;
        free(dm);
        break;
      }
      free(dm);
      breg = 0xFF;
      break;
    }
    case 0xD5: {                                            /* I$Delete */
      breg = 0xFF;
      struct stat st;
      cstr(v, xreg, path);
      int n = check_name(v, path, pd, curdir);
      pd->use = false;
      if (n >= 0 && stat(pd->name, &st) != 0) break;
      if (n >= 0 && ((st.st_mode & S_IFDIR) ? rmdir(pd->name) : unlink(pd->name)) == 0) {
        xreg = (uint16_t)(xreg + n); breg = 0;
      }
      break;
    }
    case 0xD6: {                                            /* I$Seek */
      breg = 0xFF;
      ureg = rw(v, u + 8);
      const long seek = ((long)xreg << 16) + ureg;
      breg = (pd->fp && fseek(pd->fp, seek, SEEK_SET) == 0) ? 0 : 0xFF;
      break;
    }
    case 0xD7: {                                            /* I$ReadLn */
      breg = 0xFF;
      if (!pd->fp) break;
      char *buf = (char *)calloc((size_t)yreg + 2, 1);
      const long p0 = ftell(pd->fp);
      if (yreg > 0 && fgets(buf, yreg, pd->fp)) {
        const long got = ftell(pd->fp) - p0;
        for (long i = 0; i <= got; ++i) wb(v, (uint16_t)(xreg + i), (uint8_t)buf[i]);   /* incl. the NUL */
        int i = 0;
        while (i < yreg && buf[i]) ++i;
        if (i > 0 && buf[i - 1] == '\n') { wb(v, (uint16_t)(xreg + i - 1), '\r'); yreg = (uint16_t)i; }
        ww(v, u + 6, yreg);
        breg = 0;
      }
      free(buf);
      break;
    }
    case 0xD8: {                                            /* I$Read */
      breg = 0xFF;
      size_t n = 0;
      uint8_t *buf = (uint8_t *)malloc(yreg ? yreg : 1);
      if (pd->fp) n = fread(buf, 1, yreg, pd->fp);
      for (size_t i = 0; i < n; ++i) wb(v, (uint16_t)(xreg + i), buf[i]);
      free(buf);
      ww(v, u + 6, (uint16_t)n);
      breg = (n == 0) ? 0xD3 : 0;
      break;
    }
    case 0xD9: {                                            /* I$WritLn */
      breg = 0xFF;
      if (pd->dir || !pd->fp) break;
      int len = yreg, i = 0;
      while (len > 0 && rb(v, (uint16_t)(xreg + i)) != '\r') { fputc(rb(v, (uint16_t)(xreg + i)), pd->fp); ++i; --len; }
      if (rb(v, (uint16_t)(xreg + i)) == '\r') { fputc('\n', pd->fp); ++i; }
      fflush(pd->fp);
      breg = 0;
      ww(v, u + 6, (uint16_t)i);
      break;
    }
    case 0xDA: {                                            /* I$Write */
      breg = 0xFF;
      if (!pd->dir && pd->fp) {
        uint8_t *buf = (uint8_t *)malloc(yreg ? yreg : 1);
        for (int i = 0; i < yreg; ++i) buf[i] = rb(v, (uint16_t)(xreg + i));
        size_t n = fwrite(buf, 1, yreg, pd->fp);
        free(buf);
        fflush(pd->fp);
        breg = n ? 0 : 0xFF;
        ww(v, u + 6, (uint16_t)n);
      }
      break;
    }
    case 0xDB:                                              /* I$Close */
      close_pd(pd);
      breg = 0;
      break;
    case 0xDC: {                                            /* I$GetStat (function code arrives in B) */
      struct stat st;
      memset(&st, 0, sizeof st);
      switch (breg) {
        case 0x01: {                                        /* SS.Ready */
          breg = 0xFF;
          if (!pd->fp) break;
          if (fileno(pd->fp) >= 0) fstat(fileno(pd->fp), &st);
          long pos = ftell(pd->fp);
          if (pos) { xreg = (uint16_t)(st.st_size - pos); breg = 0; }
          break;
        }
        case 0x02:                                          /* SS.Size */
          breg = 0xFF;
          if (!pd->fp) break;
          if (fileno(pd->fp) >= 0) fstat(fileno(pd->fp), &st);
          xreg = (uint16_t)st.st_size; breg = 0;
          break;
        case 0x05:                                          /* SS.Pos */
          breg = 0xFF;
          if (!pd->fp) break;
          xreg = (uint16_t)ftell(pd->fp); breg = 0;
          break;
        case 0x0F:                                          /* SS.FD */
          breg = 0xFF;
          breg = (uint8_t)file_descriptor(v, xreg, yreg, pd->name, curdir);
          break;
        case 0x20:                                          /* SS.FDInf (used by `dir`) */
          breg = 0xFF;
          ureg = rw(v, u + 8);
          breg = (uint8_t)fd_info(v, xreg, yreg & 0xFF, ((yreg & 0xFF00) >> 8) * 0x10000 + ureg, curdir);
          break;
        default: breg = 0xFF;
      }
      break;
    }
    case 0xDD:                                              /* I$SetStat : nothing is supported */
      breg = 0xFF;
      break;
    default: break;                                         /* unknown command: B is returned unchanged */
  }
  diskctl_set_status(d, breg);
  wb(v, u + 2, breg);
  ww(v, u + 4, xreg);
}

Vdisk *vdisk_new(const char *root) {
  Vdisk *v = (Vdisk *)calloc(1, sizeof *v);
  snprintf(v->root[0], kNameBuf, "%s", root);
  for (int i = 1; i < 4; ++i) snprintf(v->root[i], kNameBuf, ".");
  return v;
}
void vdisk_free(Vdisk *v) {
  if (!v) return;
  for (int i = 0; i < kMaxPd; ++i) {
    if (v->pdv[i].fp) fclose(v->pdv[i].fp);
    free(v->pdv[i].dirbuf);
  }
  for (int i = 0; i < kCdt; ++i) free(v->cdt[i]);
  free(v);
}
void vdisk_command(Vdisk *v, DiskCtl *dc, uint8_t cmd) { run(v, dc, cmd); }
