#include "console.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

typedef struct { struct termios t; int flags; } Saved;

void console_init(Console *c) {
  memset(c, 0, sizeof *c);
  c->script_eof = true;
  c->out = stdout;
  c->esc = -1;
}

void console_free(Console *c) {
  console_use_terminal(c, false);
  if (c->script) fclose(c->script);
  free(c->cap); free(c->pending);
  c->script = NULL; c->cap = NULL; c->pending = NULL;
}

bool console_open_script(Console *c, const char *path) {
  c->script = fopen(path, "r");
  c->script_eof = (c->script == NULL);
  return c->script != NULL;
}

void console_queue_input(Console *c, const char *s) {
  for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
    if (c->pend_head + c->pend_n == c->pend_cap) {
      if (c->pend_head) { memmove(c->pending, c->pending + c->pend_head, c->pend_n * sizeof(int)); c->pend_head = 0; }
      else { c->pend_cap = c->pend_cap ? c->pend_cap * 2 : 64; c->pending = (int *)realloc(c->pending, c->pend_cap * sizeof(int)); }
    }
    c->pending[c->pend_head + c->pend_n++] = (*p == '\n') ? '\r' : *p;
  }
}

void console_use_terminal(Console *c, bool on) {
  if (on && !c->termios_saved && isatty(0)) {
    Saved *s = (Saved *)malloc(sizeof *s);
    tcgetattr(0, &s->t);
    s->flags = fcntl(0, F_GETFL, 0);
    struct termios n = s->t;
    n.c_iflag &= ~(INLCR | ICRNL);
    n.c_lflag &= ~(ECHO | ICANON);
    n.c_cc[VTIME] = 0;
    n.c_cc[VMIN] = 1;
    tcsetattr(0, TCSAFLUSH, &n);
    fcntl(0, F_SETFL, s->flags | O_NONBLOCK);
    c->saved = s;
    c->termios_saved = true;
    c->term = true;
  } else if (on && !isatty(0)) {
    /* piped stdin: still poll it non-blocking */
    int fl = fcntl(0, F_GETFL, 0);
    fcntl(0, F_SETFL, fl | O_NONBLOCK);
    c->term = true;
  } else if (!on && c->termios_saved) {
    Saved *s = (Saved *)c->saved;
    tcsetattr(0, TCSAFLUSH, &s->t);
    fcntl(0, F_SETFL, s->flags);
    free(s);
    c->saved = NULL;
    c->termios_saved = false;
    c->term = false;
  } else if (!on) {
    c->term = false;
  }
}

int console_poll(Console *c) {
  if (c->hold) return -1;
  if (c->pend_n) { int ch = c->pending[c->pend_head++]; --c->pend_n; return ch; }
  if (c->script && !c->script_eof) {
    int ch = getc(c->script);
    if (ch == EOF) { c->script_eof = true; fclose(c->script); c->script = NULL; }
    else return ch == '\n' ? '\r' : ch;
  }
  if (c->term) {
    unsigned char ch;
    ssize_t r = read(0, &ch, 1);
    if (r == 1) {
      if (c->esc >= 0 && ch == c->esc) { c->quit = true; return -1; }
      return ch;
    }
  }
  return -1;
}

void console_put(Console *c, uint8_t ch) {
  if (c->capture) {
    if (c->cap_n + 1 >= c->cap_cap) { c->cap_cap = c->cap_cap ? c->cap_cap * 2 : 256; c->cap = (char *)realloc(c->cap, c->cap_cap); }
    c->cap[c->cap_n++] = (char)ch; c->cap[c->cap_n] = 0;
  }
  if (c->out) { fputc(ch, c->out); fflush(c->out); }
}
