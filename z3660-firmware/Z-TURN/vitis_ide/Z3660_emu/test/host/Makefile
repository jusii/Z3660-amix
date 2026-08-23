# Layer-1 host MMU test harness — compiles the portable UAE core subset for
# x86-64 Linux with board seams stubbed (flat-buffer memory). See README.md.
#
# Decision #6: native 64-bit, pointer-width-agnostic flat buffer, NATMEM stubbed.
# No -m32 (multilib not assumed); ASan/valgrind/gdb first-class.

CXX      := g++
ROOT     := ../..
SRC      := $(ROOT)/src
UAE      := $(SRC)/uae

# ---- Build variant, mirroring Z3660_emu/Makefile ---------------------------
# Z3660_LEAN=1 compiles the SAME sources the lean (production) firmware is built
# from: -DZ3660_EMU_PROBES=0 -DZ3660_EMU_PERF=0. Its own build dir, for the same
# reason as the firmware's -- a -D is not a tracked dependency, so sharing one dir
# would let the two variants' objects mix. `make run-lean` runs the suites against
# it; harness_main.cpp branches its expectations on runloop_build.h, so the lean
# firmware's contract is asserted, not merely compiled.
# Z3660_LEAN_EXPECTED tells the harness which variant it is SUPPOSED to be, so a
# stale/mixed build is a test failure rather than a quietly relabelled pass.
#
# Z3660_PROF=1 (CYBERSTORM C1) is the third variant and mirrors `make profile`: the
# LEAN flags PLUS -DZ3660_EMU_PROF=1. The profiling firmware is built on top of lean
# on purpose -- a profile of the default loop would describe a binary nobody runs --
# so the harness variant does the same rather than inventing a fourth combination.
Z3660_LEAN ?= 0
Z3660_PROF ?= 0
ifeq ($(Z3660_PROF),1)
BUILD        := build-prof
VARIANT_DEFS := -DZ3660_EMU_PROBES=0 -DZ3660_EMU_PERF=0 -DZ3660_EMU_PROF=1 \
                -DZ3660_LEAN_EXPECTED=1 -DZ3660_PROF_EXPECTED=1
else ifeq ($(Z3660_LEAN),1)
BUILD        := build-lean
VARIANT_DEFS := -DZ3660_EMU_PROBES=0 -DZ3660_EMU_PERF=0 -DZ3660_LEAN_EXPECTED=1
else
BUILD        := build
VARIANT_DEFS :=
endif

# Stubs dir FIRST so it shadows the absent Xilinx BSP headers.
INCLUDES := -Istubs -I$(UAE)/include -I$(UAE)/include/uae -I$(UAE)/jit -I$(SRC) -I$(UAE) -I.

# -O0 -g for fast iteration + gdb. -fno-strict-aliasing matches WinUAE assumptions.
# SDL_strcasecmp macro (sysconfig.h) -> libc strcasecmp. Exceptions ON (TRY/CATCH).
# -MMD -MP: track HEADER dependencies. Not a nicety -- without them, editing a header
# left every object stale and the link succeeded anyway, which during CYBERSTORM C1 made a
# deliberately-broken probe macro pass its own mutation test. A build system that cannot
# fail is not evidence. (-D flags are STILL untracked, which is why the variants keep
# separate build directories; the two mechanisms cover different halves of the hazard.)
# test_prof_u64_ring_depth() RE-READS the profiler source and enforces the formatter
# ring's per-printf cap against the text -- a hazard no compiler can see, because the
# broken form is legal C that prints a plausible wrong number (2026-08-23 finding F1).
# ABSOLUTE, via $(abspath): the test must not depend on the harness binary's cwd. If the
# tree moves without a rebuild the open FAILS and the test FAILS, which is the honest
# outcome -- a guard that silently skips itself is the thing being guarded against.
# Defined ABOVE CXXFLAGS because CXXFLAGS is `:=` and expands it on the spot.
PROF_SRC := $(abspath $(UAE)/z3660_prof.cpp)

# Z3660_WARP_LOCKSTEP arms the block engine's differential oracle: the same guest span run
# disarmed and then armed, compared after EVERY guest instruction across the whole set S3.1
# defines. It is a GATED BUILD and it is defined HERE and nowhere else -- the firmware
# Makefile never passes it, so WARP_LS_INSNS and WARP_DIVERGE are compiled out of every
# shipping image, which is what docs/WARP-ENGINE.md S0.5.5 requires of it.
CXXFLAGS := -O0 -g -std=c++11 -fno-strict-aliasing -fexceptions -MMD -MP \
            -DSDL_strcasecmp=strcasecmp -DHOST_TEST_HARNESS -DZ3660_WARP_LOCKSTEP=1 \
            -Wno-write-strings -Wno-narrowing $(VARIANT_DEFS) $(INCLUDES) \
            -DZ3660_PROF_SRC_PATH='"$(PROF_SRC)"'

# Portable core subset (relative to $(UAE)). All six cpuemu_* are required because
# cpustbl.cpp's op_smalltbl_N[] arrays reference handlers across all of them.
UAE_SRCS := newcpu.cpp newcpu_common.cpp readcpu.cpp cpudefs.cpp cpustbl.cpp \
            memory.cpp events.cpp fpp.cpp \
            cpuemu_0.cpp cpuemu_4.cpp cpuemu_11.cpp cpuemu_13.cpp \
            cpuemu_40.cpp cpuemu_44.cpp cpuemu_32.cpp cpuemu_31.cpp \
            cpustbl_mmu030.cpp cpustbl_mmu040.cpp \
            cpummu030.cpp cpummu.cpp newcpu_mmu_glue.cpp z3660_prof.cpp \
            z3660_warp.cpp

HOST_SRCS := board_stubs.cpp harness_main.cpp

UAE_OBJS  := $(addprefix $(BUILD)/uae/,$(UAE_SRCS:.cpp=.o))
HOST_OBJS := $(addprefix $(BUILD)/host/,$(HOST_SRCS:.cpp=.o))
OBJS      := $(UAE_OBJS) $(HOST_OBJS)

BIN := $(BUILD)/mmu_harness

# THE DEFAULT GOAL, PINNED -- and this is not a style preference.
#
# GNU make takes .DEFAULT_GOAL from the first target of the first rule it READS, and an
# `include` counts: the generated .d files below are rules, so once they exist the default
# goal silently becomes whichever object file happens to sort first. A first build (no .d
# files yet) makes `all`; every build after it made ONE object and stopped, printed
# "'...m68kcpu.o' is up to date", and exited 0.
#
# The parent Makefile invokes this one with no goal ($(MAKE) -C Z3660_emu -j 4), so the
# consequence was that `make bootbin` stopped rebuilding core1 entirely: bootgen happily
# packed a MONTHS-OLD Z3660_emu.elf into a fresh-looking BOOT.BIN, with a green build log
# and no warning anywhere. Found during CYBERSTORM C2 by disassembling the shipped lean
# image and noticing z3660_knobs_sync() did not read a mailbox word that had just been
# added to it.
#
# Pinning it costs one line and removes the entire class.
.DEFAULT_GOAL := all

# The generated header-dependency files (see -MMD above). Missing on a first build, which
# is exactly when they are not needed, hence the leading '-'.
-include $(OBJS:.o=.d)

# Standalone SCSI CD-ROM CDB unit test. Self-contained: scsi_cd_test.cpp compiles
# a3000_scsi.cpp in directly (to reach its static CDB decoders + geo[]) and stubs
# the board seams, so it links WITHOUT the UAE core objects or board_stubs.cpp.
SCSI_CD_BIN := $(BUILD)/scsi_cd_test

# core0 boot-mode classification guard. Compiles ONLY Z3660/src/config_file.h (which is
# self-contained), because the rest of core0 needs the Xilinx BSP. Catches the class of
# bug that killed the first AMIX-040 metal boot: a boot mode that core0 does not
# recognise as emulated, so core1 is never started.
BOOTMODE_BIN := $(BUILD)/bootmode_test
CORE0_CONFIG_H := $(ROOT)/../Z3660/src/config_file.h $(ROOT)/../Z3660/src/overhead_switches.h \
                  $(ROOT)/../Z3660/src/prof_config.h $(UAE)/z3660_prof.h

# AMIX RAM-window sizing guard. Compiles ONLY Z3660/src/amix_ram.h (self-contained by
# design, exactly like config_file.h above), because that header is the SINGLE copy of
# the bounds/arithmetic that core0's config parser and core1's bank map both run.
AMIX_RAM_BIN := $(BUILD)/amix_ram_test
CORE0_AMIX_RAM_H := $(ROOT)/../Z3660/src/amix_ram.h

.PHONY: all clean clean-all objs run run-lean run-prof run-scsi run-bootmode run-amix-ram
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

-include $(BUILD)/bootmode_test.d $(BUILD)/amix_ram_test.d $(BUILD)/scsi_cd_test.d

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

# The lean (production) firmware's core1, through the same suites. scsi_cd/bootmode/
# amix_ram do not touch the run loop, so the variant that matters is the MMU harness.
run-lean:
	@$(MAKE) Z3660_LEAN=1 run

# The profiling firmware's core1 (CYBERSTORM C1). Runs the whole MMU suite through the
# instrumented accessors -- which is the point: the probes sit inside the read/write/fetch
# path, so "the emulator still computes the same answers with the profiler compiled in" is
# a claim that has to be tested, not assumed. Adds the profiler's own suite on top.
run-prof:
	@$(MAKE) Z3660_PROF=1 run

run-scsi: $(SCSI_CD_BIN)
	@$(SCSI_CD_BIN)

run-bootmode: $(BOOTMODE_BIN)
	@$(BOOTMODE_BIN)

run-amix-ram: $(AMIX_RAM_BIN)
	@$(AMIX_RAM_BIN)

clean:
	rm -rf $(BUILD)

# All three variants at once -- same reasoning as Z3660_emu/Makefile's clean-all.
clean-all:
	rm -rf build build-lean build-prof
