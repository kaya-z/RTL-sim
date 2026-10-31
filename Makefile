CC ?= gcc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wno-format-truncation
CPPFLAGS += -D_GNU_SOURCE
BUILD = build

CPU_SRC = rtl/mc6809.c rtl/mc6809_ucode.c
SOC_SRC = soc/devices.c soc/sbc09.c host/console.c host/vdisk.c
HDRS    = $(wildcard rtl/*.h soc/*.h host/*.h tb/*.h)

all: $(BUILD)/rtlsim $(BUILD)/ramtest $(BUILD)/test_cpu

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/rtlsim: tb/rtlsim.c $(CPU_SRC) $(SOC_SRC) $(HDRS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ tb/rtlsim.c $(CPU_SRC) $(SOC_SRC)

$(BUILD)/ramtest: tb/ramtest.c $(CPU_SRC) $(HDRS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ tb/ramtest.c $(CPU_SRC)

$(BUILD)/test_cpu: tb/test_cpu.c $(CPU_SRC) $(HDRS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ tb/test_cpu.c $(CPU_SRC)

# directed self-checking tests of the core (no external references needed)
check-cpu: $(BUILD)/test_cpu
	$(BUILD)/test_cpu

# lock-step checks against the sbc09 reference harnesses (needs scripts/setup_refs.sh; REF_BIN=<dir with refs/refos9/refos9c>)
SEEDS ?= 1 2 3 4 5
check-ref: all
	@for s in $(SEEDS); do tests/run_lockstep.sh $$s 20000 || exit 1; done
	REF_BIN=$${REF_BIN:-$(CURDIR)/build/ref} python3 tests/alu_exhaustive.py

# everything that can run without external ROM/disk images, plus the reference lock-step when available
check: check-cpu
	@if [ -x "$${REF_BIN:-build/ref}/refs" ]; then REF_BIN=$${REF_BIN:-$(CURDIR)/build/ref} $(MAKE) --no-print-directory check-ref; \
	 else echo "(reference harness not built: run scripts/setup_refs.sh to enable check-ref)"; fi

clean:
	rm -rf $(BUILD)

.PHONY: all clean check-cpu check-ref check
