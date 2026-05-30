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
#include "cpummu030.h"
#include "harness.h"

/* 68030 TT-register bits (#defined privately in cpummu030.cpp). */
#define Z_TT_FC_MASK   0x00000007u
#define Z_TT_RWM       0x00000100u
#define Z_TT_ENABLE    0x00008000u
#define Z_TC_ENABLE    0x80000000u

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

/* ==================================================================
 * Adversarial-instruction baseline (decision #3 spike candidates).
 * These run on the current NON-MMU engine to capture known-good register/memory
 * results. When the MMU engine lands, the spike re-runs the same instructions
 * faulting mid-access and must restart to these exact results.
 * ================================================================== */
static void test_tas_rmw(void)
{
	printf("[test] TAS.B (An) read-modify-write\n");
	cpu_set_context(0x2000);
	m68k_areg(regs, 0) = 0x00050000;
	hram_poke8(0x00050000, 0x00);        /* byte starts 0 */
	hram_poke16(0x2000, 0x4AD0);         /* TAS (a0) */
	step();
	CHECK_EQ32(hram_peek8(0x00050000), 0x80, "TAS set bit7");
	MakeSR();   /* WinUAE keeps CC decomposed; compose regs.sr before reading */
	CHECK((regs.sr & 4) != 0, "TAS Z flag set (operand was 0)\n");
}

static void test_cas_rmw(void)
{
	printf("[test] CAS.L Dc,Du,(An) match + no-match\n");
	/* match: (a0)==d1 -> store d2, Z=1 */
	cpu_set_context(0x2000);
	m68k_areg(regs, 0) = 0x00060000;
	m68k_dreg(regs, 1) = 0x11111111;     /* Dc */
	m68k_dreg(regs, 2) = 0x22222222;     /* Du */
	hram_poke32(0x00060000, 0x11111111);
	hram_poke16(0x2000, 0x0ED0);         /* CAS.L ...,(a0) */
	hram_poke16(0x2002, (2 << 6) | 1);   /* ext: Du=d2, Dc=d1 */
	step();
	CHECK_EQ32(hram_peek32(0x00060000), 0x22222222, "CAS match -> stored Du");
	MakeSR();
	CHECK((regs.sr & 4) != 0, "CAS match -> Z set\n");
	/* no-match: (a0)!=d1 -> load (a0) into d1, Z=0, mem unchanged */
	cpu_set_context(0x2000);
	m68k_areg(regs, 0) = 0x00060000;
	m68k_dreg(regs, 1) = 0x11111111;
	m68k_dreg(regs, 2) = 0x33333333;
	hram_poke32(0x00060000, 0xAAAAAAAA);
	hram_poke16(0x2000, 0x0ED0);
	hram_poke16(0x2002, (2 << 6) | 1);
	step();
	CHECK_EQ32(hram_peek32(0x00060000), 0xAAAAAAAA, "CAS no-match -> mem unchanged");
	CHECK_EQ32(m68k_dreg(regs, 1), 0xAAAAAAAA, "CAS no-match -> Dc loaded from mem");
	MakeSR();
	CHECK((regs.sr & 4) == 0, "CAS no-match -> Z clear\n");
}

static void test_movem(void)
{
	printf("[test] MOVEM.L reg<->mem (multi-word access)\n");
	cpu_set_context(0x2000);
	m68k_dreg(regs, 0) = 0xDEAD0000;
	m68k_dreg(regs, 1) = 0xBEEF0001;
	m68k_areg(regs, 0) = 0x00070000;
	hram_poke16(0x2000, 0x48D0);         /* MOVEM.L d0/d1,(a0) */
	hram_poke16(0x2002, 0x0003);         /* mask d0,d1 */
	step();
	CHECK_EQ32(hram_peek32(0x00070000), 0xDEAD0000, "MOVEM store d0");
	CHECK_EQ32(hram_peek32(0x00070004), 0xBEEF0001, "MOVEM store d1");
	/* load back into d2/d3 */
	m68k_setpc(0x2004);
	m68k_areg(regs, 0) = 0x00070000;
	hram_poke16(0x2004, 0x4CD0);         /* MOVEM.L (a0),d2/d3 */
	hram_poke16(0x2006, 0x000C);         /* mask d2,d3 */
	step();
	CHECK_EQ32(m68k_dreg(regs, 2), 0xDEAD0000, "MOVEM load d2");
	CHECK_EQ32(m68k_dreg(regs, 3), 0xBEEF0001, "MOVEM load d3");
}

static void test_misaligned_long(void)
{
	printf("[test] misaligned long access (68030 allows)\n");
	cpu_set_context(0x2000);
	m68k_dreg(regs, 0) = 0x12345678;
	m68k_areg(regs, 0) = 0x00040001;     /* odd address */
	hram_poke16(0x2000, 0x2080);         /* move.l d0,(a0) */
	step();
	CHECK_EQ32(hram_peek32(0x00040001), 0x12345678, "misaligned long stored big-endian");
	/* read back */
	m68k_setpc(0x2002);
	m68k_areg(regs, 0) = 0x00040001;
	hram_poke16(0x2002, 0x2210);         /* move.l (a0),d1 */
	step();
	CHECK_EQ32(m68k_dreg(regs, 1), 0x12345678, "misaligned long read back");
}

/* ==================================================================
 * MMU030 engine — decision #6 test 1: transparent translation (TTR).
 * Drives the real engine through the PMOVE path (mmu_op30_pmove), exactly as
 * AMIX would, then checks mmu030_translate(). Validates that the imported
 * WinUAE 4.4.0 MMU engine is live, accepts register loads, enables, and
 * transparently translates a TTR-covered region.
 * ================================================================== */
static uae_u16 pmove_next(int preg) { return (uae_u16)((preg & 31) << 10); } /* write, fd=0 */

static void test_mmu030_ttr(void)
{
	printf("[test] MMU030 PMOVE + transparent translation (decision #6 test 1)\n");
	currprefs.mmu_model = 68030;
	currprefs.mmu_ec = 0;
	currprefs.cpu_memory_cycle_exact = false;
	mmu030_reset(1);                 /* hard reset: clear state + wire x_phys_* */
	m68k_setpc(0x2000);              /* PMOVE uses m68k_getpc() for logging */

	/* MMU disabled -> translate is identity */
	CHECK_EQ32(mmu030_translate(0x00012340, true, true, false), 0x00012340,
	           "MMU disabled -> identity passthrough");

	/* PMOVE write TT0: transparent-translate the 0x40xxxxxx region (match-all FC,
	 * r/w-disabled so both reads and writes are transparent). */
	uae_u32 ttr = Z_TT_ENABLE | Z_TT_RWM | 0x40000000u | Z_TT_FC_MASK; /* 0x40008107 */
	hram_poke32(0x100, ttr);
	bool err = mmu_op30_pmove(0x2000, 0xF010, pmove_next(0x02), 0x100);
	CHECK(!err, "PMOVE TT0 accepted\n");
	CHECK_EQ32(tt0_030, ttr, "tt0_030 loaded via PMOVE");

	/* PMOVE write TC: enable translation. Valid 030 TC: E | PS=12 (4KB) |
	 * TIA=10 | TIB=10  (IS 0 + PS 12 + TIA 10 + TIB 10 = 32). */
	uae_u32 tc = Z_TC_ENABLE | (12u << 20) | (10u << 12) | (10u << 8); /* 0x80C0AA00 */
	hram_poke32(0x104, tc);
	err = mmu_op30_pmove(0x2000, 0xF010, pmove_next(0x10), 0x104);
	CHECK(!err, "PMOVE TC accepted\n");
	CHECK((tc_030 & Z_TC_ENABLE) != 0, "tc_030 enable bit set\n");

	/* MMU now enabled: a TTR-covered address is transparently translated (returns
	 * the same physical address) -- proving the engine ran the TTR path, not the
	 * disabled shortcut. */
	CHECK_EQ32(mmu030_translate(0x40001234, true, true, false), 0x40001234,
	           "TTR transparent translation, supervisor read");
	CHECK_EQ32(mmu030_translate(0x40000000, false, true, true), 0x40000000,
	           "TTR transparent translation, user write");
	CHECK_EQ32(mmu030_translate(0x4000FFFC, false, false, false), 0x4000FFFC,
	           "TTR transparent translation, user instr fetch");

	/* Leave the MMU disabled for any later tests. */
	currprefs.mmu_model = 0;
	mmu030_reset(1);
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
	test_tas_rmw();
	test_cas_rmw();
	test_movem();
	test_misaligned_long();
	test_mmu030_ttr();

	printf("\n=== %d passed, %d failed ===\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
