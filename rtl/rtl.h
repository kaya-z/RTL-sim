// rtl.h - minimal RTL simulation kernel
//
// Emulates the parts of Verilog/SystemVerilog scheduling semantics that a
// purely functional ("instruction at a time") simulator does not have:
//
//   * clock edges        : every process is triggered by an explicit edge
//   * non-blocking assign: Reg<T>::nb() writes the *next* value (d); the
//                          visible value (q) only changes in the NBA region
//   * two-region step    : (1) active region  - every triggered process runs
//                              and sees only pre-edge q values,
//                          (2) NBA region     - all registers commit q <= d
//                          (3) settle         - combinational nets are re-derived
//
// A process may therefore never observe a value written by another process
// in the same edge, exactly like `always @(posedge clk) r <= f(r, other)`.
#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
#include <cassert>

namespace rtl {

// ---------------------------------------------------------------------------
// Register: q = value visible to readers, d = value being assigned.
// After commit() d == q, so an un-assigned register holds (flip-flop w/ enable).
template <typename T>
struct Reg {
  T q{};
  T d{};
  Reg() = default;
  explicit Reg(const T& init) : q(init), d(init) {}
  inline void nb(const T& v) { d = v; }          //  r <= v
  inline void commit() { q = d; }
  inline void force(const T& v) { q = d = v; }   //  initial / $deposit
};

// ---------------------------------------------------------------------------
// Memory array whose writes are non-blocking (array[a] <= v).
class NbMem {
 public:
  explicit NbMem(size_t n = 0, uint8_t fill = 0) : a_(n, fill) {}
  void resize(size_t n, uint8_t fill = 0) { a_.assign(n, fill); }
  size_t size() const { return a_.size(); }
  inline uint8_t rd(uint32_t adr) const { return a_[adr]; }
  inline void nb_wr(uint32_t adr, uint8_t v) {
    if (adr < a_.size()) pend_.push_back({adr, v});
  }
  inline void poke(uint32_t adr, uint8_t v) { a_[adr] = v; }  // initial load only
  inline void commit() {
    if (pend_.empty()) return;
    for (auto& w : pend_) a_[w.adr] = w.v;
    pend_.clear();
  }
  uint8_t* raw() { return a_.data(); }
  const uint8_t* raw() const { return a_.data(); }

 private:
  struct W { uint32_t adr; uint8_t v; };
  std::vector<uint8_t> a_;
  std::vector<W> pend_;
};

// ---------------------------------------------------------------------------
// Clock: MC6809E style quadrature clock.  Q leads E by 90 degrees.
//
//      phase :   0     1     2     3
//      Q     : _____|-----------|_____      Q rises at phase 1 boundary
//      E     : ___________|-----------      E rises at phase 2 boundary
//
// One bus cycle = one full E period = 4 edges:
//      QRise -> ERise -> QFall -> EFall
enum class Edge : uint8_t { QRise = 0, ERise = 1, QFall = 2, EFall = 3 };
inline constexpr unsigned edge_bit(Edge e) { return 1u << static_cast<unsigned>(e); }

struct ClockState {
  bool q = false;
  bool e = false;
  uint64_t cycle = 0;     // completed E cycles
};

// ---------------------------------------------------------------------------
// Module: anything with processes + registers.
class Module {
 public:
  explicit Module(const char* name, unsigned sensitivity)
      : name_(name), sens_(sensitivity) {}
  virtual ~Module() = default;
  // Active region: read q, write d / nb_wr().  Must not commit.
  virtual void eval(Edge e) = 0;
  // NBA region.
  virtual void commit() = 0;
  const char* name() const { return name_; }
  unsigned sens() const { return sens_; }

 private:
  const char* name_;
  unsigned sens_;
};

// ---------------------------------------------------------------------------
// Simulator: owns the clock, steps edges with proper region ordering.
class Simulator {
 public:
  void add(Module* m) { mods_.push_back(m); }
  void on_settle(void (*fn)(void*), void* ctx) { settle_.push_back({fn, ctx}); }
  void on_edge_end(void (*fn)(void*, Edge), void* ctx) { hooks_.push_back({fn, ctx}); }

  const ClockState& clk() const { return clk_; }

  // Advance exactly one clock edge.
  void step_edge() {
    Edge e = next_;
    switch (e) {
      case Edge::QRise: clk_.q = true;  break;
      case Edge::ERise: clk_.e = true;  break;
      case Edge::QFall: clk_.q = false; break;
      case Edge::EFall: clk_.e = false; break;
    }
    const unsigned bit = edge_bit(e);
    // active region: everything sees pre-edge state
    for (Module* m : mods_)
      if (m->sens() & bit) m->eval(e);
    // NBA region
    for (Module* m : mods_) m->commit();
    // continuous assignments re-settle
    for (auto& s : settle_) s.fn(s.ctx);
    if (e == Edge::EFall) ++clk_.cycle;
    for (auto& h : hooks_) h.fn(h.ctx, e);
    next_ = static_cast<Edge>((static_cast<unsigned>(e) + 1) & 3);
  }

  void step_cycle() { for (int i = 0; i < 4; ++i) step_edge(); }

 private:
  struct Settle { void (*fn)(void*); void* ctx; };
  struct Hook { void (*fn)(void*, Edge); void* ctx; };
  std::vector<Module*> mods_;
  std::vector<Settle> settle_;
  std::vector<Hook> hooks_;
  ClockState clk_;
  Edge next_ = Edge::QRise;
};

}  // namespace rtl
