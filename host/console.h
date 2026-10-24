/* console.h - host side of the serial console (not RTL: this is the "testbench"
 * side of the ACIA, like a $fgetc/$fwrite DPI call in a Verilog test bench). (C11) */
#ifndef CONSOLE_H
#define CONSOLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct Console {
  FILE *script;
  bool  script_eof;
  bool  term, termios_saved;
  FILE *out;
  bool  capture;
  char *cap; size_t cap_n, cap_cap;
  int  *pending; size_t pend_head, pend_n, pend_cap;     /* FIFO of programmatic input */
  bool  hold;                                            /* withhold all input (typed-ahead text waits) */
  int   esc;                                             /* terminal escape character (quits the simulation) */
  bool  quit;
  void *saved;                                           /* saved termios + fcntl flags */
} Console;

void console_init(Console *c);
void console_free(Console *c);
bool console_open_script(Console *c, const char *path);      /* script file (consumed first) */
void console_queue_input(Console *c, const char *s);         /* programmatic input */
void console_use_terminal(Console *c, bool on);              /* raw, non-blocking stdin */
int  console_poll(Console *c);                               /* -1 = nothing available */
void console_put(Console *c, uint8_t ch);
static inline bool console_script_done(const Console *c) { return c->script_eof && c->pend_n == 0; }

#endif
