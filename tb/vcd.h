/* vcd.h - tiny VCD (value change dump) writer for viewing the bus / core waveforms in GTKWave (C11) */
#ifndef VCD_H
#define VCD_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VCD_MAX_SIGS 64

typedef struct {
  const char *name;
  char     id[4];
  int      width;
  uint64_t (*get)(void *ctx);
  void    *ctx;
  uint64_t last;
  bool     valid;
} VcdSig;

typedef struct {
  FILE   *f;
  bool    header_done;
  VcdSig  sigs[VCD_MAX_SIGS];
  int     nsigs;
} Vcd;

static inline bool vcd_open(Vcd *v, const char *path, const char *timescale) {
  memset(v, 0, sizeof *v);
  v->f = fopen(path, "w");
  if (v->f) fprintf(v->f, "$timescale %s $end\n$scope module rtlsim $end\n", timescale ? timescale : "1ns");
  return v->f != NULL;
}
static inline void vcd_close(Vcd *v) { if (v->f) fclose(v->f); v->f = NULL; }

static inline void vcd_add(Vcd *v, const char *name, int width, uint64_t (*get)(void *), void *ctx) {
  VcdSig *s = &v->sigs[v->nsigs];
  s->name = name; s->width = width; s->get = get; s->ctx = ctx;
  int n = v->nsigs, k = 0;
  do { s->id[k++] = (char)('!' + n % 90); n /= 90; } while (n && k < 3);
  s->id[k] = 0;
  ++v->nsigs;
}
static inline void vcd_finish_header(Vcd *v) {
  if (!v->f) return;
  for (int i = 0; i < v->nsigs; ++i) fprintf(v->f, "$var wire %d %s %s $end\n", v->sigs[i].width, v->sigs[i].id, v->sigs[i].name);
  fprintf(v->f, "$upscope $end\n$enddefinitions $end\n");
  v->header_done = true;
}
static inline void vcd_sample(Vcd *v, uint64_t time_ns) {
  if (!v->f || !v->header_done) return;
  bool stamped = false;
  for (int i = 0; i < v->nsigs; ++i) {
    VcdSig *s = &v->sigs[i];
    const uint64_t x = s->get(s->ctx);
    if (s->valid && x == s->last) continue;
    if (!stamped) { fprintf(v->f, "#%llu\n", (unsigned long long)time_ns); stamped = true; }
    s->last = x; s->valid = true;
    if (s->width == 1) fprintf(v->f, "%c%s\n", x ? '1' : '0', s->id);
    else {
      fputc('b', v->f);
      for (int b = s->width - 1; b >= 0; --b) fputc((x >> b) & 1 ? '1' : '0', v->f);
      fprintf(v->f, " %s\n", s->id);
    }
  }
}

#endif
