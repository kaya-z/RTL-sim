CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
BUILD = build

CPU_SRC = rtl/mc6809.cpp rtl/mc6809_ucode.cpp
SOC_SRC = soc/devices.cpp soc/sbc09.cpp host/console.cpp host/vdisk.cpp
HDRS    = $(wildcard rtl/*.h soc/*.h host/*.h tb/*.h)

all: $(BUILD)/rtlsim $(BUILD)/ramtest $(BUILD)/test_cpu

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/rtlsim: tb/rtlsim.cpp $(CPU_SRC) $(SOC_SRC) $(HDRS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ tb/rtlsim.cpp $(CPU_SRC) $(SOC_SRC)

$(BUILD)/ramtest: tb/ramtest.cpp $(CPU_SRC) $(HDRS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ tb/ramtest.cpp $(CPU_SRC)

$(BUILD)/test_cpu: tb/test_cpu.cpp $(CPU_SRC) $(HDRS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ tb/test_cpu.cpp $(CPU_SRC)

# directed self-checking tests of the core (no external references needed)
check-cpu: $(BUILD)/test_cpu
	$(BUILD)/test_cpu

clean:
	rm -rf $(BUILD)

.PHONY: all clean check-cpu
