# Licensing notice

| Part | License |
|---|---|
| Everything except the items below | MIT (`LICENSE`, (c) 2026 Shin'ichi KAYANUMA) |
| `host/vdisk.cpp`, `host/vdisk.h` | **GPL-2.0-only** (`LICENSES/GPL-2.0.txt`): behavioural re-implementation of sbc09 `vdisk.c`, treated as a derived work |
| `build/rtlsim` (the linked simulator binary) | **GPL-2.0-only as a whole**, because it contains `host/vdisk.cpp`. Source distribution of the repository is unaffected: each file keeps its own license. Other binaries (`ramtest`, `test_cpu`) do not link `vdisk.cpp` and are MIT |
| MAME m6809 (BSD-3-Clause, N. Woods), MC6809E data sheet | referenced as specification only; no code copied |
| sbc09 (`v09s.c`, `engine.c`, `io.c`, `vdisk.c`, `os9/`) and NitrOS-9 | **not part of this repository**; fetched and built locally by `scripts/setup_refs.sh`. Binaries built from them (`build/ref/*`, ROM images, commands, BASIC09) must not be redistributed except under their own licenses |

To make `rtlsim` MIT again, replace `host/vdisk.cpp` by an implementation that is not derived from `vdisk.c`
(or obtain permission from its author) and remove this exception.
