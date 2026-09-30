// vcd.h - tiny VCD (value change dump) writer for viewing the bus / core waveforms in GTKWave
#pragma once
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace tb {

class Vcd {
 public:
  explicit Vcd(const char* path, const char* timescale = "1ns") : f_(fopen(path, "w")) {
    if (f_) fprintf(f_, "$timescale %s $end\n$scope module rtlsim $end\n", timescale);
  }
  ~Vcd() { if (f_) fclose(f_); }
  bool ok() const { return f_ != nullptr; }

  void add(const std::string& name, int width, std::function<uint64_t()> get) {
    Sig s;
    s.name = name; s.width = width; s.get = std::move(get);
    s.id = make_id(static_cast<int>(sigs_.size()));
    sigs_.push_back(std::move(s));
  }
  void finish_header() {
    if (!f_) return;
    for (auto& s : sigs_) fprintf(f_, "$var wire %d %s %s $end\n", s.width, s.id.c_str(), s.name.c_str());
    fprintf(f_, "$upscope $end\n$enddefinitions $end\n");
    header_done_ = true;
  }
  void sample(uint64_t time_ns) {
    if (!f_ || !header_done_) return;
    bool stamped = false;
    for (auto& s : sigs_) {
      const uint64_t v = s.get();
      if (s.valid && v == s.last) continue;
      if (!stamped) { fprintf(f_, "#%llu\n", (unsigned long long)time_ns); stamped = true; }
      s.last = v; s.valid = true;
      if (s.width == 1) fprintf(f_, "%c%s\n", v ? '1' : '0', s.id.c_str());
      else {
        fputc('b', f_);
        for (int b = s.width - 1; b >= 0; --b) fputc((v >> b) & 1 ? '1' : '0', f_);
        fprintf(f_, " %s\n", s.id.c_str());
      }
    }
  }

 private:
  struct Sig { std::string name, id; int width = 1; std::function<uint64_t()> get; uint64_t last = 0; bool valid = false; };
  static std::string make_id(int n) {
    std::string s;
    do { s.push_back(static_cast<char>('!' + n % 90)); n /= 90; } while (n);
    return s;
  }
  FILE* f_;
  bool header_done_ = false;
  std::vector<Sig> sigs_;
};

}  // namespace tb
