// console.h - host side of the serial console (not RTL: this is the "testbench"
// side of the ACIA, like a $fgetc/$fwrite DPI call in a Verilog test bench).
#pragma once
#include <cstdio>
#include <cstdint>
#include <string>
#include <deque>

namespace host {

class Console {
 public:
  Console();
  ~Console();

  // input sources: a script file (consumed first) and/or the terminal
  bool open_script(const char* path);
  void queue_input(const std::string& s);       // programmatic input
  void use_terminal(bool on);                   // raw, non-blocking stdin
  void set_stdout(FILE* f) { out_ = f; }
  void set_capture(bool on) { capture_ = on; }
  void set_escape(int c) { esc_ = c; }          // terminal escape character (quits the simulation)
  bool quit() const { return quit_; }
  void hold(bool on) { hold_ = on; }            // withhold all input (typed-ahead text waits)

  int poll();                                   // -1 = nothing available
  void put(uint8_t c);

  const std::string& captured() const { return cap_; }
  bool script_done() const { return script_eof_ && pending_.empty(); }

 private:
  FILE* script_ = nullptr;
  bool script_eof_ = true;
  bool term_ = false;
  bool termios_saved_ = false;
  FILE* out_ = stdout;
  bool capture_ = false;
  std::string cap_;
  std::deque<int> pending_;
  bool hold_ = false;
  int esc_ = -1;
  bool quit_ = false;
  struct Saved;
  Saved* saved_ = nullptr;
};

}  // namespace host
