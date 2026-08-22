# Layer-1 host MMU test harness — compiles the portable UAE core subset for
# x86-64 Linux with board seams stubbed (flat-buffer memory). See README.md.
#
# Decision #6: native 64-bit, pointer-width-agnostic flat buffer, NATMEM stubbed.
# No -m32 (multilib not assumed); ASan/valgrind/gdb first-class.

CXX      := g++
ROOT     := ../..
SRC      := $(ROOT)/src
UAE      := $(SRC)/uae
BUILD    := build

# Stubs dir FIRST so it shadows the absent Xilinx BSP headers.
INCLUDES := -Istubs -I$(UAE)/include -I$(UAE)/include/uae -I$(UAE)/jit -I$(SRC) -I$(UAE) -I.

# -O0 -g for fast iteration + gdb. -fno-strict-aliasing matches WinUAE assumptions.
# SDL_strcasecmp macro (sysconfig.h) -> libc strcasecmp. Exceptions ON (TRY/CATCH).
CXXFLAGS := -O0 -g -std=c++11 -fno-strict-aliasing -fexceptions \
            -DSDL_strcasecmp=strcasecmp -DHOST_TEST_HARNESS \
            -Wno-write-strings -Wno-narrowing $(INCLUDES)

# Portable core subset (relative to $(UAE)). All six cpuemu_* are required because
# cpustbl.cpp's op_smalltbl_N[] arrays reference handlers across all of them.
UAE_SRCS := newcpu.cpp newcpu_common.cpp readcpu.cpp cpudefs.cpp cpustbl.cpp \
            memory.cpp events.cpp fpp.cpp \
            cpuemu_0.cpp cpuemu_4.cpp cpuemu_11.cpp cpuemu_13.cpp \
            cpuemu_40.cpp cpuemu_44.cpp cpuemu_32.cpp cpuemu_31.cpp \
            cpustbl_mmu030.cpp cpustbl_mmu040.cpp \
            cpummu030.cpp cpummu.cpp newcpu_mmu_glue.cpp

HOST_SRCS := board_stubs.cpp harness_main.cpp

UAE_OBJS  := $(addprefix $(BUILD)/uae/,$(UAE_SRCS:.cpp=.o))
HOST_OBJS := $(addprefix $(BUILD)/host/,$(HOST_SRCS:.cpp=.o))
OBJS      := $(UAE_OBJS) $(HOST_OBJS)

BIN := $(BUILD)/mmu_harness

# Standalone SCSI CD-ROM CDB unit test. Self-contained: scsi_cd_test.cpp compiles
# a3000_scsi.cpp in directly (to reach its static CDB decoders + geo[]) and stubs
# the board seams, so it links WITHOUT the UAE core objects or board_stubs.cpp.
SCSI_CD_BIN := $(BUILD)/scsi_cd_test

# core0 boot-mode classification guard. Compiles ONLY Z3660/src/config_file.h (which is
# self-contained), because the rest of core0 needs the Xilinx BSP. Catches the class of
# bug that killed the first AMIX-040 metal boot: a boot mode that core0 does not
# recognise as emulated, so core1 is never started.
BOOTMODE_BIN := $(BUILD)/bootmode_test
CORE0_CONFIG_H := $(ROOT)/../Z3660/src/config_file.h $(ROOT)/../Z3660/src/overhead_switches.h

# AMIX RAM-window sizing guard. Compiles ONLY Z3660/src/amix_ram.h (self-contained by
# design, exactly like config_file.h above), because that header is the SINGLE copy of
# the bounds/arithmetic that core0's config parser and core1's bank map both run.
AMIX_RAM_BIN := $(BUILD)/amix_ram_test
CORE0_AMIX_RAM_H := $(ROOT)/../Z3660/src/amix_ram.h

.PHONY: all clean objs run run-scsi run-bootmode run-amix-ram
all: $(BIN) $(SCSI_CD_BIN) $(BOOTMODE_BIN) $(AMIX_RAM_BIN)

$(BIN): $(OBJS)
	@echo "  LINK  $@"
	@$(CXX) $(CXXFLAGS) $(OBJS) -o $@

$(SCSI_CD_BIN): scsi_cd_test.cpp $(UAE)/a3000_scsi.cpp
	@mkdir -p $(BUILD)
	@echo "  CXX+LINK  scsi_cd_test.cpp"
	@$(CXX) $(CXXFLAGS) scsi_cd_test.cpp -o $@

# Just compile every object (no link) — shakes out compile errors per-file.
objs: $(OBJS)

$(BUILD)/uae/%.o: $(UAE)/%.cpp
	@mkdir -p $(dir $@)
	@echo "  CXX   uae/$*.cpp"
	@$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/host/%.o: %.cpp
	@mkdir -p $(dir $@)
	@echo "  CXX   host/$*.cpp"
	@$(CXX) $(CXXFLAGS) -c $< -o $@

$(BOOTMODE_BIN): bootmode_test.cpp $(CORE0_CONFIG_H)
	@mkdir -p $(BUILD)
	@echo "  CXX+LINK  bootmode_test.cpp"
	@$(CXX) $(CXXFLAGS) bootmode_test.cpp -o $@

$(AMIX_RAM_BIN): amix_ram_test.cpp $(CORE0_AMIX_RAM_H)
	@mkdir -p $(BUILD)
	@echo "  CXX+LINK  amix_ram_test.cpp"
	@$(CXX) $(CXXFLAGS) amix_ram_test.cpp -o $@

run: $(BIN) $(SCSI_CD_BIN) $(BOOTMODE_BIN) $(AMIX_RAM_BIN)
	@$(BIN)
	@$(SCSI_CD_BIN)
	@$(BOOTMODE_BIN)
	@$(AMIX_RAM_BIN)

run-scsi: $(SCSI_CD_BIN)
	@$(SCSI_CD_BIN)

run-bootmode: $(BOOTMODE_BIN)
	@$(BOOTMODE_BIN)

run-amix-ram: $(AMIX_RAM_BIN)
	@$(AMIX_RAM_BIN)

clean:
	rm -rf $(BUILD)
