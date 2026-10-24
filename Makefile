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

clean:
	rm -rf $(BUILD)

.PHONY: all clean check-cpu
