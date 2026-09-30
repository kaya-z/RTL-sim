// vdisk.h - host-side "virtual RBF" (the /v0 device): maps a host directory into
// OS-9.  Behavioural model (not RTL) driven by command writes to the disk
// controller register block.
#pragma once
#include <cstdint>
#include <string>

namespace soc {

class DiskCtl;

class Vdisk {
 public:
  explicit Vdisk(const std::string& root);
  ~Vdisk();
  void command(DiskCtl& dc, uint8_t cmd);
 private:
  struct Impl;
  Impl* impl_;
};

}  // namespace soc
