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
            cpuemu_40.cpp cpuemu_44.cpp

HOST_SRCS := board_stubs.cpp harness_main.cpp

UAE_OBJS  := $(addprefix $(BUILD)/uae/,$(UAE_SRCS:.cpp=.o))
HOST_OBJS := $(addprefix $(BUILD)/host/,$(HOST_SRCS:.cpp=.o))
OBJS      := $(UAE_OBJS) $(HOST_OBJS)

BIN := $(BUILD)/mmu_harness

.PHONY: all clean objs run
all: $(BIN)

$(BIN): $(OBJS)
	@echo "  LINK  $@"
	@$(CXX) $(CXXFLAGS) $(OBJS) -o $@

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

run: $(BIN)
	@$(BIN)

clean:
	rm -rf $(BUILD)
