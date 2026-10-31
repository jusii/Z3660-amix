# Top-level Makefile for Z3660 project on macOS
#
# Targets:
#   make all        -> z3660 + z3660_emu + bootbin + version (the tracked FSBL, never rebuilt)
#   make build      -> z3660 + z3660_emu
#   make fsbl       -> REBUILD the FSBL for DDR_SPEED (explicit only; overwrites the tracked ELFs)
#   make z3660      -> build main firmware
#   make z3660_emu  -> build emulator firmware (core1)
#   make bootbin    -> generate BOOT.BIN
#   make lean       -> LEAN (production) core1 + BOOT-lean.BIN, side by side with BOOT.BIN
#   make profile    -> PROFILING core1 + BOOT-prof.BIN, side by side with the other two
#   make q2probe    -> WARP M0 Q2 unwind-probe core1 + BOOT-q2probe.BIN (experiment only)
#   make version    -> run make_version_alfa.py packaging script
#   make clean      -> clean app builds (FSBL/app dirs), all FOUR core1 build variants;
#                      keeps the tracked FSBL ELFs and the BSP lib/ dirs
#   make bsp        -> REBUILD the core0 + core1 BSP libraries (explicit only; overwrites tracked archives)
#   make fsbl_bsp   -> REBUILD the FSBL's BSP library (explicit only)
#
# FOUR FIRMWARE VARIANTS, ONE TREE (overhead switches + profiler, WARP M0):
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
# Resolution order, each candidate existence-checked so a missing one falls
# through instead of failing later with Error 127:
#   1. an mkbootimage on PATH
#   2. a host checkout at $(HOME)/git/zynq-mkbootimage
#   3. the copy the docker/ build image ships at /usr/local/bin/mkbootimage,
#      run for just this one step via `docker run` -- a host with the ARM
#      toolchain installed natively (mkbootimage isn't packaged anywhere for
#      that case) doesn't need the whole build dockerized just for this.
# Overridable from the command line / environment; IMAGE picks the tag,
# matching docker/run.sh.
IMAGE ?= z3660-build:latest
DOCKER_MKBOOTIMAGE := docker run --rm -v $(CURDIR):/work -w /work $(IMAGE) bash -lc 'exec mkbootimage "$$@"' _
BOOTGEN_EXE_HOST ?= $(or \
	$(wildcard $(shell command -v mkbootimage 2>/dev/null)), \
	$(wildcard $(HOME)/git/zynq-mkbootimage/mkbootimage), \
	$(DOCKER_MKBOOTIMAGE))
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

# PROFILING variant (the interpreter profiler). Same shape again: its own bif, its own output file,
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
#
# The core1 emulator ELF is archived alongside the BIN, same basename and CRC (e.g.
# archive/BOOT-lean-86C0BC18.elf), because the BIN alone cannot be objdumped back to a named
# function -- it is a raw boot image, symbols gone. Two build-integrity incidents
# (docs/WARP-ENGINE.md 0.13.4, 0.15.2) turned on exactly this: a flown, CRC-identified image's
# source could no longer be re-examined once its build directory's ELF had been overwritten by
# a later build. Archiving the ELF next to the BIN it was linked into makes a flown image
# objdump-readable for as long as the archive entry exists, independent of what the build
# directory holds later. Best-effort: if the ELF is missing (a stale build directory), the BIN
# still archives and a warning is printed rather than the build failing.
ARCHIVE_DIR   := Z3660_system/Alfa/sd_card/archive
CRC32         := python3 -c 'import sys,zlib; print("%08X" % (zlib.crc32(open(sys.argv[1],"rb").read()) & 0xffffffff))'

define archive_image
@mkdir -p $(ARCHIVE_DIR)
@crc=`$(CRC32) $(1)`; base=`basename $(1) .BIN`; dst=$(ARCHIVE_DIR)/$$base-$$crc.BIN; \
 cp -p $(1) $$dst; echo "archived -> $$dst"; \
 if [ -f "$(2)" ]; then \
   elfdst=$(ARCHIVE_DIR)/$$base-$$crc.elf; \
   cp -p $(2) $$elfdst; echo "archived -> $$elfdst"; \
 else \
   echo "WARNING: $(2) not found -- ELF not archived for $$base-$$crc"; \
 fi
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

# THE FSBL IS A TRACKED INPUT, NOT A BUILD PRODUCT. fsbl.elf and fsbl_ddr533.elf are committed
# (the FSBL every release image carries), and bootbin and every variant pack them as they are.
# Until 2026-10-29 the packing targets named the FSBL ELF as a FILE target behind the phony
# zynq_fsbl_bsp, so a checkout without FSBL objects recompiled it with whatever gcc was on PATH
# and overwrote both tracked ELFs: that day's eth-rxring build came out 81,920 B short
# (4F2AAE1C) with a different bootloader in it. Rebuilding is now these phony targets only.
.PHONY: fsbl fsbl_ddr533 fsbl_ddr667 fsbl_ddr800

# DDR_SPEED variable can be set to 533, 667, or 800. Defined before its first use: a
# prerequisite list expands as it is read, so a later default left `fsbl` depending on `fsbl_ddr`.
DDR_SPEED ?= 533

fsbl: fsbl_ddr$(DDR_SPEED)

fsbl_ddr533: zynq_fsbl_bsp
	@$(MAKE) -C $(FSBL_DIR) DDR_SPEED=533 -j 4

fsbl_ddr667: zynq_fsbl_bsp
	@$(MAKE) -C $(FSBL_DIR) DDR_SPEED=667 ddr667 -j 4

fsbl_ddr800: zynq_fsbl_bsp
	@$(MAKE) -C $(FSBL_DIR) DDR_SPEED=800 ddr800 -j 4

# use_fsbl -- point the bifs' fsbl.elf at the DDR_SPEED image without building anything. For the
# default 533 the two tracked files are identical and nothing is written; another speed needs
# `make fsbl DDR_SPEED=<n>` first, and then (and only then) changes the tracked fsbl.elf.
FSBL_ELF := $(FSBL_DIR)/fsbl_ddr$(DDR_SPEED).elf
define use_fsbl
@test -f $(FSBL_ELF) || { echo "ERROR: $(FSBL_ELF) is missing -- the FSBL is never rebuilt implicitly; 'make fsbl DDR_SPEED=$(DDR_SPEED)' builds one"; exit 1; }
@cmp -s $(FSBL_ELF) $(FSBL_DIR)/fsbl.elf || { echo "fsbl.elf <- $(FSBL_ELF) (DDR $(DDR_SPEED))"; cp $(FSBL_ELF) $(FSBL_DIR)/fsbl.elf; }
endef

# THE WARP RUN-LOOP GATE, ON EVERY CORE1 BUILD (docs/WARP-ENGINE.md 0.17.3 G-R6.12).
# tools/warp-gate.sh reads both run loops off the freshly linked ELF and FAILS the build if
# the disarmed one carries a hook or names the engine at all. It exists because 0.15.2 caught
# an image that is not what its own source builds -- and because the dormant-tax fix's entire
# claim is a property of generated code, which is not checkable in a source diff. A missing
# script is a warning rather than a failure: a checkout without tools/ can still build.
WARP_GATE := $(abspath $(CURDIR)/../../../tools/warp-gate.sh)
define warp_gate
	@if [ -x "$(WARP_GATE)" ]; then "$(WARP_GATE)" "$(1)"; \
	 else echo "WARNING: $(WARP_GATE) not found -- run-loop gate NOT checked"; fi
endef

# `make gate` -- re-run the gate over EVERY core1 build directory that exists, plus the
# matcher's own self-test, WITHOUT rebuilding anything.
#
# The per-variant hook above checks a variant at the moment it is linked and never again,
# which is a check on an EVENT rather than on the tree. Round 6 built all eight variants with
# one compiler and read them once; the next build of one variant went through a different
# compiler and the two could not be compared, because nothing re-read the other seven and
# nothing recorded which toolchain any of them came from. This re-reads all of them, in one
# command, and each line now names its producer.
.PHONY: gate
gate:
	@if [ ! -x "$(WARP_GATE)" ]; then \
	   echo "$(WARP_GATE) not found"; exit 1; fi
	@"$(WARP_GATE)" --selftest
	@elfs=`ls -1 $(Z3660_EMU_DIR)/build*/Z3660_emu.elf 2>/dev/null`; \
	 if [ -z "$$elfs" ]; then \
	   echo "no core1 build directory holds a Z3660_emu.elf -- nothing to gate"; exit 1; fi; \
	 "$(WARP_GATE)" $$elfs

z3660: bsp_cortexa9_0
	@$(MAKE) -C $(Z3660_DIR) -j 4

z3660_emu: bsp_cortexa9_1
	@$(MAKE) -C $(Z3660_EMU_DIR) -j 4
	$(call warp_gate,$(Z3660_EMU_DIR)/build/Z3660_emu.elf)

# LEAN core1: -DZ3660_EMU_PROBES=0 -DZ3660_EMU_PERF=0, into Z3660_emu/build-lean.
z3660_emu_lean: bsp_cortexa9_1
	@$(MAKE) -C $(Z3660_EMU_DIR) Z3660_LEAN=1 -j 4
	$(call warp_gate,$(Z3660_EMU_DIR)/build-lean/Z3660_emu.elf)

# PROFILING core1: the lean flags plus -DZ3660_EMU_PROF=1, into Z3660_emu/build-prof.
z3660_emu_prof: bsp_cortexa9_1
	@$(MAKE) -C $(Z3660_EMU_DIR) Z3660_PROF=1 -j 4
	$(call warp_gate,$(Z3660_EMU_DIR)/build-prof/Z3660_emu.elf)

# Q2-PROBE core1: the DEFAULT flags plus -DZ3660_WARP_Q2_PROBE=1 and the one extra source
# file, into Z3660_emu/build-q2probe.
z3660_emu_q2probe: bsp_cortexa9_1
	@$(MAKE) -C $(Z3660_EMU_DIR) Z3660_Q2PROBE=1 -j 4
	$(call warp_gate,$(Z3660_EMU_DIR)/build-q2probe/Z3660_emu.elf)

# THE WARP DISARM-RESIDUAL ABLATION KIT (docs/WARP-ENGINE.md 0.14.1). Five LEAN core1 builds,
# each with exactly one layer of the engine removed, into Z3660_emu/build-abl1..5. They are an
# INSTRUMENT: a metal session boots each beside the shipping lean, takes its disarmed
# dhrystone against the same reference, and reads which layer carries the -4.256 % penalty
# that two named mechanisms have now failed to explain.
z3660_emu_abl%: bsp_cortexa9_1
	@$(MAKE) -C $(Z3660_EMU_DIR) Z3660_WARP_ABLATE=$* -j 4
	$(call warp_gate,$(Z3660_EMU_DIR)/build-abl$*/Z3660_emu.elf)


# Build BSP libraries for both processors
# THE BSP LIBRARIES ARE INPUTS TOO, like the FSBL. Their lib/ directories hold the six TRACKED
# archives (libxil.a, liblwip4.a, libxilffs.a) and the untracked xil-crt0.o both links name
# directly. They used to be rebuilt whenever libxil.a was missing -- which `make clean` arranged
# by deleting all six -- with whatever gcc was on PATH. The build now only checks they are
# there; rebuilding them is `make bsp` (core0 + core1) or `make fsbl_bsp`, explicitly.
BSP0_DIR     := design_1_wrapper/ps7_cortexa9_0/standalone_domain/bsp
BSP1_DIR     := design_1_wrapper/ps7_cortexa9_1/standalone_ps7_cortexa9_1/bsp
FSBL_BSP_DIR := $(FSBL_DIR)/zynq_fsbl_bsp
BSP_LIB_DIRS := $(BSP0_DIR)/ps7_cortexa9_0/lib $(BSP1_DIR)/ps7_cortexa9_1/lib $(FSBL_BSP_DIR)/ps7_cortexa9_0/lib

# need_bsp <lib dir> -- fail with the explicit target's name instead of rebuilding behind the build's back.
define need_bsp
@for f in $(1)/libxil.a $(1)/xil-crt0.o; do test -f $$f || { echo "ERROR: $$f is missing -- BSP libraries are never rebuilt implicitly; 'make bsp' rebuilds them"; exit 1; }; done
endef

bsp_cortexa9_0:
	$(call need_bsp,$(BSP0_DIR)/ps7_cortexa9_0/lib)

bsp_cortexa9_1:
	$(call need_bsp,$(BSP1_DIR)/ps7_cortexa9_1/lib)

zynq_fsbl_bsp:
	@if [ ! -f design_1_wrapper/zynq_fsbl/fsbl.elf ]; then \
		$(MAKE) -C design_1_wrapper/zynq_fsbl; \
	fi

# Explicit BSP rebuilds -- these overwrite the tracked archives.
.PHONY: bsp fsbl_bsp
bsp:
	$(MAKE) -C $(BSP0_DIR)
	$(MAKE) -C $(BSP1_DIR)

fsbl_bsp:
	$(MAKE) -C $(FSBL_BSP_DIR)

# Prepare for bootbin generation by ensuring fsbl.elf has correct DDR speed
bootbin_prepare: build
	$(use_fsbl)

bootbin: bootbin_prepare
	@mkdir -p $(dir $(BOOTBIN))
	@echo "Generating BOOT.BIN (DDR $(DDR_SPEED)MHz)"
	$(BOOTGEN_EXE_HOST) $(BIF) $(BOOTBIN)
	@echo "BOOT.BIN -> $(BOOTBIN)"
	$(call archive_image,$(BOOTBIN),$(Z3660_EMU_DIR)/build/Z3660_emu.elf)

# `make lean` -> sd_card/BOOT-lean.BIN, the production image. Core0 and the FSBL are the
# same binaries the default image uses; only core1 differs. Does NOT touch BOOT.BIN.
lean_prepare: bsp_cortexa9_0 z3660 z3660_emu_lean
	$(use_fsbl)

lean: lean_prepare
	@mkdir -p $(dir $(LEAN_BOOTBIN))
	@echo "Generating BOOT-lean.BIN (DDR $(DDR_SPEED)MHz, run-loop diagnostics compiled out)"
	$(BOOTGEN_EXE_HOST) $(LEAN_BIF) $(LEAN_BOOTBIN)
	@echo "BOOT-lean.BIN -> $(LEAN_BOOTBIN)"
	$(call archive_image,$(LEAN_BOOTBIN),$(Z3660_EMU_DIR)/build-lean/Z3660_emu.elf)

# `make profile` -> sd_card/BOOT-prof.BIN. Core0 and the FSBL are the same binaries the
# other two images use; only core1 differs. Does NOT touch BOOT.BIN or BOOT-lean.BIN.
profile_prepare: bsp_cortexa9_0 z3660 z3660_emu_prof
	$(use_fsbl)

profile: profile_prepare
	@mkdir -p $(dir $(PROF_BOOTBIN))
	@echo "Generating BOOT-prof.BIN (DDR $(DDR_SPEED)MHz, lean loop + interpreter profiler)"
	$(BOOTGEN_EXE_HOST) $(PROF_BIF) $(PROF_BOOTBIN)
	@echo "BOOT-prof.BIN -> $(PROF_BOOTBIN)"
	$(call archive_image,$(PROF_BOOTBIN),$(Z3660_EMU_DIR)/build-prof/Z3660_emu.elf)

# `make q2probe` -> sd_card/BOOT-q2probe.BIN. Core0 and the FSBL are the same binaries the
# other three images use. Does NOT touch BOOT.BIN, BOOT-lean.BIN or BOOT-prof.BIN.
q2probe_prepare: bsp_cortexa9_0 z3660 z3660_emu_q2probe
	$(use_fsbl)

q2probe: q2probe_prepare
	@mkdir -p $(dir $(Q2PROBE_BOOTBIN))
	@echo "Generating BOOT-q2probe.BIN (DDR $(DDR_SPEED)MHz, WARP M0 Q2 unwind probe)"
	$(BOOTGEN_EXE_HOST) $(Q2PROBE_BIF) $(Q2PROBE_BOOTBIN)
	@echo "BOOT-q2probe.BIN -> $(Q2PROBE_BOOTBIN)"
	$(call archive_image,$(Q2PROBE_BOOTBIN),$(Z3660_EMU_DIR)/build-q2probe/Z3660_emu.elf)

# `make abl1` .. `make abl5` -> sd_card/BOOT-abl<N>.BIN, each archived CRC-stamped like every
# other image. `make ablation` builds all five. Core0 and the FSBL are the same binaries the
# other images use -- an ablation changes core1's run loop and nothing else, so rebuilding
# core0 for it would be noise AND would move a variable the kit is trying to hold still.
ABL_LEVELS := 1 2 3 4 5

abl%: bsp_cortexa9_0 z3660 z3660_emu_abl%
	$(use_fsbl)
	@mkdir -p $(dir $(BOOTBIN))
	@echo "Generating BOOT-abl$*.BIN (DDR $(DDR_SPEED)MHz, WARP ablation level $*)"
	$(BOOTGEN_EXE_HOST) Z3660_system/Alfa/system_cli_abl$*.bif \
	    Z3660_system/Alfa/sd_card/BOOT-abl$*.BIN
	@echo "BOOT-abl$*.BIN -> Z3660_system/Alfa/sd_card/BOOT-abl$*.BIN"
	$(call archive_image,Z3660_system/Alfa/sd_card/BOOT-abl$*.BIN,$(Z3660_EMU_DIR)/build-abl$*/Z3660_emu.elf)

.PHONY: ablation
ablation: $(addprefix abl,$(ABL_LEVELS))

version:
	python3 Z3660_system/make_version_alfa.py

# Clean targets
clean:
	# Cleaning FSBL objects (fsbl.elf and fsbl_ddr533.elf are TRACKED inputs and stay)
	-@cd $(FSBL_DIR) && rm -rf *.o *.d fsbl_ddr667.elf fsbl_ddr800.elf
	# Clean Z3660
	-@cd $(Z3660_DIR) && rm -rf build *.o *.d
	# Clean Z3660_emu (all four build variants)
	-@cd $(Z3660_EMU_DIR) && rm -rf build build-lean build-prof build-q2probe *.o *.d
	# Clean additional Xilinx compiled libraries
	-@rm -rf design_1_wrapper/ps7_cortexa9_0/freertos10_xilinx_ps7_cortexa9_0/*.o
	-@rm -rf design_1_wrapper/ps7_cortexa9_0/freertos10_xilinx_ps7_cortexa9_0/*.a
	-@rm -rf design_1_wrapper/ps7_cortexa9_1/standalone_ps7_cortexa9_1/*.o
	-@rm -rf design_1_wrapper/ps7_cortexa9_1/standalone_ps7_cortexa9_1/*.a
	# Clean Xilinx BSP intermediates recursively -- NOT the BSP lib/ dirs (archives + xil-crt0.o), see need_bsp
	-@find design_1_wrapper $(foreach d,$(BSP_LIB_DIRS),-path $(d) -prune -o) \( -name "*.o" -o -name "*.d" \) -exec rm -f {} +
	-@find design_1_wrapper -name "build" -type d -exec rm -rf {} + 2>/dev/null || true
	# Clean complete

