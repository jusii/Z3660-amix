# Top-level Makefile for Z3660 project on macOS
#
# Targets:
#   make all        -> z3660 + z3660_emu + bootbin + version (fsbl only if needed)
#   make build      -> z3660 + z3660_emu
#   make fsbl       -> build FSBL (only if not exists)
#   make z3660      -> build main firmware
#   make z3660_emu  -> build emulator firmware (core1)
#   make bootbin    -> generate BOOT.BIN
#   make lean       -> LEAN (production) core1 + BOOT-lean.BIN, side by side with BOOT.BIN
#   make profile    -> PROFILING core1 + BOOT-prof.BIN, side by side with the other two
#   make q2probe    -> WARP M0 Q2 unwind-probe core1 + BOOT-q2probe.BIN (experiment only)
#   make version    -> run make_version_alfa.py packaging script
#   make clean      -> clean app builds (FSBL/app dirs), all FOUR core1 build variants
#
# FOUR FIRMWARE VARIANTS, ONE TREE (CYBERSTORM C3 + C1, WARP M0):
#   BOOT.BIN       default -- the run-loop diagnostics are compiled in and switchable at
#                  runtime (PROBES / PERFS / CSTAT). Flash this when something needs
#                  looking at.
#   BOOT-lean.BIN  production -- core1 built -DZ3660_EMU_PROBES=0 -DZ3660_EMU_PERF=0, so
#                  the instruments AND their per-instruction predicates are gone.
#                  `Z3660_emu/src/uae/runloop_build.h` is the contract.
#   BOOT-prof.BIN  profiling -- the LEAN core1 PLUS -DZ3660_EMU_PROF=1: a wall-clock
#                  guest-PC sampler and per-stage ARM cycle attribution for the
#                  interpreter. Built on lean so the map describes the loop that ships.
#                  Not for production; it costs measured time. Contract:
#                  `Z3660_emu/src/uae/z3660_prof.h`.
#   BOOT-q2probe.BIN
#                  experiment -- the DEFAULT core1 plus `src/warp_q2probe.cpp`, which
#                  answers Plan WARP ENGINE's Q2 (can a C++ throw cross a frame of
#                  generated code?) early in boot and prints the verdict on serial. It
#                  emits code into the heap and deliberately provokes an unwind failure,
#                  so it exists to be booted once and never to be left on a card.
# The four never share objects: build / build-lean / build-prof / build-q2probe.
#

# Set the path to bootgen.exe on your host system (e.g., macOS or Linux).
# Prefer an mkbootimage on PATH (the docker build image ships one in
# /usr/local/bin); fall back to the host checkout. Overridable from the
# command line / environment.
BOOTGEN_EXE_HOST ?= $(or $(shell command -v mkbootimage 2>/dev/null),$(HOME)/git/zynq-mkbootimage/mkbootimage)
MAKE = make

# Paths (Unix side). system_cli.bif uses explicit ELF paths (mkbootimage-friendly)
# and is committed; the old default (system_macos.bif) is not in the repo, so a
# clean Linux/docker checkout could never assemble BOOT.BIN.
ifeq ($(OS),Windows_NT)
	BIF_FILE = system.bif
else
	BIF_FILE ?= system_cli.bif
endif
BIF           := Z3660_system/Alfa/$(BIF_FILE)
BOOTBIN       := Z3660_system/Alfa/sd_card/BOOT.BIN

# LEAN (production) variant -- core1 built with the run-loop diagnostics compiled out.
# Separate bif and separate output file so both images exist side by side: a metal A/B
# needs to re-flash lean-vs-default without rebuilding in between.
LEAN_BIF      := Z3660_system/Alfa/system_cli_lean.bif
LEAN_BOOTBIN  := Z3660_system/Alfa/sd_card/BOOT-lean.BIN

# PROFILING variant (CYBERSTORM C1). Same shape again: its own bif, its own output file,
# so all three images exist side by side and a metal session can re-flash between them
# without a rebuild.
PROF_BIF      := Z3660_system/Alfa/system_cli_prof.bif
PROF_BOOTBIN  := Z3660_system/Alfa/sd_card/BOOT-prof.BIN

# Q2 PROBE variant (Plan WARP ENGINE, M0). Not an instrument and not a production image:
# a one-question experiment that emits ARM code into the heap and provokes a C++ unwind
# across it. Own bif, own output file, own build dir -- and src/warp_q2probe.cpp is
# filtered out of every other variant's source list, so BOOT.BIN / BOOT-lean.BIN /
# BOOT-prof.BIN are byte-identical with the probe in the tree and with it removed.
Q2PROBE_BIF     := Z3660_system/Alfa/system_cli_q2probe.bif
Q2PROBE_BOOTBIN := Z3660_system/Alfa/sd_card/BOOT-q2probe.BIN

# THE ARCHIVE. Every image this Makefile produces is also copied here, CRC-stamped, e.g.
# archive/BOOT-lean-86C0BC18.BIN. Gitignored (Z3660_system/.gitignore ignores /Alfa/*).
#
# Each variant is written to a FIXED filename, so building a new one DESTROYS the image it
# is about to be judged against. 2026-08-23: the rung-3 rebuild overwrote BOOT-lean.BIN and
# crc DEC21748 -- the reference arm of that day's A/B, still on the card -- existed nowhere
# on the host afterwards, so the verdict session could not flash the revert it had just
# decided. One `cp` per build removes the entire class.
#
# Unconditional and dumb on purpose: the CRC is the identity, so a rebuild that changes
# nothing costs one redundant copy of the same bytes, and a rebuild that changes something
# keeps both. Pruning is a human decision about disk, never the build's.
ARCHIVE_DIR   := Z3660_system/Alfa/sd_card/archive
CRC32         := python3 -c 'import sys,zlib; print("%08X" % (zlib.crc32(open(sys.argv[1],"rb").read()) & 0xffffffff))'

define archive_image
@mkdir -p $(ARCHIVE_DIR)
@crc=`$(CRC32) $(1)`; dst=$(ARCHIVE_DIR)/`basename $(1) .BIN`-$$crc.BIN; \
 cp -p $(1) $$dst; echo "archived -> $$dst"
endef

# Component directories
FSBL_DIR      := design_1_wrapper/zynq_fsbl
Z3660_DIR     := Z3660
Z3660_EMU_DIR := Z3660_emu

# Default target
.PHONY: all build z3660 z3660_emu z3660_emu_lean z3660_emu_prof z3660_emu_q2probe \
        lean lean_prepare profile profile_prepare q2probe q2probe_prepare \
        bootbin version bsp_cortexa9_0 bsp_cortexa9_1
# Default: build firmware only to avoid rebuilding FSBL/BOOT every `make`
all: bootbin version

build: bsp_cortexa9_0 bsp_cortexa9_1 z3660 z3660_emu

# DDR speed targets for FSBL
fsbl_ddr533: $(FSBL_DIR)/fsbl_ddr533.elf

$(FSBL_DIR)/fsbl_ddr533.elf: zynq_fsbl_bsp
	@$(MAKE) -C $(FSBL_DIR) DDR_SPEED=533 -j 4

fsbl_ddr667: $(FSBL_DIR)/fsbl_ddr667.elf

$(FSBL_DIR)/fsbl_ddr667.elf: zynq_fsbl_bsp
	@$(MAKE) -C $(FSBL_DIR) DDR_SPEED=667 ddr667 -j 4

fsbl_ddr800: $(FSBL_DIR)/fsbl_ddr800.elf

$(FSBL_DIR)/fsbl_ddr800.elf: zynq_fsbl_bsp
	@$(MAKE) -C $(FSBL_DIR) DDR_SPEED=800 ddr800 -j 4

fsbl: $(FSBL_DIR)/fsbl_ddr$(DDR_SPEED).elf

$(FSBL_DIR)/fsbl_ddr$(DDR_SPEED).elf: zynq_fsbl_bsp
	@$(MAKE) -C $(FSBL_DIR) DDR_SPEED=$(DDR_SPEED) -j 4

z3660: bsp_cortexa9_0
	@$(MAKE) -C $(Z3660_DIR) -j 4

z3660_emu: bsp_cortexa9_1
	@$(MAKE) -C $(Z3660_EMU_DIR) -j 4

# LEAN core1: -DZ3660_EMU_PROBES=0 -DZ3660_EMU_PERF=0, into Z3660_emu/build-lean.
z3660_emu_lean: bsp_cortexa9_1
	@$(MAKE) -C $(Z3660_EMU_DIR) Z3660_LEAN=1 -j 4

# PROFILING core1: the lean flags plus -DZ3660_EMU_PROF=1, into Z3660_emu/build-prof.
z3660_emu_prof: bsp_cortexa9_1
	@$(MAKE) -C $(Z3660_EMU_DIR) Z3660_PROF=1 -j 4

# Q2-PROBE core1: the DEFAULT flags plus -DZ3660_WARP_Q2_PROBE=1 and the one extra source
# file, into Z3660_emu/build-q2probe.
z3660_emu_q2probe: bsp_cortexa9_1
	@$(MAKE) -C $(Z3660_EMU_DIR) Z3660_Q2PROBE=1 -j 4

# DDR_SPEED variable can be set to 533, 667, or 800
DDR_SPEED ?= 533

# Build BSP libraries for both processors
bsp_cortexa9_0:
	@if [ ! -f design_1_wrapper/ps7_cortexa9_0/standalone_domain/bsp/ps7_cortexa9_0/lib/libxil.a ]; then \
		$(MAKE) -C design_1_wrapper/ps7_cortexa9_0/standalone_domain/bsp; \
	fi

bsp_cortexa9_1:
	@if [ ! -f design_1_wrapper/ps7_cortexa9_1/standalone_ps7_cortexa9_1/bsp/ps7_cortexa9_1/lib/libxil.a ]; then \
		$(MAKE) -C design_1_wrapper/ps7_cortexa9_1/standalone_ps7_cortexa9_1/bsp; \
	fi

zynq_fsbl_bsp:
	@if [ ! -f design_1_wrapper/zynq_fsbl/fsbl.elf ]; then \
		$(MAKE) -C design_1_wrapper/zynq_fsbl; \
	fi
bsp: bsp_cortexa9_0 bsp_cortexa9_1

# Prepare for bootbin generation by ensuring fsbl.elf has correct DDR speed
bootbin_prepare: build $(FSBL_DIR)/fsbl_ddr$(DDR_SPEED).elf
	cp $(FSBL_DIR)/fsbl_ddr$(DDR_SPEED).elf $(FSBL_DIR)/fsbl.elf

bootbin: bootbin_prepare
	@mkdir -p $(dir $(BOOTBIN))
	@echo "Generating BOOT.BIN (DDR $(DDR_SPEED)MHz)"
	$(BOOTGEN_EXE_HOST) $(BIF) $(BOOTBIN)
	@echo "BOOT.BIN -> $(BOOTBIN)"
	$(call archive_image,$(BOOTBIN))

# `make lean` -> sd_card/BOOT-lean.BIN, the production image. Core0 and the FSBL are the
# same binaries the default image uses; only core1 differs. Does NOT touch BOOT.BIN.
lean_prepare: bsp_cortexa9_0 z3660 z3660_emu_lean $(FSBL_DIR)/fsbl_ddr$(DDR_SPEED).elf
	cp $(FSBL_DIR)/fsbl_ddr$(DDR_SPEED).elf $(FSBL_DIR)/fsbl.elf

lean: lean_prepare
	@mkdir -p $(dir $(LEAN_BOOTBIN))
	@echo "Generating BOOT-lean.BIN (DDR $(DDR_SPEED)MHz, run-loop diagnostics compiled out)"
	$(BOOTGEN_EXE_HOST) $(LEAN_BIF) $(LEAN_BOOTBIN)
	@echo "BOOT-lean.BIN -> $(LEAN_BOOTBIN)"
	$(call archive_image,$(LEAN_BOOTBIN))

# `make profile` -> sd_card/BOOT-prof.BIN. Core0 and the FSBL are the same binaries the
# other two images use; only core1 differs. Does NOT touch BOOT.BIN or BOOT-lean.BIN.
profile_prepare: bsp_cortexa9_0 z3660 z3660_emu_prof $(FSBL_DIR)/fsbl_ddr$(DDR_SPEED).elf
	cp $(FSBL_DIR)/fsbl_ddr$(DDR_SPEED).elf $(FSBL_DIR)/fsbl.elf

profile: profile_prepare
	@mkdir -p $(dir $(PROF_BOOTBIN))
	@echo "Generating BOOT-prof.BIN (DDR $(DDR_SPEED)MHz, lean loop + interpreter profiler)"
	$(BOOTGEN_EXE_HOST) $(PROF_BIF) $(PROF_BOOTBIN)
	@echo "BOOT-prof.BIN -> $(PROF_BOOTBIN)"
	$(call archive_image,$(PROF_BOOTBIN))

# `make q2probe` -> sd_card/BOOT-q2probe.BIN. Core0 and the FSBL are the same binaries the
# other three images use. Does NOT touch BOOT.BIN, BOOT-lean.BIN or BOOT-prof.BIN.
q2probe_prepare: bsp_cortexa9_0 z3660 z3660_emu_q2probe $(FSBL_DIR)/fsbl_ddr$(DDR_SPEED).elf
	cp $(FSBL_DIR)/fsbl_ddr$(DDR_SPEED).elf $(FSBL_DIR)/fsbl.elf

q2probe: q2probe_prepare
	@mkdir -p $(dir $(Q2PROBE_BOOTBIN))
	@echo "Generating BOOT-q2probe.BIN (DDR $(DDR_SPEED)MHz, WARP M0 Q2 unwind probe)"
	$(BOOTGEN_EXE_HOST) $(Q2PROBE_BIF) $(Q2PROBE_BOOTBIN)
	@echo "BOOT-q2probe.BIN -> $(Q2PROBE_BOOTBIN)"
	$(call archive_image,$(Q2PROBE_BOOTBIN))

version:
	python3 Z3660_system/make_version_alfa.py

# Clean targets
clean:
	# Cleaning FSBL
	-@cd $(FSBL_DIR) && rm -rf *.o *.d fsbl.elf fsbl_ddr*.elf
	-@cd $(FSBL_DIR)/zynq_fsbl_bsp/ps7_cortexa9_0/lib && rm -f *.o *.a *.d
	# Clean Z3660
	-@cd $(Z3660_DIR) && rm -rf build *.o *.d
	# Clean Z3660_emu (all four build variants)
	-@cd $(Z3660_EMU_DIR) && rm -rf build build-lean build-prof build-q2probe *.o *.d
	# Clean BSP libraries
	-@cd design_1_wrapper/ps7_cortexa9_0/standalone_domain/bsp && rm -rf *.o *.a
	-@cd design_1_wrapper/ps7_cortexa9_1/standalone_ps7_cortexa9_1/bsp && rm -rf *.o *.a
	# Clean additional Xilinx compiled libraries
	-@rm -rf design_1_wrapper/ps7_cortexa9_0/freertos10_xilinx_ps7_cortexa9_0/*.o
	-@rm -rf design_1_wrapper/ps7_cortexa9_0/freertos10_xilinx_ps7_cortexa9_0/*.a
	-@rm -rf design_1_wrapper/ps7_cortexa9_1/standalone_ps7_cortexa9_1/*.o
	-@rm -rf design_1_wrapper/ps7_cortexa9_1/standalone_ps7_cortexa9_1/*.a
	# Clean Xilinx BSP objects and archives recursively
	-@find design_1_wrapper -name "*.o" -delete
	-@find design_1_wrapper -name "*.d" -delete
	-@find design_1_wrapper -name "*.a" -delete
	-@find design_1_wrapper -name "build" -type d -exec rm -rf {} + 2>/dev/null || true
	# Clean complete

