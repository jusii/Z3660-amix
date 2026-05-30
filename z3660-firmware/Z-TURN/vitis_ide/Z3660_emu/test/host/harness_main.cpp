/* Layer-1 host MMU test harness — driver + tests.
 *
 * Brings up the UAE 68030 interpreter (direct mode, no JIT, no MMU yet) on the
 * flat-RAM machine from board_stubs.cpp and exercises it. These foundation tests
 * validate the whole accessor pipeline (decode -> handler -> x_* -> bank ->
 * flat buffer) that the real MMU translation tests will later sit on top of.
 *
 * Run loop contract copied from m68k_run_2_020() in newcpu.cpp:
 *     regs.opcode = get_diword(0);
 *     (*cpufunctbl[regs.opcode])(regs.opcode);
 */

#include "sysconfig.h"
#include "sysdeps.h"
#include "options.h"
#include "memory.h"
#include "newcpu.h"
#include "harness.h"

#include <cstdio>
#include <cstring>

extern "C" void harness_set_x_funcs(void);   /* HOST_TEST_HARNESS hook in newcpu.cpp */
extern void build_cpufunctbl(void);          /* defined in newcpu.cpp (no header decl) */

/* ------------------------------------------------------------------ */
static int g_pass = 0, g_fail = 0;
#define CHECK(cond, ...) do { \
	if (cond) { g_pass++; } \
	else { g_fail++; printf("  FAIL: " __VA_ARGS__); printf("   [%s:%d]\n", __FILE__, __LINE__); } \
} while (0)

#define CHECK_EQ32(got, want, label) do { \
	uae_u32 _g = (uae_u32)(got), _w = (uae_u32)(want); \
	if (_g == _w) { g_pass++; } \
	else { g_fail++; printf("  FAIL: %s: got 0x%08x want 0x%08x\n", (label), _g, _w); } \
} while (0)

/* ------------------------------------------------------------------ */
static void cpu_bringup(int cpu_model)
{
	memset(&currprefs, 0, sizeof currprefs);
	currprefs.cpu_model        = cpu_model;
	currprefs.fpu_model        = 0;
	currprefs.cpu_compatible   = false;   /* -> direct interpreter (mode 0) */
	currprefs.cpu_cycle_exact  = false;
	currprefs.cpu_memory_cycle_exact = false;
	currprefs.cachesize        = 0;       /* JIT off (mandatory for MMU mode) */
	currprefs.address_space_24 = false;
	currprefs.mmu_model        = 0;
	currprefs.m68k_speed       = -1;
	changed_prefs = currprefs;

	init_m68k();
	build_cpufunctbl();
	harness_set_x_funcs();

	regs.address_space_mask = 0xffffffff;
}

/* Set up a clean supervisor context with stack at 0x00080000 and PC at `start`. */
static void cpu_set_context(uae_u32 start)
{
	regs.s = 1;
	regs.m = 0;
	regs.t0 = regs.t1 = 0;
	regs.intmask = 7;
	regs.spcflags = 0;
	m68k_areg(regs, 7) = 0x00080000;
	m68k_setpc(start);
}

/* Single interpreted instruction at PC (direct, no prefetch). */
static void step(void)
{
	regs.instruction_pc = m68k_getpc();
	regs.opcode = get_diword(0);
	(*cpufunctbl[regs.opcode])(regs.opcode);
}

/* ================================================================== */
static void test_moveq(void)
{
	printf("[test] moveq #imm,Dn\n");
	cpu_set_context(0x2000);
	hram_poke16(0x2000, 0x7042);   /* moveq #$42,d0 */
	step();
	CHECK_EQ32(m68k_dreg(regs, 0), 0x00000042, "d0 after moveq #$42");
	CHECK_EQ32(m68k_getpc(), 0x2002, "pc advanced by 2");
}

static void test_add_sequence(void)
{
	printf("[test] moveq;moveq;add.l Dn,Dn\n");
	cpu_set_context(0x2000);
	hram_poke16(0x2000, 0x7005);   /* moveq #5,d0 */
	hram_poke16(0x2002, 0x7203);   /* moveq #3,d1 */
	hram_poke16(0x2004, 0xD081);   /* add.l d1,d0 */
	step(); step(); step();
	CHECK_EQ32(m68k_dreg(regs, 0), 8, "d0 = 5 + 3");
	CHECK_EQ32(m68k_dreg(regs, 1), 3, "d1 = 3");
	CHECK_EQ32(m68k_getpc(), 0x2006, "pc advanced by 6");
}

static void test_immediate_and_memory(void)
{
	printf("[test] move.l #imm,d0 ; move.l d0,(a0) -> flat buffer\n");
	cpu_set_context(0x2000);
	m68k_areg(regs, 0) = 0x00010000;     /* a0 -> target RAM address */
	hram_poke16(0x2000, 0x203C);         /* move.l #imm,d0 */
	hram_poke32(0x2002, 0x12345678);     /* imm32 */
	hram_poke16(0x2006, 0x2080);         /* move.l d0,(a0) */
	step();
	CHECK_EQ32(m68k_dreg(regs, 0), 0x12345678, "d0 = imm32");
	step();
	CHECK_EQ32(hram_peek32(0x00010000), 0x12345678, "(a0) written big-endian to buffer");
	/* read it back through the CPU: move.l (a0),d1 = 0x2210 */
	hram_poke16(0x2008, 0x2210);         /* move.l (a0),d1 */
	step();
	CHECK_EQ32(m68k_dreg(regs, 1), 0x12345678, "d1 = (a0) read back via CPU");
}

static void test_memory_roundtrip_endianness(void)
{
	printf("[test] byte/word/long endianness through banks\n");
	/* Poke a known big-endian long; read its bytes via CPU byte/word reads. */
	hram_poke32(0x00020000, 0xAABBCCDD);
	cpu_set_context(0x2000);
	m68k_areg(regs, 0) = 0x00020000;
	hram_poke16(0x2000, 0x1010);   /* move.b (a0),d0  -> 0xAA */
	hram_poke16(0x2002, 0x3210);   /* move.w (a0),d1  -> 0xAABB */
	step();
	CHECK_EQ32(m68k_dreg(regs, 0) & 0xff, 0xAA, "move.b (a0),d0 high byte first");
	step();
	CHECK_EQ32(m68k_dreg(regs, 1) & 0xffff, 0xAABB, "move.w (a0),d1 big-endian");
}

/* ================================================================== */
int main(int argc, char **argv)
{
	(void)argc; (void)argv;
	printf("=== Z3660 UAE host MMU harness (Layer 1) ===\n");

	harness_mem_init();
	cpu_bringup(68030);
	printf("cpu_model=%d mmu_model=%d cachesize=%d\n",
	       currprefs.cpu_model, currprefs.mmu_model, currprefs.cachesize);

	test_moveq();
	test_add_sequence();
	test_immediate_and_memory();
	test_memory_roundtrip_endianness();

	printf("\n=== %d passed, %d failed ===\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
