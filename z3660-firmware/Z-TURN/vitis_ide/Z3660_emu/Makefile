# Makefile for Zynq Cortex-A9 (Core 1) bare-metal app in Z3660_emu

CROSS_COMPILE ?= arm-none-eabi-
CC      := $(CROSS_COMPILE)gcc
CXX     := $(CROSS_COMPILE)g++
AS      := $(CROSS_COMPILE)gcc
AR      := $(CROSS_COMPILE)ar
OBJCOPY := $(CROSS_COMPILE)objcopy
SIZE    := $(CROSS_COMPILE)size

ROOT    := $(abspath .)
SRCDIR  := $(ROOT)/src
LDSCRIPT:= $(SRCDIR)/lscript.ld

# ---- Build variant (CYBERSTORM C3) -----------------------------------------
# Z3660_LEAN=1 builds the LEAN (production) core1: the run-loop diagnostics are
# compiled OUT rather than merely switched off at runtime, so the hot path carries
# neither the instruments nor the predicates that gate them. See
# src/uae/runloop_build.h for what that buys and why both variants exist.
#
# It builds into its OWN directory ON PURPOSE. The variant is selected by -D flags
# and NOTHING tracks a -D as a dependency: sharing one build dir would let a
# variant switch leave half the objects from the other one in place, and the link
# would happily succeed. Separate dirs make that impossible instead of merely
# unlikely, and let both BOOT.BINs exist at once for an A/B re-flash pair.
#
# Z3660_PROF=1 (CYBERSTORM C1) is the THIRD variant: the lean flags PLUS
# -DZ3660_EMU_PROF=1, into build-prof. It is built on top of LEAN and not on top of the
# default on purpose -- a profile of the diagnostic loop would be a map of a binary nobody
# runs, and the C3 calibration priced the difference between the two loops at 9.1 % of
# guest throughput, larger than several of the effects C2 is meant to rank. See
# src/uae/z3660_prof.h.
#
# Z3660_Q2PROBE=1 is the FOURTH variant, and it is not a measuring instrument like the
# other two -- it is a one-question experiment (Plan WARP ENGINE, M0, Q2: can a C++ throw
# cross a frame of generated code?). It is the DEFAULT variant plus src/warp_q2probe.cpp,
# which emits ARM instructions into the heap, executes them, and provokes an unwind
# failure across them. It must never reach a card that is running anything. It builds
# into build-q2probe and produces BOOT-q2probe.BIN, and the probe source is filtered out
# of the source list for every other variant below -- so the other three images are
# byte-identical whether this file exists or not.
Z3660_LEAN ?= 0
Z3660_PROF ?= 0
Z3660_Q2PROBE ?= 0
ifeq ($(Z3660_Q2PROBE),1)
BUILDDIR    := $(ROOT)/build-q2probe
VARIANT_DEFS:= -DZ3660_WARP_Q2_PROBE=1
else ifeq ($(Z3660_PROF),1)
BUILDDIR    := $(ROOT)/build-prof
VARIANT_DEFS:= -DZ3660_EMU_PROBES=0 -DZ3660_EMU_PERF=0 -DZ3660_EMU_PROF=1
else ifeq ($(Z3660_LEAN),1)
BUILDDIR    := $(ROOT)/build-lean
VARIANT_DEFS:= -DZ3660_EMU_PROBES=0 -DZ3660_EMU_PERF=0
else
BUILDDIR    := $(ROOT)/build
VARIANT_DEFS:=
endif

# Xilinx BSP include paths for Core 1 (ps7_cortexa9_1)
BSP_CORE1_INC := $(ROOT)/../design_1_wrapper/ps7_cortexa9_1/standalone_ps7_cortexa9_1/bsp/ps7_cortexa9_1/include
BSP_CORE1_LIB := $(dir $(BSP_CORE1_INC))lib
# Exported BSP include/lib provided by Vitis (flattened)
BSP_CORE1_EXPORT_INC := $(ROOT)/../design_1_wrapper/export/design_1_wrapper/sw/design_1_wrapper/standalone_ps7_cortexa9_1/bspinclude/include
BSP_CORE1_EXPORT_LIB := $(ROOT)/../design_1_wrapper/export/design_1_wrapper/sw/design_1_wrapper/standalone_ps7_cortexa9_1/bsplib/lib

INCLUDES:= -I$(SRCDIR) \
           -I$(SRCDIR)/uae/include \
           -I$(SRCDIR)/musashi \
           -I$(SRCDIR)/uae/machdep \
           -I$(SRCDIR)/uae \
           -I$(SRCDIR)/uae/jit \
           -I$(SRCDIR)/uae/include/uae \
           -I$(BSP_CORE1_INC) \
           -I$(BSP_CORE1_EXPORT_INC)

# -MMD -MP: track HEADER dependencies. Until CYBERSTORM C1 nothing here did, so a header
# edit left stale objects in every build dir and the link succeeded regardless -- the
# reason `rm -rf build*` has been the standing advice. It generates .d files only and
# cannot change a single byte of output. (-D flags remain untracked; that is what the
# separate build directories are for.)
CFLAGS  := -mcpu=cortex-a9 -mfpu=neon-vfpv3 -mfloat-abi=hard -Os -std=c++11 -MMD -MP \
           -ffunction-sections -fdata-sections \
           -ffast-math -ftree-vectorize -mvectorize-with-neon-quad \
           -D__ARM_ARCH=7 -DARM_ASSEMBLY -DARMV6T2 -DARMV6_ASSEMBLY \
           $(VARIANT_DEFS)
#-Wall -Wextra -marm

# Additional C++ flags
CXXFLAGS := $(CFLAGS)
ASFLAGS := $(CFLAGS)
LDFLAGS := $(CFLAGS) -T $(LDSCRIPT) -specs=$(SRCDIR)/Xilinx.spec \
           -Wl,-build-id=none,-Map=$(BUILDDIR)/mapfile.txt \
           -Wl,--gc-sections \
           -flto -L$(BSP_CORE1_EXPORT_LIB) -L$(BSP_CORE1_LIB) -L$(BSP_CORE1_LIB)/cpu/cortexa9 -L$(dir $(BSP_CORE1_LIB))
# --gc-sections: behavior-preserving dead-code strip (critical sections are KEEP'd in lscript.ld).
# Shrinks Z3660_emu so BOOT.BIN clears the SD-write size threshold (~12MB) for TFTP deploy.
#-nostdlib -specs=nosys.specs 

# Sources (musashi/uae emulators)
C_SRCS   := $(shell find $(SRCDIR) -type f -name "*.c")
CXX_SRCS := $(shell find $(SRCDIR) -type f \( -name "*.cc" -o -name "*.cpp" -o -name "*.cxx" \))
# Exclude specific files
C_SRCS   := $(filter-out $(SRCDIR)/musashi/m68kfpu.c,$(C_SRCS))
CXX_SRCS := $(filter-out \
	$(SRCDIR)/uae/fpp_native.cpp \
	$(SRCDIR)/uae/jit/codegen_arm.cpp \
	$(SRCDIR)/uae/jit/compemu_midfunc_arm.cpp \
	$(SRCDIR)/uae/jit/compemu_midfunc_arm2.cpp,$(CXX_SRCS))
# The WARP Q2 unwind probe exists in exactly one variant. This filter is the first of the
# two gates that keep it there; the second is the #if around the whole file (and around
# its call site in main.cpp), so losing either one still ships nothing.
ifneq ($(Z3660_Q2PROBE),1)
CXX_SRCS := $(filter-out $(SRCDIR)/warp_q2probe.cpp,$(CXX_SRCS))
endif
S_SRCS := $(shell find $(SRCDIR) -type f -name "*.S")
SRCS   := $(C_SRCS) $(CXX_SRCS) $(S_SRCS)
OBJS := $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/%.o,$(C_SRCS))
OBJS += $(patsubst $(SRCDIR)/%.cc,$(BUILDDIR)/%.o,$(filter %.cc,$(CXX_SRCS)))
OBJS += $(patsubst $(SRCDIR)/%.cpp,$(BUILDDIR)/%.o,$(filter %.cpp,$(CXX_SRCS)))
OBJS += $(patsubst $(SRCDIR)/%.cxx,$(BUILDDIR)/%.o,$(filter %.cxx,$(CXX_SRCS)))
OBJS += $(patsubst $(SRCDIR)/%.S,$(BUILDDIR)/%.o,$(S_SRCS))

# THE OTHER HALF OF -MMD, AND WITHOUT IT THE FIRST HALF IS DECORATION.
#
# CFLAGS has carried -MMD -MP since CYBERSTORM C1 and the .d files have been written ever
# since -- correctly, listing every header each object really reads. Nothing ever READ them.
# So a header edit still left every object stale and the link still succeeded, which is the
# exact hazard the -MMD comment above says was fixed.
#
# WARP round 4 walked into it: moving the arm flag into `struct regstruct` touched only
# include/newcpu.h, `make lean` recompiled z3660_warp.cpp alone, and the image shipped a run
# loop still reading the OLD gate -- provably, because the literal in m68k_run_mmu040's pool
# still pointed at z3660_warp_gate. An objdump caught it; nothing in the build would have.
-include $(OBJS:.o=.d)

ELF := $(BUILDDIR)/Z3660_emu.elf
BIN := $(BUILDDIR)/Z3660_emu.bin

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

# The generated header-dependency files (see -MMD above). Absent on a first build, which
# is the one build that does not need them.
-include $(OBJS:.o=.d)

.PHONY: all clean clean-all size bin elf

all: $(ELF) $(BIN)
	@echo "Generated: $(ELF) and $(BIN)"

elf: $(ELF)

bin: $(BIN)

$(BUILDDIR)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(dir $@)
	@echo "Compiling $(notdir $<)"
	@$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(BUILDDIR)/%.o: $(SRCDIR)/%.cc
	@mkdir -p $(dir $@)
	@echo "Compiling $(notdir $<)"
	@$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(BUILDDIR)/%.o: $(SRCDIR)/%.cpp
	@mkdir -p $(dir $@)
	@echo "Compiling $(notdir $<)"
	@$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(BUILDDIR)/%.o: $(SRCDIR)/%.cxx
	@mkdir -p $(dir $@)
	@echo "Compiling $(notdir $<)"
	@$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(BUILDDIR)/%.o: $(SRCDIR)/%.S
	@mkdir -p $(dir $@)
	@echo "Compiling $(notdir $<)"
	@$(AS) $(ASFLAGS) -c $< -o $@

# Link libraries: Xilinx BSP, C/C++ stdlibs, math. Include BSP's crt0.
# Xilinx standalone BSP typically produces libxil.a and xil-crt0.o
LIBS := $(BSP_CORE1_LIB)/xil-crt0.o \
        -Wl,--start-group,-lxil,-lgcc,-lstdc++,-lm,-lc,--end-group

$(ELF): $(OBJS)
	@mkdir -p $(BUILDDIR)
	@echo "Linking Z3660_emu project"
	@$(CXX) $(LDFLAGS) $^ $(LIBS) -o $@
	$(SIZE) $@

$(BIN): $(ELF)
	$(OBJCOPY) -O binary $< $@

clean:
	rm -rf $(BUILDDIR)

# All three variants at once. `clean` above removes only the dir this invocation selected,
# which is right for a rebuild of one variant and wrong when the other two also need to go
# -- a -D is not a tracked dependency (headers now are, via -MMD), so the variants are
# kept apart by directory and cleaned together by this target.
clean-all:
	rm -rf $(ROOT)/build $(ROOT)/build-lean $(ROOT)/build-prof $(ROOT)/build-q2probe

