#include "console.h"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

namespace host {

struct Console::Saved {
  termios t;
  int flags;
};

Console::Console() {}

Console::~Console() {
  use_terminal(false);
  if (script_) fclose(script_);
}

bool Console::open_script(const char* path) {
  script_ = fopen(path, "r");
  script_eof_ = (script_ == nullptr);
  return script_ != nullptr;
}

void Console::queue_input(const std::string& s) {
  for (unsigned char c : s) pending_.push_back(c == '\n' ? '\r' : c);
}

void Console::use_terminal(bool on) {
  if (on && !termios_saved_ && isatty(0)) {
    saved_ = new Saved;
    tcgetattr(0, &saved_->t);
    saved_->flags = fcntl(0, F_GETFL, 0);
    termios n = saved_->t;
    n.c_iflag &= ~(INLCR | ICRNL);
    n.c_lflag &= ~(ECHO | ICANON);
    n.c_cc[VTIME] = 0;
    n.c_cc[VMIN] = 1;
    tcsetattr(0, TCSAFLUSH, &n);
    fcntl(0, F_SETFL, saved_->flags | O_NONBLOCK);
    termios_saved_ = true;
    term_ = true;
  } else if (on && !isatty(0)) {
    // piped stdin: still poll it non-blocking
    int fl = fcntl(0, F_GETFL, 0);
    fcntl(0, F_SETFL, fl | O_NONBLOCK);
    term_ = true;
  } else if (!on && termios_saved_) {
    tcsetattr(0, TCSAFLUSH, &saved_->t);
    fcntl(0, F_SETFL, saved_->flags);
    delete saved_;
    saved_ = nullptr;
    termios_saved_ = false;
    term_ = false;
  } else if (!on) {
    term_ = false;
  }
}

int Console::poll() {
  if (hold_) return -1;
  if (!pending_.empty()) { int c = pending_.front(); pending_.pop_front(); return c; }
  if (script_ && !script_eof_) {
    int c = getc(script_);
    if (c == EOF) { script_eof_ = true; fclose(script_); script_ = nullptr; }
    else return c == '\n' ? '\r' : c;
  }
  if (term_) {
    unsigned char c;
    ssize_t r = read(0, &c, 1);
    if (r == 1) {
      if (esc_ >= 0 && c == esc_) { quit_ = true; return -1; }
      return c;
    }
  }
  return -1;
}

void Console::put(uint8_t c) {
  if (capture_) cap_.push_back(static_cast<char>(c));
  if (out_) { fputc(c, out_); fflush(out_); }
}

}  // namespace host
