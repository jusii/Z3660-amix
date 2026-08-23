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
#include "cpummu.h"        /* 68040 MMU: mmu_translate, mmu_set_tc, mmu_match_ttr, TTR/descriptor defines */
#include "z3660_prof.h"    /* CYBERSTORM C1: the profiler's wire format + phase machine */
#include "z3660_warp.h"    /* WARP ENGINE: the block engine's switch rule, key and geometry */
#include "z3660_blk.h"     /* the neighbouring knobs' vocabulary, for the arm-magic sweep   */
#include "z3660_dfast.h"
#include "z3660_dopc.h" 
#include "harness.h"
#include "main.h"          /* the cross-core SHARED struct -- ABI guard below */
extern SHARED *shared;     /* board_stubs.cpp; the tests write knobs through it */

/* 68030 TT-register bits (#defined privately in cpummu030.cpp). */
#define Z_TT_FC_MASK   0x00000007u
#define Z_TT_RWM       0x00000100u
#define Z_TT_ENABLE    0x00008000u
#define Z_TC_ENABLE    0x80000000u

#include <cstdio>
#include <cstring>
#include <cstddef>      /* offsetof -- the SHARED mailbox ABI guard */
#include <cstdlib>      /* atoi   -- the formatter-ring source scan */
#include <cctype>       /* isalnum/isdigit/isspace, same                */
#include <string>       /* the scan holds the profiler source in memory */
#include <algorithm>    /* std::count, to turn an offset into a line number */

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

/* ==================================================================
 * MMU030 engine — decision #6 test 2: real 2-level page-table walk.
 * Builds a short-descriptor (4-byte) table in the flat buffer matching the TC
 * (TIA=10 @ shift22, TIB=10 @ shift12, 4KB pages) and asserts a logical address
 * translates to the expected physical address through mmu030_table_search/ATC.
 * Guarded with TRY/CATCH so a malformed descriptor fails an assertion instead of
 * unwinding out of the test.
 * ================================================================== */
static void test_mmu030_pagewalk(void)
{
	printf("[test] MMU030 2-level page-table walk (decision #6 test 2)\n");
	harness_mem_reset();
	currprefs.mmu_model = 68030;
	currprefs.mmu_ec = 0;
	currprefs.cpu_memory_cycle_exact = false;
	mmu030_reset(1);
	m68k_setpc(0x2000);

	const uae_u32 TABLE_A = 0x09000000;   /* root table (1024 * 4B) */
	const uae_u32 TABLE_B = 0x09001000;   /* second-level table     */
	const uae_u32 PHYS_PG = 0x0A000000;   /* physical page frame    */
	const uae_u32 L       = 0x00405678;   /* logical address to map */

	/* index math for TIA/TIB=10, IS=0, PS=12 */
	uae_u32 ai = (L >> 22) & 0x3FF;       /* table A index = 1   */
	uae_u32 bi = (L >> 12) & 0x3FF;       /* table B index = 5   */
	uae_u32 off = L & 0xFFF;              /* page offset = 0x678 */

	/* TableA[ai] = VALID4 (type 2) descriptor -> TableB base */
	hram_poke32(TABLE_A + ai * 4, (TABLE_B & 0xFFFFFFF0u) | 0x2u);
	/* TableB[bi] = PAGE (type 1) descriptor -> physical page (bits 31-8) */
	hram_poke32(TABLE_B + bi * 4, (PHYS_PG & 0xFFFFFF00u) | 0x1u);

	/* Root pointers (set directly): upper longword = limit 0x7FFF (upper limit,
	 * passes all indices) | type 2 (VALID4); lower = TableA base. Both SRP & CRP
	 * point at the same root so the test works regardless of SRE/fc selection. */
	uae_u64 rp = ((uae_u64)0x7FFF0002u << 32) | (TABLE_A & 0xFFFFFFF0u);
	srp_030 = rp;
	crp_030 = rp;

	/* Enable translation with a valid TC (PS=12, TIA=10, TIB=10). */
	tc_030 = Z_TC_ENABLE | (12u << 20) | (10u << 12) | (10u << 8);
	bool tc_err = mmu030_decode_tc(tc_030, false);   /* returns true on ERROR */
	CHECK(!tc_err, "decode_tc accepted valid TC for page walk\n");

	uae_u32 phys = 0; bool faulted = false;
	TRY(p) {
		phys = mmu030_translate(L, true, true, false);
	} CATCH(p) {
		faulted = true; (void)p;
	} ENDTRY
	CHECK(!faulted, "page walk completed without bus fault\n");
	CHECK_EQ32(phys, PHYS_PG | off, "logical 0x00405678 -> physical 0x0A000678");

	/* A second access to the same page should hit the ATC and give the same result. */
	faulted = false;
	TRY(p2) {
		phys = mmu030_translate(L & ~0xFFFu, true, true, false);
	} CATCH(p2) { faulted = true; (void)p2; } ENDTRY
	CHECK(!faulted, "ATC re-access no fault\n");
	CHECK_EQ32(phys, PHYS_PG, "ATC hit: page base maps to physical page base");

	currprefs.mmu_model = 0;
	mmu030_reset(1);
}

/* ==================================================================
 * Shared 2-level page-table fixture for MMU tests 3-5.
 *   TableA @ 0x09000000, TableB @ 0x09001000, SRP/CRP -> TableA, TC enabled.
 *   Logical 0x004xxxxx uses TableA[1] -> TableB; bi = (L>>12)&0x3FF.
 * ================================================================== */
#define MPT_A     0x09000000u
#define MPT_B     0x09001000u
static void mmu_pt_init(void)
{
	harness_mem_reset();
	currprefs.mmu_model = 68030;
	currprefs.mmu_ec = 0;
	currprefs.cpu_compatible = false;
	currprefs.cpu_memory_cycle_exact = false;
	mmu030_reset(1);
	m68k_setpc(0x2000);
	regs.s = 1;
	hram_poke32(MPT_A + 1 * 4, (MPT_B & 0xFFFFFFF0u) | 0x2u);   /* TableA[1] -> TableB (VALID4) */
	uae_u64 rp = ((uae_u64)0x7FFF0002u << 32) | (MPT_A & 0xFFFFFFF0u);
	srp_030 = crp_030 = rp;
	tc_030 = Z_TC_ENABLE | (12u << 20) | (10u << 12) | (10u << 8);
	mmu030_decode_tc(tc_030, false);
}
static void mmu_map_page(uae_u32 bi, uae_u32 phys) { hram_poke32(MPT_B + bi * 4, (phys & 0xFFFFFF00u) | 0x1u); }
static void mmu_unmap_page(uae_u32 bi)             { hram_poke32(MPT_B + bi * 4, 0u); } /* invalid descriptor */

static void test_mmu030_fault_restart(void)
{
	printf("[test] MMU030 page fault -> map -> restart (decision #6 test 3)\n");
	const uae_u32 L = 0x00405678, BI = 5, PHYS = 0x0A000000;
	mmu_pt_init();
	mmu_unmap_page(BI);
	mmu030_flush_atc_all();

	bool faulted = false;
	TRY(p) { (void)mmu030_get_long(L, 5); } CATCH(p) { faulted = true; (void)p; } ENDTRY
	CHECK(faulted, "unmapped logical access raises a bus fault (THROW)\n");

	/* fault handler maps the page; flush stale ATC; restart succeeds */
	mmu_map_page(BI, PHYS);
	mmu030_flush_atc_all();
	hram_poke32(PHYS | (L & 0xFFF), 0xCAFEBABE);
	uae_u32 v = 0; faulted = false;
	TRY(p2) { v = mmu030_get_long(L, 5); } CATCH(p2) { faulted = true; (void)p2; } ENDTRY
	CHECK(!faulted, "restart after mapping does not fault\n");
	CHECK_EQ32(v, 0xCAFEBABE, "restarted access reads the now-mapped page");

	currprefs.mmu_model = 0; mmu030_reset(1);
}

static void test_mmu030_lrmw(void)
{
	printf("[test] MMU030 locked RMW through translation (decision #6 test 4)\n");
	const uae_u32 L = 0x00405678, BI = 5, PHYS = 0x0A000000, PA = PHYS | (L & 0xFFF);
	mmu_pt_init();
	mmu_map_page(BI, PHYS);
	mmu030_flush_atc_all();
	hram_poke32(PA, 0x11112222);

	uae_u32 v = 0; bool faulted = false;
	TRY(p) { v = uae_mmu030_get_lrmw(L, sz_long); } CATCH(p) { faulted = true; (void)p; } ENDTRY
	CHECK(!faulted, "lrmw read no fault\n");
	CHECK_EQ32(v, 0x11112222, "lrmw read returns translated value");
	faulted = false;
	TRY(p2) { uae_mmu030_put_lrmw(L, 0x33334444, sz_long); } CATCH(p2) { faulted = true; (void)p2; } ENDTRY
	CHECK(!faulted, "lrmw write no fault\n");
	CHECK_EQ32(hram_peek32(PA), 0x33334444, "lrmw write lands at translated physical");

	currprefs.mmu_model = 0; mmu030_reset(1);
}

static void test_mmu030_pflush(void)
{
	printf("[test] MMU030 PFLUSH / ATC invalidation (decision #6 test 5)\n");
	const uae_u32 L = 0x00405678, BI = 5, PA = 0x0A000000, PB = 0x0B000000;
	mmu_pt_init();
	mmu_map_page(BI, PA);
	mmu030_flush_atc_all();
	hram_poke32(PA | (L & 0xFFF), 0xAAAA0000);
	CHECK_EQ32(mmu030_get_long(L, 5), 0xAAAA0000, "initial mapping -> page A (caches ATC)");

	/* remap to page B; without a flush the ATC still resolves to A */
	mmu_map_page(BI, PB);
	hram_poke32(PB | (L & 0xFFF), 0xBBBB1111);
	CHECK_EQ32(mmu030_get_long(L, 5), 0xAAAA0000, "stale ATC still resolves to page A pre-flush");

	/* PFLUSH (flush ATC) -> re-walk picks up the new mapping */
	mmu030_flush_atc_all();
	CHECK_EQ32(mmu030_get_long(L, 5), 0xBBBB1111, "after PFLUSH, mapping -> page B");

	currprefs.mmu_model = 0; mmu030_reset(1);
}

/* ==================================================================
 * MMU030 interpreter end-to-end: execute an instruction through the generated
 * cpuemu_32 (op_smalltbl_32_ff) handlers, fetching the opcode from a *virtual*
 * code page that the MMU translates. Proves build_cpufunctbl(mode 4) +
 * set_x_funcs(MMU arm, x_prefetch=get_iword_mmu030) + the page-table walk all
 * cooperate to run translated code (the m68k_run_mmu030 inner step).
 * ================================================================== */
static void test_mmu030_interp_exec(void)
{
	printf("[test] MMU030 interpreter executes from a translated virtual page\n");
	const uae_u32 VCODE = 0x00408000;        /* virtual code addr (TableA[1] range) */
	const uae_u32 BI_C  = (VCODE >> 12) & 0x3FF;
	const uae_u32 PCODE = 0x0C000000;        /* physical code frame  */

	mmu_pt_init();                           /* mmu_model=68030, TC enabled */
	currprefs.cpu_model = 68030;
	mmu_map_page(BI_C, PCODE);
	mmu030_flush_atc_all();
	hram_poke16(PCODE | (VCODE & 0xFFF), 0x7042);   /* moveq #$42,d0 at phys */

	init_m68k();
	build_cpufunctbl();          /* mmu_model set -> mode 4 -> op_smalltbl_32_ff */
	harness_set_x_funcs();       /* MMU arm: x_prefetch = get_iword_mmu030 */

	regs.s = 1;
	m68k_dreg(regs, 0) = 0;
	m68k_setpc(VCODE);

	/* one m68k_run_mmu030 inner iteration */
	mmu030_state[0] = mmu030_state[1] = mmu030_state[2] = 0;
	mmu030_opcode = -1;
	bool faulted = false;
	TRY(p) {
		regs.opcode = x_prefetch(0);          /* translate VCODE->PCODE, read 0x7042 */
		mmu030_opcode = regs.opcode;
		mmu030_idx_done = 0;
		regs.opcode = regs.irc = mmu030_opcode;
		mmu030_idx = 0; mmu030_retry = false;
		(*cpufunctbl[regs.opcode])(regs.opcode);
	} CATCH(p) { faulted = true; (void)p; } ENDTRY

	CHECK(!faulted, "MMU interpreter step did not fault\n");
	CHECK_EQ32(regs.opcode, 0x7042, "opcode fetched via MMU from virtual page");
	CHECK_EQ32(m68k_dreg(regs, 0), 0x42, "moveq executed via cpuemu_32 (op_smalltbl_32_ff)");
	CHECK_EQ32(m68k_getpc(), VCODE + 2, "pc advanced past moveq via translated fetch");

	currprefs.mmu_model = 0; mmu030_reset(1);
	cpu_bringup(68030);          /* restore the non-MMU cpufunctbl for any later use */
}

/* ==================================================================
 * 68040-MMU interpreter smoke (MMU OFF / transparent). Proves the P3.2 wiring
 * end to end on the host: build_cpufunctbl(mmu_model==68040 -> mode 4 ->
 * cputbls[4][4] = op_smalltbl_31_ff) + set_x_funcs(040 arm,
 * x_prefetch=get_iword_mmu040) dispatch the generated cpuemu_31 table and it
 * executes. With regs.mmu_enabled==0 (TC.E off) the *_mmu040 accessors are
 * transparent (identity), so opcode fetch + register ops run against the flat
 * buffer -- the validatable build-ahead path (no 68040 AMIX kernel yet; real
 * 040 table-walk/TTR translation is the separate 3.3 harness). Also exercises a
 * memory operand (move.l Dn,(An)+ / read-back) through put/get_long_mmu040.
 * ================================================================== */
static void test_mmu040_interp_exec(void)
{
	printf("[test] MMU040 interpreter executes (MMU off / transparent) via op_smalltbl_31_ff\n");
	cpu_bringup(68040);
	currprefs.cpu_model = 68040;
	currprefs.mmu_model = 68040;         /* select the 040 MMU column */
	changed_prefs = currprefs;
	init_m68k();
	build_cpufunctbl();                  /* mmu_model=68040 -> mode 4 -> op_smalltbl_31_ff */
	harness_set_x_funcs();               /* 040 arm: x_prefetch = get_iword_mmu040 */
	regs.mmu_enabled = 0;                /* TC.E off -> *_mmu040 accessors are transparent */

	cpu_set_context(0x2000);
	m68k_dreg(regs, 0) = 0;
	m68k_dreg(regs, 1) = 0;
	m68k_areg(regs, 0) = 0x3000;         /* scratch data address */
	hram_poke16(0x2000, 0x7042);         /* moveq #$42,d0            */
	hram_poke16(0x2002, 0x7207);         /* moveq #7,d1             */
	hram_poke16(0x2004, 0xd280);         /* add.l d0,d1  -> d1=0x49 */
	hram_poke16(0x2006, 0x2081);         /* move.l d1,(a0) -> [0x3000]=0x49 (put_long_mmu040) */

	bool faulted = false;
	TRY(p) {
		for (int i = 0; i < 4; i++) {
			regs.instruction_pc = m68k_getpc();
			regs.opcode = x_prefetch(0);              /* 040 MMU opcode fetch (transparent) */
			(*cpufunctbl[regs.opcode])(regs.opcode);
		}
	} CATCH(p) { faulted = true; (void)p; } ENDTRY

	CHECK(!faulted, "040 interpreter steps did not fault\n");
	CHECK_EQ32(m68k_dreg(regs, 0), 0x42, "moveq #$42,d0 via cpuemu_31 (op_smalltbl_31_ff)");
	CHECK_EQ32(m68k_dreg(regs, 1), 0x49, "moveq #7,d1; add.l d0,d1 via cpuemu_31");
	CHECK_EQ32(hram_peek32(0x3000), 0x49, "move.l d1,(a0) stored via put_long_mmu040");
	CHECK_EQ32(m68k_getpc(), 0x2008, "pc advanced past 4 insns via 040 MMU fetch");

	currprefs.mmu_model = 0;
	cpu_bringup(68030);                  /* restore a clean non-MMU cpufunctbl */
}

/* ==================================================================
 * 68040 MMU ADDRESS TRANSLATION vectors (MMU ON). These drive the real 040
 * table walk in cpummu.cpp (mmu_translate / mmu_fill_atc) with hand-built root
 * -> pointer -> page descriptor tables in the flat buffer, plus TTR matching,
 * and assert physical addresses / faults / U+M bit updates against the 68040
 * spec. This is the MMU-ON counterpart to test_mmu040_interp_exec (which was
 * MMU-off/transparent) and the 040 analog of the test_mmu030_* vectors.
 *
 * 4K layout: VA[31:25]=root idx, VA[24:18]=ptr idx, VA[17:12]=page idx, [11:0]
 * offset. 8K: page idx = VA[17:13], offset [12:0]. Descriptor validity: root/ptr
 * need bit1 (UDT valid); page needs bit0 (resident). Fixed scratch tables:
 * ================================================================== */
#define M4_ROOT 0x00100000u   /* root table  (512-byte aligned) */
#define M4_PTR  0x00101000u   /* pointer table                  */
#define M4_PAGE 0x00102000u   /* page table  (256/128-byte aligned) */

/* Build one VA->PA mapping and enable the 040 MMU. page_extra ORs bits into the
 * page descriptor (MMU_DES_WP / MMU_DES_SUPER). Returns the page-descriptor
 * flat-buffer address so a test can inspect U/M bits afterwards. */
static uae_u32 mmu040_map1(uae_u32 va, uae_u32 pa, uae_u32 page_extra, bool page8k)
{
	harness_mem_reset();
	currprefs.cpu_model = 68040;
	currprefs.mmu_model = 68040;
	currprefs.mmu_ec    = 0;
	changed_prefs = currprefs;
	mmu_set_tc((uae_u16)(0x8000 | (page8k ? 0x4000 : 0)));   /* enable + page size */
	regs.itt0 = regs.itt1 = regs.dtt0 = regs.dtt1 = 0;       /* no transparent windows */
	mmu_tt_modified();
	regs.srp = regs.urp = M4_ROOT;

	uae_u32 ri = (va >> 23) & 0x1fc;
	uae_u32 pi = (va >> 16) & 0x1fc;
	uae_u32 gi = page8k ? ((va >> 11) & 0x7c) : ((va >> 10) & 0xfc);
	uae_u32 ptr_mask  = page8k ? MMU_PTR_PAGE_ADDR_MASK_8 : MMU_PTR_PAGE_ADDR_MASK_4;
	uae_u32 page_mask = page8k ? MMU_PAGE_ADDR_MASK_8      : MMU_PAGE_ADDR_MASK_4;
	uae_u32 pg_desc_addr = M4_PAGE + gi;

	hram_poke32(M4_ROOT + ri, (M4_PTR  & MMU_ROOT_PTR_ADDR_MASK) | 2);   /* root -> ptr table  */
	hram_poke32(M4_PTR  + pi, (M4_PAGE & ptr_mask)               | 2);   /* ptr  -> page table */
	hram_poke32(pg_desc_addr, (pa & page_mask) | 1 | page_extra);        /* page (resident)    */
	mmu_flush_atc_all(true);
	return pg_desc_addr;
}

static void mmu040_off(void)   /* disable + flush + back to non-MMU cpufunctbl */
{
	regs.itt0 = regs.itt1 = regs.dtt0 = regs.dtt1 = 0;
	mmu_tt_modified();
	mmu_set_tc(0);
	mmu_flush_atc_all(true);
	currprefs.mmu_model = 0;
	cpu_bringup(68030);
}

/* --- TTR transparent-translation matching (bypasses the table walk) --- */
static void test_mmu040_ttr(void)
{
	printf("[test] MMU040 TTR transparent match / no-match / write-protect\n");
	harness_mem_reset();
	currprefs.cpu_model = 68040; currprefs.mmu_model = 68040; currprefs.mmu_ec = 0;
	changed_prefs = currprefs;
	mmu_set_tc(0x8000);                       /* MMU enabled, 4K */
	regs.s = 1;

	/* DTT0: transparent window over 0x0A000000..0x0AFFFFFF (base byte 0x0A, mask 0),
	 * S-field enabled so it matches both supervisor and user. */
	regs.dtt0 = 0x0A000000 | MMU_TTR_BIT_ENABLED | MMU_TTR_BIT_SFIELD_ENABLED;
	regs.dtt1 = 0;
	mmu_tt_modified();
	CHECK(mmu_match_ttr(0x0A123456, true,  true) == TTR_OK_MATCH, "DTT0 matches in-window (super)\n");
	CHECK(mmu_match_ttr(0x0A800000, false, true) == TTR_OK_MATCH, "DTT0 matches in-window (user, S-field)\n");
	CHECK(mmu_match_ttr(0x0B000000, true,  true) == TTR_NO_MATCH, "DTT0 no-match out-of-window\n");

	/* End-to-end bypass: no page tables at all, but an in-window read returns the
	 * physical value via TTR (no fault, no table walk). */
	hram_poke32(0x0A001000, 0xCAFEF00D);
	bool faulted = false; uae_u32 v = 0;
	SAVE_EXCEPTION;
	TRY(p) { v = get_long_mmu040(0x0A001000); } CATCH(p) { faulted = true; (void)p; } ENDTRY
	RESTORE_EXCEPTION;
	CHECK(!faulted, "TTR bypass read did not fault (no page table needed)\n");
	CHECK_EQ32(v, 0xCAFEF00D, "TTR transparent read returns physical value");

	/* Write-protected TTR window -> writes report TTR_NO_WRITE. */
	regs.dtt0 = 0x0C000000 | MMU_TTR_BIT_ENABLED | MMU_TTR_BIT_SFIELD_ENABLED | MMU_TTR_BIT_WRITE_PROTECT;
	mmu_tt_modified();
	CHECK(mmu_match_ttr(0x0C000000, true, true) == TTR_NO_WRITE, "write-protected TTR reports NO_WRITE\n");

	mmu040_off();
}

/* --- 4K page-table walk: the single-translation smoke (hit + U bit) --- */
static void test_mmu040_translate_4k(void)
{
	printf("[test] MMU040 single translation: 4K root->ptr->page walk\n");
	const uae_u32 VA = 0x00408678, PA = 0x0A000000;
	uae_u32 pd = mmu040_map1(VA, PA, 0, false);
	regs.s = 1;

	uae_u32 phys = 0; bool faulted = false;
	SAVE_EXCEPTION;
	TRY(p) { phys = mmu_translate(VA, 0, true, true, false, sz_long); }
	CATCH(p) { faulted = true; (void)p; } ENDTRY
	RESTORE_EXCEPTION;
	CHECK(!faulted, "4K walk completed without fault\n");
	CHECK_EQ32(phys, PA | (VA & 0xFFF), "VA 0x00408678 -> PA 0x0A000678 (4K)");
	CHECK((hram_peek32(pd) & MMU_DES_USED) != 0, "page descriptor U (used) bit set after read\n");

	/* ATC hit: re-translate the page base returns the frame base, no fault. */
	faulted = false; uae_u32 phys2 = 0;
	SAVE_EXCEPTION;
	TRY(p2) { phys2 = mmu_translate(VA & ~0xFFFu, 0, true, true, false, sz_long); }
	CATCH(p2) { faulted = true; (void)p2; } ENDTRY
	RESTORE_EXCEPTION;
	CHECK(!faulted, "ATC re-access no fault\n");
	CHECK_EQ32(phys2, PA, "ATC hit: page base -> frame base");

	mmu040_off();
}

/* --- 8K page-table walk --- */
static void test_mmu040_translate_8k(void)
{
	printf("[test] MMU040 single translation: 8K root->ptr->page walk\n");
	const uae_u32 VA = 0x00408678, PA = 0x0A002000;
	mmu040_map1(VA, PA, 0, true);
	regs.s = 1;

	uae_u32 phys = 0; bool faulted = false;
	SAVE_EXCEPTION;
	TRY(p) { phys = mmu_translate(VA, 0, true, true, false, sz_long); }
	CATCH(p) { faulted = true; (void)p; } ENDTRY
	RESTORE_EXCEPTION;
	CHECK(!faulted, "8K walk completed without fault\n");
	CHECK_EQ32(phys, PA | (VA & 0x1FFF), "VA 0x00408678 -> PA 0x0A002678 (8K)");

	mmu040_off();
}

/* --- protection & fault: invalid page, write-protect, supervisor-only --- */
static void test_mmu040_protection(void)
{
	printf("[test] MMU040 protection: miss/fault, write-protect, supervisor-only\n");
	const uae_u32 VA = 0x00408678, PA = 0x0A000000;
	bool faulted;

	/* (a) invalid page descriptor (resident bit clear) -> bus fault */
	uae_u32 pd = mmu040_map1(VA, PA, 0, false);
	hram_poke32(pd, 0);                       /* clear the page descriptor -> not resident */
	mmu_flush_atc_all(true);
	regs.s = 1; faulted = false;
	SAVE_EXCEPTION;
	TRY(p) { (void)mmu_translate(VA, 0, true, true, false, sz_long); }
	CATCH(p) { faulted = true; (void)p; } ENDTRY
	RESTORE_EXCEPTION;
	CHECK(faulted, "invalid page descriptor faults (miss)\n");

	/* (b) write-protected page: read OK, write faults */
	mmu040_map1(VA, PA, MMU_DES_WP, false);
	regs.s = 1; faulted = false;
	SAVE_EXCEPTION;
	TRY(p) { (void)mmu_translate(VA, 0, true, true, false, sz_long); }
	CATCH(p) { faulted = true; (void)p; } ENDTRY
	RESTORE_EXCEPTION;
	CHECK(!faulted, "write-protected page allows read\n");
	faulted = false;
	SAVE_EXCEPTION;
	TRY(p2) { (void)mmu_translate(VA, 0, true, true, true, sz_long); }
	CATCH(p2) { faulted = true; (void)p2; } ENDTRY
	RESTORE_EXCEPTION;
	CHECK(faulted, "write to write-protected page faults\n");

	/* (c) supervisor-only page: super read OK, user read faults */
	mmu040_map1(VA, PA, MMU_DES_SUPER, false);
	regs.s = 1; faulted = false;
	SAVE_EXCEPTION;
	TRY(p3) { (void)mmu_translate(VA, 0, true, true, false, sz_long); }
	CATCH(p3) { faulted = true; (void)p3; } ENDTRY
	RESTORE_EXCEPTION;
	CHECK(!faulted, "supervisor page allows supervisor read\n");
	faulted = false;
	SAVE_EXCEPTION;
	TRY(p4) { (void)mmu_translate(VA, 0, false, true, false, sz_long); }
	CATCH(p4) { faulted = true; (void)p4; } ENDTRY
	RESTORE_EXCEPTION;
	CHECK(faulted, "user read of supervisor-only page faults\n");

	mmu040_off();
}

/* --- modified (M) bit is set on the first write --- */
static void test_mmu040_modified_bit(void)
{
	printf("[test] MMU040 modified (M) bit set on first write\n");
	const uae_u32 VA = 0x00408678, PA = 0x0A000000;
	uae_u32 pd = mmu040_map1(VA, PA, 0, false);
	regs.s = 1;

	CHECK((hram_peek32(pd) & MMU_DES_MODIFIED) == 0, "M bit clear before write\n");
	bool faulted = false;
	SAVE_EXCEPTION;
	TRY(p) { (void)mmu_translate(VA, 0, true, true, true, sz_long); }
	CATCH(p) { faulted = true; (void)p; } ENDTRY
	RESTORE_EXCEPTION;
	CHECK(!faulted, "write translation no fault\n");
	CHECK((hram_peek32(pd) & (MMU_DES_USED|MMU_DES_MODIFIED)) == (MMU_DES_USED|MMU_DES_MODIFIED),
	      "U and M bits set in page descriptor after write\n");

	mmu040_off();
}

/* ==================================================================
 * 68040 FPU unimplemented-instruction trap delivery.
 *
 * currprefs.fpu_no_unimplemented reads backwards: SET means "this FPU does NOT
 * implement the whole opcode set inside the emulator", so fpp.cpp raises the
 * genuine silicon trap (fault_if_unimplemented_680x0 -> fp_unimp_instruction ->
 * Exception(11), stack frame format $2) instead of quietly computing the result.
 * The AMIX 68040 kernel links Motorola's FPSP and only works if that trap
 * arrives; uae_emulator() therefore sets the pref for fpu_model==68040 when
 * amix_mode is on, and leaves it clear for a plain AmigaOS 040 (which has no
 * FPSP of its own here and relies on the emulator doing the arithmetic).
 * This vector pins BOTH directions so the pref cannot be flipped unnoticed.
 * ================================================================== */
#define FPU040_VEC11_HANDLER 0x00009000u

static void fpu040_bringup(int no_unimplemented)
{
	harness_mem_reset();
	cpu_bringup(68040);
	currprefs.cpu_model            = 68040;
	currprefs.fpu_model            = 68040;
	currprefs.fpu_strict           = true;
	currprefs.fpu_no_unimplemented = (no_unimplemented != 0);
	currprefs.mmu_model            = 0;      /* MMU off: this is about the FPU trap, not translation */
	changed_prefs = currprefs;
	init_m68k();
	build_cpufunctbl();
	harness_set_x_funcs();
	fpu_reset();
	regs.address_space_mask = 0xffffffff;

	cpu_set_context(0x2000);
	regs.vbr = 0;
	hram_poke32(11 * 4, FPU040_VEC11_HANDLER);  /* vector 11 = F-line / FP unimplemented */
	hram_poke16(0x2000, 0xf200);                /* FSIN fp0,fp0 -- opcode $F200 ...      */
	hram_poke16(0x2002, 0x000e);                /* ... ext: R/M=0, src=fp0, dst=fp0, opmode $0E */
	hram_poke16(0x2004, 0x4e71);                /* nop (reached only if no trap is taken) */
}

static void test_fpu040_unimplemented_trap(void)
{
	printf("[test] FPU040 unimplemented FP instruction: trap delivery follows fpu_no_unimplemented\n");

	/* amix_mode ON: FSIN is unimplemented in 040 silicon -> vector 11, format $2, FPSP's entry. */
	fpu040_bringup(1);
	bool faulted = false;
	TRY(p) { step(); } CATCH(p) { faulted = true; (void)p; } ENDTRY
	CHECK(!faulted, "FSIN with fpu_no_unimplemented=1 raised no bus fault\n");
	CHECK_EQ32(m68k_getpc(), FPU040_VEC11_HANDLER, "FSIN trapped into the vector-11 handler");
	{
		uae_u32 sp = m68k_areg(regs, 7);
		CHECK_EQ32(hram_peek16(sp + 6), 0x202c, "68040 stack frame is format $2, vector offset $02C");
		CHECK_EQ32(hram_peek32(sp + 2), 0x00002004, "stacked PC is the instruction after FSIN");
	}

	/* amix_mode OFF (plain AmigaOS on this 040 mode): unchanged -- emulator computes FSIN. */
	fpu040_bringup(0);
	faulted = false;
	TRY(p2) { step(); } CATCH(p2) { faulted = true; (void)p2; } ENDTRY
	CHECK(!faulted, "FSIN with fpu_no_unimplemented=0 raised no fault\n");
	CHECK_EQ32(m68k_getpc(), 0x00002004, "FSIN executed in-emulator, no trap (AmigaOS path unchanged)");

	currprefs.fpu_model = 0;
	changed_prefs = currprefs;
	cpu_bringup(68030);
}

/* ==================================================================
 * 68040 ACCESS ERROR -- the format $7 frame, CONTENTS INCLUDED.
 *
 * The 68040 does not re-run a faulted WRITE on rte: the instruction has already
 * retired (the generated handler does `m68k_incpci; regs.instruction_pc =
 * m68k_getpci(); mmu_restart = false;` immediately before the store), and the
 * pending store travels to the OS in the frame's WB3 slot for the kernel's
 * write-back handler to complete. A faulted READ is restartable, so nothing is
 * pending and WB3 must be reported invalid.
 *
 * These two vectors pin both halves, because either one silently broken loses
 * data: a dropped WB3 loses the store outright, a spurious WB3 on a restarted
 * instruction applies it twice.
 * ================================================================== */

/* Root/pointer tables covering VAs in [0x00400000,0x00440000) -- every VA in that
 * span shares root index 0 and pointer index 16, so one page table serves them
 * all. MMU on, 4K pages, supervisor, nothing mapped yet. */
static void mmu040_pt_init(void)
{
	harness_mem_reset();
	cpu_bringup(68040);
	currprefs.cpu_model = 68040;
	currprefs.mmu_model = 68040;
	currprefs.mmu_ec    = 0;
	changed_prefs = currprefs;
	init_m68k();
	build_cpufunctbl();
	harness_set_x_funcs();

	regs.itt0 = regs.itt1 = regs.dtt0 = regs.dtt1 = 0;
	mmu_tt_modified();
	regs.srp = regs.urp = M4_ROOT;
	hram_poke32(M4_ROOT + 0,  (M4_PTR  & MMU_ROOT_PTR_ADDR_MASK)   | 2);
	hram_poke32(M4_PTR  + 64, (M4_PAGE & MMU_PTR_PAGE_ADDR_MASK_4) | 2);
	mmu_set_tc(0x8000);                 /* enable, 4K pages */
	mmu_flush_atc_all(true);
	regs.s = 1;
	regs.address_space_mask = 0xffffffff;
}

static void mmu040_map_page(uae_u32 va, uae_u32 pa)
{
	hram_poke32(M4_PAGE + ((va >> 10) & 0xfc), (pa & MMU_PAGE_ADDR_MASK_4) | 1);
	mmu_flush_atc_all(true);
}

/* One instruction, exactly as m68k_run_mmu040() runs it (including the CATCH). */
static bool mmu040_step_catch(void)
{
	struct flag_struct f = regs.ccrflags;
	bool faulted = false;
	mmu_restart = true;
	regs.instruction_pc = m68k_getpc();
	TRY(prb) {
		mmu_opcode = -1;
		mmu_opcode = regs.opcode = x_prefetch(0);
		(*cpufunctbl[regs.opcode])(regs.opcode);
	} CATCH(prb) {
		faulted = true;
		if (mmu_restart) {                       /* restartable fault: rewind */
			regs.ccrflags = f;
			m68k_setpci(regs.instruction_pc);
		}
		cpu_restore_fixup();
		Exception(prb);                          /* -> the format $7 frame */
	} ENDTRY
	return faulted;
}

static void test_mmu040_access_error_frame(void)
{
	printf("[test] MMU040 access error -> format $7 frame with real FA/SSW/write-back slots\n");
	const uae_u32 VCODE = 0x00400000, PCODE = 0x0C000000;
	const uae_u32 VSTK  = 0x00401000, PSTK  = 0x0E000000;
	const uae_u32 VVEC  = 0x00402000, PVEC  = 0x0F000000;
	const uae_u32 VDATA = 0x00403000;              /* deliberately never mapped */
	const uae_u32 FAULT = VDATA + 0x10;
	const uae_u32 VALUE = 0xDEADBEEF;
	const uae_u32 HANDLER = VCODE + 0x100;
	const uae_u32 SP0 = VSTK | 0xF00;
	uae_u32 sp;
/* the frame is written THROUGH the MMU; read it back from the stack page's frame */
#define F16(off) ((uae_u32)hram_peek16(PSTK | ((sp + (off)) & 0xFFF)))
#define F32(off) ((uae_u32)hram_peek32(PSTK | ((sp + (off)) & 0xFFF)))

	/* ---- (a) WRITE fault: instruction retired, store pending in WB3 ---- */
	mmu040_pt_init();
	mmu040_map_page(VCODE, PCODE);
	mmu040_map_page(VSTK,  PSTK);
	mmu040_map_page(VVEC,  PVEC);
	hram_poke16(PCODE | (VCODE & 0xFFF), 0x2080);      /* move.l d0,(a0) */
	hram_poke32(PVEC  | 8, HANDLER);                   /* vector 2 -> handler */

	regs.vbr = VVEC;
	m68k_dreg(regs, 0) = VALUE;
	m68k_areg(regs, 0) = FAULT;
	m68k_areg(regs, 7) = SP0;
	m68k_setpc(VCODE);
	regs.wb2_address = 0;                              /* only a MOVE16 line write sets this */
	for (int i = 0; i < 4; i++) mmu040_move16[i] = 0;

	CHECK(mmu040_step_catch(), "store to an unmapped page raised a 68040 access error\n");
	CHECK_EQ32(m68k_getpc(), HANDLER, "vectored through vector 2 into the handler");

	sp = m68k_areg(regs, 7);
	CHECK_EQ32(SP0 - sp, 60, "format $7 frame is 60 bytes");
	CHECK_EQ32(F16(0x06), 0x7008, "format $7, vector offset $008 (bus error)");
	CHECK_EQ32(F32(0x02), VCODE + 2, "stacked PC = NEXT insn (write fault CONTINUES, no restart)");
	CHECK_EQ32(F32(0x08), FAULT, "EA");
	CHECK_EQ32(F16(0x0C), 0x0405, "SSW = ATC | TM 5 (super data) | SIZE long | RW clear (write)");
	CHECK_EQ32(F16(0x0E), 0x0085, "WB3S = valid $80 | SIZE long | FC 5");
	CHECK_EQ32(F16(0x10), 0, "WB2S invalid");
	CHECK_EQ32(F16(0x12), 0, "WB1S invalid");
	CHECK_EQ32(F32(0x14), FAULT, "FA");
	CHECK_EQ32(F32(0x18), FAULT, "WB3A = the pending store's address");
	CHECK_EQ32(F32(0x1C), VALUE, "WB3D = the pending store's DATA (wb040.s replays this verbatim)");
	CHECK_EQ32(F32(0x20), 0, "WB2A (MOVE16 only)");
	CHECK_EQ32(F32(0x24), 0, "WB2D (unused on 68040)");
	CHECK_EQ32(F32(0x28), 0, "WB1A (no store buffer modelled)");
	CHECK_EQ32(F32(0x2C), 0, "PD0 (MOVE16 push data)");

	/* ---- (b) READ fault: nothing pending, instruction restarts ---- */
	mmu040_pt_init();
	mmu040_map_page(VCODE, PCODE);
	mmu040_map_page(VSTK,  PSTK);
	mmu040_map_page(VVEC,  PVEC);
	hram_poke16(PCODE | (VCODE & 0xFFF), 0x2210);      /* move.l (a0),d1 */
	hram_poke32(PVEC  | 8, HANDLER);

	regs.vbr = VVEC;
	m68k_dreg(regs, 1) = 0;
	m68k_areg(regs, 0) = FAULT;
	m68k_areg(regs, 7) = SP0;
	m68k_setpc(VCODE);

	CHECK(mmu040_step_catch(), "load from an unmapped page raised a 68040 access error\n");
	sp = m68k_areg(regs, 7);
	CHECK_EQ32(F16(0x06), 0x7008, "read fault also builds a format $7 frame");
	CHECK_EQ32(F32(0x02), VCODE, "stacked PC = the faulting insn (read fault RESTARTS)");
	CHECK_EQ32(F16(0x0C) & 0x0100, 0x0100, "SSW RW set (read)");
	CHECK_EQ32(F16(0x0E), 0, "WB3S invalid: a read fault leaves NO pending write-back");
	CHECK_EQ32(F32(0x1C), 0, "WB3D zero on a read fault");
	CHECK_EQ32(F32(0x14), FAULT, "FA");

#undef F16
#undef F32
	mmu040_off();
}

/* ==================================================================
 * MOVEC to TC must actually ENABLE the 68040 MMU.
 *
 * The 68040 turns translation on by writing TC with MOVEC; everything derived
 * from it (regs.mmu_enabled -- which every *_mmu040 accessor tests before it will
 * translate at all -- the page size, its masks and shifts, the ATC flush) lives
 * behind mmu_set_tc(). m68k_move2c() used to only store regs.tcr, so the guest's
 * MMU never engaged: the AMIX 68040 kernel installed its tables, enabled the MMU,
 * believed it was running in virtual space, and every access stayed transparent.
 * Kernel VAs at 0x40000000+ then landed in the Z3 expansion window with no RAM.
 *
 * The 68030 does NOT come through here (it uses PMOVE, handled in cpummu030.cpp),
 * which is exactly why the proven 030 path never showed this.
 * ================================================================== */
static void test_mmu040_movec_tc_enables_mmu(void)
{
	printf("[test] MOVEC to TC enables the 68040 MMU (not just stores TCR)\n");
	const uae_u32 VA = 0x00408678, PA = 0x0A000000;
	(void)mmu040_map1(VA, PA, 0, false);      /* tables built; fixture enables via mmu_set_tc */

	mmu_set_tc(0x0000);                        /* start from a known-OFF state */
	CHECK_EQ32(regs.mmu_enabled, 0, "MMU off before the MOVEC");

	init_m68k();
	build_cpufunctbl();
	harness_set_x_funcs();
	cpu_set_context(0x00002000);
	regs.s = 1;
	hram_poke16(0x2000, 0x4e7b);               /* MOVEC Rn,Rc        */
	hram_poke16(0x2002, 0x0003);               /*   ext: D0 -> TC    */
	m68k_dreg(regs, 0) = 0x00008000;           /* TC: E=1, 4K pages  */

	bool faulted = false;
	TRY(p) {
		regs.instruction_pc = m68k_getpc();
		regs.opcode = x_prefetch(0);
		(*cpufunctbl[regs.opcode])(regs.opcode);
	} CATCH(p) { faulted = true; (void)p; } ENDTRY

	CHECK(!faulted, "movec d0,tc did not fault\n");
	CHECK_EQ32(regs.mmu_enabled, 1, "MOVEC to TC turned translation ON");
	CHECK_EQ32(regs.mmu_page_size, 4096, "page size derived from the TC the guest wrote");
	CHECK_EQ32(regs.tcr & 0x8000, 0x8000, "TCR stored as well");

	/* and translation now genuinely walks the guest's tables */
	bool tfault = false; uaecptr pa = 0;
	SAVE_EXCEPTION;
	TRY(p2) { pa = mmu_translate(VA, 0, true, true, false, sz_long); }
	CATCH(p2) { tfault = true; (void)p2; } ENDTRY
	RESTORE_EXCEPTION;
	CHECK(!tfault, "translation after the MOVEC-enable did not fault\n");
	CHECK_EQ32(pa, PA + (VA & 0xFFF), "VA translated through the tables the guest installed");

	/* MOVEC to a TTR must take effect too (mmu_tt_modified recomputes the enables).
	 * MMU back OFF for this arm so the instruction fetch at 0x2000 stays transparent --
	 * only VA 0x00408678's page is mapped in this fixture. */
	mmu_set_tc(0x0000);
	regs.dtt0 = regs.dtt1 = regs.itt0 = regs.itt1 = 0;
	mmu_tt_modified();
	CHECK_EQ32(mmu_ttr_enabled_data, 0, "no data TTR enabled to begin with");
	cpu_set_context(0x00002000);
	regs.s = 1;
	hram_poke16(0x2000, 0x4e7b);
	hram_poke16(0x2002, 0x0006);               /*   ext: D0 -> DTT0  */
	m68k_dreg(regs, 0) = MMU_TTR_BIT_ENABLED;  /* enable bit only    */
	bool ttrfault = false;
	TRY(p3) {
		regs.instruction_pc = m68k_getpc();
		regs.opcode = x_prefetch(0);
		(*cpufunctbl[regs.opcode])(regs.opcode);
	} CATCH(p3) { ttrfault = true; (void)p3; } ENDTRY
	CHECK(!ttrfault, "movec d0,dtt0 did not fault\n");
	CHECK(mmu_ttr_enabled_data != 0, "MOVEC to DTT0 armed the data transparent window\n");

	mmu040_off();
}

/* ==================================================================
 * PFLUSH must invalidate the ATC, and PTEST must walk the real tables.
 *
 * cpuemu_31's six F5xx handlers call mmu_op(opcode, 0) -- byte-identical to the
 * bench-proven Amiberry core. The divergence was in mmu_op() itself: this fork had
 * dropped upstream's `if (currprefs.mmu_model) { mmu_op_real(...); return; }`, so
 * every 68040 MMU instruction fell into the "fake" path meant for configs with no
 * MMU model. PFLUSH became a no-op (the ATC kept serving translations the guest had
 * just invalidated) and PTEST became fabricated (it never walked the tables).
 *
 * The shape of the guard is the shape of the bug: change a page descriptor behind
 * the ATC's back, prove the stale entry is still served, then PFLUSH and prove the
 * new mapping takes effect. A no-op PFLUSH fails the last assertion.
 * ================================================================== */
static void test_mmu040_pflush_ptest(void)
{
	printf("[test] PFLUSH invalidates the ATC; PTEST walks the guest's tables\n");
	const uae_u32 VCODE = 0x00400000, PCODE = 0x0C000000;
	const uae_u32 VDATA = 0x00401000, PA1 = 0x0A000000, PA2 = 0x0B000000;
	const uae_u32 pd = M4_PAGE + ((VDATA >> 10) & 0xfc);   /* VDATA's page descriptor */

	mmu040_pt_init();
	mmu040_map_page(VCODE, PCODE);
	mmu040_map_page(VDATA, PA1);
	regs.s = 1;
	regs.dfc = 5;                          /* supervisor data: mmu_op_real decodes super/data from DFC */

	uaecptr pa = 0; bool f = false;
	SAVE_EXCEPTION;
	TRY(p) { pa = mmu_translate(VDATA, 0, true, true, false, sz_long); } CATCH(p) { f = true; (void)p; } ENDTRY
	RESTORE_EXCEPTION;
	CHECK(!f, "first translation did not fault\n");
	CHECK_EQ32(pa, PA1, "VDATA translates to the frame its descriptor names");

	/* repoint the descriptor behind the ATC's back -- the stale entry must still win */
	hram_poke32(pd, (PA2 & MMU_PAGE_ADDR_MASK_4) | 1);
	SAVE_EXCEPTION;
	TRY(p2) { pa = mmu_translate(VDATA, 0, true, true, false, sz_long); } CATCH(p2) { (void)p2; } ENDTRY
	RESTORE_EXCEPTION;
	CHECK_EQ32(pa, PA1, "unflushed descriptor change is masked by the ATC (so the ATC is real)");

	/* a genuine PFLUSHA, executed through the 040 dispatch table */
	cpu_set_context(VCODE);
	regs.s = 1;
	hram_poke16(PCODE | (VCODE & 0xFFF), 0xf518);      /* PFLUSHA */
	bool pf = false;
	TRY(p3) {
		regs.instruction_pc = m68k_getpc();
		regs.opcode = x_prefetch(0);
		(*cpufunctbl[regs.opcode])(regs.opcode);
	} CATCH(p3) { pf = true; (void)p3; } ENDTRY
	CHECK(!pf, "PFLUSHA did not fault\n");

	SAVE_EXCEPTION;
	TRY(p4) { pa = mmu_translate(VDATA, 0, true, true, false, sz_long); } CATCH(p4) { (void)p4; } ENDTRY
	RESTORE_EXCEPTION;
	CHECK_EQ32(pa, PA2, "PFLUSHA made the updated descriptor take effect");

	/* PTEST must report a REAL walk: resident, with the frame it actually found.
	 * The fake path only ever produced (addr & 0xfffff000) with no R bit. */
	regs.mmusr = 0;
	cpu_set_context(VCODE);
	regs.s = 1;
	regs.dfc = 5;
	m68k_areg(regs, 0) = VDATA;
	hram_poke16(PCODE | (VCODE & 0xFFF), 0xf568);      /* PTESTR (A0) */
	bool pt = false;
	TRY(p5) {
		regs.instruction_pc = m68k_getpc();
		regs.opcode = x_prefetch(0);
		(*cpufunctbl[regs.opcode])(regs.opcode);
	} CATCH(p5) { pt = true; (void)p5; } ENDTRY
	CHECK(!pt, "PTESTR did not fault\n");
	CHECK((regs.mmusr & MMU_MMUSR_R) != 0, "PTEST reports the page RESIDENT (a real table walk)\n");
	CHECK_EQ32(regs.mmusr & MMU_MMUSR_ADDR_MASK, PA2, "PTEST reports the frame the walk found");

	mmu040_off();
}

/* ==================================================================
 * A guest RESET must clear the 68040 MMU's out-of-regs state too.
 *
 * m68k_reset_newcpu() zeroes regs.itt0..dtt1 / tcr / srp / urp, but
 * regs.mmu_enabled, the page geometry and the mmu_ttr_enabled* flags live
 * outside `regs` and are only recomputed by mmu_set_tc() / mmu_tt_modified().
 * Left unapplied, a warm reset carries the previous session's MMU state into the
 * Kickstart ROM that runs next -- ttr_enabled true against a now-zero ITT0, and
 * mmu_enabled true with a zero root pointer, so the walk reads descriptors from
 * address 0. Same shape as the MOVEC->TC and mmu_op bugs, third instance.
 * ================================================================== */
void m68k_reset_newcpu(bool hardreset);   /* C++ linkage: defined in newcpu.cpp */

static void test_mmu040_reset_clears_mmu_state(void)
{
	printf("[test] a guest reset clears the 68040 MMU state, not just the registers\n");
	(void)mmu040_map1(0x00408678, 0x0A000000, 0, false);   /* MMU on, tables built */
	regs.dtt0 = MMU_TTR_BIT_ENABLED;                        /* arm a transparent window */
	mmu_tt_modified();
	CHECK_EQ32(regs.mmu_enabled, 1, "MMU on before the reset");
	CHECK(mmu_ttr_enabled_data != 0, "a data TTR is armed before the reset\n");

	m68k_reset_newcpu(true);

	CHECK_EQ32(regs.mmu_enabled, 0, "reset cleared regs.mmu_enabled (mmu_set_tc applied)");
	CHECK_EQ32(regs.itt0 | regs.itt1 | regs.dtt0 | regs.dtt1, 0, "reset cleared the TTR registers");
	CHECK(mmu_ttr_enabled == 0, "reset recomputed mmu_ttr_enabled (mmu_tt_modified applied)\n");
	CHECK(mmu_ttr_enabled_ins == 0, "reset recomputed mmu_ttr_enabled_ins\n");
	CHECK(mmu_ttr_enabled_data == 0, "reset recomputed mmu_ttr_enabled_data\n");

	currprefs.mmu_model = 0;
	cpu_bringup(68030);
}

/* ==================================================================
 * ISSUE-49: bitfield ops on TABLE-MAPPED memory must translate.
 *
 * Metal (five instrumented boots + a flag histogram over the kernel's page
 * array): `bfins %d0,%a2@{2:1}` with d0=0 against a kernel VA in the
 * table-walked window does NOT write, while andi.b/ori.b/move.w to the SAME
 * byte nine instructions away land correctly. 149 of 149 allocated page structs
 * read byte0 == 0x22 -- the ori.b's bit landed, the bfins' clear did not.
 *
 * Cause: the generated 040 table calls the BARE get_bitfield/put_bitfield, which
 * use the PHYSICAL get_byte/get_word/get_long, so a bitfield access goes to the
 * UNTRANSLATED logical address. Invisible wherever ITT0/DTT0 identity-map the
 * address (logical == physical), fatal in the table-walked window where the same
 * number is slow_bank with no RAM.
 *
 * The reproduction is the guard. Ladder, in the order that localises it:
 *   1  table-mapped  -> the metal signature itself (0x20 -> 0x00, then 0x22)
 *   2  TTR-covered   -> must PASS even unfixed  (isolates translation, not decode)
 *   3  MMU off       -> must PASS even unfixed
 *   4  width > 1 and byte-crossing fields
 *   5  register destination -> must PASS (never went near memory)
 *   6  bfextu read back through the same path -> pins the read half too
 * ================================================================== */
#define BF_VCODE 0x00400000u
#define BF_PCODE 0x0C000000u
#define BF_VDATA 0x00403000u
#define BF_PDATA 0x0D000000u

/* Run one instruction at BF_VCODE with the 040 MMU on and a2 = target. */
static bool bf_run(uae_u16 op0, uae_u16 op1, uae_u32 a2val, int words)
{
	cpu_set_context(BF_VCODE);
	regs.s = 1;
	m68k_areg(regs, 2) = a2val;
	hram_poke16(BF_PCODE | (BF_VCODE & 0xFFF), op0);
	hram_poke16((BF_PCODE | (BF_VCODE & 0xFFF)) + 2, op1);
	(void)words;
	bool faulted = false;
	TRY(p) {
		regs.instruction_pc = m68k_getpc();
		regs.opcode = x_prefetch(0);
		(*cpufunctbl[regs.opcode])(regs.opcode);
	} CATCH(p) { faulted = true; (void)p; } ENDTRY
	return !faulted;
}

static void test_mmu040_bitfield_translates(void)
{
	printf("[test] ISSUE-49: bitfield ops translate through the 040 page tables\n");

	/* --- 1. the metal signature: table-mapped, BFINS {2:1} with d0=0 --- */
	mmu040_pt_init();
	mmu040_map_page(BF_VCODE, BF_PCODE);
	mmu040_map_page(BF_VDATA, BF_PDATA);
	hram_poke8(BF_PDATA, 0x20);                      /* seed byte0 = 0x20 */
	m68k_dreg(regs, 0) = 0;
	/* BFINS Dn,<ea>{offset:width}: 0xEFC0 | ea ; ext = (Dn<<12)|(off<<6)|width
	 * ea = (a2) -> mode 010 reg 2 -> 0x12 ; d0 -> 0x0000 ; off=2 width=1 -> 0x0081 */
	CHECK(bf_run(0xEFD2, 0x0081, BF_VDATA, 2), "BFINS to a table-mapped page did not fault\n");
	CHECK_EQ32(hram_peek8(BF_PDATA), 0x00,
	           "BFINS {2:1} d0=0 CLEARED the bit through the page tables (metal saw 0x20 stay)");

	/* the exact metal cascade: ori.b #2 after the bfins -> 0x02, not 0x22 */
	hram_poke8(BF_PDATA, 0x20);
	m68k_dreg(regs, 0) = 0;
	(void)bf_run(0xEFD2, 0x0081, BF_VDATA, 2);
	{   /* ori.b #2,(a2) = 0x0012 0x0002 */
		bool ok = bf_run(0x0012, 0x0002, BF_VDATA, 2);
		CHECK(ok, "ori.b #2,(a2) did not fault\n");
	}
	CHECK_EQ32(hram_peek8(BF_PDATA), 0x02,
	           "bfins-then-ori gives 0x02, not the metal signature 0x22");

	/* --- 4. wider field, and a byte-crossing one --- */
	hram_poke32(BF_PDATA, 0xFFFFFFFFu);
	m68k_dreg(regs, 0) = 0;
	CHECK(bf_run(0xEFD2, 0x0008, BF_VDATA, 2), "BFINS {0:8} did not fault\n");
	CHECK_EQ32(hram_peek8(BF_PDATA), 0x00, "BFINS {0:8} cleared a whole byte");
	hram_poke32(BF_PDATA, 0xFFFFFFFFu);
	m68k_dreg(regs, 0) = 0;
	CHECK(bf_run(0xEFD2, 0x0108, BF_VDATA, 2), "BFINS {4:8} (byte-crossing) did not fault\n");
	CHECK_EQ32(hram_peek8(BF_PDATA + 0), 0xF0, "byte-crossing BFINS cleared the high nibble of byte 0");
	CHECK_EQ32(hram_peek8(BF_PDATA + 1), 0x0F, "byte-crossing BFINS cleared the low nibble of byte 1");

	/* --- 6. BFEXTU reads back through the same path --- */
	hram_poke8(BF_PDATA, 0x40);                      /* bit 1 set (offset 1) */
	m68k_dreg(regs, 1) = 0xDEAD;
	/* BFEXTU <ea>{o:w},Dn = 0xE9C0|ea ; ext = (Dn<<12)|(off<<6)|width */
	CHECK(bf_run(0xE9D2, 0x1041, BF_VDATA, 2), "BFEXTU did not fault\n");
	CHECK_EQ32(m68k_dreg(regs, 1), 1, "BFEXTU read the bit back through the page tables");

	mmu040_off();

	/* --- 2. TTR-covered: must pass even unfixed (logical == physical) --- */
	mmu040_pt_init();
	mmu040_map_page(BF_VCODE, BF_PCODE);
	regs.dtt0 = MMU_TTR_BIT_ENABLED | MMU_TTR_BIT_SFIELD_ENABLED;  /* base 0, mask 0, match BOTH privileges */
	mmu_tt_modified();
	hram_poke8(0x00050000u, 0x20);
	m68k_dreg(regs, 0) = 0;
	CHECK(bf_run(0xEFD2, 0x0081, 0x00050000u, 2), "BFINS on a TTR-covered address did not fault\n");
	CHECK_EQ32(hram_peek8(0x00050000u), 0x00, "BFINS works on a TTR-covered address (control arm)");
	mmu040_off();

	/* --- 3. MMU off: must pass even unfixed --- */
	harness_mem_reset();
	cpu_bringup(68040);
	currprefs.cpu_model = 68040; currprefs.mmu_model = 68040;
	changed_prefs = currprefs;
	init_m68k(); build_cpufunctbl(); harness_set_x_funcs();
	regs.mmu_enabled = 0;
	hram_poke8(0x00003000u, 0x20);
	m68k_dreg(regs, 0) = 0;
	cpu_set_context(0x2000); regs.s = 1;
	m68k_areg(regs, 2) = 0x00003000u;
	hram_poke16(0x2000, 0xEFD2); hram_poke16(0x2002, 0x0081);
	TRY(p9) { regs.instruction_pc = m68k_getpc(); regs.opcode = x_prefetch(0);
	          (*cpufunctbl[regs.opcode])(regs.opcode); } CATCH(p9) { (void)p9; } ENDTRY
	CHECK_EQ32(hram_peek8(0x00003000u), 0x00, "BFINS works with the MMU off (control arm)");

	/* --- 5. register destination: never goes near memory --- */
	m68k_dreg(regs, 3) = 0xFFFFFFFFu;
	m68k_dreg(regs, 0) = 0;
	cpu_set_context(0x2000); regs.s = 1;
	hram_poke16(0x2000, 0xEFC3); hram_poke16(0x2002, 0x0081);   /* BFINS d0,d3{2:1} */
	TRY(pa) { regs.instruction_pc = m68k_getpc(); regs.opcode = x_prefetch(0);
	          (*cpufunctbl[regs.opcode])(regs.opcode); } CATCH(pa) { (void)pa; } ENDTRY
	CHECK_EQ32(m68k_dreg(regs, 3), 0xDFFFFFFFu, "BFINS to a register destination (control arm)");

	currprefs.mmu_model = 0;
	cpu_bringup(68030);
}

/* ==================================================================
 * USER MODE: root selection (URP vs SRP) and the user-mode fault frame.
 *
 * init is the first user-mode code ever run through this 040 MMU path, and every
 * fix so far was supervisor-side. Two things have never been exercised:
 *   - does the table walker pick URP for user accesses and SRP for supervisor?
 *   - does a fault taken IN USER MODE build a format-$7 frame the kernel can
 *     classify -- SSW TM = user, and a stacked SR with S clear?
 * The metal report is "User BUS ERROR at 40001FC0, PC:80000012 FAULT:6 PID:1".
 * ================================================================== */
#define M4_UROOT 0x00110000u
#define M4_UPTR  0x00111000u
#define M4_UPAGE 0x00112000u

/* Give the USER root its own tables, mapping va -> pa (4K). */
static void mmu040_map_user(uae_u32 va, uae_u32 pa)
{
	hram_poke32(M4_UROOT + ((va >> 23) & 0x1fc), (M4_UPTR  & MMU_ROOT_PTR_ADDR_MASK)   | 2);
	hram_poke32(M4_UPTR  + ((va >> 16) & 0x1fc), (M4_UPAGE & MMU_PTR_PAGE_ADDR_MASK_4) | 2);
	hram_poke32(M4_UPAGE + ((va >> 10) & 0xfc),  (pa & MMU_PAGE_ADDR_MASK_4) | 1);
	mmu_flush_atc_all(true);
}

static void test_mmu040_user_mode(void)
{
	printf("[test] user mode: URP root selection and the user-mode fault frame\n");
	const uae_u32 VCODE = 0x00400000, PCODE = 0x0C000000;
	const uae_u32 VSTK  = 0x00401000, PSTK  = 0x0E000000;
	const uae_u32 VVEC  = 0x00402000, PVEC  = 0x0F000000;
	const uae_u32 VSHR  = 0x00403000;                 /* mapped in BOTH roots, to different frames */
	const uae_u32 PSUP  = 0x0A000000, PUSR = 0x0B000000;
	const uae_u32 VUNMAP = 0x00404000;                /* mapped in SRP only -> user must fault */

	mmu040_pt_init();                                  /* srp = urp = M4_ROOT */
	mmu040_map_page(VCODE, PCODE);
	mmu040_map_page(VSTK,  PSTK);
	mmu040_map_page(VVEC,  PVEC);
	mmu040_map_page(VSHR,  PSUP);
	mmu040_map_page(VUNMAP, 0x0D000000);               /* supervisor-only mapping */
	regs.urp = M4_UROOT;                               /* user gets its OWN root */
	mmu040_map_user(VCODE, PCODE);                     /* user can fetch code */
	mmu040_map_user(VSHR,  PUSR);                      /* same VA, different frame */
	hram_poke32(PSUP, 0x5A5A5A5Au);
	hram_poke32(PUSR, 0xA5A5A5A5u);

	/* --- A. root selection: same VA must translate differently per privilege --- */
	uaecptr pa_s = 0, pa_u = 0; bool f = false;
	SAVE_EXCEPTION;
	TRY(p1) { pa_s = mmu_translate(VSHR, 0, true,  true, false, sz_long); } CATCH(p1) { f = true; (void)p1; } ENDTRY
	TRY(p2) { pa_u = mmu_translate(VSHR, 0, false, true, false, sz_long); } CATCH(p2) { f = true; (void)p2; } ENDTRY
	RESTORE_EXCEPTION;
	CHECK(!f, "both privileged translations of the shared VA succeeded\n");
	CHECK_EQ32(pa_s, PSUP, "supervisor access walks SRP");
	CHECK_EQ32(pa_u, PUSR, "user access walks URP (not SRP)");

	/* a VA present only in SRP must FAULT for user -- proves URP is really consulted */
	bool ufault = false;
	SAVE_EXCEPTION;
	TRY(p3) { (void)mmu_translate(VUNMAP, 0, false, true, false, sz_long); }
	CATCH(p3) { ufault = true; (void)p3; } ENDTRY
	RESTORE_EXCEPTION;
	CHECK(ufault, "a supervisor-only mapping is NOT visible to user mode\n");

	/* --- B. a fault taken IN USER MODE builds a user-context format-$7 frame --- */
	hram_poke16(PCODE | (VCODE & 0xFFF), 0x2039);      /* move.l VUNMAP.l,d0 */
	hram_poke16((PCODE | (VCODE & 0xFFF)) + 2, (uae_u16)(VUNMAP >> 16));
	hram_poke16((PCODE | (VCODE & 0xFFF)) + 4, (uae_u16)(VUNMAP & 0xFFFF));
	hram_poke32(PVEC | 8, VCODE + 0x100);              /* vector 2 handler (in the mapped code page) */

	regs.vbr = VVEC;
	regs.isp = VSTK | 0xF00;                           /* supervisor stack the frame lands on */
	regs.msp = VSTK | 0xF00;
	regs.m = 0;
	m68k_areg(regs, 7) = VSTK | 0xE00;                 /* user sp (unused here) */
	m68k_setpc(VCODE);
	regs.s = 0;                                        /* <-- USER MODE */
	MakeSR();

	bool faulted = false;
	{
		struct flag_struct fl = regs.ccrflags;
		mmu_restart = true;
		regs.instruction_pc = m68k_getpc();
		TRY(p4) {
			mmu_opcode = -1;
			mmu_opcode = regs.opcode = x_prefetch(0);
			(*cpufunctbl[regs.opcode])(regs.opcode);
		} CATCH(p4) {
			faulted = true;
			if (mmu_restart) { regs.ccrflags = fl; m68k_setpci(regs.instruction_pc); }
			cpu_restore_fixup();
			Exception(p4);
		} ENDTRY
	}
	CHECK(faulted, "user-mode access to an unmapped user page faulted\n");
	CHECK_EQ32(regs.s, 1, "the exception switched to supervisor");

	{
		uae_u32 sp = m68k_areg(regs, 7);
#define UF16(o) ((uae_u32)hram_peek16(PSTK | ((sp + (o)) & 0xFFF)))
#define UF32(o) ((uae_u32)hram_peek32(PSTK | ((sp + (o)) & 0xFFF)))
		CHECK_EQ32(UF16(0x06), 0x7008, "user fault builds a format $7 frame, vector offset $008");
		CHECK_EQ32(UF16(0x00) & 0x2000, 0x0000,
		           "stacked SR has the S bit CLEAR -- the kernel must see a USER fault");
		CHECK_EQ32(UF16(0x0C) & MMU_SSW_TM, 1,
		           "SSW TM = 1 (user data) -- not a supervisor function code");
		CHECK_EQ32(UF32(0x14), VUNMAP, "FA is the faulting user address");
		CHECK_EQ32(UF32(0x18), VUNMAP, "WB3A matches");
#undef UF16
#undef UF32
	}

	regs.urp = regs.srp;
	mmu040_off();
}

/* ==================================================================
 * RTE must deliver the frame's PC exactly, from a TABLE-WALKED stack.
 *
 * ISSUE-52: the kernel's _start builds a format-0 frame on its kernel stack (in
 * the table-walked u-block window), pushes PC = 0x80800000, and rte's to user
 * mode -- and PID 1 surfaces at 0x80000012 instead. The hypothesis was that the
 * exception-return path reads the frame through the wrong accessor family, the
 * way the bitfield path did. This pins the delivery both ways.
 * ================================================================== */
static void rte_frame_arm(const char *what, uae_u32 frame_va, uae_u32 frame_pa,
                          uae_u32 vcode, uae_u32 pcode)
{
	const uae_u32 WANT_PC = 0x80800000u;
	const uae_u16 WANT_SR = 0x0000;          /* user mode, all interrupts enabled */

	/* format-0 frame: SR(word) PC(long) format&vector(word) */
	hram_poke16(frame_pa + 0, WANT_SR);
	hram_poke32(frame_pa + 2, WANT_PC);
	hram_poke16(frame_pa + 6, 0x0000);       /* format 0, vector offset 0 */

	hram_poke16(pcode, 0x4e73);              /* RTE */

	cpu_set_context(vcode);
	regs.s = 1;
	MakeSR();
	regs.usp = 0xC0800000u;                  /* the icode stack the kernel built */
	m68k_areg(regs, 7) = frame_va;

	bool faulted = false;
	TRY(p) {
		regs.instruction_pc = m68k_getpc();
		regs.opcode = x_prefetch(0);
		(*cpufunctbl[regs.opcode])(regs.opcode);
	} CATCH(p) { faulted = true; (void)p; } ENDTRY

	if (faulted) { g_fail++; printf("  FAIL: %s: RTE faulted\n", what); return; }
	g_pass++;
	CHECK_EQ32(m68k_getpc(), WANT_PC, what);
	CHECK_EQ32(regs.s, 0, "RTE dropped to user mode (S clear from the popped SR)");
	CHECK_EQ32(m68k_areg(regs, 7), 0xC0800000u, "RTE switched a7 to the USP");
}

static void test_mmu040_rte_delivers_pc(void)
{
	printf("[test] RTE delivers the frame's PC exactly, table-walked and TTR-covered\n");
	const uae_u32 VCODE = 0x00400000, PCODE = 0x0C000000;
	const uae_u32 VSTK  = 0x00401000, PSTK  = 0x0E000000;

	/* --- the metal case: frame on a TABLE-WALKED page --- */
	mmu040_pt_init();
	mmu040_map_page(VCODE, PCODE);
	mmu040_map_page(VSTK,  PSTK);
	rte_frame_arm("RTE from a TABLE-WALKED frame delivers the exact PC",
	              VSTK | 0xF00, PSTK | 0xF00, VCODE, PCODE | (VCODE & 0xFFF));
	mmu040_off();

	/* --- the exact metal case: MISALIGNED frame in a table-walked page (ISSUE-52).
	 * a7 = ...FB8 so the frame PC longword sits at a7+2 = ...FBA, bus-misaligned but not
	 * page-crossing. The value is correct on the host either way (flat buffer composes any
	 * alignment), so the meaningful assertion is the ROUTING invariant: the misaligned long
	 * read must go through the unaligned splitter, which issues only aligned sub-accesses --
	 * the fix that keeps the board's memory layer from ever seeing a misaligned transfer. */
	mmu040_pt_init();
	mmu040_map_page(VCODE, PCODE);
	mmu040_map_page(VSTK,  PSTK);
	extern unsigned long mmu_unaligned_split_count;
	mmu_unaligned_split_count = 0;
	rte_frame_arm("RTE from a MISALIGNED table-walked frame delivers the exact PC",
	              VSTK | 0xFB8, PSTK | 0xFB8, VCODE, PCODE | (VCODE & 0xFFF));
	CHECK(mmu_unaligned_split_count >= 1,
	      "the misaligned frame PC longword was read via the aligned splitter, not a raw "
	      "misaligned physical access (the metal bit-23 bug)\n");
	mmu040_off();

	/* --- control: frame on a TTR-covered address (logical == physical) --- */
	mmu040_pt_init();
	mmu040_map_page(VCODE, PCODE);
	regs.dtt0 = MMU_TTR_BIT_ENABLED | MMU_TTR_BIT_SFIELD_ENABLED;   /* covers 0x00xxxxxx */
	mmu_tt_modified();
	rte_frame_arm("RTE from a TTR-covered frame delivers the exact PC",
	              0x00060F00u, 0x00060F00u, VCODE, PCODE | (VCODE & 0xFFF));
	mmu040_off();

	cpu_bringup(68030);
}

/* ==================================================================
 * SYSCALL-RETURN RTE: A7 must be the USP, never the leftover SSP.
 *
 * ISSUE-52 round-4: PID 1's icode runs (lea sets SP, trap #0), then faults after
 * the syscall RETURN with a corrupt A7 (metal fa = 0x40001FC0 = the kernel SSP;
 * a kernel ring saw usp = 0xCB7C0002, garbage/odd). A syscall-return RTE pops a
 * format-0 frame off the SUPERVISOR (kernel) stack and, because the frame SR has
 * S clear, MakeFromSR must install regs.usp into A7 -- NOT leave A7 on the popped
 * supervisor stack. This pins the emulator's half: given a valid regs.usp, the
 * RTE lands A7 exactly on it and not on the SSP. So if the metal [URTE] read shows
 * a corrupt usp at firing #2, the emulator's install is exonerated and the bad
 * value was already in regs.usp (kernel move-to-USP or trap-entry save) -- i.e.
 * the corruption is upstream of the RTE, not in it.
 * ================================================================== */
static void test_mmu040_rte_syscall_return_usp(void)
{
	printf("[test] syscall-return RTE lands A7 on USP, not the leftover kernel SSP\n");
	const uae_u32 VCODE = 0x00400000, PCODE = 0x0C000000;
	const uae_u32 VSSP  = 0x00401000, PSSP  = 0x0E000000;   /* the kernel (supervisor) stack */
	const uae_u32 SSP   = VSSP | 0xFC0;                     /* mirrors the metal 0x...FC0 */
	const uae_u32 GOODUSP = 0x7FFF0000u;                    /* a valid user stack, distinct from SSP */
	const uae_u32 RETPC   = 0x80800012u;                    /* the syscall-return user PC */

	mmu040_pt_init();
	mmu040_map_page(VCODE, PCODE);
	mmu040_map_page(VSSP,  PSSP);

	/* format-0 frame the kernel's syscall path built on the supervisor stack */
	hram_poke16(PSSP | 0xFC0, 0x0000);        /* SR: S clear -> return to user */
	hram_poke32((PSSP | 0xFC0) + 2, RETPC);   /* return PC */
	hram_poke16((PSSP | 0xFC0) + 6, 0x0000);  /* format 0 */
	hram_poke16(PCODE, 0x4e73);               /* RTE */

	cpu_set_context(VCODE);
	regs.s = 1; MakeSR();
	regs.usp = GOODUSP;                        /* the user SP saved at trap entry */
	m68k_areg(regs, 7) = SSP;

	bool faulted = false;
	TRY(p) {
		regs.instruction_pc = m68k_getpc();
		regs.opcode = x_prefetch(0);
		(*cpufunctbl[regs.opcode])(regs.opcode);
	} CATCH(p) { faulted = true; (void)p; } ENDTRY

	CHECK(!faulted, "syscall-return RTE did not fault\n");
	CHECK_EQ32(regs.s, 0, "dropped to user mode");
	CHECK_EQ32(m68k_getpc(), RETPC, "delivered the syscall-return PC");
	CHECK_EQ32(m68k_areg(regs, 7), GOODUSP, "A7 == USP after the return (the decisive install)");
	CHECK(m68k_areg(regs, 7) != SSP, "A7 is NOT the leftover kernel SSP (the metal corruption)\n");

	mmu040_off();
	cpu_bringup(68030);
}

/* ==================================================================
 * Full-format PC-relative LEA (the icode's first instruction) on the 040 path.
 *
 * ISSUE-52 round-5: proc 1's icode opens with
 *   lea %pc@(L%stack),%sp   ; 4FFB 0170 0000 0028
 * a full-format (dp bit 8), base-not-suppressed, index-suppressed (dp bit 6),
 * 32-bit base-displacement (dp bits 5-4 = 11) PC-relative EA. The correct result
 * is EA = (pc_of_ext_word) + 0x28 and the instruction is 8 bytes (opword 2 + ext 2
 * + bd 4). This pins both the EA value and the length on the mmu040 accessor path.
 * ================================================================== */
static void test_mmu040_lea_pcrel_fullformat(void)
{
	printf("[test] full-format PC-relative LEA: EA and instruction length (icode)\n");
	const uae_u32 VCODE = 0x00408000, PCODE = 0x0C000000;
	mmu040_pt_init();                            /* wires the mmu040 x-funcs + cpufunctbl */
	mmu040_map_page(VCODE, PCODE);               /* one mapped, table-walked code page */
	regs.s = 1;

	/* lea %pc@(0x28,ZPC),%a2  -- dst A2 to avoid disturbing the stack pointer.
	 * opword 0x45FB = LEA, dst An=2, mode 7 reg 3; ext 0x0170; bd 0x00000028. */
	hram_poke16(PCODE + 0, 0x45FB);
	hram_poke16(PCODE + 2, 0x0170);
	hram_poke32(PCODE + 4, 0x00000028);
	hram_poke16(PCODE + 8, 0x4e71);              /* nop (must be the next instruction) */

	cpu_set_context(VCODE);
	regs.s = 1;
	m68k_areg(regs, 2) = 0xDEADBEEFu;

	bool faulted = false;
	TRY(p) {
		regs.instruction_pc = m68k_getpc();
		regs.opcode = x_prefetch(0);
		(*cpufunctbl[regs.opcode])(regs.opcode);
	} CATCH(p) { faulted = true; (void)p; } ENDTRY

	CHECK(!faulted, "full-format PC-relative LEA did not fault\n");
	/* base anchor is the extension-word address = VCODE + 2; EA = base + 0x28 */
	CHECK_EQ32(m68k_areg(regs, 2), (VCODE + 2) + 0x28,
	           "EA = (pc+2) + base-disp 0x28 (full-format PC-relative)");
	CHECK_EQ32(m68k_getpc(), VCODE + 8,
	           "instruction length is 8 bytes (opword 2 + ext 2 + bd 4)");

	/* the very next fetch must be the nop, proving the stream did not skew */
	regs.instruction_pc = m68k_getpc();
	uae_u32 nextop = x_prefetch(0);
	CHECK_EQ32(nextop, 0x4e71, "the instruction after the LEA decodes correctly (stream aligned)");

	mmu040_off();
	cpu_bringup(68030);
}

/* ==================================================================
 * ISSUE-52 round-7: the USER instruction stream on a TABLE-WALKED page.
 *
 * mmu040_pt_init()'s fixed root/pointer pair only covers VAs in
 * [0x00400000,0x00440000).  proc 1's icode lives at 0x80800000 and its kernel
 * stack in the 0x40000000 u-block window, so this lane needs mappings at
 * arbitrary VAs.  Same 4K descriptor format, but the pointer and page tables
 * are bump-allocated per (root index, pointer index) pair.
 * ================================================================== */
#define M4G_ROOT  0x00180000u          /* root table (512B) */
#define M4G_HEAP  0x00181000u          /* bump arena for ptr/page tables */
static uae_u32 m4g_next;

static void mmu040_gen_init(void)
{
	harness_mem_reset();
	cpu_bringup(68040);
	currprefs.cpu_model = 68040;
	currprefs.mmu_model = 68040;
	currprefs.mmu_ec    = 0;
	changed_prefs = currprefs;
	init_m68k();
	build_cpufunctbl();
	harness_set_x_funcs();

	regs.itt0 = regs.itt1 = regs.dtt0 = regs.dtt1 = 0;
	mmu_tt_modified();
	regs.srp = regs.urp = M4G_ROOT;
	m4g_next = M4G_HEAP;
	mmu_set_tc(0x8000);                 /* enable, 4K pages */
	mmu_flush_atc_all(true);
	regs.s = 1;
	regs.address_space_mask = 0xffffffff;
}

static void mmu040_gen_map(uae_u32 va, uae_u32 pa, uae_u32 page_extra)
{
	uae_u32 ri = (va >> 23) & 0x1fc;
	uae_u32 pi = (va >> 16) & 0x1fc;
	uae_u32 gi = (va >> 10) & 0xfc;

	uae_u32 rd = hram_peek32(M4G_ROOT + ri), ptbl;
	if ((rd & 2) == 0) {
		ptbl = m4g_next; m4g_next += 0x200;
		hram_poke32(M4G_ROOT + ri, (ptbl & MMU_ROOT_PTR_ADDR_MASK) | 2);
	} else ptbl = rd & MMU_ROOT_PTR_ADDR_MASK;

	uae_u32 pd = hram_peek32(ptbl + pi), pgtbl;
	if ((pd & 2) == 0) {
		pgtbl = m4g_next; m4g_next += 0x200;
		hram_poke32(ptbl + pi, (pgtbl & MMU_PTR_PAGE_ADDR_MASK_4) | 2);
	} else pgtbl = pd & MMU_PTR_PAGE_ADDR_MASK_4;

	hram_poke32(pgtbl + gi, (pa & MMU_PAGE_ADDR_MASK_4) | 1 | page_extra);
	mmu_flush_atc_all(true);
}

/* ==================================================================
 * The metal ISSUE-52 vector, end to end, through the REAL run-loop fetch path.
 *
 * The kernel RTEs to user mode at 0x80800000 (a table-walked user page) and the
 * icode's first instruction is
 *     lea %pc@(L%stack),%sp     ; 4FFB 0170 0000 0028
 * whose 32-bit base displacement sits at page offset 4 -- i.e. it is fetched by
 * x_next_ilong() from the MIDDLE of an instruction, not by the run loop's
 * opword prefetch.  Metal (firmware 368993CA, [USTEP] probe) returned
 *     bd   = 0x4AFC0000  (the long at guest physical 0 -- the kernel's
 *                         ILLEGAL-at-null sentinel)  -> a7 = 0xCB7C0002
 *     next opword = 0x0800 (the word at guest physical 4)
 * i.e. the fetch address collapsed to zero mid-instruction and stayed
 * authoritative for the following opcode fetch, while m68k_getpc() stayed
 * correct throughout.  Guest 0/4 are planted with exactly those metal values so
 * a collapsed fetch reproduces the metal bytes rather than a benign zero.
 *
 * This drives the whole mechanism -- table walk, user privilege, RTE-to-user
 * arming, x_prefetch, and the mid-instruction x_next_iword/x_next_ilong -- not
 * an accessor called directly.
 * ================================================================== */
static void test_mmu040_user_icode_stream(void)
{
	printf("[test] user icode stream: RTE to a translated page, mid-instruction 32-bit "
	       "base displacement\n");
	const uae_u32 VUCODE = 0x80800000u, PUCODE = 0x0C000000u;   /* proc 1 icode page  */
	const uae_u32 VKCODE = 0x00400000u, PKCODE = 0x0D000000u;   /* kernel code page   */
	const uae_u32 VKSTK  = 0x40001000u, PKSTK  = 0x0E000000u;   /* kernel stack (u-block) */
	const uae_u32 USP0   = 0x08003118u;                         /* metal's pre-lea a7 */

	mmu040_gen_init();
	mmu040_gen_map(VKCODE, PKCODE, 0);
	mmu040_gen_map(VKSTK,  PKSTK,  0);
	mmu040_gen_map(VUCODE, PUCODE, 0);

	/* the two metal decoder values, at the addresses a collapsed fetch reads */
	hram_poke32(0x00000000u, 0x4AFC0000u);   /* kernel ILLEGAL-at-null sentinel */
	hram_poke32(0x00000004u, 0x080007F0u);   /* pseudo-ExecBase pointer         */

	/* the icode: lea %pc@(0x28,ZPC),%sp ; moveq #11,d0 */
	hram_poke16(PUCODE + 0, 0x4FFB);
	hram_poke16(PUCODE + 2, 0x0170);
	hram_poke32(PUCODE + 4, 0x00000028);
	hram_poke16(PUCODE + 8, 0x700B);

	/* the kernel's launch frame: format 0, SR with S clear -> rte drops to user */
	hram_poke16(PKSTK + 0xFC0 + 0, 0x0000);
	hram_poke32(PKSTK + 0xFC0 + 2, VUCODE);
	hram_poke16(PKSTK + 0xFC0 + 6, 0x0000);
	hram_poke16(PKCODE, 0x4E73);                 /* rte */

	cpu_set_context(VKCODE);
	regs.s = 1; MakeSR();
	regs.usp = USP0;
	m68k_areg(regs, 7) = VKSTK | 0xFC0;

	CHECK(!mmu040_step_catch(), "the launch RTE did not fault\n");
	CHECK_EQ32(regs.s, 0, "RTE dropped to USER mode");
	CHECK_EQ32(m68k_getpc(), VUCODE, "RTE delivered the icode entry pc");
	CHECK_EQ32(m68k_areg(regs, 7), USP0, "a7 is the user stack the kernel saved");

	/* ---- USTEP0: the icode LEA, run exactly as m68k_run_mmu040 runs it ---- */
	bool f = mmu040_step_catch();
	printf("       USTEP0 pc=%08X op=%04X -> USTEP1 pc=%08X a7=%08X\n",
	       (unsigned)VUCODE, (unsigned)(regs.opcode & 0xFFFF),
	       (unsigned)m68k_getpc(), (unsigned)m68k_areg(regs, 7));
	CHECK(!f, "the icode LEA did not fault\n");
	CHECK_EQ32(regs.opcode & 0xFFFF, 0x4FFB, "USTEP0 opword is the icode LEA (offset 0 fetch)");
	CHECK_EQ32(m68k_areg(regs, 7), VUCODE + 2 + 0x28,
	           "a7 = (pc+2) + bd 0x28 -- the 32-bit base displacement came from the USER "
	           "page, not from guest address 0");
	CHECK(m68k_areg(regs, 7) != 0xCB7C0002u,
	      "a7 is not the metal value 0xCB7C0002 (base + the guest-0 sentinel)\n");
	CHECK_EQ32(m68k_getpc(), VUCODE + 8, "instruction length 8 (opword 2 + ext 2 + bd 4)");

	/* ---- USTEP1: the next opcode fetch must not inherit a collapsed address ---- */
	f = mmu040_step_catch();
	CHECK(!f, "the instruction after the LEA did not fault\n");
	CHECK_EQ32(regs.opcode & 0xFFFF, 0x700B,
	           "next opword is moveq #11,d0 -- the fetch address did not stay collapsed");
	CHECK(( regs.opcode & 0xFFFF) != 0x0800,
	      "next opword is not the metal value 0x0800 (the word at guest address 4)\n");
	CHECK_EQ32(m68k_dreg(regs, 0), 11, "moveq #11,d0 executed with the right operand");

	regs.s = 1;
	mmu040_off();
	cpu_bringup(68030);
}

/* ==================================================================
 * ISSUE-52 round-7 / ISSUE-10: the format-$7 write-back slot must be CONSUMED.
 *
 * The 68040 does not re-run a faulted write on rte -- the pending store travels
 * to the guest in the frame's WB3 slot (WB3S/WB3A/WB3D) and the kernel's
 * write-back handler (AMIX src/wb040.s) performs it:
 *     moves.<WB3S size> WB3D -> (WB3A)      with DFC = WB3S & 7
 * gated ONLY on WB3S bit 7 (valid).  Handing the slot to the guest therefore
 * CONSUMES it: the emulator must clear regs.wb3_status (and regs.wb2_status)
 * as it stacks them, or the valid bit outlives the frame that delivered it.
 *
 * mmu_bus_error() recomputes both status words on every fault it handles, which
 * hides the missing clear for MMU faults.  It does NOT hide it for the fault
 * classes that reach Exception(2) WITHOUT mmu_bus_error -- hardware/physical bus
 * errors (hardware_exception2 / exception2_read / exception2_fetch), which under
 * this non-cpu_compatible config THROW straight to the run loop's CATCH.  Those
 * frames then carry the PREVIOUS write fault's still-valid WB3S and its stale
 * WB3D, paired with the CURRENT fault's address as WB3A -- so the guest's handler
 * fabricates a store of a stale datum into the page that just faulted.  A stray
 * store with no instruction behind it, which is exactly the ISSUE-10 signature.
 *
 * Both halves are asserted here, the first through the real run loop on a
 * table-walked page in user mode.
 * ================================================================== */
static void test_mmu040_writeback_slot_consumed(void)
{
	printf("[test] format-$7 write-back slot is consumed by the frame that delivers it\n");
	const uae_u32 VUCODE = 0x80800000u, PUCODE = 0x0C000000u;   /* user text page      */
	const uae_u32 VKSTK  = 0x40001000u, PKSTK  = 0x0E000000u;   /* kernel stack        */
	const uae_u32 VVEC   = 0x00402000u, PVEC   = 0x0F000000u;   /* vector table        */
	const uae_u32 VUNMAP = 0x80802000u;                         /* never mapped        */
	const uae_u32 POISON = 0x4AFC0000u;                         /* the ILLEGAL poison  */
	const uae_u32 HANDLER = VUCODE + 0x100;
	const uae_u32 SP0 = VKSTK | 0xF00;
	uae_u32 sp;
#define W16(off) ((uae_u32)hram_peek16(PKSTK | ((sp + (off)) & 0xFFF)))
#define W32(off) ((uae_u32)hram_peek32(PKSTK | ((sp + (off)) & 0xFFF)))

	mmu040_gen_init();
	mmu040_gen_map(VUCODE, PUCODE, 0);
	mmu040_gen_map(VKSTK,  PKSTK,  0);
	mmu040_gen_map(VVEC,   PVEC,   0);
	hram_poke32(0x00000000u, POISON);            /* the guest-0 sentinel, per the metal decode */

	/* ---- (a) a USER-mode store that faults: WB3 armed, then delivered ---- */
	hram_poke16(PUCODE, 0x2080);                 /* move.l d0,(a0) */
	hram_poke32(PVEC | 8, HANDLER);              /* vector 2 */

	regs.vbr = VVEC;
	regs.isp = regs.msp = SP0;
	regs.m = 0;
	m68k_dreg(regs, 0) = POISON;                 /* the datum the handler would replay */
	m68k_areg(regs, 0) = VUNMAP;
	m68k_areg(regs, 7) = VKSTK | 0xE00;          /* user sp (unused) */
	m68k_setpc(VUCODE);
	regs.s = 0;                                  /* USER mode */
	MakeSR();
	regs.wb2_address = 0;
	for (int i = 0; i < 4; i++) mmu040_move16[i] = 0;

	CHECK(mmu040_step_catch(), "user-mode store to an unmapped page raised an access error\n");
	sp = m68k_areg(regs, 7);
	CHECK_EQ32(W16(0x0E), 0x0081, "WB3S = valid $80 | SIZE long | FC 1 (user data)");
	CHECK_EQ32(W32(0x18), VUNMAP, "WB3A = the pending store's address");
	CHECK_EQ32(W32(0x1C), POISON, "WB3D = the pending store's datum");

	/* THE INVARIANT: the slot has been handed to the guest, so it is spent. */
	CHECK_EQ32(regs.wb3_status, 0,
	           "wb3_status is CLEARED by the frame that delivered it (not left armed)");
	CHECK_EQ32(regs.wb2_status, 0,
	           "wb2_status is CLEARED by the frame that delivered it (not left armed)");

	/* ---- (b) a HARDWARE bus error next: no mmu_bus_error runs, so the frame is
	 * built from whatever the write fault left behind. It must report NO pending
	 * write-back -- otherwise the guest fabricates a store of the stale WB3D into
	 * the address that just faulted. ---- */
	{
		const uae_u32 HWFAULT = VUCODE + 0x40;   /* a MAPPED user address: a replay here
		                                          * would scribble the text page itself */
		regs.isp = regs.msp = SP0;
		m68k_areg(regs, 7) = VKSTK | 0xE00;
		m68k_setpc(VUCODE);
		regs.s = 0;
		MakeSR();

		SAVE_EXCEPTION;
		exception2_read(0x2210, HWFAULT, sz_long, 1);   /* physical/hardware bus error */
		RESTORE_EXCEPTION;

		sp = m68k_areg(regs, 7);
		CHECK_EQ32(W16(0x06), 0x7008, "hardware bus error also builds a format $7 frame");
		CHECK_EQ32(W16(0x0E), 0,
		           "WB3S invalid: a fault with no pending store must not inherit the "
		           "previous write fault's valid bit");
		CHECK_EQ32(W16(0x10), 0, "WB2S invalid on the same frame");
		/* With WB3S invalid the guest's wb040.s skips the slot, so the stale WB3D is
		 * inert. Left armed, it would have replayed POISON into WB3A = this fault's
		 * address -- a stray store with no instruction behind it. */
		CHECK(!(W16(0x0E) & 0x80) || W32(0x1C) != POISON,
		      "no armed write-back carrying the stale poison datum\n");
	}

#undef W16
#undef W32
	regs.s = 1;
	mmu040_off();
	cpu_bringup(68030);
}

/* Direct accessor-level pin of the ISSUE-52 routing invariant, both directions. */
static void test_mmu040_misaligned_split(void)
{
	printf("[test] bus-misaligned 040 access routes through the aligned splitter\n");
	extern unsigned long mmu_unaligned_split_count;
	const uae_u32 VA = 0x00408000, PA = 0x0A000000;   /* one mapped page */
	(void)mmu040_map1(VA, PA, 0, false);
	regs.s = 1;

	hram_poke32(PA + 0x10, 0x11223344u);              /* aligned longword (non-overlapping) */
	hram_poke32(PA + 0x02, 0x80800000u);              /* the metal value at a misaligned VA */

	/* aligned read: single path, splitter NOT used, value correct */
	mmu_unaligned_split_count = 0;
	uae_u32 v_al = get_long_mmu040(VA + 0x10);
	CHECK_EQ32(mmu_unaligned_split_count, 0, "aligned long read does not use the splitter");
	CHECK_EQ32(v_al, 0x11223344u, "aligned long read value");

	/* misaligned, same page: splitter USED, value still correct (bit 23 preserved) */
	mmu_unaligned_split_count = 0;
	uae_u32 v_un = get_long_mmu040(VA + 0x02);
	CHECK(mmu_unaligned_split_count >= 1, "misaligned in-page long read uses the splitter\n");
	CHECK_EQ32(v_un, 0x80800000u, "misaligned long read preserves bit 23 (0x80800000)");

	/* misaligned write must also route through the splitter */
	mmu_unaligned_split_count = 0;
	put_long_mmu040(VA + 0x02, 0x80800000u);
	CHECK(mmu_unaligned_split_count >= 1, "misaligned in-page long WRITE uses the splitter\n");
	CHECK_EQ32(get_long_mmu040(VA + 0x02), 0x80800000u, "misaligned write round-trips exactly");

	mmu040_off();
}

/* ==================================================================
 * ISSUE-52 round-8 / ISSUE-10 GENESIS: a kernel copyout whose destination page
 * first-touch faults must resume with BOTH of its pointers intact.
 *
 * mmufixup[] is the run loop's (An)+/-(An) rollback record: a handler arms a slot
 * with an address register's PRE-increment value before an access that may fault,
 * and disarms it once the access lands.  m68k_run_mmu040()'s CATCH calls
 * cpu_restore_fixup(), which writes every ARMED slot back into its register.
 *
 * The 68040 table (cpuemu_31) uses slot 0 only -- 388 arms, zero for slot 1.  Slot
 * 1 belongs to the 68030 engine (cpuemu_32 / cpummu030's mmu030fixupreg(...,1)).
 * mmufixup lives in BSS, so at cold boot BOTH slots read {reg = 0, value = 0} --
 * and `reg = 0` is not "disarmed", it is "A0 is armed, restore it to 0".  Upstream
 * (amiberry src/newcpu.cpp:3703-3704, WinUAE m68k_reset2) clears both at reset:
 *     mmufixup[0].reg = -1;
 *     mmufixup[1].reg = -1;
 * This port kept only the first line.  On the 68030 path the omission is invisible
 * -- m68k_run_mmu030's CATCH clears both on its first fault, harmlessly, long
 * before anything depends on it.  On the 68040 path nothing ever clears slot 1, so
 * the FIRST bus fault of the session runs cpu_restore_fixup() against a slot that
 * says "A0 := 0", and A0 is destroyed.
 *
 * Metal (firmware B29BF9EB, kernel 68040-260821-27) is exactly that, once:
 *   [XT01] vec=2 ipc=080005C6 pc=080005C6 fa=80800000 ssw=0401 sr=2109
 * SSW 0x0401 = ATC | RW=0 (write) | SIZE=long | TT=00 TM=001 (user data from
 * supervisor mode) -- a MOVES store through DFC.  The kernel's local copyout is
 *   080005C0 lcolloop: move.l  (a0)+,d1     ; A0 = kernel source
 *   080005C2           moves.l d1,(a1)+     ; A1 = user dest, DFC=1  <-- faults
 *   080005C6           subq.l  #4,d0        ; <- the frame PC (write faults do not
 *   080005C8           bge.l   lcolloop     ;    restart; the store rides in WB3)
 * so the frame PC is 0x080005C6, one instruction past the MOVES, exactly as
 * op_0e98_31_ff's `regs.instruction_pc = m68k_getpci(); mmu_restart = false;`
 * arranges.  The handler had already disarmed slot 0 (correct -- a continued write
 * must NOT rewind A1), so cpu_restore_fixup() found only the cold slot 1 and set
 * A0 = 0.  The kernel then replayed WB3 (wb040.s) and RTE'd back to 0x080005C6,
 * and the loop resumed reading from guest address 0.  Every metal cell falls out:
 *   - destination offset 0 correct (the WB3 replay of the last good read),
 *   - offsets 4..0x37 = guest low memory 0..0x33 (0x4AFC0000, 0x080007F0, the KS
 *     ROM vector stubs) -- A0 walking up from zero,
 *   - copy LENGTH preserved (D0 and A1 both untouched),
 *   - copyout returns success, so execve completes and PID 1 executes the poison.
 * Bench amiberry boots the same kernel because it never lost the second line.
 *
 * The vector drives the REAL m68k_run_mmu040() -- not a transcription of it --
 * because the defect lives in state that only the real CATCH consumes.
 * SPCFLAG_MODE_CHANGE makes do_specialties() return 1 after each instruction, so
 * the loop returns and the fixture can step it.
 * ================================================================== */
void m68k_run_mmu040(void);            /* the real 68040 MMU run loop (newcpu.cpp) */
#define Z_SPCFLAG_MODE_CHANGE 8192     /* custom.h, not includable in the harness */

static void test_mmu040_copyout_fault_keeps_source_reg(void)
{
	printf("[test] a kernel copyout that faults on its destination keeps its SOURCE register\n");
	const uae_u32 VKCODE = 0x00400000u, PKCODE = 0x0D000000u;   /* kernel text       */
	const uae_u32 VVEC   = 0x00402000u, PVEC   = 0x0F000000u;   /* vector table      */
	const uae_u32 VSRC   = 0x00403000u, PSRC   = 0x0B000000u;   /* kernel source buf */
	const uae_u32 VKSTK  = 0x40001000u, PKSTK  = 0x0E000000u;   /* kernel stack      */
	const uae_u32 VDST   = 0x80800000u, PDST   = 0x0C000000u;   /* user dest (UNMAPPED at first) */
	const uae_u32 NBYTES = 0x38u;                                /* the metal icode length */
	const uae_u32 LOOP   = VKCODE + 0x18, DONE = VKCODE + 0x22, HANDLER = VKCODE + 0x100;

	/* --- (a) the cold-boot mmufixup[] state, and what a guest reset must do to it --- */
	mmufixup[0].reg = 0; mmufixup[0].value = 0;      /* BSS: slot armed on A0, value 0 */
	mmufixup[1].reg = 0; mmufixup[1].value = 0;
	m68k_reset_newcpu(true);
	CHECK(mmufixup[0].reg == -1, "a guest reset disarms mmufixup[0]\n");
	CHECK(mmufixup[1].reg == -1,
	      "a guest reset disarms mmufixup[1] -- BSS zero means 'A0 armed, restore 0', and\n"
	      "        the 68040 table never arms slot 1, so nothing else ever clears it\n");

	/* --- (b) the metal scenario, through the real run loop --- */
	mmu040_gen_init();
	mmu040_gen_map(VKCODE, PKCODE, 0);
	mmu040_gen_map(VVEC,   PVEC,   0);
	mmu040_gen_map(VSRC,   PSRC,   0);
	mmu040_gen_map(VKSTK,  PKSTK,  0);
	mmu040_gen_map(0x00000000u, 0x00000000u, 0);   /* kernel low memory is readable on metal
	                                                  (ITT/DTT), so a collapsed A0 reproduces
	                                                  the metal bytes instead of re-faulting */
	/* VDST deliberately NOT mapped yet -- the first store must fault. */

	/* guest low memory, exactly as the metal DM dump decoded it */
	hram_poke32(0x00000000u, 0x4AFC0000u);         /* ILLEGAL-at-null sentinel */
	hram_poke32(0x00000004u, 0x080007F0u);         /* pseudo-ExecBase pointer  */
	for (uae_u32 i = 8; i < NBYTES; i += 4)
		hram_poke32(i, 0x00F80A00u | i);           /* KS ROM vector-stub family */

	/* the payload: long 0 is the metal icode's first long, then a recognisable ramp */
	hram_poke32(PSRC + 0, 0x4FFB0170u);
	for (uae_u32 i = 4; i < NBYTES; i += 4)
		hram_poke32(PSRC + i, 0xC0DE0000u | i);

	/* the copy: DFC prologue + the lcolloop body, instruction for instruction */
	hram_poke16(PKCODE + 0x00, 0x7001);                        /* moveq  #1,d0        */
	hram_poke16(PKCODE + 0x02, 0x4E7B);                        /* movec  d0,dfc       */
	hram_poke16(PKCODE + 0x04, 0x0001);
	hram_poke16(PKCODE + 0x06, 0x207C);                        /* movea.l #VSRC,a0    */
	hram_poke32(PKCODE + 0x08, VSRC);
	hram_poke16(PKCODE + 0x0C, 0x227C);                        /* movea.l #VDST,a1    */
	hram_poke32(PKCODE + 0x0E, VDST);
	hram_poke16(PKCODE + 0x12, 0x203C);                        /* move.l #NBYTES-4,d0 */
	hram_poke32(PKCODE + 0x14, NBYTES - 4);
	hram_poke16(PKCODE + 0x18, 0x2218);                        /* lcolloop: move.l (a0)+,d1 */
	hram_poke16(PKCODE + 0x1A, 0x0E99);                        /* moves.l d1,(a1)+    */
	hram_poke16(PKCODE + 0x1C, 0x1800);
	hram_poke16(PKCODE + 0x1E, 0x5980);                        /* subq.l #4,d0        */
	hram_poke16(PKCODE + 0x20, 0x6CF6);                        /* bge.s  lcolloop     */
	hram_poke16(PKCODE + 0x22, 0x4E71);                        /* DONE: nop           */

	/* vector 2 = the kernel's write-back handler, modelled on AMIX src/wb040.s:
	   replay WB3 with `moves.l WB3D,(WB3A)` (DFC is still 1), consume WB3S, rte. */
	hram_poke16(PKCODE + 0x100, 0x246F);                       /* movea.l (0x18,a7),a2 */
	hram_poke16(PKCODE + 0x102, 0x0018);
	hram_poke16(PKCODE + 0x104, 0x242F);                       /* move.l  (0x1C,a7),d2 */
	hram_poke16(PKCODE + 0x106, 0x001C);
	hram_poke16(PKCODE + 0x108, 0x0E92);                       /* moves.l d2,(a2)      */
	hram_poke16(PKCODE + 0x10A, 0x2800);
	hram_poke16(PKCODE + 0x10C, 0x426F);                       /* clr.w   (0x0E,a7)    */
	hram_poke16(PKCODE + 0x10E, 0x000E);
	hram_poke16(PKCODE + 0x110, 0x4E73);                       /* rte                  */
	hram_poke32(PVEC | 8, HANDLER);

	cpu_set_context(VKCODE);
	regs.vbr = VVEC;
	regs.s = 1; MakeSR();
	m68k_areg(regs, 7) = VKSTK | 0xF00;

	regs.spcflags = Z_SPCFLAG_MODE_CHANGE;   /* -> one instruction per run-loop call */
	bool sawfault = false, mapped = false;
	int guard = 400;
	while (guard-- > 0) {
		uae_u32 pc = m68k_getpc();
		if (pc == DONE) break;
		if (pc >= HANDLER && pc <= HANDLER + 0x08) {
			sawfault = true;
			if (!mapped) {                        /* the kernel resolves the fault */
				mmu040_gen_map(VDST, PDST, 0);
				mapped = true;
			}
		}
		m68k_run_mmu040();
	}
	regs.spcflags = 0;

	CHECK(sawfault, "the first store to the unmapped destination page faulted\n");
	CHECK(guard > 0, "the copy loop terminated within the step budget\n");
	CHECK_EQ32(m68k_getpc(), DONE, "execution reached the instruction after the loop");

	CHECK_EQ32(m68k_areg(regs, 0), VSRC + NBYTES,
	           "A0 (the copy SOURCE) survived the fault -- it is not 0");
	CHECK(m68k_areg(regs, 0) != 0, "A0 is not the mmufixup[1] cold value 0\n");
	CHECK_EQ32(m68k_areg(regs, 1), VDST + NBYTES, "A1 (the destination) advanced by the full length");
	CHECK_EQ32(m68k_dreg(regs, 0), (uae_u32)-4, "the loop counter ran out exactly once");

	CHECK_EQ32(hram_peek32(PDST + 0), 0x4FFB0170u,
	           "destination long 0 is correct (the WB3 replay of the faulted store)");
	CHECK(hram_peek32(PDST + 4) != 0x4AFC0000u,
	      "destination long 1 is NOT the guest-0 sentinel 0x4AFC0000 (the metal poison)\n");
	{
		uae_u32 bad = 0;
		for (uae_u32 i = 0; i < NBYTES; i += 4)
			if (hram_peek32(PDST + i) != hram_peek32(PSRC + i)) bad++;
		CHECK_EQ32(bad, 0, "every longword of the copy matches the source");
	}

	mmu040_off();
	cpu_bringup(68030);
}

/* ==================================================================
 * STOP must advance the pc, on EVERY CPU table this firmware can select.
 *
 * A 68k STOP is `4E72 <sr>`: the CPU halts, and when an interrupt resumes it the
 * NEXT instruction runs. Every table in this tree ends STOP with
 * m68k_incpc(4)/m68k_incpci(4) -- except the generated 68040 MMU table, which
 * came from a newer WinUAE gencpu whose STOP is re-entrant (pc deliberately left
 * on the instruction, upstream's run loop advancing it when the stop ends). This
 * tree's run loop does not do that half, so the pc stayed pinned on the STOP:
 * interrupts were still taken, but every rte re-stopped and the instruction after
 * STOP never ran. On metal that is AmigaOS exec's idle loop never reaching the
 * `bra` after its `stop #$2000` -- no task ever rescheduled, grey screen forever.
 *
 * Host-checkable, and cheaply: execute a real STOP through each dispatch table and
 * assert the pc moved. Written as a loop over the tables rather than a one-off so
 * a table added later is covered by construction.
 * ================================================================== */
static void stop_check_one(const char *what, int cpu_model, int mmu_model)
{
	const uae_u32 VCODE = 0x00002000;
	harness_mem_reset();
	cpu_bringup(cpu_model);
	currprefs.cpu_model = cpu_model;
	currprefs.mmu_model = mmu_model;
	changed_prefs = currprefs;
	init_m68k();
	build_cpufunctbl();
	harness_set_x_funcs();
	regs.mmu_enabled = 0;              /* MMU off: this is about the pc, not translation */

	cpu_set_context(VCODE);
	regs.s = 1;
	regs.stopped = 0;
	regs.spcflags = 0;
	hram_poke16(VCODE + 0, 0x4e72);    /* STOP #$2000 */
	hram_poke16(VCODE + 2, 0x2000);
	hram_poke16(VCODE + 4, 0x4e71);    /* nop -- the instruction that must be reachable */

	bool faulted = false;
	TRY(p) {
		regs.instruction_pc = m68k_getpc();
		mmu030_state[0] = mmu030_state[1] = mmu030_state[2] = 0;
		mmu030_opcode = -1;
		regs.opcode = x_prefetch(0);
		mmu030_opcode = regs.irc = regs.opcode;
		mmu030_idx = 0; mmu030_idx_done = 0; mmu030_retry = false;
		(*cpufunctbl[regs.opcode])(regs.opcode);
	} CATCH(p) { faulted = true; (void)p; } ENDTRY

	if (faulted) {
		g_fail++;
		printf("  FAIL: %s: STOP faulted\n", what);
		return;
	}
	if (m68k_getpc() == VCODE) {
		g_fail++;
		printf("  FAIL: %s: STOP left the pc ON the instruction (%08x). An rte would\n"
		       "        re-stop forever and the next instruction would never run.\n",
		       what, (unsigned)m68k_getpc());
	} else {
		g_pass++;
	}
	CHECK_EQ32(m68k_getpc(), VCODE + 4, what);
	CHECK(regs.stopped != 0, "%s: regs.stopped set\n", what);
	CHECK_EQ32(regs.intmask, 0, "STOP #$2000 loaded intmask 0 from the operand");
}

static void test_stop_advances_pc_all_tables(void)
{
	printf("[test] STOP advances the pc on every selectable CPU table\n");
	stop_check_one("68030 non-MMU table",  68030, 0);
	stop_check_one("68040 non-MMU table",  68040, 0);
	stop_check_one("68030 MMU table (cpuemu_32)", 68030, 68030);
	stop_check_one("68040 MMU table (cpuemu_31)", 68040, 68040);
	currprefs.mmu_model = 0;
	cpu_bringup(68030);
}

/* ==================================================================
 * AMIX 040 latch (cpummu.cpp mmu_set_tc). amix_mmu_on says "the AMIX kernel is
 * up" and gates the emulated A3000 SCSI's timing regime; amix_kimage_030 says
 * "the guest is the 68030 kernel image" and gates the hard-coded 0x07xxxxxx
 * reads. The 040 latch must set the first and never the second.
 *
 * The harness links neither a3000_scsi.cpp nor its timing model, so this pins
 * the LATCH ONLY -- that the flags move exactly when they should. The pacing
 * behaviour they gate (set_status/wd_check_interrupt/pump suppression) is not
 * modelled here and is not asserted.
 * ================================================================== */
extern "C" {
	extern volatile int a3000_amix_mode;
	extern volatile int amix_mmu_on;
	extern volatile int amix_kimage_030;
}

static void test_amix_mmu040_latch(void)
{
	printf("[test] AMIX 040 latch: amix_mmu_on on the TC enable, amix_kimage_030 left alone\n");

	/* amix_mode OFF: a plain AmigaOS 040 must not arm the AMIX timing regime. */
	a3000_amix_mode = 0; amix_mmu_on = 0; amix_kimage_030 = 0;
	mmu040_pt_init();                                  /* enables the 040 MMU */
	CHECK_EQ32(amix_mmu_on, 0, "no latch without amix_mode");

	/* amix_mode ON + the TC E-bit: latch. */
	a3000_amix_mode = 1; amix_mmu_on = 0;
	mmu040_pt_init();
	CHECK_EQ32(amix_mmu_on, 1, "040 MMU enable under amix_mode latches amix_mmu_on");
	CHECK_EQ32(amix_kimage_030, 0, "the 040 latch does NOT arm the 68030-image reads");

	/* E-bit clear must not latch. */
	amix_mmu_on = 0;
	mmu_set_tc(0x0000);
	CHECK_EQ32(amix_mmu_on, 0, "TC with E clear does not latch");

	/* Wrong model must not latch here (the 030 latch lives in cpummu030.cpp). */
	amix_mmu_on = 0;
	currprefs.mmu_model = 68030;
	mmu_set_tc(0x8000);
	CHECK_EQ32(amix_mmu_on, 0, "mmu_set_tc does not latch for mmu_model 68030");
	CHECK_EQ32(amix_kimage_030, 0, "amix_kimage_030 still untouched");

	a3000_amix_mode = 0; amix_mmu_on = 0;              /* leave the globals inert */
	currprefs.mmu_model = 0;
	cpu_bringup(68030);
}

/* ==================================================================
 * MMU030 demand-paging END TO END (the path AMIX actually depends on).
 * Earlier MMU tests stop at the THROW; this one drives the WHOLE loop:
 *   fault -> (run-loop CATCH) Exception(2) builds the 030 bus-error frame
 *         -> "OS" maps the page -> m68k_do_rte_mmu030() -> instruction resumes.
 * It fails unless the stacked SSW carries MMU030_SSW_DF (so RTE re-performs the
 * faulted access) AND the frame stores the faulting opcode (so the resume
 * re-dispatches the right instruction). Both were broken before the fix.
 *
 * Maps everything inside the fixture's TableA[1] window (0x00400000+):
 *   VCODE/VDATA/VSTK/VVEC virtual pages -> distinct physical frames.
 * ================================================================== */
static void test_mmu030_demand_page_read(void)
{
	printf("[test] MMU030 demand-paged READ fault -> Exception -> RTE -> resume (decision #6 test 6)\n");
	const uae_u32 VCODE = 0x00408000, PCODE = 0x0C000000;
	const uae_u32 VDATA = 0x00409000, PDATA = 0x0D000000;
	const uae_u32 VSTK  = 0x0040A000, PSTK  = 0x0E000000;
	const uae_u32 VVEC  = 0x0040B000, PVEC  = 0x0F000000;
	const uae_u32 VALUE = 0xC0DEF00D;

	mmu_pt_init();                                  /* mmu_model=68030, TC on, regs.s=1 */
	currprefs.cpu_model = 68030;
	mmu_map_page((VCODE >> 12) & 0x3FF, PCODE);     /* code page mapped */
	mmu_map_page((VSTK  >> 12) & 0x3FF, PSTK);      /* supervisor stack mapped */
	mmu_map_page((VVEC  >> 12) & 0x3FF, PVEC);      /* vector table mapped */
	mmu030_flush_atc_all();                         /* VDATA deliberately left UNMAPPED */

	hram_poke16(PCODE | (VCODE & 0xFFF), 0x2210);   /* move.l (a0),d1 */
	hram_poke32(PVEC  | 8, VCODE);                  /* bus-error vector (2) -> even handler addr */

	init_m68k();
	build_cpufunctbl();                             /* mode 4 -> op_smalltbl_32_ff */
	harness_set_x_funcs();                          /* MMU arm: x_prefetch/x_get/x_put -> mmu030 */

	regs.s = 1;
	regs.vbr = VVEC;
	m68k_dreg(regs, 1) = 0;
	m68k_areg(regs, 0) = VDATA;                     /* load source (still unmapped) */
	m68k_areg(regs, 7) = VSTK | 0xFF0;              /* supervisor stack (mapped) */
	m68k_setpc(VCODE);

	struct flag_struct f;
	bool faulted = false;
	regs.instruction_pc = m68k_getpc();
	f = regs.ccrflags;
	mmu030_state[0] = mmu030_state[1] = mmu030_state[2] = 0;
	mmu030_opcode = -1;
	TRY(prb) {
		regs.opcode = x_prefetch(0);                /* fetch 0x2210 from mapped code page */
		mmu030_opcode = regs.opcode;
		mmu030_idx_done = 0;
		regs.opcode = regs.irc = mmu030_opcode;
		mmu030_idx = 0; mmu030_retry = false;
		(*cpufunctbl[regs.opcode])(regs.opcode);    /* read VDATA (unmapped) -> THROW(2) */
	} CATCH(prb) {
		faulted = true;
		/* exactly m68k_run_mmu030's CATCH for a non-last-write data fault */
		regs.ccrflags = f;
		cpu_restore_fixup();
		m68k_setpci(regs.instruction_pc);
		Exception(prb);                             /* build the 030 bus-error stack frame */
	} ENDTRY

	CHECK(faulted, "unmapped load raised a bus fault (THROW)\n");
	CHECK_EQ32(m68k_dreg(regs, 1), 0, "d1 not written before the page is mapped");

	/* "OS page-fault handler": map the page, deposit the value, flush the stale ATC */
	mmu_map_page((VDATA >> 12) & 0x3FF, PDATA);
	mmu030_flush_atc_all();
	hram_poke32(PDATA | (VDATA & 0xFFF), VALUE);

	/* RTE: restore frame state, re-perform the faulted access, arm mmu030_retry */
	m68k_do_rte_mmu030(m68k_areg(regs, 7));

	/* resume: re-dispatch the faulted instruction, as m68k_run_mmu030 does on retry */
	int guard = 16; bool rfaulted = false;
	TRY(prb2) {
		while (mmu030_retry && guard-- > 0) {
			regs.opcode = regs.irc = mmu030_opcode;
			mmu030_idx = 0; mmu030_retry = false;
			(*cpufunctbl[regs.opcode])(regs.opcode);
		}
	} CATCH(prb2) { rfaulted = true; (void)prb2; } ENDTRY

	CHECK(!rfaulted, "resumed instruction did not re-fault\n");
	CHECK_EQ32(m68k_dreg(regs, 1), VALUE, "demand-paged load resumed and wrote d1");
	CHECK_EQ32(m68k_getpc(), VCODE + 2, "pc advanced past the resumed instruction");

	currprefs.mmu_model = 0; mmu030_reset(1);
	cpu_bringup(68030);
}

/* Companion to the read test: a demand-paged WRITE fault. Exercises the
 * last-write (frame A) path + the data-output-buffer fix (regs.wb3_data) — RTE
 * must re-issue the REAL stored value, not 0. */
static void test_mmu030_demand_page_write(void)
{
	printf("[test] MMU030 demand-paged WRITE fault -> Exception -> RTE -> resume (decision #6 test 7)\n");
	const uae_u32 VCODE = 0x00408000, PCODE = 0x0C000000;
	const uae_u32 VDATA = 0x00409000, PDATA = 0x0D000000;
	const uae_u32 VSTK  = 0x0040A000, PSTK  = 0x0E000000;
	const uae_u32 VVEC  = 0x0040B000, PVEC  = 0x0F000000;
	const uae_u32 VALUE = 0xFEEDFACE;

	mmu_pt_init();
	currprefs.cpu_model = 68030;
	mmu_map_page((VCODE >> 12) & 0x3FF, PCODE);
	mmu_map_page((VSTK  >> 12) & 0x3FF, PSTK);
	mmu_map_page((VVEC  >> 12) & 0x3FF, PVEC);
	mmu030_flush_atc_all();                         /* VDATA left UNMAPPED */

	hram_poke16(PCODE | (VCODE & 0xFFF), 0x2081);   /* move.l d1,(a0) */
	hram_poke32(PVEC  | 8, VCODE);

	init_m68k();
	build_cpufunctbl();
	harness_set_x_funcs();

	regs.s = 1;
	regs.vbr = VVEC;
	m68k_dreg(regs, 1) = VALUE;                      /* value to store */
	m68k_areg(regs, 0) = VDATA;                      /* dest (unmapped) */
	m68k_areg(regs, 7) = VSTK | 0xFF0;
	m68k_setpc(VCODE);

	struct flag_struct f;
	bool faulted = false;
	regs.instruction_pc = m68k_getpc();
	f = regs.ccrflags;
	mmu030_state[0] = mmu030_state[1] = mmu030_state[2] = 0;
	mmu030_opcode = -1;
	TRY(prb) {
		regs.opcode = x_prefetch(0);
		mmu030_opcode = regs.opcode;
		mmu030_idx_done = 0;
		regs.opcode = regs.irc = mmu030_opcode;
		mmu030_idx = 0; mmu030_retry = false;
		(*cpufunctbl[regs.opcode])(regs.opcode);    /* store to VDATA (unmapped) -> THROW(2) */
	} CATCH(prb) {
		faulted = true;
		/* full m68k_run_mmu030 CATCH branch logic (this is the last-write case) */
		if (mmu030_opcode == -1) {
			mmufixup[0].reg = -1; mmufixup[1].reg = -1;
		} else if (mmu030_state[1] & MMU030_STATEFLAG1_LASTWRITE) {
			mmufixup[0].reg = -1; mmufixup[1].reg = -1;
		} else {
			regs.ccrflags = f; cpu_restore_fixup();
		}
		m68k_setpci(regs.instruction_pc);
		bool dbl = false;
		TRY(prb2) { Exception(prb); } CATCH(prb2) { dbl = true; (void)prb2; } ENDTRY
		CHECK(!dbl, "Exception(2) for write fault did not double-fault\n");
	} ENDTRY

	CHECK(faulted, "unmapped store raised a bus fault (THROW)\n");
	CHECK_EQ32(hram_peek32(PDATA | (VDATA & 0xFFF)), 0, "nothing written before the page is mapped");

	mmu_map_page((VDATA >> 12) & 0x3FF, PDATA);
	mmu030_flush_atc_all();

	m68k_do_rte_mmu030(m68k_areg(regs, 7));

	int guard = 16; bool rfaulted = false;
	TRY(prb2) {
		while (mmu030_retry && guard-- > 0) {
			regs.opcode = regs.irc = mmu030_opcode;
			mmu030_idx = 0; mmu030_retry = false;
			(*cpufunctbl[regs.opcode])(regs.opcode);
		}
	} CATCH(prb2) { rfaulted = true; (void)prb2; } ENDTRY

	CHECK(!rfaulted, "resumed store did not re-fault\n");
	CHECK_EQ32(hram_peek32(PDATA | (VDATA & 0xFFF)), VALUE, "demand-paged store resumed and wrote memory");

	currprefs.mmu_model = 0; mmu030_reset(1);
	cpu_bringup(68030);
}

/* ==================================================================
 * MULTI-FAULT CONTINUATION (the residual [WILD] bug, 2026-06-26).
 * One instruction (MOVEM.L (a0)+,d0-d7) whose read spans TWO unmapped pages:
 *   fault A -> Exception frame -> "OS" maps A -> m68k_do_rte_mmu030() -> resume
 *   re-runs the MOVEM, which now faults on page B (the continuation).
 * Faithfully mirrors m68k_run_mmu030: the inner-loop 7ff5774 re-snapshot
 * (regs.instruction_pc = mmu030_insn_start_pc = m68k_getpc(), newcpu.cpp:4432)
 * runs between the RTE and the re-dispatch. The rebuilt frame B must carry the
 * INSTRUCTION pc (VCODE) so the next RTE resumes there -- not a skewed pc.
 * The board [WILD] shows frame B carrying a wild pc -> user-mode wild jump.
 * ================================================================== */
static void test_mmu030_multifault_continuation(void)
{
	printf("[test] MMU030 multi-fault continuation -> 2nd-page frame carries the instruction pc\n");
	const uae_u32 VCODE = 0x00408000, PCODE = 0x0C000000;
	const uae_u32 VD1   = 0x00409000, PD1   = 0x0D000000;   /* MOVEM source page A */
	const uae_u32 VD2   = 0x0040A000, PD2   = 0x0D100000;   /* MOVEM source page B */
	const uae_u32 VSTK  = 0x0040E000, PSTK  = 0x0E000000;
	const uae_u32 VVEC  = 0x0040B000, PVEC  = 0x0F000000;

	mmu_pt_init();
	currprefs.cpu_model = 68030;
	mmu_map_page((VCODE >> 12) & 0x3FF, PCODE);
	mmu_map_page((VSTK  >> 12) & 0x3FF, PSTK);
	mmu_map_page((VVEC  >> 12) & 0x3FF, PVEC);
	mmu030_flush_atc_all();                          /* VD1, VD2 left UNMAPPED */

	hram_poke16(PCODE | (VCODE & 0xFFF),       0x4CD8);   /* MOVEM.L (a0)+,<list> */
	hram_poke16(PCODE | ((VCODE + 2) & 0xFFF), 0x00FF);   /* mask = d0-d7 (8 longs) */
	hram_poke32(PVEC  | 8, VCODE);                        /* bus-error vector (2) */

	init_m68k();
	build_cpufunctbl();
	harness_set_x_funcs();

	regs.s = 1;
	regs.vbr = VVEC;
	m68k_areg(regs, 0) = VD1 | 0xFF0;                /* 32-byte read spans VD1[FF0..FFF]+VD2[000..00F] */
	m68k_areg(regs, 7) = VSTK | 0xFF0;
	m68k_setpc(VCODE);

	struct flag_struct f;
	/* outer snapshot (m68k_run_mmu030:4375-4376) */
	regs.instruction_pc = m68k_getpc();
	uae_u32 snap = regs.instruction_pc;       /* mirrors newcpu.cpp's static mmu030_insn_start_pc */
	f = regs.ccrflags;
	mmu030_state[0] = mmu030_state[1] = mmu030_state[2] = 0;
	mmu030_opcode = -1;

	/* ---- first execution -> fault on page A ---- */
	bool f1 = false;
	TRY(p1) {
		regs.opcode = x_prefetch(0);
		mmu030_opcode = regs.opcode;
		mmu030_idx_done = 0;
		regs.opcode = regs.irc = mmu030_opcode;
		mmu030_idx = 0; mmu030_retry = false;
		(*cpufunctbl[regs.opcode])(regs.opcode);
	} CATCH(p1) {
		f1 = true;                                   /* run-loop CATCH: frame-$B read fault */
		regs.ccrflags = f; cpu_restore_fixup();
		regs.instruction_pc = snap;
		m68k_setpci(regs.instruction_pc);
		Exception(p1);
	} ENDTRY
	CHECK(f1, "MOVEM faulted on page A\n");

	/* ---- OS: map page A, RTE ---- */
	mmu_map_page((VD1 >> 12) & 0x3FF, PD1);
	mmu030_flush_atc_all();
	m68k_do_rte_mmu030(m68k_areg(regs, 7));

	/* ---- inner-loop 7ff5774 re-snapshot (m68k_run_mmu030:4432) -- THE SUSPECT ---- */
	uae_u32 resnap = m68k_getpc();
	printf("       re-snapshot: m68k_getpc()=%08X  regs.pc(getpci)=%08X  expected=%08X\n",
	       (unsigned)resnap, (unsigned)m68k_getpci(), (unsigned)VCODE);
	CHECK_EQ32(resnap, VCODE, "re-snapshot pc == instruction pc (no prefetch skew)");
	regs.instruction_pc = snap = resnap;

	/* ---- re-dispatch -> fault on page B (the continuation) ---- */
	bool f2 = false;
	TRY(p2) {
		int guard = 8;
		while (mmu030_retry && guard-- > 0) {
			regs.opcode = regs.irc = mmu030_opcode;
			mmu030_idx = 0; mmu030_retry = false;
			(*cpufunctbl[regs.opcode])(regs.opcode);
		}
	} CATCH(p2) {
		f2 = true;
		regs.ccrflags = f; cpu_restore_fixup();
		regs.instruction_pc = snap;
		m68k_setpci(regs.instruction_pc);
		Exception(p2);
	} ENDTRY
	CHECK(f2, "resumed MOVEM faulted again on page B (continuation)\n");

	/* ---- map page B, RTE from frame B: must resume at the instruction pc ---- */
	mmu_map_page((VD2 >> 12) & 0x3FF, PD2);
	mmu030_flush_atc_all();
	m68k_do_rte_mmu030(m68k_areg(regs, 7));
	uae_u32 resume2 = m68k_getpci();
	printf("       RTE from frame B resumes at pc=%08X  expected=%08X\n",
	       (unsigned)resume2, (unsigned)VCODE);
	CHECK_EQ32(resume2, VCODE, "RTE from the 2nd-page frame resumes at the instruction pc (not a wild pc)");

	currprefs.mmu_model = 0; mmu030_reset(1);
	cpu_bringup(68030);
}

/* ================================================================== */
/* ==================================================================
 * CYBERSTORM C3: the run-loop overhead switches.
 *
 * What has to be true, and what a metal calibration session is entitled to assume:
 *
 *   1. DEFAULTS ARE TODAY. With the mailbox at its power-on state the accumulator
 *      counts every retired instruction and the probes are armed -- i.e. absent
 *      config keys reproduce the loop the firmware has always run.
 *   2. OFF IS REACHABLE, AND IT IS ACTUALLY OFF. perf_stats 0 must stop the counter
 *      dead: not slow it, stop it. That is the whole point -- a knob that only
 *      reduces the cost cannot separate "overhead reclaimed" from "noise".
 *   3. THE KNOBS STILL REACH THE LOOP WITH perf_stats OFF. z3660_perf_tick() is
 *      where SERV was picked up, and it no longer runs; if the resync did not move
 *      somewhere free, turning perf_stats off would be a one-way door.
 *   4. service_cadence REACHES THE 040 LOOP and is clamped (dispatch item 2c).
 *
 * Driven through the REAL m68k_run_mmu040(), one instruction per call via
 * SPCFLAG_MODE_CHANGE, on a plain MMU-off machine: the knobs are orthogonal to
 * translation, and this keeps the test about the loop tail and nothing else.
 * ================================================================== */
extern "C" void    z3660_knobs_sync(void);
extern "C" void    z3660_knobs_get(int *perf_on, int *probes_on, int *cadence);
extern "C" uae_u64 z3660_perf_count_get(void);

/* THE SAME SOURCE PROVES BOTH BUILD VARIANTS.
 *
 * The lean (production) firmware is this tree compiled with -DZ3660_EMU_PROBES=0
 * -DZ3660_EMU_PERF=0, and its correct behaviour is NOT the default build's: an
 * instrument that is compiled out cannot be switched on, and the accumulator cannot
 * count. Asserting the default contract against a lean build would fail for a good
 * reason, and asserting nothing would let the lean build ship untested. So the
 * expectations below branch on the same header the run loop compiled against, and the
 * suite is run twice -- `make run` and `make run-lean`. */
#include "runloop_build.h"

/* Run `n` instructions through the genuine 040 run loop. Returns how far the perf
 * accumulator moved across them. */
static uae_u64 run040_steps(uae_u32 start, int n)
{
	cpu_set_context(start);
	regs.spcflags = Z_SPCFLAG_MODE_CHANGE;   /* -> one instruction per run-loop call */
	uae_u64 before = z3660_perf_count_get();
	for (int i = 0; i < n; i++)
		m68k_run_mmu040();
	regs.spcflags = 0;
	return z3660_perf_count_get() - before;
}

static void test_runloop_overhead_switches(void)
{
	printf("[test] run-loop overhead switches: perf_stats / emu_probes / service_cadence\n");

	cpu_bringup(68040);
	currprefs.mmu_model = 0;                 /* MMU off: the tail is what is under test */
	const uae_u32 CODE = 0x00010000u;
	for (uae_u32 i = 0; i < 64; i += 2)
		hram_poke16(CODE + i, 0x4E71);       /* a field of NOPs */

	/* ---- 1. defaults reproduce today ---- */
	harness_shared_defaults();
	z3660_knobs_sync();
	{
		int perf = -1, probes = -1, cad = -1;
		z3660_knobs_get(&perf, &probes, &cad);
		CHECK_EQ32(perf,   Z3660_EMU_PERF   ? 1 : 0, "power-on default: perf_stats is ON (0 if compiled out)");
		CHECK_EQ32(probes, Z3660_EMU_PROBES ? 1 : 0, "power-on default: emu_probes is ON (0 if compiled out)");
		CHECK_EQ32(cad,    1, "power-on default: service_cadence is 1 (poll every instruction)");
	}
#if Z3660_EMU_PERF
	CHECK_EQ32((uae_u32)run040_steps(CODE, 8), 8,
	           "perf_stats ON: the accumulator counts every retired instruction");

	/* ---- 2. off is reachable and is a hard stop ---- */
	shared->perf_stats = 0;
	z3660_knobs_sync();
	{
		int perf = -1; z3660_knobs_get(&perf, NULL, NULL);
		CHECK_EQ32(perf, 0, "perf_stats 0 reaches the run loop");
	}
	CHECK_EQ32((uae_u32)run040_steps(CODE, 8), 0,
	           "perf_stats OFF: the accumulator does not move at all");

	/* ---- 3. the knobs still reach the loop while perf_stats is off ----
	 * do_specialties() is the resync point, and the loop calls it every step here
	 * (SPCFLAG_MODE_CHANGE), so a knob written to the mailbox must take effect
	 * WITHOUT any explicit z3660_knobs_sync() call by the test. The poll is
	 * rate-divided, so give it more steps than the divisor. */
	shared->perf_stats = 1;
	run040_steps(CODE, 200);                 /* no manual sync: do_specialties must do it */
	{
		int perf = -1; z3660_knobs_get(&perf, NULL, NULL);
		CHECK_EQ32(perf, 1,
		           "perf_stats can be turned back ON with the accumulator stopped "
		           "(resync is not hostage to perf_tick)");
	}
#else
	/* LEAN: the accumulator is not there to move, and the switch cannot resurrect it.
	 * Assert BOTH -- a lean build whose counter still ticks is not lean, and a lean
	 * build that reports perf_stats ON would lie to a metal session. */
	CHECK_EQ32((uae_u32)run040_steps(CODE, 8), 0,
	           "LEAN: no accumulator exists, so nothing counts");
	shared->perf_stats = 1;
	z3660_knobs_sync();
	{
		int perf = -1; z3660_knobs_get(&perf, NULL, NULL);
		CHECK_EQ32(perf, 0, "LEAN: perf_stats cannot be switched on -- it is compiled out");
	}
	CHECK_EQ32((uae_u32)run040_steps(CODE, 8), 0,
	           "LEAN: and asking for it on still counts nothing");
#endif

	/* ---- 4. emu_probes: a live two-way switch by default, absent when compiled out.
	 * The invariant that holds in BOTH variants is the load-bearing one: the probes
	 * change no emulation, so the same instructions retire either way. */
	shared->emu_probes = 0;
	z3660_knobs_sync();
	{
		int probes = -1; z3660_knobs_get(NULL, &probes, NULL);
		CHECK_EQ32(probes, 0, "emu_probes 0 reaches the run loop");
	}
	{	/* pc-based, not accumulator-based: the accumulator is one of the things that
		 * can be compiled away, and this assertion has to mean something in the lean
		 * build too -- it is the one that says the switch is a DIAGNOSTIC switch. */
		cpu_set_context(CODE);
		regs.spcflags = Z_SPCFLAG_MODE_CHANGE;
		for (int i = 0; i < 4; i++)
			m68k_run_mmu040();
		regs.spcflags = 0;
		CHECK_EQ32((uae_u32)m68k_getpc(), CODE + 8,
		           "probes OFF changes no emulation: four NOPs still retire (pc advanced by 8)");
	}
	shared->emu_probes = 1;
	z3660_knobs_sync();
	{
		int probes = -1; z3660_knobs_get(NULL, &probes, NULL);
		CHECK_EQ32(probes, Z3660_EMU_PROBES ? 1 : 0,
		           Z3660_EMU_PROBES
		             ? "emu_probes is a two-way switch -- the instruments come back"
		             : "LEAN: emu_probes cannot be switched on -- it is compiled out");
	}

	/* ---- 5. service_cadence reaches the 040 loop, and is clamped ---- */
	shared->service_cadence = 8;
	z3660_knobs_sync();
	{
		int cad = -1; z3660_knobs_get(NULL, NULL, &cad);
		CHECK_EQ32(cad, 8, "service_cadence reaches the 040 loop (dispatch item 2c)");
	}
	shared->service_cadence = 0;             /* nonsense value */
	z3660_knobs_sync();
	{
		int cad = -1; z3660_knobs_get(NULL, NULL, &cad);
		CHECK_EQ32(cad, 1, "service_cadence 0 is clamped to 1, never used as a divisor");
	}
	shared->service_cadence = 0xFFFFFFFFu;   /* -1 through the int cast */
	z3660_knobs_sync();
	{
		int cad = -1; z3660_knobs_get(NULL, NULL, &cad);
		CHECK_EQ32(cad, 1, "a negative service_cadence is clamped to 1");
	}

	/* Leave the fixture exactly as the firmware powers up. */
	harness_shared_defaults();
	z3660_knobs_sync();
	currprefs.mmu_model = 0;
	cpu_bringup(68030);
}

/* ==================================================================
 * The [PERF] line's clock (C2 riding item a).
 *
 * z3660_perf_tick() used COUNTS_PER_SECOND twice -- as the ~1 Hz window length and as
 * the kIPS numerator -- and COUNTS_PER_SECOND is a COMPILE-TIME BSP constant frozen at
 * the 666.67 MHz default while ps7_init_custom() retunes the PLL from the config. On the
 * 1100 MHz bench board every [PERF] figure was therefore low by exactly 1.65x, and the
 * C2 map had to ship a CLOCK-CORRECTION.txt to undo it before the map could be read.
 *
 * The same defect was fixed in the profiler by publishing the retuned rate in
 * SHARED.arm_wall_hz and choosing between it and the constant with
 * z3660_prof_pick_wall_hz(). This asserts the [PERF] path now walks that same road --
 * both branches of it, because the fallback is what a board with an older core0 gets and
 * a fallback nobody exercises is a fallback nobody has.
 *
 * A scale error is invisible to any test that only checks a number is "reasonable", which
 * is why the assertion is on the RATE THE CODE DIVIDES BY rather than on a printed kIPS.
 * ================================================================== */
extern "C" void z3660_perf_clock_get(uae_u32 *wall_hz, int *clk_cfg);
#include "xtime_l.h"     /* COUNTS_PER_SECOND -- the constant that WAS the bug */

static void test_perf_line_clock(void)
{
	printf("[test] the [PERF] kIPS line divides by the RETUNED ARM clock, not the BSP constant\n");

	const uae_u32 SAVED = (uae_u32)shared->arm_wall_hz;

	/* (a) core0 published a retuned rate -- the 1100 MHz bench board's global timer. */
	shared->arm_wall_hz = 550000000u;
	{
		uae_u32 hz = 0; int cfg = -1;
		z3660_perf_clock_get(&hz, &cfg);
#if Z3660_EMU_PERF
		CHECK_EQ32(hz, 550000000u,
		           "[PERF] measures against SHARED.arm_wall_hz when core0 published one");
		CHECK_EQ32((uae_u32)cfg, 1, "...and says so: clk=cfg");
		CHECK(hz != (uae_u32)COUNTS_PER_SECOND,
		      "...and it is NOT the frozen BSP constant -- that was the 1.65x\n");
#else
		CHECK_EQ32(hz, 0, "LEAN build: no [PERF] line, so no rate to report");
		CHECK_EQ32((uae_u32)cfg, 0, "LEAN build: clk source reported as absent, not as 'bsp'");
#endif
	}

	/* (b) an older core0 that never wrote the field: the constant, and the caveat. */
	shared->arm_wall_hz = 0;
	{
		uae_u32 hz = 0; int cfg = -1;
		z3660_perf_clock_get(&hz, &cfg);
#if Z3660_EMU_PERF
		CHECK_EQ32(hz, (uae_u32)COUNTS_PER_SECOND,
		           "unpublished rate falls back to the BSP constant rather than to zero");
		CHECK_EQ32((uae_u32)cfg, 0, "...and the line is labelled bsp (UNCALIBRATED)");
#else
		CHECK_EQ32(hz, 0, "LEAN build: still nothing to report");
#endif
	}

	/* (c) a mailbox word that is not a clock rate. Below 1 MHz is 'absent', never
	 *     'plausible' -- a rate of 3 would make kIPS a divide-by-nothing. */
	shared->arm_wall_hz = 3u;
	{
		uae_u32 hz = 0; int cfg = -1;
		z3660_perf_clock_get(&hz, &cfg);
#if Z3660_EMU_PERF
		CHECK_EQ32(hz, (uae_u32)COUNTS_PER_SECOND,
		           "a sub-1 MHz mailbox word is 'absent', not a clock rate");
		CHECK_EQ32((uae_u32)cfg, 0, "...reported as bsp");
#else
		CHECK_EQ32(hz, 0, "LEAN build: nothing to report");
#endif
	}

	shared->arm_wall_hz = SAVED;
}

/* ==================================================================
 * Build-variant contract (CYBERSTORM C3 follow-up).
 *
 * runloop_build.h is the single copy of "which instruments is this binary carrying",
 * and it is read by the run loop, by this harness, and (as -D flags) by the Makefile.
 * The thing that must never drift is the DEFAULT: an unflagged build has to be the
 * firmware this fork has always shipped, instruments in and switchable. If someone
 * ever flips a default to make a benchmark look better, that is a silent loss of every
 * diagnostic on the card, and it should fail here rather than on the metal at 2 a.m.
 *
 * The lean arm asserts the mirror image, so `make run-lean` is not merely "it still
 * compiles": the production binary's contract is stated and checked.
 * ================================================================== */
static void test_runloop_build_variant(void)
{
	printf("[test] run-loop build variant: %s\n",
	       Z3660_RUNLOOP_BUILD_IS_LEAN ? "LEAN (diagnostics compiled out)"
	                                   : "DEFAULT (diagnostics compiled in)");

	/* The flags word is what a build script / a future serial dump would key off. */
	CHECK_EQ32((uae_u32)Z3660_RUNLOOP_BUILD_FLAGS,
	           (uae_u32)((Z3660_EMU_PROBES ? 1 : 0) | (Z3660_EMU_PERF ? 2 : 0)),
	           "build flags word agrees with the two -D switches that produced it");
	CHECK_EQ32((uae_u32)Z3660_RUNLOOP_BUILD_IS_LEAN,
	           (uae_u32)(!Z3660_EMU_PROBES && !Z3660_EMU_PERF),
	           "'lean' means BOTH instruments out, never just one");

#if !defined(Z3660_LEAN_EXPECTED)
	/* Unflagged build: this is the shipped diagnostic firmware. Both must be 1. */
	CHECK_EQ32((uae_u32)Z3660_EMU_PROBES, 1,
	           "DEFAULT build: emu_probes compiled IN (absent -D must not change the loop)");
	CHECK_EQ32((uae_u32)Z3660_EMU_PERF, 1,
	           "DEFAULT build: perf accumulator compiled IN (absent -D must not change the loop)");
	CHECK_EQ32((uae_u32)Z3660_RUNLOOP_BUILD_FLAGS, 3,
	           "DEFAULT build: flags == 3, the historical shape");
#else
	CHECK_EQ32((uae_u32)Z3660_EMU_PROBES, 0, "LEAN build: emu_probes compiled OUT");
	CHECK_EQ32((uae_u32)Z3660_EMU_PERF,   0, "LEAN build: perf accumulator compiled OUT");
	CHECK_EQ32((uae_u32)Z3660_RUNLOOP_BUILD_FLAGS, 0, "LEAN build: flags == 0");
#endif

	/* service_cadence is NOT an instrument. It survives every variant, because the
	 * A3000-SCSI interrupt-delay pump rides on the poll it gates (docs/AMIX.md) --
	 * compiling it away would change emulation, not just measurement. */
	shared->service_cadence = 4;
	z3660_knobs_sync();
	{
		int cad = -1; z3660_knobs_get(NULL, NULL, &cad);
		CHECK_EQ32(cad, 4, "service_cadence reaches the loop in EVERY build variant");
	}
	harness_shared_defaults();
	z3660_knobs_sync();
}

/* ==================================================================
 * Cross-core SHARED ABI guard.
 *
 * The beta-21 resync collided head-on with this struct once already: upstream
 * appended eight floppy fields at 0xFFFF009C..0xFFFF00B8, exactly where this fork's
 * debug_emu/amix_mode/service_cadence/perf_report sat, and a mis-merge would have
 * aliased service_cadence onto write_floppy_data with no diagnostic whatsoever
 * (docs/piscsi-service-path.md 5). The two copies of SHARED -- Z3660/src/main.h and
 * Z3660_emu/src/main.h -- are edited by hand and are only ever checked by the two
 * cores agreeing at runtime, on metal, silently.
 *
 * So pin the offsets. This is the copy core1 compiles; the numbers are the addresses
 * the comments in BOTH headers claim.
 * ================================================================== */
static void test_shared_mailbox_offsets(void)
{
	printf("[test] cross-core SHARED mailbox: the knob fields sit where both headers say\n");
	CHECK_EQ32((uae_u32)offsetof(SHARED, debug_emu),       0xBC, "debug_emu       @ 0xFFFF00BC");
	CHECK_EQ32((uae_u32)offsetof(SHARED, amix_mode),       0xC0, "amix_mode       @ 0xFFFF00C0");
	CHECK_EQ32((uae_u32)offsetof(SHARED, service_cadence), 0xC4, "service_cadence @ 0xFFFF00C4");
	CHECK_EQ32((uae_u32)offsetof(SHARED, perf_report),     0xC8, "perf_report     @ 0xFFFF00C8");
	CHECK_EQ32((uae_u32)offsetof(SHARED, amix_ram),        0xCC, "amix_ram        @ 0xFFFF00CC");
	CHECK_EQ32((uae_u32)offsetof(SHARED, perf_stats),      0xD0, "perf_stats      @ 0xFFFF00D0 (appended, never inserted)");
	CHECK_EQ32((uae_u32)offsetof(SHARED, emu_probes),      0xD4, "emu_probes      @ 0xFFFF00D4 (appended, never inserted)");
	CHECK_EQ32((uae_u32)offsetof(SHARED, prof_ctl),        0xD8, "prof_ctl        @ 0xFFFF00D8 (appended, never inserted)");
	CHECK_EQ32((uae_u32)offsetof(SHARED, prof_hz),         0xDC, "prof_hz         @ 0xFFFF00DC (appended, never inserted)");
	CHECK_EQ32((uae_u32)offsetof(SHARED, arm_wall_hz),     0xE0, "arm_wall_hz     @ 0xFFFF00E0 (appended, never inserted)");
	CHECK_EQ32((uae_u32)offsetof(SHARED, blk_chunk),       0xE4, "blk_chunk       @ 0xFFFF00E4 (appended, never inserted)");
	CHECK_EQ32((uae_u32)offsetof(SHARED, dopc_on),         0xE8, "dopc_on         @ 0xFFFF00E8 (appended, never inserted)");
	CHECK_EQ32((uae_u32)offsetof(SHARED, dfast_on),        0xEC, "dfast_on        @ 0xFFFF00EC (appended, never inserted)");
	CHECK_EQ32((uae_u32)offsetof(SHARED, warp_on),         0xF0, "warp_on         @ 0xFFFF00F0 (appended, never inserted)");
	CHECK_EQ32((uae_u32)sizeof(SHARED),                    0xF4, "SHARED is 0xF4 bytes -- new fields go on the END");

	/* C2 rung 2: the knob's RULE, not just its offset. UNSET must mean ON -- a core0
	 * built before this field existed publishes a zeroed struct, and reading that as
	 * "disarmed" would silently take the rung out of the shipped firmware while every
	 * offset assertion above still passed. Same trap z3660_dopc_resolve() documents. */
	CHECK_EQ32((uae_u32)z3660_dfast_resolve(0u),                    1u,
	           "an UNSET dfast_on means ON, not off");
	CHECK_EQ32((uae_u32)z3660_dfast_resolve(Z3660_DFAST_DEFAULT),   1u, "DEFAULT means ON");
	CHECK_EQ32((uae_u32)z3660_dfast_resolve(Z3660_DFAST_OFF),       0u, "the OFF sentinel means off");
	CHECK_EQ32((uae_u32)z3660_dfast_resolve(12345u),                1u,
	           "and anything else means ON -- only the sentinel disarms");
}

/* ==================================================================
 * CYBERSTORM C1 -- the interpreter profiler.
 *
 * Four things are worth testing here and they are not the same thing:
 *
 *   1. THE WIRE FORMAT, because a host-side symbolizer is written against z3660_prof.h
 *      once and then never rebuilt in step with the firmware. A silently-resized record
 *      or a renumbered bucket would not fail anywhere -- it would produce a plausible,
 *      wrong profile. Sizes, offsets, flag bits and ids are pinned here.
 *
 *   2. THE BUCKET ARITHMETIC, which is the reason the cycle source is injectable on the
 *      host at all. Against a real clock you can only observe that the numbers look
 *      sensible; against a clock you set by hand you can assert that 100 cycles inside a
 *      nested stage land in that stage and NOT in its parent, and that the buckets sum to
 *      the elapsed span exactly. Attribution that is never checked against known input is
 *      not measurement.
 *
 *   3. RING INTEGRITY under wrap -- including that `drops` counts what a dump cannot show
 *      and that WEIGHT saturates instead of overflowing into the flag bits underneath it.
 *
 *   4. THAT THE PROFILER CHANGES NO EMULATION. Its probes sit inside the read, write and
 *      fetch path of the interpreter -- not beside it. "The answers are still right" is
 *      therefore a claim about this build, and the whole MMU suite running green in the
 *      profiling variant is most of the evidence; this test adds the direct A/B.
 * ================================================================== */
/* ==================================================================
 * THE DUMP'S OWN FORMATTER RING -- A CAP THAT COULD ONLY BE BROKEN SILENTLY.
 *
 * z3660_prof_u64() formats into a 4-slot rotating static ring, so the fifth call in one
 * printf argument list overwrites the slot the first is still pointing at. Every argument
 * is evaluated before printf runs, so the failure is not a crash or a garbled field: the
 * line prints a DIFFERENT, plausible number where the first one belonged.
 *
 * It happened. The rung-1b way-histogram line made five calls; `wsum` landed on `w[0]`,
 * and the line reported way0 as 100 % of hits -- past the pre-registered 85 % threshold
 * that STRIKES the DOPCFIND rung from the C2 ladder, two lines above the firmware's own
 * (correct, integer-computed) verdict that it stays. One aliased buffer, one inverted
 * engineering decision. Found on its first metal appearance.
 *
 * WHY THIS TEST READS THE SOURCE FILE. The hazard is not in any value this harness can
 * call: it is a property of how a CALL SITE is written, and it is invisible to the
 * compiler (the types are right, the count is legal, the result is wrong). A comment at
 * the function asking future writers to count their arguments is exactly the guarantee
 * that already failed. So the cap is enforced against the text, mechanically, on every
 * harness run -- and the cap itself is read out of the same file, so widening the ring
 * widens the check and deleting the constant fails the test.
 *
 * The scan is comment- and string-aware: `z3660_printf` inside a comment (there are
 * several, including in the header note this test is named in) must not be walked as if
 * it were code, or an unbalanced paren in prose would take the parser off the end.
 *
 * ENGAGEMENT IS PINNED, for the reason rung 2's corpus made the house rule: a scanner
 * that silently found nothing would pass this test forever. It must find the file, find
 * dozens of printf sites, and find at least one sitting exactly ON the cap -- which is a
 * real line in the file today (the ring-header dump, four values) and the closest thing
 * to a positive control available without adding a defect on purpose.
 * ================================================================== */
#ifndef Z3660_PROF_SRC_PATH
#error "Z3660_PROF_SRC_PATH must be defined by the Makefile -- see test_prof_u64_ring_depth"
#endif

/* Replace every comment body and string/char literal with spaces, so the paren walk below
 * sees code and only code. Returns 0 if the file could not be read.
 *
 * NEWLINES SURVIVE THE BLANKING. They carry no syntax, so blanking them costs nothing to
 * the parse -- but the offsets are turned back into a LINE NUMBER for the report, and a
 * blanked newline inside one of this file's very long block comments moved that number by
 * hundreds of lines. A guard against a print that reported a wrong number is not allowed
 * to report a wrong number itself; caught by disbelieving its own first output. */
#define PROF_SRC_BLANK(dst, src, k) ((dst)[k] = ((src)[k] == '\n') ? '\n' : ' ')

static int prof_src_read_code_only(const char *path, std::string &out)
{
	FILE *f = fopen(path, "rb");
	std::string raw;
	char chunk[4096];
	size_t got;
	size_t i, n;

	if (!f)
		return 0;
	while ((got = fread(chunk, 1, sizeof chunk, f)) > 0)
		raw.append(chunk, got);
	fclose(f);

	out = raw;
	n = raw.size();
	for (i = 0; i < n; i++) {
		char c = raw[i];
		if (c == '/' && i + 1 < n && raw[i + 1] == '/') {
			while (i < n && raw[i] != '\n') { out[i] = ' '; i++; }
			i--;
		} else if (c == '/' && i + 1 < n && raw[i + 1] == '*') {
			out[i] = ' '; out[i + 1] = ' '; i += 2;
			while (i + 1 < n && !(raw[i] == '*' && raw[i + 1] == '/')) {
				PROF_SRC_BLANK(out, raw, i); i++;
			}
			if (i + 1 < n) { out[i] = ' '; out[i + 1] = ' '; i++; }
		} else if (c == '"' || c == '\'') {
			char q = c;
			i++;
			while (i < n && raw[i] != q) {
				if (raw[i] == '\\' && i + 1 < n) { PROF_SRC_BLANK(out, raw, i); i++; }
				PROF_SRC_BLANK(out, raw, i);
				i++;
			}
		}
	}
	return 1;
}

static int prof_src_ident_at(const std::string &s, size_t pos, const char *id)
{
	size_t len = strlen(id);
	if (pos + len > s.size() || s.compare(pos, len, id) != 0)
		return 0;
	if (pos > 0 && (isalnum((unsigned char)s[pos - 1]) || s[pos - 1] == '_'))
		return 0;
	return 1;
}

/* The identifier is followed (past whitespace) by '(' -- which is what distinguishes the
 * RING form z3660_prof_u64( from the caller-owned z3660_prof_u64_into(. */
static int prof_src_call_at(const std::string &s, size_t pos, const char *id)
{
	size_t j;
	if (!prof_src_ident_at(s, pos, id))
		return 0;
	j = pos + strlen(id);
	while (j < s.size() && isspace((unsigned char)s[j]))
		j++;
	return j < s.size() && s[j] == '(';
}

static void test_prof_u64_ring_depth(void)
{
	std::string code;
	int slots = -1;
	int sites = 0, worst = 0, at_cap = 0;
	long worst_line = 0;
	size_t i;

	printf("[test] the dump's 64-bit formatter ring: no printf may outrun its slots\n");

	CHECK(prof_src_read_code_only(Z3660_PROF_SRC_PATH, code),
	      "the profiler source is readable at " Z3660_PROF_SRC_PATH "\n");
	if (code.empty())
		return;

	/* The cap comes out of the file, not out of this test: widening the ring must widen
	 * the check by itself, and deleting the constant must fail here rather than quietly
	 * disarm the guard. Read from the RAW text is unnecessary -- a #define survives the
	 * comment/string blanking above. */
	for (i = 0; i < code.size(); i++) {
		if (prof_src_ident_at(code, i, "Z3660_PROF_U64_SLOTS")) {
			size_t j = i + strlen("Z3660_PROF_U64_SLOTS");
			while (j < code.size() && (code[j] == ' ' || code[j] == '\t')) j++;
			if (j < code.size() && isdigit((unsigned char)code[j])) {
				slots = atoi(code.c_str() + j);
				break;
			}
		}
	}
	CHECK(slots > 0, "the ring depth is declared as Z3660_PROF_U64_SLOTS\n");
	if (slots <= 0)
		return;

	for (i = 0; i < code.size(); i++) {
		size_t j, start;
		int depth, calls;

		if (!prof_src_call_at(code, i, "z3660_printf"))
			continue;
		start = i;
		j = i + strlen("z3660_printf");
		while (j < code.size() && code[j] != '(') j++;
		depth = 0;
		calls = 0;
		for (; j < code.size(); j++) {
			if (code[j] == '(') {
				depth++;
			} else if (code[j] == ')') {
				if (--depth == 0) break;
			} else if (prof_src_call_at(code, j, "z3660_prof_u64")) {
				calls++;
			}
		}
		sites++;
		if (calls == slots)
			at_cap++;
		if (calls > worst) {
			worst = calls;
			worst_line = 1 + (long)std::count(code.begin(), code.begin() + (long)start, '\n');
		}
		i = j;
	}

	printf("       %d z3660_printf() sites, ring depth %d, worst line uses %d"
	       " (line %ld), %d at the cap\n", sites, slots, worst, worst_line, at_cap);

	/* Engagement, then the property. In that order: the second assertion is worthless
	 * unless the first two say the scanner actually looked at something. */
	CHECK(sites >= 40, "the scanner found the file's printf sites (engagement check)\n");
	CHECK(at_cap >= 1,
	      "...and at least one of them sits exactly ON the cap, so the counter counts\n");
	CHECK(worst <= slots,
	      "no z3660_printf() argument list outruns the formatter ring\n");
}

static void test_prof_wire_format(void)
{
	printf("[test] profiler wire format: what a symbolizer is allowed to rely on\n");

	CHECK_EQ32((uae_u32)sizeof(z3660_prof_sample_t), Z3660_PROF_REC_SIZE,
	           "sample record is exactly Z3660_PROF_REC_SIZE bytes");
	CHECK_EQ32((uae_u32)Z3660_PROF_REC_SIZE, 8, "...and that is 8");
	CHECK_EQ32((uae_u32)offsetof(z3660_prof_sample_t, pc),     0, "sample.pc     @ 0");
	CHECK_EQ32((uae_u32)offsetof(z3660_prof_sample_t, opcode), 4, "sample.opcode @ 4");
	CHECK_EQ32((uae_u32)offsetof(z3660_prof_sample_t, flags),  6, "sample.flags  @ 6");

	CHECK_EQ32((uae_u32)sizeof(z3660_prof_hdr_t), Z3660_PROF_HDR_SIZE,
	           "ring header is exactly Z3660_PROF_HDR_SIZE bytes");
	CHECK_EQ32((uae_u32)Z3660_PROF_HDR_SIZE, 80, "...and that is 80 (four uint64_t force 8-byte alignment)");
	CHECK_EQ32((uae_u32)offsetof(z3660_prof_hdr_t, samples), 32,
	           "the 64-bit block starts at 32 -- eight leading uint32_t, no hidden padding");
	CHECK_EQ32((uae_u32)Z3660_PROF_MAGIC, 0x5A335032u, "magic is 'Z3P2'");
	CHECK_EQ32((uae_u32)Z3660_PROF_VERSION, 2u, "version 2");
	/* The magic ENCODES the version, and that is load-bearing rather than decorative: a
	 * symbolizer that only greps the magic must still refuse a grammar it does not know.
	 * Derived here from the version so bumping one and forgetting the other fails. */
	CHECK_EQ32((uae_u32)Z3660_PROF_MAGIC,
	           0x5A335030u + (uae_u32)Z3660_PROF_VERSION,
	           "magic tracks the version -- 'Z3P' + the version digit");
	CHECK(strcmp(Z3660_PROF_MAGIC_STR, "Z3P2") == 0,
	      "...and the ASCII dump spells the same magic the binary header carries\n");

	/* v2 spent the two reserved words rather than resizing the header. If a future field
	 * ever grows it past 80, every symbolizer that slices the binary layout by offset
	 * breaks silently -- so pin the two new fields where they landed. */
	CHECK_EQ32((uae_u32)offsetof(z3660_prof_hdr_t, wall_hz),    64, "hdr.wall_hz    @ 64");
	CHECK_EQ32((uae_u32)offsetof(z3660_prof_hdr_t, build_flags),68, "hdr.build_flags@ 68");
	CHECK_EQ32((uae_u32)offsetof(z3660_prof_hdr_t, probe_cyc),  72,
	           "hdr.probe_cyc  @ 72 -- v2, in the first of v1's two reserved words");
	CHECK_EQ32((uae_u32)offsetof(z3660_prof_hdr_t, clk_src),    76,
	           "hdr.clk_src    @ 76 -- v2, in the second; the header is still 80 bytes");
	CHECK_EQ32((uae_u32)Z3660_PROF_CLK_BSP, 0u, "clk_src 0 = the compile-time BSP constant");
	CHECK_EQ32((uae_u32)Z3660_PROF_CLK_CFG, 1u, "clk_src 1 = the rate core0 published at runtime");

	/* Flag bits. Spelled as literals on purpose: re-deriving them from the macros would
	 * assert only that the macros equal themselves. */
	CHECK_EQ32((uae_u32)Z3660_PROF_F_SUPER,  0x0001u, "flag bit 0 = supervisor");
	CHECK_EQ32((uae_u32)Z3660_PROF_F_MMU,    0x0002u, "flag bit 1 = MMU enabled");
	CHECK_EQ32((uae_u32)Z3660_PROF_F_AMIX,   0x0004u, "flag bit 2 = AMIX mode");
	CHECK_EQ32((uae_u32)Z3660_PROF_F_CPU040, 0x0008u, "flag bit 3 = 68040 run loop");
	CHECK_EQ32((uae_u32)Z3660_PROF_F_WEIGHT(0x0100u), 1u,  "WEIGHT extracts from bits 15..8");
	CHECK_EQ32((uae_u32)Z3660_PROF_F_WEIGHT(0xFF0Fu), 255u, "WEIGHT tops out at 255");
	CHECK_EQ32((uae_u32)Z3660_PROF_F_WEIGHT(0x000Fu), 0u,
	           "a zero weight field is possible on the wire (unwritten slot) and means 'unknown'");

	/* Bucket and counter ids are wire values. A reorder silently relabels every capture
	 * ever taken, so pin the ones a symbolizer will hard-code and the two counts. */
	CHECK_EQ32((uae_u32)Z3660_PROF_B_LOOP,    0,  "bucket LOOP == 0");
	CHECK_EQ32((uae_u32)Z3660_PROF_B_FETCHOP, 1,  "bucket FETCHOP == 1");
	CHECK_EQ32((uae_u32)Z3660_PROF_B_HANDLER, 7,  "bucket HANDLER == 7");
	CHECK_EQ32((uae_u32)Z3660_PROF_B_TAILSPEC, 13, "bucket TAILSPEC == 13 (v2's last)");
	/* C2 rung 1d's appends, pinned individually for the reason the counter appends below
	 * are: a bare count check would pass just as happily if one had been INSERTED in the
	 * middle, which is the change that silently relabels every capture ever taken. */
	CHECK_EQ32((uae_u32)Z3660_PROF_B_DOPCFIND, 14, "bucket DOPCFIND == 14 (appended)");
	CHECK_EQ32((uae_u32)Z3660_PROF_B_DOPCFILL, 15, "bucket DOPCFILL == 15 (appended)");
	CHECK_EQ32((uae_u32)Z3660_PROF_B_BLKREC,   16, "bucket BLKREC == 16 (appended)");
	CHECK_EQ32((uae_u32)Z3660_PROF_B_COUNT,   17, "17 buckets");
	/* Id 0's NAME moved with its meaning, exactly as v1's TAIL became v2's TAILADV. A
	 * capture whose dump says LOOP holds the whole dispatch loop; one that says LOOPRES
	 * holds the residue and prints the whole on the [PROF] l line. That distinction is the
	 * only thing standing between the map's 26.4 % LOOP and a silently smaller number, so
	 * it is pinned rather than left to the name table's good intentions. */
	CHECK(strcmp(z3660_prof_bucket_names[Z3660_PROF_B_LOOP], "LOOPRES") == 0,
	      "id 0 prints as LOOPRES, not LOOP -- the name moves with the meaning\n");
	CHECK(strcmp(z3660_prof_bucket_names[Z3660_PROF_B_DOPCFIND], "DOPCFIND") == 0,
	      "names line up with ids (DOPCFIND, the C2 rung-1d append)\n");
	CHECK(strcmp(z3660_prof_bucket_names[Z3660_PROF_B_BLKREC], "BLKREC") == 0,
	      "names line up with ids (BLKREC)\n");
	CHECK_EQ32((uae_u32)Z3660_PROF_C_INSNS,   0,  "counter INSNS == 0");
	/* Appended ids, pinned individually: the whole safety of an append is that every id
	 * BELOW it still means what a v2 symbolizer thinks it means, and a bare count check
	 * would pass just as happily if someone had inserted one in the middle instead. */
	CHECK_EQ32((uae_u32)Z3660_PROF_C_ATC_HIT,      19, "counter ATC_HIT == 19 (v2's append)");
	CHECK_EQ32((uae_u32)Z3660_PROF_C_IFETCH_CALLS, 20, "counter IFETCH_CALLS == 20 (appended)");
	CHECK_EQ32((uae_u32)Z3660_PROF_C_BLK_HIT,      21, "counter BLK_HIT == 21 (appended)");
	CHECK_EQ32((uae_u32)Z3660_PROF_C_BLK_INSNS,    22, "counter BLK_INSNS == 22 (appended)");
	CHECK_EQ32((uae_u32)Z3660_PROF_C_BLK_BYTES,    23, "counter BLK_BYTES == 23 (appended)");
	CHECK_EQ32((uae_u32)Z3660_PROF_C_DOPC_HIT,     24, "counter DOPC_HIT == 24 (appended)");
	CHECK_EQ32((uae_u32)Z3660_PROF_C_DOPC_MISS,    25, "counter DOPC_MISS == 25 (appended)");
	CHECK_EQ32((uae_u32)Z3660_PROF_C_DOPC_EXT,     26, "counter DOPC_EXT == 26 (appended)");
	CHECK_EQ32((uae_u32)Z3660_PROF_C_DOPC_INVAL,   27, "counter DOPC_INVAL == 27 (appended)");
	CHECK(strcmp(z3660_prof_counter_names[Z3660_PROF_C_DOPC_HIT], "DOPC_HIT") == 0,
	      "names line up with ids (DOPC_HIT, the C2 rung-1 append)\n");
	CHECK(strcmp(z3660_prof_counter_names[Z3660_PROF_C_DOPC_INVAL], "DOPC_INVAL") == 0,
	      "names line up with ids (DOPC_INVAL)\n");

	/* C2 rung 1b: the invalidation-attribution block, ids 28..37, one per
	 * Z3660_DOPC_INV_* cause. Both ends are pinned and so is the ORDER, because
	 * z3660_dopc_count_cause() indexes this block arithmetically -- BASE + cause -- so an
	 * id that drifts out of cause order would file every invalidation under its
	 * neighbour's name and nothing would fail. */
	CHECK_EQ32((uae_u32)Z3660_PROF_C_DOPC_IV_BASE,    28, "IV block starts at 28");
	CHECK_EQ32((uae_u32)Z3660_PROF_C_DOPC_IV_FLUSH,   28 + Z3660_DOPC_INV_FLUSH,   "IV_FLUSH at BASE+FLUSH");
	CHECK_EQ32((uae_u32)Z3660_PROF_C_DOPC_IV_ROOT,    28 + Z3660_DOPC_INV_ROOT,    "IV_ROOT at BASE+ROOT");
	CHECK_EQ32((uae_u32)Z3660_PROF_C_DOPC_IV_DMA,     28 + Z3660_DOPC_INV_DMA,     "IV_DMA at BASE+DMA");
	CHECK_EQ32((uae_u32)Z3660_PROF_C_DOPC_IV_TABLE,   28 + Z3660_DOPC_INV_TABLE,   "IV_TABLE at BASE+TABLE");
	CHECK_EQ32((uae_u32)Z3660_PROF_C_DOPC_IV_KNOB,    28 + Z3660_DOPC_INV_KNOB,    "IV_KNOB at BASE+KNOB");
	CHECK_EQ32((uae_u32)Z3660_PROF_C_DOPC_IV_WRAP,    28 + Z3660_DOPC_INV_WRAP,    "IV_WRAP at BASE+WRAP");
	CHECK_EQ32((uae_u32)Z3660_PROF_C_DOPC_IV_MAP,     28 + Z3660_DOPC_INV_MAP,     "IV_MAP at BASE+MAP");
	CHECK_EQ32((uae_u32)Z3660_PROF_C_DOPC_IV_PFLUSH,  28 + Z3660_DOPC_INV_PFLUSH,  "IV_PFLUSH at BASE+PFLUSH");
	CHECK_EQ32((uae_u32)Z3660_PROF_C_DOPC_IV_PFLUSHA, 28 + Z3660_DOPC_INV_PFLUSHA, "IV_PFLUSHA at BASE+PFLUSHA");
	CHECK_EQ32((uae_u32)Z3660_PROF_C_DOPC_IV_CACR,    28 + Z3660_DOPC_INV_CACR,    "IV_CACR at BASE+CACR");
	CHECK(strcmp(z3660_prof_counter_names[Z3660_PROF_C_DOPC_IV_PFLUSH], "IV_PFLUSH") == 0,
	      "names line up with ids (IV_PFLUSH, the one logically-scoped cause)\n");
	CHECK_EQ32((uae_u32)Z3660_DOPC_INV_COUNT, 10, "ten invalidation causes");

	/* C2 rung 1c: id 38 sits AFTER the cause block and is not a member of it -- it counts
	 * requests the CACR call site proved inert, which never reach the invalidator at all.
	 * Pinned as "the block ends where 38 begins" rather than "the block ends at COUNT",
	 * because the block is no longer last: against COUNT, a new cause could alias onto 38
	 * with every assertion here still passing. */
	CHECK_EQ32((uae_u32)Z3660_PROF_C_DOPC_IV_CACR_SKIP, 38, "IV_CACR_SKIP at 38");
	CHECK_EQ32((uae_u32)(Z3660_PROF_C_DOPC_IV_BASE + Z3660_DOPC_INV_COUNT),
	           (uae_u32)Z3660_PROF_C_DOPC_IV_CACR_SKIP,
	           "...immediately after the cause block, which is where the tripwire pins it");
	CHECK(strcmp(z3660_prof_counter_names[Z3660_PROF_C_DOPC_IV_CACR_SKIP],
	             "IV_CACR_SKIP") == 0,
	      "names line up with ids (IV_CACR_SKIP)\n");
	/* C2 v3.1 SS25.2: the way histogram, appended at 39..42 and pinned by the same
	 * argument as the cause block -- it is indexed WAY_BASE + way inside the lookup, so
	 * an off-by-one in the base would file every hit under the wrong way and the verdict
	 * the map pre-registered would be a verdict about the wrong number. Pinned to the
	 * counter that FOLLOWS the block rather than to COUNT -- the block was last for
	 * exactly one commit, and against COUNT this would have gone on passing while way 3
	 * aliased onto rung 2's MISALIGN_I. */
	CHECK_EQ32((uae_u32)Z3660_PROF_C_DOPC_WAY0, 39, "DOPC_WAY0 at 39");
	CHECK_EQ32((uae_u32)(Z3660_PROF_C_DOPC_WAY_BASE + Z3660_DOPC_WAYS),
	           (uae_u32)Z3660_PROF_C_MISALIGN_I,
	           "...and the way block ends where the counter after it begins");
	CHECK(strcmp(z3660_prof_counter_names[Z3660_PROF_C_DOPC_WAY0], "DOPC_WAY0") == 0,
	      "names line up with ids (DOPC_WAY0)\n");
	CHECK(strcmp(z3660_prof_counter_names[Z3660_PROF_C_DOPC_WAY3], "DOPC_WAY3") == 0,
	      "names line up with ids (DOPC_WAY3, the far end of the block)\n");
	/* C2 rung 2's three: the misaligned-read denominator split, and the fast path's
	 * coverage counters. Appended after the way block, same rule as everything above. */
	CHECK_EQ32((uae_u32)Z3660_PROF_C_MISALIGN_I, 43, "MISALIGN_I at 43");
	CHECK_EQ32((uae_u32)Z3660_PROF_C_DFAST_HIT,  44, "DFAST_HIT at 44");
	CHECK_EQ32((uae_u32)Z3660_PROF_C_DFAST_XPAR, 45, "DFAST_XPAR at 45");
	CHECK(strcmp(z3660_prof_counter_names[Z3660_PROF_C_MISALIGN_I], "MISALIGN_I") == 0,
	      "names line up with ids (MISALIGN_I)\n");
	CHECK(strcmp(z3660_prof_counter_names[Z3660_PROF_C_DFAST_XPAR], "DFAST_XPAR") == 0,
	      "names line up with ids (DFAST_XPAR)\n");
	/* ID 46 IS PERMANENTLY RETIRED, and this is the assertion that keeps it that way.
	 *
	 * 46 was the withdrawn `f` measurement build's IV_SUPPRESSED -- a counter whose
	 * non-zero value means "this capture is not a profile of a correct machine", which
	 * docs/profiler.md tells every reading tool to refuse. Putting a WARP counter there
	 * would have made every legitimate capture refusable by a tool obeying that rule, and
	 * the tools outlive the build. The placeholder keeps the indices aligned and says so in
	 * the dump; nothing increments it, ever. */
	CHECK(strcmp(z3660_prof_counter_names[Z3660_PROF_C_RETIRED_46], "(retired)") == 0,
	      "id 46 carries a placeholder name and is never reused\n");
	CHECK_EQ32((uae_u32)Z3660_PROF_C_WARP_ENTER, 47, "WARP's counter block starts at 47");
	CHECK(strcmp(z3660_prof_counter_names[Z3660_PROF_C_WARP_ENTER], "WARP_ENTER") == 0
	   && strcmp(z3660_prof_counter_names[Z3660_PROF_C_WARP_BLK], "WARP_BLK") == 0
	   && strcmp(z3660_prof_counter_names[Z3660_PROF_C_WARP_INSNS], "WARP_INSNS") == 0
	   && strcmp(z3660_prof_counter_names[Z3660_PROF_C_WARP_BAIL], "WARP_BAIL") == 0
	   && strcmp(z3660_prof_counter_names[Z3660_PROF_C_WARP_BAIL_ST], "WARP_BAIL_ST") == 0
	   && strcmp(z3660_prof_counter_names[Z3660_PROF_C_WARP_COMPILE], "WARP_COMPILE") == 0
	   && strcmp(z3660_prof_counter_names[Z3660_PROF_C_WARP_CINSNS], "WARP_CINSNS") == 0
	   && strcmp(z3660_prof_counter_names[Z3660_PROF_C_WARP_CBYTES], "WARP_CBYTES") == 0
	   && strcmp(z3660_prof_counter_names[Z3660_PROF_C_WARP_CFETCH], "WARP_CFETCH") == 0
	   && strcmp(z3660_prof_counter_names[Z3660_PROF_C_WARP_DISC], "WARP_DISC") == 0
	   && strcmp(z3660_prof_counter_names[Z3660_PROF_C_WARP_DISC_ROOT], "WARP_DISC_ROOT") == 0
	   && strcmp(z3660_prof_counter_names[Z3660_PROF_C_WARP_FLUSH], "WARP_FLUSH") == 0
	   && strcmp(z3660_prof_counter_names[Z3660_PROF_C_WARP_DECLINE], "WARP_DECLINE") == 0,
	      "all thirteen WARP names line up with the ids S0.5.6 pre-registered\n");
	CHECK_EQ32((uae_u32)Z3660_PROF_C_COUNT,   60, "60 counters");
	/* The append is deliberately NOT a version bump: no id below it changed meaning, the
	 * binary header layout is untouched, and a v2 symbolizer reports an id past the end of
	 * its table uninterpreted rather than mis-labelled. Bumping would make every existing
	 * tool refuse a capture it can read correctly. */
	CHECK_EQ32((uae_u32)Z3660_PROF_VERSION,   2,
	           "wire version stays 2: an APPEND changes no existing id's meaning");

	/* v2's four new/renamed ids. These are the whole reason the version moved, so pin the
	 * numbers a symbolizer will hard-code AND the names it will print beside them -- an
	 * id that keeps its number while its meaning moves is exactly the ATC_HIT defect,
	 * and it took a metal session and a cross-check on someone else's tool to catch. */
	CHECK_EQ32((uae_u32)Z3660_PROF_B_TAIL,     8,  "bucket 8 is the tail RESIDUE in v2");
	CHECK_EQ32((uae_u32)Z3660_PROF_B_PROF,     10, "bucket PROF == 10 (v1 ids did not move)");
	CHECK_EQ32((uae_u32)Z3660_PROF_B_TAILSAMP, 11, "bucket TAILSAMP == 11 (appended)");
	CHECK_EQ32((uae_u32)Z3660_PROF_B_TAILPOLL, 12, "bucket TAILPOLL == 12 (appended)");
	CHECK_EQ32((uae_u32)Z3660_PROF_B_TAILSPEC, 13, "bucket TAILSPEC == 13 (appended)");
	CHECK(strcmp(z3660_prof_bucket_names[Z3660_PROF_B_TAIL], "TAILADV") == 0,
	      "bucket 8 is NAMED TAILADV -- v1's 'TAIL' meant the whole tail, so the name "
	      "moves with the meaning rather than covering both\n");
	CHECK(strcmp(z3660_prof_bucket_names[Z3660_PROF_B_TAILSAMP], "TAILSAMP") == 0,
	      "names line up with ids (TAILSAMP)\n");
	CHECK(strcmp(z3660_prof_bucket_names[Z3660_PROF_B_TAILSPEC], "TAILSPEC") == 0,
	      "names line up with ids (TAILSPEC)\n");

	CHECK_EQ32((uae_u32)Z3660_PROF_C_XLATE,    11, "counter XLATE == 11 (unmoved)");
	CHECK_EQ32((uae_u32)Z3660_PROF_C_XLATE_OK, 12,
	           "counter 12 is XLATE_OK -- same id, same site, the name now says what it counts");
	CHECK_EQ32((uae_u32)Z3660_PROF_C_ATC_MISS, 13, "counter ATC_MISS == 13 (unmoved)");
	CHECK_EQ32((uae_u32)Z3660_PROF_C_ATC_HIT,  19,
	           "counter ATC_HIT == 19 -- APPENDED, because the real thing needed a new id");
	CHECK(strcmp(z3660_prof_counter_names[Z3660_PROF_C_XLATE_OK], "XLATE_OK") == 0,
	      "names line up with ids (XLATE_OK)\n");
	CHECK(strcmp(z3660_prof_counter_names[Z3660_PROF_C_ATC_HIT], "ATC_HIT") == 0,
	      "names line up with ids (ATC_HIT, at its new id)\n");
	CHECK_EQ32((uae_u32)Z3660_PROF_RING_ENTRIES, 65536u, "ring holds 65536 samples");
	CHECK((Z3660_PROF_RING_ENTRIES & Z3660_PROF_RING_MASK) == 0,
	      "ring capacity is a power of two (the head is masked, not divided)\n");

	/* The name tables are what makes a capture self-describing; a NULL in one would
	 * crash the dump on the board, at the end of a long metal run. */
	{
		int i, ok = 1;
		for (i = 0; i < Z3660_PROF_B_COUNT; i++) if (!z3660_prof_bucket_names[i]) ok = 0;
		CHECK(ok, "every bucket has a name\n");
		ok = 1;
		for (i = 0; i < Z3660_PROF_C_COUNT; i++) if (!z3660_prof_counter_names[i]) ok = 0;
		CHECK(ok, "every counter has a name\n");
		CHECK(strcmp(z3660_prof_bucket_names[Z3660_PROF_B_WALK], "WALK") == 0,
		      "names line up with ids (WALK)\n");
		CHECK(strcmp(z3660_prof_counter_names[Z3660_PROF_C_ATC_MISS], "ATC_MISS") == 0,
		      "names line up with ids (ATC_MISS)\n");
	}
}

static void test_prof_build_variant(void)
{
	printf("[test] profiler build variant: %s\n",
	       Z3660_EMU_PROF ? "PROFILING (sampler + stage buckets compiled in)"
	                      : "not a profiling build");

#if defined(Z3660_PROF_EXPECTED)
	CHECK_EQ32((uae_u32)Z3660_EMU_PROF, 1, "PROFILING build: the profiler is compiled in");
	CHECK_EQ32((uae_u32)z3660_prof_present(), 1, "...and says so at runtime");
	/* The profiling variant is built ON TOP OF LEAN. If someone ever bases it on the
	 * default loop, the profile would describe a binary nobody runs -- and the C3
	 * calibration measured that difference at 9.1 %, bigger than several of the effects
	 * this instrument exists to rank. Fail here rather than mislead C2. */
	CHECK_EQ32((uae_u32)Z3660_RUNLOOP_BUILD_IS_LEAN, 1,
	           "PROFILING build is lean-based (diagnostics compiled out under the profiler)");
	CHECK_EQ32((uae_u32)(Z3660_RUNLOOP_BUILD_FLAGS | Z3660_PROF_BUILD_PROF), 0x04u,
	           "build_flags word for the shipped profiling combination is 0x04");
#else
	CHECK_EQ32((uae_u32)Z3660_EMU_PROF, 0,
	           "DEFAULT/LEAN build: the profiler is compiled OUT (absent -D changes nothing)");
	CHECK_EQ32((uae_u32)z3660_prof_present(), 0,
	           "...and the API stubs say so, so a console command can refuse instead of no-op");
	CHECK_EQ32((uae_u32)z3660_prof_cpu_hz(), 0, "no calibrated clock without a profiler");
#endif
}

#if Z3660_EMU_PROF
/* Drive the phase machine with a clock we set by hand. */
static void prof_clock(uae_u32 c) { z3660_prof_host_cycles = c; }

static void test_prof_bucket_arithmetic(void)
{
	printf("[test] profiler stage buckets: exclusive attribution against a known clock\n");

	z3660_prof_set_buckets(1);
	z3660_prof_zero();
	prof_clock(1000);
	z3660_prof_loop_reset();          /* cur = LOOP, last = 1000 */

	/* 10 cycles of LOOP, then 100 of HANDLER with 30 of READ nested inside it,
	 * then 5 more of LOOP. */
	prof_clock(1010); z3660_prof_enter(Z3660_PROF_B_HANDLER);
	prof_clock(1040); z3660_prof_enter(Z3660_PROF_B_READ);
	prof_clock(1070); z3660_prof_exit();       /* 30 cycles of READ */
	prof_clock(1110); z3660_prof_exit();       /* HANDLER got 30 + 40 = 70 */
	prof_clock(1115);
	z3660_prof_enter(Z3660_PROF_B_TAIL);       /* closes 5 more cycles of LOOP */
	prof_clock(1120); z3660_prof_exit();       /* 5 cycles of TAIL */

	CHECK_EQ32((uae_u32)z3660_prof_acc[Z3660_PROF_B_READ], 30,
	           "a nested stage gets exactly its own span");
	CHECK_EQ32((uae_u32)z3660_prof_acc[Z3660_PROF_B_HANDLER], 70,
	           "...and the parent gets its span MINUS the nested one (exclusive, not inclusive)");
	CHECK_EQ32((uae_u32)z3660_prof_acc[Z3660_PROF_B_LOOP], 15,
	           "LOOP collects only what ran outside every named stage");
	CHECK_EQ32((uae_u32)z3660_prof_acc[Z3660_PROF_B_TAIL], 5, "TAIL gets its 5");
	{
		uae_u64 total = 0; int i;
		for (i = 0; i < Z3660_PROF_B_COUNT; i++) total += z3660_prof_acc[i];
		CHECK_EQ32((uae_u32)total, 120,
		           "the buckets sum to the whole elapsed span -- no cycle counted twice, none lost");
	}
	CHECK_EQ32((uae_u32)z3660_prof_cnt[Z3660_PROF_C_TRANSITIONS], 6,
	           "every transition is counted, so probe overhead is subtractable");

	/* CYBERSTORM C2 rung 1b -- PER-BUCKET TRANSITION SPANS.
	 *
	 * The aggregate above makes the probe subtractable from the TOTAL; it has never been
	 * subtractable per bucket, and the 2026-08-23 verdict paid for that by having to
	 * report LOOP -- the largest bucket in the capture, at 23.78 % raw -- as an interval
	 * rather than a number. The right divisor for a bucket is the number of SPANS charged
	 * to it, which is what this array counts, and this fixture is where that is pinned
	 * against literal values instead of against a running clock.
	 *
	 * Read the fixture above and the counts fall out: LOOP is left twice (at 1010 and at
	 * 1115), HANDLER is left twice (once to enter READ, once to exit), READ once and TAIL
	 * once. Six charges for six transitions -- the identity, and the reason the per-bucket
	 * correction and the global one cannot disagree. */
	CHECK_EQ32((uae_u32)z3660_prof_tcnt[Z3660_PROF_B_LOOP],    2, "LOOP was charged twice");
	CHECK_EQ32((uae_u32)z3660_prof_tcnt[Z3660_PROF_B_HANDLER], 2,
	           "HANDLER twice -- entering a nested stage closes a span just as exiting does, "
	           "which is exactly why one probe per CALL would be the wrong divisor");
	CHECK_EQ32((uae_u32)z3660_prof_tcnt[Z3660_PROF_B_READ],    1, "READ once");
	CHECK_EQ32((uae_u32)z3660_prof_tcnt[Z3660_PROF_B_TAIL],    1, "TAIL once");
	CHECK_EQ32((uae_u32)z3660_prof_tcnt[Z3660_PROF_B_XLATE],   0,
	           "a bucket that never ran is charged nothing");
	{
		uae_u64 spans = 0; int i;
		for (i = 0; i < Z3660_PROF_B_COUNT; i++) spans += z3660_prof_tcnt[i];
		CHECK_EQ32((uae_u32)spans, (uae_u32)z3660_prof_cnt[Z3660_PROF_C_TRANSITIONS],
		           "THE IDENTITY: sum(per-bucket spans) == TRANSITIONS, so the per-bucket "
		           "subtraction decomposes the global one exactly");
	}

	/* The 32-bit cycle counter wraps every ~6.4 s at 666 MHz. Modular subtraction has to
	 * make that a non-event, or every bucket total is garbage a few seconds into a run. */
	z3660_prof_zero();
	prof_clock(0xFFFFFFF0u); z3660_prof_loop_reset();
	prof_clock(0x0000000Fu); z3660_prof_enter(Z3660_PROF_B_READ);
	CHECK_EQ32((uae_u32)z3660_prof_acc[Z3660_PROF_B_LOOP], 31,
	           "a span across the cycle-counter wrap measures 31, not 4 billion");

	/* Unwind. A guest page fault is a C++ throw straight past every pending exit(). */
	z3660_prof_zero();
	prof_clock(500); z3660_prof_loop_reset();
	z3660_prof_enter(Z3660_PROF_B_HANDLER);
	z3660_prof_enter(Z3660_PROF_B_READ);
	z3660_prof_enter(Z3660_PROF_B_XLATE);
	CHECK_EQ32((uae_u32)z3660_prof_sp, 3, "three stages nested");
	prof_clock(600); z3660_prof_unwound();
	CHECK_EQ32((uae_u32)z3660_prof_acc[Z3660_PROF_B_FAULT], 100,
	           "the abandoned span is charged to FAULT, not silently lost");
	/* C2 rung 1b: cycles, but NO span. An unwind is not a probe transition and has never
	 * been in TRANSITIONS, so counting it per bucket would make the two corrections
	 * disagree. The consequence is that FAULT is reported uncorrected; asserted here so
	 * that it is a documented property rather than a discovered one. */
	CHECK_EQ32((uae_u32)z3660_prof_tcnt[Z3660_PROF_B_FAULT], 0,
	           "...and charged NO span, so FAULT reads uncorrected and the sum identity "
	           "with TRANSITIONS survives an unwind");
	CHECK_EQ32((uae_u32)z3660_prof_sp, 0, "and the stack is back at the floor");
	CHECK_EQ32((uae_u32)z3660_prof_cur, (uae_u32)Z3660_PROF_B_LOOP, "...running LOOP again");

	/* Overflow must DROP the transition, never clamp the index: a clamp would corrupt the
	 * pop order and mis-attribute every stage after it. Depth 16 against a real maximum
	 * nesting of four means this is unreachable in the firmware -- which is exactly why
	 * it needs a test rather than a comment. */
	z3660_prof_zero();
	prof_clock(0); z3660_prof_loop_reset();
	{
		int i;
		for (i = 0; i < Z3660_PROF_STACK_DEPTH + 4; i++) z3660_prof_enter(Z3660_PROF_B_READ);
		CHECK_EQ32((uae_u32)z3660_prof_sp, Z3660_PROF_STACK_DEPTH, "the stack stops at its depth");
		CHECK_EQ32((uae_u32)z3660_prof_cnt[Z3660_PROF_C_STACK_OVF], 4,
		           "and every dropped transition is counted (a dump showing non-zero here is not trustworthy)");
		for (i = 0; i < Z3660_PROF_STACK_DEPTH; i++) z3660_prof_exit();
		CHECK_EQ32((uae_u32)z3660_prof_sp, 0, "and it unwinds cleanly afterwards");
	}

	/* Switched off, the machine must be inert -- not merely quiet. PROFB off is what a
	 * session uses to take an undistorted PC map. */
	z3660_prof_set_buckets(0);
	z3660_prof_zero();
	prof_clock(0); z3660_prof_enter(Z3660_PROF_B_HANDLER);
	prof_clock(9999); z3660_prof_exit();
	{
		uae_u64 total = 0; int i;
		for (i = 0; i < Z3660_PROF_B_COUNT; i++) total += z3660_prof_acc[i];
		CHECK_EQ32((uae_u32)total, 0, "buckets OFF: nothing is charged anywhere");
		CHECK_EQ32((uae_u32)z3660_prof_cnt[Z3660_PROF_C_TRANSITIONS], 0,
		           "buckets OFF: not even a transition count");
	}
}

/* ==================================================================
 * The probe macros put the work INSIDE the bucket.
 *
 * This test exists because the first build of this feature got it backwards and nothing
 * caught it. `Z3660_PROF_RET(expr)` was written as `exit(); return (expr);` -- which
 * sequences the bucket CLOSE before the accessor's work, so every data read and every
 * instruction fetch charged its cycles to the enclosing HANDLER instead of to READ or
 * FETCH. Every other test still passed: the counters were right, the stack balanced, the
 * emulation was correct, and the sums added up. Only the attribution -- the one thing the
 * instrument is for -- was wrong, and it was found by reading the generated ARM, where
 * enter/count/exit stood in three adjacent `bl`s ahead of the memory access.
 *
 * So the ordering gets its own test, on stand-ins built from the same macros the real
 * accessors use, with a "memory access" that costs a known 100 cycles.
 * ================================================================== */
static uae_u32 prof_fake_work_u32(void)  { z3660_prof_host_cycles += 100; return 0xABCD1234u; }
static void    prof_fake_work_void(void) { z3660_prof_host_cycles += 100; }

static uae_u32 prof_victim_ret(void)
{
	Z3660_PROF_ENTER(Z3660_PROF_B_READ);
	Z3660_PROF_CNT(Z3660_PROF_C_READ);
	Z3660_PROF_RET(prof_fake_work_u32());
}
static void prof_victim_retv(void)
{
	Z3660_PROF_ENTER(Z3660_PROF_B_WRITE);
	Z3660_PROF_CNT(Z3660_PROF_C_WRITE);
	Z3660_PROF_RETV(prof_fake_work_void());
}

static void test_prof_ret_macro_ordering(void)
{
	printf("[test] profiler probe macros: the accessor's work is INSIDE its bucket\n");

	z3660_prof_set_buckets(1);
	z3660_prof_zero();
	prof_clock(0);
	z3660_prof_loop_reset();

	{
		uae_u32 v = prof_victim_ret();
		CHECK_EQ32(v, 0xABCD1234u, "the value still comes back through the macro");
	}
	CHECK_EQ32((uae_u32)z3660_prof_acc[Z3660_PROF_B_READ], 100,
	           "Z3660_PROF_RET: the 100 cycles of work land in READ, not in the caller's bucket");
	CHECK_EQ32((uae_u32)z3660_prof_acc[Z3660_PROF_B_LOOP], 0,
	           "...and LOOP gets none of them (0 here is what a correct ordering looks like)");

	z3660_prof_zero();
	prof_clock(0);
	z3660_prof_loop_reset();
	prof_victim_retv();
	CHECK_EQ32((uae_u32)z3660_prof_acc[Z3660_PROF_B_WRITE], 100,
	           "Z3660_PROF_RETV: same for the void form");
	CHECK_EQ32((uae_u32)z3660_prof_acc[Z3660_PROF_B_LOOP], 0,
	           "...and LOOP gets none of those either");

	z3660_prof_set_buckets(0);
}

/* ==================================================================
 * CYBERSTORM C2-prep -- the probe calibration reports a PER-TRANSITION cost.
 *
 * This is the defect that inverted the top of the C2 attack map, and it is worth being
 * precise about why no existing test could see it. v1 timed a batch of N enter/exit
 * PAIRS, divided by N, and handed the result to a reporting line that multiplied it by
 * TRANSITIONS -- a counter that increments in enter() AND in exit(). Two numbers of the
 * right TYPE and the wrong UNIT, so every arithmetic check downstream agreed with itself
 * while the answer was exactly 2x too big; big enough that the model demanded 43.8 G
 * cycles of probe out of a bucket containing 28.0 G, and the host tool clamped rather
 * than complained.
 *
 * The harness could not catch it because on a frozen injected clock the calibration
 * measures a span of zero, and zero divided by N and by 2N are the same zero. So the
 * host cycle source can now STEP: each read advances it by z3660_prof_host_step, which
 * gives the probe a known price and turns the divisor into something observable.
 *
 * With a step of S, each clock READ costs S. The calibration reads once for `last`, once
 * for c0, twice per pair inside the loop, and once for c1 -- so c1-c0 spans (2*pairs+1)
 * reads and the per-transition answer is S*(2*pairs+1)/(2*pairs), which is S for any
 * realistic batch. A v1-style divide-by-pairs would give 2S, and that is the assertion.
 * ================================================================== */
static void test_prof_probe_calibration(void)
{
	printf("[test] profiler probe calibration: the unit is ONE transition, not a pair\n");

	const uae_u32 STEP = 64;
	z3660_prof_set_buckets(0);
	z3660_prof_zero();
	prof_clock(0);
	z3660_prof_host_step = STEP;
	z3660_prof_recalibrate_probe_for_test();
	z3660_prof_host_step = 0;

	CHECK_EQ32((uae_u32)z3660_prof_probe_cyc_per_transition(), STEP,
	           "a probe costing 64 cycles per clock read is priced at 64 cyc/TRANSITION");
	CHECK(z3660_prof_probe_cyc_per_transition() != (int)(2 * STEP),
	      "...and NOT at 128 -- which is what dividing a pair-batch by the pair count gives\n");

	/* THE THIRD UNIT ERROR (C2 riding item c): the price must be the MARGINAL cost of an
	 * armed transition, not the cost of the whole call.
	 *
	 * The map found 59 cyc/transition arithmetically impossible -- at 59 the subtraction
	 * demands 4.2 G cycles that LOOP and TAILADV do not contain, and both clamp. LOOP is
	 * exhausted at 50.59 and TAILADV at 54.11, so 59 is above the whole usable range and
	 * above the 45.5-58 band this firmware's own docs predicted. The cause is that the
	 * calibration used to time the armed loop ALONE and charge the `bl`, the prologue, the
	 * `if (!bucket_on)` test and the for-loop scaffolding to the probe -- all of which
	 * SURVIVE in the machine the subtraction is meant to leave behind (map §10.1: probe
	 * subtraction removes the probe BODIES; the instrumented accessors' out-of-line calls
	 * stay, and docs/profiler.md prices those separately at -23.2 %). Subtracting them as
	 * probe and leaving them in the machine is a double count.
	 *
	 * So the two spans are timed and differenced. The injectable clock makes that exact
	 * rather than approximate: it advances on every READ, the armed pass makes 2*PAIRS
	 * reads plus its two bracketing ones, the unarmed pass makes only the two, and the
	 * difference over 2*PAIRS is STEP on the nose. The old single-pass formula landed on
	 * the same integer only because the excess 1/2048 rounded away. */
	{
		uae_u32 armed = 0, unarmed = 0, empty = 0;
		z3660_prof_probe_cal_spans(&armed, &unarmed, &empty);
		CHECK(armed > unarmed,
		      "the armed pass costs more than the identical unarmed one\n");
		CHECK_EQ32(armed, STEP * (2 * 1024 + 1),
		           "armed span == one clock read per transition, plus the two bracketing reads");
		CHECK_EQ32(unarmed, STEP,
		           "unarmed span == the two bracketing reads only: enter/exit return before "
		           "the clock read, so the calls and the loop are timed but the BODY is not");
		CHECK_EQ32((armed - unarmed) / (2 * 1024), STEP,
		           "the DIFFERENCE over 2*PAIRS is the marginal per-transition cost, exactly");

		/* THE THIRD SPAN (C2 riding item a): the same loop with no calls in it, which is
		 * what separates the call scaffolding -- real, and inside the buckets, because
		 * enter() stamps `last` before its own epilogue runs -- from the `for` loop's own
		 * compare and branch, which exist only in this calibration.
		 *
		 * On the host both the unarmed and the empty pass make exactly the two bracketing
		 * clock reads and nothing else, so they measure the SAME span; the in-bucket price
		 * therefore collapses onto the marginal one here, and the clamp is what makes that
		 * the reported answer instead of a spurious difference. On metal the two differ by
		 * the call scaffolding and the bracket opens. What is testable here is the
		 * ORDERING and the arithmetic, and those are the things that would silently
		 * invert if a future edit moved the cycle read inside enter(). */
		CHECK_EQ32(empty, STEP,
		           "empty span == the two bracketing reads: an uncalled loop reads no clock");
		CHECK(armed >= unarmed && unarmed >= empty,
		      "the three spans are ordered armed >= unarmed >= empty\n");
		CHECK_EQ32((uae_u32)z3660_prof_probe_cyc_in_bucket(),
		           (uae_u32)z3660_prof_probe_cyc_per_transition(),
		           "with no measurable call scaffolding the bracket is a point, and the "
		           "in-bucket price clamps to the marginal one rather than going below it");
		CHECK((armed - empty) / (2 * 1024) >= (armed - unarmed) / (2 * 1024),
		      "the in-bucket price can never be BELOW the marginal one\n");
	}

	/* The other half of the fix, and the one that made LOOP's share a bracket rather
	 * than a number: the probes are called, not inlined, during calibration. A frozen
	 * clock cannot time that, but it CAN show the calibration went through the real
	 * phase machine -- the wrappers move the accumulators and the transition count
	 * exactly as the interpreter's own probes do. */
	z3660_prof_zero();
	prof_clock(0);
	z3660_prof_set_buckets(1);
	z3660_prof_loop_reset();
	z3660_prof_recalibrate_probe_for_test();
	CHECK(z3660_prof_cnt[Z3660_PROF_C_TRANSITIONS] > 0,
	      "the calibration drives the genuine enter/exit path, not a transcription of it\n");
	CHECK_EQ32((uae_u32)(z3660_prof_cnt[Z3660_PROF_C_TRANSITIONS] & 1u), 0,
	           "...and leaves an EVEN transition count: every enter found its exit");
	CHECK_EQ32((uae_u32)z3660_prof_sp, 0, "the phase stack is back at the floor afterwards");
	CHECK_EQ32((uae_u32)z3660_prof_cnt[Z3660_PROF_C_STACK_OVF], 0,
	           "and the calibration never overflows it");

	z3660_prof_set_buckets(0);
	z3660_prof_zero();
}

/* ==================================================================
 * CYBERSTORM C2 v3.1 riding item -- THE PRICE BRACKET'S CEILING.
 *
 * The map's §26.3 item 3, and the second time the same arithmetic has bitten: a capture's
 * corrected cycles are acc[b] - spans[b] x price, so a price above min_b(acc[b]/spans[b])
 * drives some bucket negative, the dump clamps it to zero, and the clamped zero reads like
 * a measurement. v2 PICKED 59 when the capture allowed ~50; the 2026-08-23 boot line
 * ADVERTISED 68 when that capture's ceiling was 59.17.
 *
 * What is testable here is the arithmetic and WHICH bucket binds it -- the min, not the
 * first, and skipping the buckets with no spans (FAULT is charged cycles and no span on an
 * unwind, and a zero-span bucket dividing into its cycles is not a price at all).
 * ================================================================== */
static void test_prof_price_ceiling(void)
{
	printf("[test] profiler price bracket: the ceiling above which a sweep is impossible\n");

	z3660_prof_set_buckets(1);
	z3660_prof_zero();
	prof_clock(0);
	z3660_prof_loop_reset();

	/* Hand-built so the ceiling is unambiguous and is NOT the first bucket with spans.
	 *   LOOP    100 cyc over 2 spans -> 50
	 *   HANDLER 600 cyc over 2 spans -> 300
	 *   READ     30 cyc over 1 span  -> 30    <-- the minimum, and it binds
	 * Every other bucket has zero spans and must be skipped rather than divided. */
	prof_clock(50);  z3660_prof_enter(Z3660_PROF_B_HANDLER);  /* LOOP  += 50, span 1 */
	prof_clock(350); z3660_prof_enter(Z3660_PROF_B_READ);     /* HAND  += 300, span 1 */
	prof_clock(380); z3660_prof_exit();                       /* READ  += 30, span 1 */
	prof_clock(680); z3660_prof_exit();                       /* HAND  += 300, span 2 */
	prof_clock(730); z3660_prof_enter(Z3660_PROF_B_TAIL);     /* LOOP  += 50, span 2 */
	prof_clock(730); z3660_prof_exit();                       /* TAIL  += 0,  span 1 */

	CHECK_EQ32((uae_u32)z3660_prof_acc[Z3660_PROF_B_LOOP],    100, "LOOP has its 100");
	CHECK_EQ32((uae_u32)z3660_prof_acc[Z3660_PROF_B_HANDLER], 600, "HANDLER has its 600");
	CHECK_EQ32((uae_u32)z3660_prof_acc[Z3660_PROF_B_READ],     30, "READ has its 30");

	/* TAIL took 0 cycles over 1 span, so its own ratio is 0 and IT is the true minimum.
	 * That is not a defect of the ceiling -- a bucket that measured zero cycles across a
	 * real span genuinely cannot survive any positive price -- and the ceiling has to say
	 * so rather than quietly skipping it, or it would advertise a price at which a bucket
	 * clamps. This is exactly the shape of the TAILADV row that bound the metal capture. */
	CHECK_EQ32((uae_u32)z3660_prof_price_ceiling(), 0,
	           "a bucket with a real span and zero cycles binds the ceiling at 0");
	CHECK_EQ32((uae_u32)z3660_prof_price_ceiling_bucket(), (uae_u32)Z3660_PROF_B_TAIL,
	           "...and the dump names THAT bucket, not the first one with spans");

	/* Now give TAIL enough cycles that READ is the binding row, which is the case the
	 * metal dumps are actually in. */
	prof_clock(1000); z3660_prof_enter(Z3660_PROF_B_TAIL);
	prof_clock(1200); z3660_prof_exit();     /* TAIL += 200 over a 2nd span -> 200/2 = 100 */

	CHECK_EQ32((uae_u32)z3660_prof_price_ceiling(), 30,
	           "with TAIL at 100 the minimum is READ's 30");
	CHECK_EQ32((uae_u32)z3660_prof_price_ceiling_bucket(), (uae_u32)Z3660_PROF_B_READ,
	           "...and READ is named as the bucket that binds it");

	/* The property the whole item exists for: at the ceiling nothing clamps, one cycle
	 * above it something does. Asserted directly on the same subtraction the dump does. */
	{
		int price = z3660_prof_price_ceiling();
		int i, clamped_at_ceiling = 0, clamped_above = 0;
		for (i = 0; i < Z3660_PROF_B_COUNT; i++) {
			if (!z3660_prof_tcnt[i]) continue;
			if (z3660_prof_acc[i] < z3660_prof_tcnt[i] * (uae_u64)price)
				clamped_at_ceiling = 1;
			if (z3660_prof_acc[i] < z3660_prof_tcnt[i] * (uae_u64)(price + 1))
				clamped_above = 1;
		}
		CHECK(!clamped_at_ceiling,
		      "at the ceiling no bucket's subtraction goes negative\n");
		CHECK(clamped_above,
		      "one cycle above it, one does -- which is what makes it a ceiling\n");
	}

	/* A capture with nothing bucketed yet has no ceiling to report, and must say 0/-1
	 * rather than dividing by zero. The boot line is exactly in this state, which is why
	 * the ceiling is printed on PROFD and not there. */
	z3660_prof_zero();
	CHECK_EQ32((uae_u32)z3660_prof_price_ceiling(), 0,
	           "an empty capture reports no ceiling rather than dividing by zero");
	CHECK_EQ32((uae_u32)z3660_prof_price_ceiling_bucket(), (uae_u32)-1,
	           "...and names no bucket");

	z3660_prof_set_buckets(0);
	z3660_prof_zero();
}

/* ==================================================================
 * CYBERSTORM C2-prep -- the tail split.
 *
 * The map's single highest-value instrument ask: TAIL was 20 % of guest time with no
 * internal breakdown. What has to hold is what holds for every other nested stage --
 * exclusive attribution -- plus the one thing that is new here, that the four ids add
 * back up to the figure v1 called TAIL, so a v2 capture can still be compared with a v1
 * one.
 * ================================================================== */
static void test_prof_tail_split(void)
{
	printf("[test] profiler tail split: four ids, exclusive, and they re-sum to v1's TAIL\n");

	z3660_prof_set_buckets(1);
	z3660_prof_zero();
	prof_clock(1000);
	z3660_prof_loop_reset();

	/* Walk one tail exactly as the run loop does: open TAIL, then the three sub-stages
	 * in order, then close. Spans chosen distinct so a mis-attribution cannot alias. */
	prof_clock(1010);
	z3660_prof_enter(Z3660_PROF_B_TAIL);       /* 10 cycles of LOOP closed here      */
	prof_clock(1013);                          /* 3 cycles of TAILADV (adjust_cycles)*/
	z3660_prof_enter(Z3660_PROF_B_TAILSAMP);
	prof_clock(1020); z3660_prof_exit();       /* 7 of TAILSAMP                      */
	z3660_prof_enter(Z3660_PROF_B_TAILPOLL);
	prof_clock(1040); z3660_prof_exit();       /* 20 of TAILPOLL                     */
	z3660_prof_enter(Z3660_PROF_B_TAILSPEC);
	prof_clock(1100); z3660_prof_exit();       /* 60 of TAILSPEC                     */
	prof_clock(1105); z3660_prof_exit();       /* 5 more of TAILADV, tail closed     */

	CHECK_EQ32((uae_u32)z3660_prof_acc[Z3660_PROF_B_TAILSAMP], 7,  "TAILSAMP gets its 7");
	CHECK_EQ32((uae_u32)z3660_prof_acc[Z3660_PROF_B_TAILPOLL], 20, "TAILPOLL gets its 20");
	CHECK_EQ32((uae_u32)z3660_prof_acc[Z3660_PROF_B_TAILSPEC], 60, "TAILSPEC gets its 60");
	CHECK_EQ32((uae_u32)z3660_prof_acc[Z3660_PROF_B_TAIL], 8,
	           "TAILADV keeps only the RESIDUE (3 + 5), not the sub-stages nested in it");
	CHECK_EQ32((uae_u32)z3660_prof_acc[Z3660_PROF_B_LOOP], 10,
	           "...and LOOP keeps only what ran before the tail opened");
	{
		uae_u64 tail = z3660_prof_acc[Z3660_PROF_B_TAIL]
		             + z3660_prof_acc[Z3660_PROF_B_TAILSAMP]
		             + z3660_prof_acc[Z3660_PROF_B_TAILPOLL]
		             + z3660_prof_acc[Z3660_PROF_B_TAILSPEC];
		CHECK_EQ32((uae_u32)tail, 95,
		           "the four ids re-sum to the whole tail -- which is what v1 reported as TAIL");
		uae_u64 total = 0; int i;
		for (i = 0; i < Z3660_PROF_B_COUNT; i++) total += z3660_prof_acc[i];
		CHECK_EQ32((uae_u32)total, 105,
		           "and the split changes no total: sum(buckets) is still the elapsed span");
	}
	/* Four extra transitions per instruction is what the split costs, and the cost has
	 * to be VISIBLE in the transition count or the probe subtraction under-corrects. */
	CHECK_EQ32((uae_u32)z3660_prof_cnt[Z3660_PROF_C_TRANSITIONS], 8,
	           "one tail = 8 transitions (v1's 2, plus 3 sub-stage pairs)");

	z3660_prof_set_buckets(0);
	z3660_prof_zero();
}

/* ==================================================================
 * CYBERSTORM C2-prep -- ATC_HIT counts ATC hits.
 *
 * The v1 counter sat on mmu_translate()'s success return, which is reached BOTH by an
 * ATC tag match and by `goto atc_retry` after a table walk -- so it counted successful
 * translates and carried no ATC information at all. Nothing failed: `ATC_HIT + ATC_MISS`
 * was simply a meaningless sum that four metal captures dutifully reported.
 *
 * The test is the smallest thing that discriminates: translate the same page twice. The
 * first must walk, the second must hit. Under v1 both increments landed on ATC_HIT and
 * this test would report 2 where it wants 1.
 * ================================================================== */
static void test_prof_translation_counters(void)
{
	printf("[test] profiler translation counters: a walk is not a hit\n");

	const uae_u32 VA = 0x00408678, PA = 0x0A000000;
	mmu040_map1(VA, PA, 0, false);
	regs.s = 1;

	z3660_prof_set_buckets(0);
	z3660_prof_zero();

	uae_u32 p1 = 0, p2 = 0; bool faulted = false;
	SAVE_EXCEPTION;
	TRY(t1) { p1 = mmu_translate(VA, 0, true, true, false, sz_long); }
	CATCH(t1) { faulted = true; (void)t1; } ENDTRY
	RESTORE_EXCEPTION;
	CHECK(!faulted, "first translate completed (it had to walk)\n");

	CHECK_EQ32((uae_u32)z3660_prof_cnt[Z3660_PROF_C_XLATE], 1, "one call into mmu_translate");
	CHECK_EQ32((uae_u32)z3660_prof_cnt[Z3660_PROF_C_ATC_MISS], 1, "...which walked the tables");
	CHECK_EQ32((uae_u32)z3660_prof_cnt[Z3660_PROF_C_ATC_HIT], 0,
	           "...and was NOT an ATC hit -- the v1 counter said it was");
	CHECK_EQ32((uae_u32)z3660_prof_cnt[Z3660_PROF_C_XLATE_OK], 1,
	           "...but it did return an address, which is what XLATE_OK counts");

	SAVE_EXCEPTION;
	TRY(t2) { p2 = mmu_translate(VA, 0, true, true, false, sz_long); }
	CATCH(t2) { faulted = true; (void)t2; } ENDTRY
	RESTORE_EXCEPTION;
	CHECK(!faulted, "second translate of the same page completed\n");
	CHECK_EQ32(p2, p1, "...to the same physical address");

	CHECK_EQ32((uae_u32)z3660_prof_cnt[Z3660_PROF_C_XLATE], 2, "two calls now");
	CHECK_EQ32((uae_u32)z3660_prof_cnt[Z3660_PROF_C_ATC_MISS], 1,
	           "...still one walk: the second was served from the ATC");
	CHECK_EQ32((uae_u32)z3660_prof_cnt[Z3660_PROF_C_ATC_HIT], 1,
	           "...and THAT is the one ATC hit (v1 would have counted two)");
	CHECK_EQ32((uae_u32)z3660_prof_cnt[Z3660_PROF_C_XLATE_OK], 2, "both returned an address");

	/* The identity the C2 audit had to reconstruct by hand, now true by construction. */
	CHECK_EQ32((uae_u32)(z3660_prof_cnt[Z3660_PROF_C_ATC_HIT]
	                   + z3660_prof_cnt[Z3660_PROF_C_ATC_MISS]),
	           (uae_u32)z3660_prof_cnt[Z3660_PROF_C_XLATE],
	           "ATC_HIT + ATC_MISS == XLATE, no translate having faulted");

	mmu040_off();
	z3660_prof_zero();
}

/* ==================================================================
 * IFETCH_CALLS, and the map hypothesis it closes (C2 riding item b).
 *
 * The C2 landing model's +10.25 % transition overshoot had one surviving candidate: that
 * `FETCH` counts instruction-stream WORDS while the shared FETCHOP/FETCHEX bracket is
 * entered once per accessor CALL, so a longword fetch would be two words and one entry.
 * If that held, the model over-charged probe cycles to the two ifetch buckets and rung
 * 1's share was understated by up to 4.3 points.
 *
 * It does not hold, and this is where that is asserted rather than argued. Every one of
 * the five ENTER_IFETCH sites in the tree is followed immediately by exactly one
 * Z3660_PROF_CNT(C_FETCH), so the two are the same count -- a longword instruction fetch
 * increments FETCH once. The counter exists to make that an identity a metal capture
 * carries, and to fail HERE on the day someone adds a sixth fetch site to one path only.
 *
 * The word/long asymmetry is the whole point of the fixture: an iword and an ilong fetch
 * must both move both counters by exactly one, and a naive "FETCH counts words" reading
 * predicts two for the ilong.
 * ================================================================== */
static void test_prof_ifetch_calls(void)
{
	printf("[test] IFETCH_CALLS == FETCH: the shared ifetch bracket counts CALLS, not words\n");

	const uae_u32 VA = 0x00408000, PA = 0x0A000000;
	mmu040_map1(VA, PA, 0, false);
	regs.s = 1;
	hram_poke32(PA + 0x40, 0xDEADBEEFu);

	z3660_prof_set_buckets(0);
	z3660_prof_zero();

	bool faulted = false;
	uae_u16 w = 0; uae_u32 l = 0;
	SAVE_EXCEPTION;
	TRY(f1) { w = uae_mmu040_get_iword(VA + 0x40); } CATCH(f1) { faulted = true; (void)f1; } ENDTRY
	RESTORE_EXCEPTION;
	CHECK(!faulted, "the word instruction fetch completed\n");
	CHECK_EQ32(w, 0xDEAD, "...and returned the mapped word");
	CHECK_EQ32((uae_u32)z3660_prof_cnt[Z3660_PROF_C_IFETCH_CALLS], 1,
	           "one word fetch == one bracket entry");
	CHECK_EQ32((uae_u32)z3660_prof_cnt[Z3660_PROF_C_FETCH], 1,
	           "...and one FETCH");

	SAVE_EXCEPTION;
	TRY(f2) { l = uae_mmu040_get_ilong(VA + 0x40); } CATCH(f2) { faulted = true; (void)f2; } ENDTRY
	RESTORE_EXCEPTION;
	CHECK(!faulted, "the LONG instruction fetch completed\n");
	CHECK_EQ32(l, 0xDEADBEEFu, "...and returned the mapped longword");
	CHECK_EQ32((uae_u32)z3660_prof_cnt[Z3660_PROF_C_IFETCH_CALLS], 2,
	           "a longword fetch is ONE bracket entry, not two");
	CHECK_EQ32((uae_u32)z3660_prof_cnt[Z3660_PROF_C_FETCH], 2,
	           "...and ONE FETCH -- so FETCH has never counted words");

	CHECK_EQ32((uae_u32)z3660_prof_cnt[Z3660_PROF_C_IFETCH_CALLS],
	           (uae_u32)z3660_prof_cnt[Z3660_PROF_C_FETCH],
	           "IFETCH_CALLS == FETCH identically: the map's recon-B is refuted by this source");

	/* And it is genuinely the BRACKET being counted, not a bystander: with the buckets
	 * armed the same two fetches must move the two ifetch buckets' transition count. */
	z3660_prof_zero();
	z3660_prof_set_buckets(1);
	z3660_prof_loop_reset();
	SAVE_EXCEPTION;
	TRY(f3) { (void)uae_mmu040_get_ilong(VA + 0x40); } CATCH(f3) { (void)f3; } ENDTRY
	RESTORE_EXCEPTION;
	CHECK_EQ32((uae_u32)z3660_prof_cnt[Z3660_PROF_C_IFETCH_CALLS], 1,
	           "the counter lives inside the bracket, so an armed run still counts it once");
	CHECK_EQ32((uae_u32)z3660_prof_cnt[Z3660_PROF_C_TRANSITIONS], 2,
	           "...and the bracket really opened and closed: one enter, one exit");
	CHECK_EQ32((uae_u32)z3660_prof_sp, 0, "the phase stack is back at the floor");

	z3660_prof_set_buckets(0);
	mmu040_off();
	z3660_prof_zero();
}

/* ==================================================================
 * CYBERSTORM C2-prep -- where the absolute ARM clock comes from.
 *
 * The measurable part (the PMCCNTR : global-timer RATIO) is fixed at 2:1 in silicon and
 * so says nothing about the PLL; the absolute rate has to be taken from the running
 * configuration. v1 took it from a compile-time BSP constant instead, and every rate and
 * duration it printed for a whole metal session was low by exactly 1.65 on a board
 * configured for 1100 MHz. The selection is a RULE, so it is testable here even though
 * the measurement around it -- which needs a global timer that runs -- is not.
 * ================================================================== */
static void test_prof_clock_source(void)
{
	printf("[test] profiler clock source: the runtime rate wins, and the dump says which\n");

	const uae_u32 BSP  = 333333343u;    /* COUNTS_PER_SECOND at the 666.67 MHz default */
	const uae_u32 REAL = 550000000u;    /* what an 1100 MHz board's timer really ticks  */
	uae_u32 src = 0xFFFFFFFFu;

	CHECK_EQ32(z3660_prof_pick_wall_hz(REAL, BSP, &src), REAL,
	           "a published runtime rate is believed over the compile-time constant");
	CHECK_EQ32(src, (uae_u32)Z3660_PROF_CLK_CFG, "...and is labelled clk=cfg");

	src = 0xFFFFFFFFu;
	CHECK_EQ32(z3660_prof_pick_wall_hz(0, BSP, &src), BSP,
	           "an unwritten mailbox word falls back to the BSP constant");
	CHECK_EQ32(src, (uae_u32)Z3660_PROF_CLK_BSP,
	           "...and is labelled clk=bsp, so a stale clock is visible IN THE CAPTURE");

	src = 0xFFFFFFFFu;
	CHECK_EQ32(z3660_prof_pick_wall_hz(999999u, BSP, &src), BSP,
	           "a sub-MHz 'rate' is a stale mailbox word, not a clock");
	CHECK_EQ32(src, (uae_u32)Z3660_PROF_CLK_BSP, "...and is labelled as the fallback it is");

	/* The 1.65 the C2 audit had to apply by hand, expressed as the thing that produced
	 * it -- so a future regression fails here rather than in a document. Stated over the
	 * CPU rates the two timer rates are halves of, because that is where the constant
	 * comes from: XPAR_..._CORE_CLOCK_FREQ_HZ against the configured arm_frequency.
	 * The exact ratio is 1.64999995 -- the BSP constant is 666666687, not 666666666 --
	 * so it is asserted rounded to two decimals rather than floored, which is also how
	 * every document that quotes "x1.65" arrived at the figure. */
	CHECK_EQ32((uae_u32)((((uae_u64)1100000000u * 100u) + 666666687u/2) / 666666687u), 165u,
	           "the C1 rig's error factor was 1100 MHz / the BSP's 666666687 = 1.65");
	CHECK_EQ32(REAL * 2u, 1100000000u, "...and the timer rate is exactly half the CPU rate");
}

static void test_prof_ring(void)
{
	printf("[test] profiler ring: wrap, drop accounting and sample weight\n");

	z3660_prof_set_buckets(0);
	z3660_prof_arm(1000);
	/* z3660_prof_arm() calibrated a period from the host's fake clock; pin it so the
	 * weight arithmetic below is arithmetic and not a measurement. */
	z3660_prof_period   = 100;
	prof_clock(0);
	z3660_prof_deadline = 100;

	/* An on-time sample: the deadline has just come due. */
	prof_clock(100);
	z3660_prof_sample_slow(0x08001234u, 0x4E71u, Z3660_PROF_F_SUPER | Z3660_PROF_F_CPU040);
	CHECK_EQ32((uae_u32)z3660_prof_deadline, 200, "the deadline re-arms from itself, so the rate does not drift");

	/* A LATE sample: 4 periods went by with the sampler unable to fire. The weight is the
	 * whole point of this instrument -- without it a mailbox round trip or a STOP spin
	 * would simply vanish from the profile instead of showing up as a heavy sample. */
	prof_clock(600);
	z3660_prof_sample_slow(0x08005678u, 0x4E75u, 0);
	CHECK_EQ32((uae_u32)z3660_prof_deadline, 700,
	           "and a late sample skips forward to the next future deadline rather than firing a burst");

	/* Weight must saturate, not wrap into the flag bits underneath it. */
	prof_clock(600 + 100 * 400);
	z3660_prof_sample_slow(0x08009ABCu, 0x0000u, Z3660_PROF_F_MMU);
	CHECK_EQ32((uae_u32)z3660_prof_cnt[Z3660_PROF_C_STACK_OVF], 0, "no stack damage from the ring path");

	/* Drop accounting. Fill past capacity and check that `drops` says how much of the run
	 * the dump cannot show -- an unreported wrap is a profile of the last few seconds
	 * presented as a profile of the whole run. */
	z3660_prof_zero();
	z3660_prof_period = 100;
	{
		uae_u32 i;
		for (i = 0; i < Z3660_PROF_RING_ENTRIES + 5000u; i++) {
			prof_clock(100 * (i + 1));
			z3660_prof_deadline = 100 * (i + 1);
			z3660_prof_sample_slow(0x08000000u + i * 2u, (uae_u16)(i & 0xFFFFu), 0);
		}
	}
	/* z3660_prof_samples / drops are file-static; the dump is where they surface, so
	 * exercise the real dump and let it assert its own consistency by not crashing on a
	 * wrapped ring -- the failure mode that matters here is an out-of-range slot index. */
	z3660_prof_dump_ring(4);
	z3660_prof_dump_stats();
	CHECK(1, "a wrapped ring dumps without walking off the end of the buffer\n");

	z3660_prof_stop();
}

static void test_prof_under_execution(void)
{
	printf("[test] profiler under real execution: counters move, and emulation does not\n");

	/* A 68040 program that reads, writes and fetches extension words.
	 *
	 * Setting currprefs.mmu_model is NOT enough on its own -- build_cpufunctbl() has to run
	 * again to select the cpuemu_31 column, and harness_set_x_funcs() to point x_prefetch
	 * at get_iword_mmu040. Without both, the run loop dispatches the NON-MMU handlers,
	 * which never touch uae_mmu040_* and so never trip a probe. That is exactly how the
	 * first version of this test failed: the counters stayed at zero and it looked like
	 * broken instrumentation rather than a fixture that never reached it. */
	cpu_bringup(68040);
	currprefs.cpu_model = 68040;
	currprefs.mmu_model = 68040;
	changed_prefs = currprefs;
	init_m68k();
	build_cpufunctbl();
	harness_set_x_funcs();
	regs.mmu_enabled = 0;                /* TC.E off -> the accessors are transparent */
	const uae_u32 CODE = 0x00010000u;
	const uae_u32 DATA = 0x00020000u;
	hram_poke32(DATA, 0x11223344u);
	hram_poke16(CODE + 0, 0x2039); hram_poke32(CODE + 2, DATA);        /* move.l DATA,d0 */
	hram_poke16(CODE + 6, 0x23C0); hram_poke32(CODE + 8, DATA + 8);    /* move.l d0,DATA+8 */
	hram_poke16(CODE + 12, 0x4E71);                                    /* nop            */

	/* ---- with the buckets ON ---- */
	z3660_prof_set_buckets(1);
	z3660_prof_zero();
	run040_steps(CODE, 3);
	uae_u32 with_d0 = (uae_u32)m68k_dreg(regs, 0);
	uae_u32 with_mem = hram_peek32(DATA + 8);

	CHECK_EQ32((uae_u32)z3660_prof_cnt[Z3660_PROF_C_INSNS], 3,
	           "INSNS counts instructions retired through the run-loop tail");
	CHECK(z3660_prof_cnt[Z3660_PROF_C_FETCH] >= 3,
	      "instruction-stream fetches are counted (opcode + extension words)\n");
	CHECK(z3660_prof_cnt[Z3660_PROF_C_READ] >= 1, "data reads are counted\n");
	CHECK(z3660_prof_cnt[Z3660_PROF_C_WRITE] >= 1, "data writes are counted\n");
	/* Deliberately NOT an assertion about cycles. On the host the cycle source is the
	 * injectable counter, and nothing advances it during real execution -- so every span
	 * measures zero here, and a test that demanded otherwise would be testing the fake
	 * clock. What IS testable, and is the thing that would actually break, is that the
	 * probes fire in matched pairs through thousands of real accessor calls and land the
	 * phase machine back at the floor. Absolute cycle attribution is a metal measurement;
	 * this is the structural half of it. */
	CHECK(z3660_prof_cnt[Z3660_PROF_C_TRANSITIONS] > 0,
	      "the probes fire on the real accessor path, not only on the synthetic one\n");
	CHECK_EQ32((uae_u32)z3660_prof_sp, 0,
	           "every enter has found its exit -- the phase stack is back at the floor");
	CHECK_EQ32((uae_u32)z3660_prof_cur, (uae_u32)Z3660_PROF_B_LOOP,
	           "...and the run loop is charging LOOP again, not stuck inside a stage");
	CHECK_EQ32((uae_u32)z3660_prof_cnt[Z3660_PROF_C_STACK_OVF], 0,
	           "real execution never overflows the phase stack (max nesting is four)");
	CHECK(z3660_prof_cnt[Z3660_PROF_C_INSNS_SUPER] <= z3660_prof_cnt[Z3660_PROF_C_INSNS],
	      "the supervisor share cannot exceed the whole\n");

	/* Opcode fetch and extension fetch share one accessor and are separated by a one-shot
	 * mark set in the run loop. If the mark ever leaks, FETCHOP would collect extension
	 * words too -- so assert the loop consumed it rather than left it standing. */
	CHECK_EQ32((uae_u32)z3660_prof_opfetch_hint, 0,
	           "the opcode-fetch mark is consumed, not left armed for the next extension word");

	/* ---- and with them OFF: same answers ---- */
	z3660_prof_set_buckets(0);
	hram_poke32(DATA + 8, 0);
	run040_steps(CODE, 3);
	CHECK_EQ32((uae_u32)m68k_dreg(regs, 0), with_d0,
	           "the profiler changes no register result");
	CHECK_EQ32(hram_peek32(DATA + 8), with_mem,
	           "...and no memory result: the probes measure the accessors, they do not replace them");
}
#endif /* Z3660_EMU_PROF */

/* ==================================================================
 * CYBERSTORM C2 RUNG 0 -- THE BLOCK-IDIOM FAST PATH, BY STATE EQUIVALENCE
 *
 * The map's instruction for this test is precise and it is not "spot checks": for a
 * corpus of start states, run the idiom once through the recognizer and once with the
 * recognizer out, and assert the register file, the affected memory, the fault outcome
 * and the guest pc are byte-identical.
 *
 * So that is what this is. Every case runs TWICE from an identical fixture -- once with
 * SHARED.blk_chunk = Z3660_BLK_OFF, which is a pure passthrough returning before the
 * recognizer has examined anything, and once with the chunk cap under test -- and the two
 * outcomes are compared as whole objects:
 *
 *   - all sixteen registers, the pc, regs.instruction_pc, the SR (via MakeSR, so it is
 *     the architectural word and not this host's flag representation), USP and ISP;
 *   - a ONE MEGABYTE window of guest physical memory covering everything the fixture can
 *     touch: the page tables, the code page, the vector table, the supervisor stack (and
 *     therefore any exception frame built on it), and both source and both destination
 *     pages. Compared with memcmp, so a single wrong byte anywhere in it fails -- including
 *     a U or M descriptor bit set by a walk one path made and the other did not.
 *
 * The memory window is why the layout below is contiguous and why the physical frames sit
 * next to the page tables: a comparison that only looks where the test expects damage is
 * the comparison that misses the page-crossing DMA that ate an ELF header.
 *
 * The corpus the dispatch requires, and what each case is actually for:
 *
 *   n = 0, n = 1     the DBcc boundary. DBF from Dn.w == 0 runs the body ONE more time;
 *                    an off-by-one here is invisible in a large fill and corrupts exactly
 *                    four bytes past the end of every buffer in the system.
 *   page crossing    both pages mapped: the fill must stop at the boundary, re-validate
 *                    and continue, writing both pages identically.
 *   second page      UNMAPPED: the 68040 write-back contract. Same fault pc, same faulted
 *   unmapped         address, same A0/D1, same stacked frame -- compared as memory, so the
 *                    frame is compared field by field without naming its fields.
 *   non-RAM dest     the device gate: an emulated-device destination must decline to the
 *                    interpreted path, and the guest must not be able to tell.
 *   interrupt        the poll must still run mid-block. Measured, not asserted: the board
 *   pending          stub counts every check_uae_int_request(), so "yields every `cap`
 *                    iterations" is a number the test checks against `cap`.
 *   misaligned       must decline (and still be correct).
 *   supervisor/user  both, because the dpagecache is indexed by regs.s.
 *
 * Cap 1 and cap 1024 are run against the same cases as the default 256: a fast path whose
 * answer depends on its chunk size has a boundary bug, and the cheapest way to find one is
 * to move the boundary.
 * ================================================================== */
#include "z3660_blk.h"
extern "C" unsigned z3660_blk_get(void);
extern "C" unsigned z3660_blk_set(unsigned requested);

/* Physical layout. Everything inside ONE megabyte starting at the generator's root table,
 * so a single memcmp covers tables, code, vectors, stack and data. */
#define BLKW_BASE   0x00180000u          /* == M4G_ROOT */
#define BLKW_LEN    0x00100000u
#define BLK_PCODE   0x00190000u
#define BLK_PVEC    0x00191000u
#define BLK_PCODE2  0x00193000u          /* the code page AFTER BLK_PCODE, for a straddling idiom */
#define BLK_PSTK    0x00192000u
#define BLK_PSRC0   0x001A0000u
#define BLK_PSRC1   0x001A1000u
#define BLK_PDST0   0x001B0000u
#define BLK_PDST1   0x001B1000u

#define BLK_VCODE   0x00400000u
#define BLK_VVEC    0x00420000u          /* deliberately NOT adjacent to BLK_VCODE: the
                                          * page after the code page has to be genuinely
                                          * absent for the straddle case to mean anything */
#define BLK_VSTK    0x40001000u
#define BLK_VSRC    0x00500000u
#define BLK_VDST    0x00600000u

/* DONE is code_off + 6 (stages 1/2) or + 10 (stage 3); computed in blk_run. */
#define BLK_FAULTPC (BLK_VCODE + 0x100u)

struct blk_snap {
	uae_u32 d[8], a[8];
	uae_u32 pc, ipc, sr, usp, isp;
	unsigned long polls;
	int steps;
	int reached;                 /* 0 = ran out of budget, 1 = DONE, 2 = fault handler */
	uae_u8 mem[BLKW_LEN];
};
/* File scope, not automatic: two of these are 2 MB and the harness has no business
 * putting that on the stack. */
static struct blk_snap g_blk_ref, g_blk_fast;

/* A destination bank that is NOT plain RAM. Same accessors as the harness RAM -- the
 * bytes must land in the same place, or the comparison would be measuring the bank and
 * not the fast path -- but without ABFLAG_RAM, which is the only thing the device gate
 * looks at. */
static uae_u32 REGPARAM2 blkio_lget(uaecptr a){ return hram_peek32(a); }
static uae_u32 REGPARAM2 blkio_wget(uaecptr a){ return hram_peek16(a); }
static uae_u32 REGPARAM2 blkio_bget(uaecptr a){ return hram_peek8(a); }
static void REGPARAM2 blkio_lput(uaecptr a, uae_u32 v){ hram_poke32(a, v); }
static void REGPARAM2 blkio_wput(uaecptr a, uae_u32 v){ hram_poke16(a, (uae_u16)v); }
static void REGPARAM2 blkio_bput(uaecptr a, uae_u32 v){ hram_poke8 (a, (uae_u8)v); }
static addrbank blk_io_bank = {
	blkio_lget, blkio_wget, blkio_bget,
	blkio_lput, blkio_wput, blkio_bput,
	NULL, NULL, NULL, _T("blkio"), _T("Harness non-RAM"),
	blkio_lget, blkio_wget,
	ABFLAG_IO, 0, 0
};

struct blk_desc {
	int      stage;      /* 1 = bzero, 2 = mcpy, 3 = CPUSHL loop */
	uae_u32  n;          /* Dn.w at loop entry: the body runs n+1 times */
	uae_u32  dst_off;    /* byte offset of the destination pointer into its first page */
	uae_u32  src_off;
	int      map_dst2;   /* map the second destination page? */
	int      map_src2;
	int      dst_io;     /* point the destination's bank at blk_io_bank */
	int      user;       /* run in user mode */
	int      prewarm;    /* establish the page-cache entries BEFORE the loop starts */
	uae_u32  code_off;   /* where in the code page the idiom is planted */
	int      map_code2;  /* map the SECOND code page (for a straddling idiom) */
	int      badloop;    /* plant a DBcc displacement that does NOT target the head */
	int      notdbf;     /* plant something that is not a DBcc after the MOVE */
	uae_u16  dbcc;       /* override the DBcc opcode (0 = DBF, the idiom's own) */
	int      adda_reg;   /* stage 3: make the ADDA advance a DIFFERENT register (1-7) */
	int      trace;      /* run with T1 set: every instruction must be traced */
	uae_s16  step;       /* stage 3: the ADDA.W immediate */
	uae_u32  fill;       /* stage 1: the value in A1 */
};

static void blk_build(const struct blk_desc *c)
{
	mmu040_gen_init();                       /* resets RAM, brings up 68040 + MMU, S=1 */

	mmu040_gen_map(BLK_VCODE, BLK_PCODE, 0);
	if (c->map_code2) mmu040_gen_map(BLK_VCODE + 0x1000u, BLK_PCODE2, 0);
	mmu040_gen_map(BLK_VVEC,  BLK_PVEC,  0);
	mmu040_gen_map(BLK_VSTK,  BLK_PSTK,  0);
	mmu040_gen_map(BLK_VSRC,             BLK_PSRC0, 0);
	if (c->map_src2) mmu040_gen_map(BLK_VSRC + 0x1000u, BLK_PSRC1, 0);
	mmu040_gen_map(BLK_VDST,             BLK_PDST0, 0);
	if (c->map_dst2) mmu040_gen_map(BLK_VDST + 0x1000u, BLK_PDST1, 0);

	/* Source pages get a pattern that is wrong everywhere if a copy shifts by one. */
	for (uae_u32 i = 0; i < 0x2000u; i += 4)
		hram_poke32(BLK_PSRC0 + i, 0x5EED0000u | i);
	/* Destinations get a different one, so a MISSING write is as visible as a wrong one. */
	for (uae_u32 i = 0; i < 0x2000u; i += 4)
		hram_poke32(BLK_PDST0 + i, 0xD00D0000u | i);

	/* Plant by VIRTUAL offset from the head, so an idiom that straddles the code page
	 * lands correctly in the second frame instead of wrapping inside the first. */
	{
		#define BLK_POKE(off, w)                                                     \
			do {                                                                     \
				uae_u32 _a = c->code_off + (uae_u32)(off);                           \
				if (_a < 0x1000u) hram_poke16(BLK_PCODE  + _a, (uae_u16)(w));         \
				else              hram_poke16(BLK_PCODE2 + _a - 0x1000u, (uae_u16)(w)); \
			} while (0)
		if (c->stage == 3) {
			BLK_POKE(0x00, 0xF468);                   /* CPUSHL (A0)   */
			BLK_POKE(0x02, 0xD0FC | ((uae_u16)c->adda_reg << 9)); /* ADDA.W #n,An */
			BLK_POKE(0x04, (uae_u16)c->step);
			BLK_POKE(0x06, 0x51C9);                   /* DBF D1,-8     */
			BLK_POKE(0x08, 0xFFF8);
			BLK_POKE(0x0A, 0x4E71);                   /* DONE: nop     */
		} else {
			BLK_POKE(0x00, c->stage == 1 ? 0x20C9 : 0x22D8);
			if (c->notdbf) {
				BLK_POKE(0x02, 0x4E71);               /* nop -- not a DBcc at all */
				BLK_POKE(0x04, 0x4E71);               /* nop */
			} else {
				BLK_POKE(0x02, c->dbcc ? c->dbcc : 0x51C9);   /* DBF D1,... */
				BLK_POKE(0x04, c->badloop ? 0xFFFE    /* -> the DBcc itself */
				                          : 0xFFFC);  /* -> the head: a loop */
			}
			BLK_POKE(0x06, 0x4E71);                   /* DONE: nop     */
		}
		#undef BLK_POKE
	}
	hram_poke16(BLK_PCODE + 0x100, 0x60FE);               /* FAULT: bra.s . */
	hram_poke32(BLK_PVEC | 8,    BLK_FAULTPC);            /* vector 2: access fault */
	hram_poke32(BLK_PVEC | 0x24, BLK_FAULTPC);            /* vector 9: trace         */

	cpu_set_context(BLK_VCODE + c->code_off);
	regs.vbr = BLK_VVEC;
	regs.s = 1;
	m68k_areg(regs, 7) = BLK_VSTK | 0xF00;

	/* POISON THE WHOLE REGISTER FILE AND THE CCR before every run.
	 *
	 * The two runs of a case are sequential on one machine, so anything the fixture does
	 * not set is inherited -- and what the first (reference) run leaves behind is exactly
	 * the state the second run is being checked for producing. That makes a fast path
	 * which FORGETS to write a register agree perfectly with the interpreter that wrote
	 * it, and the comparison passes for the worst possible reason. Caught by mutation
	 * testing: deleting stage A's CLEAR_CZNV/SET_ZFLG/SET_NFLG left the whole corpus
	 * green, because the reference run had already put those exact flags there.
	 *
	 * Every register that the idiom does not itself set gets a distinct recognisable
	 * value, so a fast path that scribbles on an unrelated register fails too; and the
	 * CCR starts with EVERY flag set, so "did not update the flags" is maximally
	 * different from every correct answer (the MOVE clears C and V and sets Z/N from the
	 * source, so no correct outcome leaves all four standing). */
	for (int i = 0; i < 8; i++) {
		m68k_dreg(regs, i) = 0xD0D00000u | (uae_u32)i;
		if (i != 7) m68k_areg(regs, i) = 0xA0A00000u | (uae_u32)i;
	}
	CLEAR_CZNV();
	SET_CFLG(1); SET_VFLG(1); SET_ZFLG(1); SET_NFLG(1); SET_XFLG(1);
	if (c->trace) { regs.t1 = 1; }
	MakeSR();
	MakeFromSR();            /* -> SPCFLAG_TRACE, the way a guest arming T1 would */

	m68k_areg(regs, 0) = (c->stage == 2) ? (BLK_VSRC + c->src_off)
	                                     : (BLK_VDST + c->dst_off);
	m68k_areg(regs, 1) = (c->stage == 1) ? c->fill
	                                     : (BLK_VDST + c->dst_off);
	if (c->stage == 3) m68k_areg(regs, 0) = BLK_VDST + c->dst_off;
	m68k_dreg(regs, 1) = 0xABCD0000u | c->n;   /* the high half must survive the DBcc */
	MakeSR();

	if (c->dst_io)
		mem_banks[BLK_PDST0 >> 16] = &blk_io_bank;

	if (c->user) {
		regs.usp = BLK_VSTK | 0xE00;
		regs.s = 0;
		MakeSR();
		m68k_areg(regs, 7) = regs.usp;
	}

	/* PRE-WARM: touch the pages from the host side so their data-page-cache entries
	 * already exist when the loop starts.
	 *
	 * This is not a convenience, it is the case that makes the hoisted loop invariants
	 * observable. Cold, the device gate declines the first iteration of every page, the
	 * INTERPRETER runs it, and the flags/pc/mmu_restart it sets are the ones the whole
	 * rest of the loop needs -- so a fast path that forgot to set any of them would still
	 * come out right, and the corpus would agree about the wrong thing. Warm, the
	 * recognizer engages on the very first pass and has to establish that state itself.
	 *
	 * Done through the ordinary accessors, at the same privilege the run will use (hence
	 * after the user-mode switch, since the cache is indexed by regs.s), writing back the
	 * value already there so memory is unchanged. The U/M descriptor bits it sets are set
	 * identically in both runs of a case, so the memory comparison stays exact.
	 *
	 * It is also the REALISTIC case: a kernel bzero'ing a page it has just touched, or
	 * mcpy'ing between two buffers it has just written, arrives with the entries warm. */
	if (c->prewarm) {
		SAVE_EXCEPTION;
		TRY(pw) {
			uaecptr d = BLK_VDST + c->dst_off;
			put_long_mmu040(d, get_long_mmu040(d));
			if (c->stage == 2) (void)get_long_mmu040(BLK_VSRC + c->src_off);
		} CATCH(pw) { (void)pw; } ENDTRY
		RESTORE_EXCEPTION;
	}
}

static void blk_take(struct blk_snap *s, unsigned long polls0, int steps, int reached)
{
	MakeSR();
	for (int i = 0; i < 8; i++) { s->d[i] = m68k_dreg(regs, i); s->a[i] = m68k_areg(regs, i); }
	s->pc  = m68k_getpc();
	s->ipc = (uae_u32)regs.instruction_pc;
	s->sr  = (uae_u32)regs.sr;
	s->usp = (uae_u32)regs.usp;
	s->isp = (uae_u32)regs.isp;
	s->polls = harness_ipl_polls - polls0;
	s->steps = steps;
	s->reached = reached;
	memcpy(s->mem, hram_base() + BLKW_BASE, BLKW_LEN);
}

/* Build, set the knob THROUGH THE MAILBOX (do_specialties resyncs the knobs every 64th
 * call, so a directly-poked variable would be clobbered mid-run), drive to completion or
 * to the fault handler, snapshot. */
static void blk_run(struct blk_snap *s, const struct blk_desc *c, unsigned knob, int budget)
{
	const uae_u32 done = BLK_VCODE + c->code_off + ((c->stage == 3) ? 0x0Au : 0x06u);
	unsigned long polls0;
	int steps = 0, reached = 0;

	addrbank *saved_bank = mem_banks[BLK_PDST0 >> 16];

	blk_build(c);
	shared->blk_chunk = knob;
	z3660_knobs_sync();

	polls0 = harness_ipl_polls;
	regs.spcflags |= Z_SPCFLAG_MODE_CHANGE;  /* the loop returns after each pass; OR, so
	                                          * an armed SPCFLAG_TRACE survives */
	while (steps < budget) {
		uae_u32 pc = m68k_getpc();
		if (pc == done)        { reached = 1; break; }
		if (pc == BLK_FAULTPC) { reached = 2; break; }
		m68k_run_mmu040();
		steps++;
	}
	regs.spcflags = 0;

	blk_take(s, polls0, steps, reached);

	/* Leave the fixture as the firmware powers up. */
	mem_banks[BLK_PDST0 >> 16] = saved_bank;
	shared->blk_chunk = Z3660_BLK_UNSET;
	z3660_knobs_sync();
	mmu040_off();
}

static void blk_equal(const struct blk_snap *r, const struct blk_snap *f, const char *what)
{
	int bad = 0;
	for (int i = 0; i < 8; i++) {
		if (r->d[i] != f->d[i]) {
			g_fail++; bad++;
			printf("  FAIL: %s: D%d ref 0x%08x fast 0x%08x\n", what, i, r->d[i], f->d[i]);
		}
		if (r->a[i] != f->a[i]) {
			g_fail++; bad++;
			printf("  FAIL: %s: A%d ref 0x%08x fast 0x%08x\n", what, i, r->a[i], f->a[i]);
		}
	}
	if (!bad) g_pass++;
	CHECK_EQ32(f->pc,      r->pc,      what);
	CHECK_EQ32(f->ipc,     r->ipc,     what);
	CHECK_EQ32(f->sr,      r->sr,      what);
	CHECK_EQ32(f->usp,     r->usp,     what);
	CHECK_EQ32(f->isp,     r->isp,     what);
	CHECK_EQ32((uae_u32)f->reached, (uae_u32)r->reached, what);
	/* Anti-vacuity: two runs that both ran out of budget agree perfectly and prove
	 * nothing. Every case must actually arrive somewhere. */
	CHECK(r->reached != 0, "%s: the reference run finished (DONE or the fault handler), "
	                       "so the comparison is not of two timeouts\n", what);
	{
		int off = -1;
		for (uae_u32 i = 0; i < BLKW_LEN; i++)
			if (r->mem[i] != f->mem[i]) { off = (int)i; break; }
		if (off < 0) g_pass++;
		else {
			g_fail++;
			printf("  FAIL: %s: guest memory differs at physical 0x%08x "
			       "(ref 0x%02x, fast 0x%02x)\n",
			       what, (unsigned)(BLKW_BASE + (uae_u32)off),
			       r->mem[off], f->mem[off]);
		}
	}
}

/* One corpus entry: run it with the recognizer OFF and with `cap`, and demand they agree.
 *
 * `engage` says what the run-loop pass count must show, because "the two paths agree" is
 * trivially true when the fast path silently never fired -- the failure a state-
 * equivalence test is blindest to:
 *
 *   BLK_MUST    strictly fewer passes than the interpreter. The recognizer HAS to fire.
 *   BLK_MAY     no MORE passes. Used where the whole loop is one iteration: the device
 *               gate declines on a cold page, the interpreter runs that iteration and
 *               warms the entry, and the loop is over before there is anything left to
 *               fast-path. Correct, and the `<=` is the useful claim in its own right --
 *               the recognizer must never make anything SLOWER, and a declined recognizer
 *               that ate a pass would show up right here.
 *   BLK_NEVER   exactly as many passes. The decline cases: non-RAM, misaligned, user-mode
 *               CPUSHL. These must reach the interpreter untouched.
 */
#define BLK_NEVER 0
#define BLK_MAY   1
#define BLK_MUST  2
static void blk_case(const struct blk_desc *c, unsigned cap, int engage,
                     const char *what)
{
	char lbl[192];
	int budget = (int)(c->n + 2u) * 4 + 64;

	blk_run(&g_blk_ref,  c, Z3660_BLK_OFF, budget);
	blk_run(&g_blk_fast, c, cap,           budget);

	snprintf(lbl, sizeof lbl, "%s (cap %u)", what, cap);
	blk_equal(&g_blk_ref, &g_blk_fast, lbl);

	if (engage == BLK_MUST)
		CHECK(g_blk_fast.steps < g_blk_ref.steps,
		      "%s: the fast path took FEWER run-loop passes than the interpreter "
		      "(%d vs %d) -- it actually engaged\n", lbl, g_blk_fast.steps, g_blk_ref.steps);
	else if (engage == BLK_MAY)
		CHECK(g_blk_fast.steps <= g_blk_ref.steps,
		      "%s: the recognizer never costs a pass (%d vs %d)\n",
		      lbl, g_blk_fast.steps, g_blk_ref.steps);
	else
		CHECK_EQ32((uae_u32)g_blk_fast.steps, (uae_u32)g_blk_ref.steps, lbl);
}

/* ==================================================================
 * CYBERSTORM C2 RUNG 2 -- THE VERDICT TABLE, REPLAYED EXHAUSTIVELY
 *
 * The fast path replaces an out-of-line call to mmu_match_ttr() with one load out of a
 * 2 x 256 table. The table's build (cpummu.cpp, z3660_dfast_rebuild) is written as a
 * transcription of mmu_do_match_ttr(), and a transcription is exactly the kind of thing
 * that is right in the four cases someone thought of.
 *
 * So this does not sample. For each register configuration it walks the WHOLE (super x
 * top-byte) space -- 512 points -- and demands that the table agree with the live
 * mmu_match_ttr(), verdict AND installed cache state. 512 points is small enough to be
 * exhaustive and that is the entire argument for indexing the table this way in the
 * first place: the match depends on the address only through its top byte.
 * ================================================================== */
static void test_dfast_verdict_table(void)
{
	printf("[test] C2 accessor fast path: the verdict table replays mmu_match_ttr exactly\n");

	static const struct { uae_u32 dtt0, dtt1; const char *what; } CFG[] = {
		{ 0, 0, "no window enabled" },
		/* The AMIX-040 kernel's own registers, from pstart040.s. The configuration this
		 * rung was designed against, so it is in the corpus by name rather than by luck. */
		{ 0x003fc060u, 0x807fa060u, "AMIX-040: DTT0 0-1GB both, DTT1 2GB+ supervisor" },
		{ 0x003fc000u, 0, "0-1GB, S-field enabled (matches both), copyback" },
		{ 0x0A000000u | MMU_TTR_BIT_ENABLED | MMU_TTR_BIT_SFIELD_ENABLED, 0,
		  "one 16MB window, both privilege levels" },
		{ 0x0A000000u | MMU_TTR_BIT_ENABLED, 0, "one 16MB window, USER only (S=00)" },
		{ 0x0A000000u | MMU_TTR_BIT_ENABLED | MMU_TTR_BIT_SFIELD_SUPER, 0,
		  "one 16MB window, SUPERVISOR only (S=01)" },
		{ 0x0A000000u | MMU_TTR_BIT_ENABLED | MMU_TTR_BIT_SFIELD_ENABLED
		    | MMU_TTR_BIT_WRITE_PROTECT, 0, "write-protected window" },
		{ 0x0A000000u | MMU_TTR_BIT_ENABLED | MMU_TTR_BIT_SFIELD_ENABLED
		    | MMU_TTR_CACHE_DISABLE, 0, "cache-inhibited window" },
		{ 0x0A000000u | MMU_TTR_BIT_ENABLED | MMU_TTR_BIT_SFIELD_ENABLED
		    | MMU_TTR_CACHE_MODE, 0, "copyback window" },
		/* Both windows live, and OVERLAPPING, so the "dtt0 first, dtt1 only on a
		 * no-match" ordering is exercised rather than assumed. */
		{ 0x0A000000u | MMU_TTR_BIT_ENABLED | MMU_TTR_BIT_SFIELD_ENABLED,
		  0x0A000000u | MMU_TTR_BIT_ENABLED | MMU_TTR_BIT_SFIELD_ENABLED
		    | MMU_TTR_BIT_WRITE_PROTECT, "overlapping windows: dtt0 must win" },
		/* dtt0 disabled but dtt1 live: the fall-through must actually fall through. */
		{ 0, 0x0C000000u | MMU_TTR_BIT_ENABLED | MMU_TTR_BIT_SFIELD_ENABLED,
		  "dtt0 off, dtt1 on" },
		/* A full-mask window: everything matches. */
		{ 0x00ff0000u | MMU_TTR_BIT_ENABLED | MMU_TTR_BIT_SFIELD_ENABLED, 0,
		  "mask 0xff: the whole address space is transparent" },
	};
	const int NCFG = (int)(sizeof CFG / sizeof CFG[0]);
	int c, mmuon, super, msb, mismatched = 0, checked = 0;

	mmu040_gen_init();

	for (mmuon = 1; mmuon >= 0; mmuon--) {
		mmu_set_tc((uae_u16)(mmuon ? 0x8000 : 0));
		for (c = 0; c < NCFG; c++) {
			regs.dtt0 = CFG[c].dtt0;
			regs.dtt1 = CFG[c].dtt1;
			mmu_tt_modified();                 /* rebuilds the table */
			for (super = 0; super < 2; super++) {
				for (msb = 0; msb < 256; msb++) {
					uae_u32 addr = (uae_u32)msb << 24;
					uae_u16 vr = z3660_dfv_r[super][msb];
					uae_u16 vw = z3660_dfv_w[super][msb];
					int live, want_r, want_w;
					uae_u8 live_cs;

					mmu_cache_state = 0xEE;
					live = mmu_match_ttr(addr, super != 0, true);
					live_cs = mmu_cache_state;

					if (live == TTR_OK_MATCH) {
						want_r = Z3660_DFAST_XPAR; want_w = Z3660_DFAST_XPAR;
					} else if (live == TTR_NO_WRITE) {
						want_r = Z3660_DFAST_XPAR; want_w = Z3660_DFAST_SLOW;
					} else if (mmuon) {
						want_r = Z3660_DFAST_PAGE; want_w = Z3660_DFAST_PAGE;
					} else {
						want_r = Z3660_DFAST_SLOW; want_w = Z3660_DFAST_SLOW;
					}
					checked++;
					if ((int)Z3660_DFAST_VERDICT(vr) != want_r ||
					    (int)Z3660_DFAST_VERDICT(vw) != want_w) {
						if (!mismatched)
							printf("  FAIL: %s: mmuon=%d super=%d msb=%02x live=%d "
							       "table r=%u w=%u want r=%d w=%d\n",
							       CFG[c].what, mmuon, super, msb, live,
							       (unsigned)Z3660_DFAST_VERDICT(vr),
							       (unsigned)Z3660_DFAST_VERDICT(vw), want_r, want_w);
						mismatched++;
					}
					/* The cache state a match installs is the OTHER half of the
					 * equivalence and the half a transcription is likeliest to get
					 * wrong -- it is three bits of CM decoded into three constants. */
					if (live == TTR_OK_MATCH || live == TTR_NO_WRITE) {
						if (Z3660_DFAST_CSTATE(vr) != live_cs) {
							if (!mismatched)
								printf("  FAIL: %s: msb=%02x cache state table=%02x "
								       "live=%02x\n", CFG[c].what, msb,
								       (unsigned)Z3660_DFAST_CSTATE(vr),
								       (unsigned)live_cs);
							mismatched++;
						}
					}
				}
			}
		}
	}
	CHECK(mismatched == 0,
	      "the table agrees with mmu_match_ttr over all %d (config x mmu-state x super x "
	      "top-byte) points (%d mismatches)\n", checked, mismatched);
	printf("       %d points checked across %d register configurations\n", checked, NCFG);

	/* And the disarm has to survive every one of those rebuilds: mmu_tt_modified() is
	 * called on each configuration above, so a rebuild that ignored the switch would
	 * silently re-arm the fast path in the middle of a reference run. */
	{
		int i, s, allslow = 1;
		z3660_dfast_arm(0);
		regs.dtt0 = 0x003fc060u; regs.dtt1 = 0x807fa060u;
		mmu_tt_modified();
		for (s = 0; s < 2; s++)
			for (i = 0; i < 256; i++)
				if (z3660_dfv_r[s][i] != Z3660_DFAST_SLOW ||
				    z3660_dfv_w[s][i] != Z3660_DFAST_SLOW) allslow = 0;
		CHECK(allslow, "a rebuild while disarmed still produces an all-SLOW table\n");
		z3660_dfast_arm(1);
	}

	regs.dtt0 = regs.dtt1 = 0;
	mmu_tt_modified();
	mmu040_off();
}

/* ==================================================================
 * CYBERSTORM C2 RUNG 2 -- THE ACCESSOR FAST PATH, BY STATE EQUIVALENCE
 *
 * The map's instruction, quoted: "state equivalence, not spot checks -- for a corpus of
 * start states, run each access inlined and out-of-line and assert register file,
 * affected memory, fault outcome and guest PC are byte-identical."
 *
 * The reference arm is not a second implementation: `z3660_dfast_arm(0)` rebuilds the
 * verdict tables all-SLOW, so every access falls through to the ORIGINAL accessor -- the
 * same text that ran before rung 2 existed. That is what makes this an equivalence test
 * rather than two new things agreeing with each other.
 *
 * AND IT PINS ENGAGEMENT, because "the two agree" is trivially true when the fast path
 * silently never fired -- the failure a state-equivalence test is blindest to. Every case
 * says whether the fast path MUST serve, MUST NOT serve, or may do either, and the
 * DFAST_HIT counter is checked against that. Half this corpus is decline cases; without
 * the engagement check they would all pass against a fast path that was never compiled in.
 * ================================================================== */

/* The whole observable outcome of one guest access. */
struct dfast_obs {
	uae_u32 val;              /* what a read returned                                  */
	int     faulted;          /* did it throw                                          */
	uae_u32 fault_addr;       /* regs.mmu_fault_addr                                   */
	uae_u32 pc;               /* m68k_getpc() afterwards -- the 68040 restarts the      *
	                           * instruction, so a fault must not have moved it         */
	uae_u32 wb3_status, wb2_status, wb3_data, wb2_address;
	uae_u32 mmu_ssw;
	uae_u8  cache_state;      /* mmu_cache_state: real state, and the field the fast    *
	                           * path is most likely to get wrong                       */
	uae_u32 mem[4];           /* the destination longwords after a write               */
	uae_u32 regs_d[8], regs_a[8];
	uae_u64 rhit, rmiss, whit, wmiss;   /* the page-cache counters MUST NOT MOVE       */
	uae_u64 dfast_hit;        /* the PROFILING build's counter (0 elsewhere)           */
	unsigned long serves;     /* the harness counter -- present in ALL three variants   */
	unsigned long io_hits;    /* did the access reach a device bank's functions        */
};

/* What the corpus case does. */
#define DF_RD_L 0
#define DF_RD_W 1
#define DF_RD_B 2
#define DF_WR_L 3
#define DF_WR_W 4
#define DF_WR_B 5

/* Whether the fast path is allowed / required to serve. */
#define DF_MUST   0   /* it has to fire: this is a hit case                            */
#define DF_NEVER  1   /* it must decline: fault, device, miss, TTR-write-protect        */

static const char *df_opname(int op)
{
	switch (op) {
	case DF_RD_L: return "read.l";  case DF_RD_W: return "read.w";  case DF_RD_B: return "read.b";
	case DF_WR_L: return "write.l"; case DF_WR_W: return "write.w"; default: return "write.b";
	}
}

/* One access, fully observed. `probe` is the address; `pobs` the physical window base to
 * snapshot afterwards (0 = do not snapshot memory). */
static void df_run(struct dfast_obs *o, int op, uae_u32 addr, uae_u32 val, uae_u32 pobs)
{
	int i;

	memset(o, 0, sizeof *o);
	mmu_cache_state = 0xEE;                 /* a poison the accessor must overwrite */
	regs.mmu_fault_addr = 0;
	regs.wb3_status = regs.wb2_status = 0;
	hdirect_io_hits = 0;
	z3660_dfast_serves = 0;
	z3660_prof_zero();

	SAVE_EXCEPTION;
	TRY(p) {
		switch (op) {
		case DF_RD_L: o->val = get_long_mmu040(addr); break;
		case DF_RD_W: o->val = get_word_mmu040(addr); break;
		case DF_RD_B: o->val = get_byte_mmu040(addr); break;
		case DF_WR_L: put_long_mmu040(addr, val);     break;
		case DF_WR_W: put_word_mmu040(addr, val);     break;
		default:      put_byte_mmu040(addr, val);     break;
		}
	} CATCH(p) { o->faulted = 1; (void)p; } ENDTRY
	RESTORE_EXCEPTION;

	o->fault_addr  = regs.mmu_fault_addr;
	o->pc          = m68k_getpc();
	o->wb3_status  = regs.wb3_status;
	o->wb2_status  = regs.wb2_status;
	o->wb3_data    = regs.wb3_data;
	o->wb2_address = regs.wb2_address;
	o->mmu_ssw     = regs.mmu_ssw;
	o->cache_state = mmu_cache_state;
	o->io_hits     = hdirect_io_hits;
	for (i = 0; i < 8; i++) { o->regs_d[i] = m68k_dreg(regs, i); o->regs_a[i] = m68k_areg(regs, i); }
	if (pobs)
		for (i = 0; i < 4; i++) o->mem[i] = hdirect_peek32(pobs + (uae_u32)i * 4);
#if Z3660_EMU_PROF
	/* The page-cache counters exist only in the profiling build (CACHE_HIT_COUNT), so
	 * the "hit rates must not move" half of the equivalence is checkable there and
	 * nowhere else. It is the half that catches a fast path which probes the cache and
	 * then declines -- which is exactly the defect this corpus found on its first run --
	 * so the profiling variant is where that claim is made, and the harness's own serve
	 * counter carries the engagement half in all three. */
	o->rhit  = z3660_prof_cnt[Z3660_PROF_C_DPAGE_RHIT];
	o->rmiss = z3660_prof_cnt[Z3660_PROF_C_DPAGE_RMISS];
	o->whit  = z3660_prof_cnt[Z3660_PROF_C_DPAGE_WHIT];
	o->wmiss = z3660_prof_cnt[Z3660_PROF_C_DPAGE_WMISS];
	o->dfast_hit = z3660_prof_cnt[Z3660_PROF_C_DFAST_HIT];
#endif
	o->serves    = z3660_dfast_serves;
}

static void df_equal(const struct dfast_obs *r, const struct dfast_obs *f, const char *what)
{
	int i;
	CHECK_EQ32(f->val,        r->val,        what);
	CHECK_EQ32((uae_u32)f->faulted, (uae_u32)r->faulted, what);
	CHECK_EQ32(f->fault_addr, r->fault_addr, what);
	CHECK_EQ32(f->pc,         r->pc,         what);
	CHECK_EQ32(f->wb3_status, r->wb3_status, what);
	CHECK_EQ32(f->wb2_status, r->wb2_status, what);
	CHECK_EQ32(f->wb3_data,   r->wb3_data,   what);
	CHECK_EQ32(f->wb2_address,r->wb2_address,what);
	CHECK_EQ32(f->mmu_ssw,    r->mmu_ssw,    what);
	CHECK_EQ32((uae_u32)f->cache_state, (uae_u32)r->cache_state, what);
	for (i = 0; i < 4; i++) CHECK_EQ32(f->mem[i], r->mem[i], what);
	for (i = 0; i < 8; i++) {
		CHECK_EQ32(f->regs_d[i], r->regs_d[i], what);
		CHECK_EQ32(f->regs_a[i], r->regs_a[i], what);
	}
	/* THE MAP'S OWN PRE-REGISTRATION, asserted here rather than left to metal: "the
	 * dpagecache hit rates must not move -- if they change, the change altered WHAT is
	 * being cached and the speed comparison is not like-for-like." The fast path shares
	 * the existing index and tag arithmetic precisely so this holds. */
	CHECK_EQ32((uae_u32)f->rhit,  (uae_u32)r->rhit,  what);
	CHECK_EQ32((uae_u32)f->rmiss, (uae_u32)r->rmiss, what);
	CHECK_EQ32((uae_u32)f->whit,  (uae_u32)r->whit,  what);
	CHECK_EQ32((uae_u32)f->wmiss, (uae_u32)r->wmiss, what);
}

/* One corpus entry: run it disarmed, then armed, demand agreement, and demand the
 * engagement the case declares. Both arms are run TWICE with the caches warmed the same
 * way, so a case whose first access is a page-cache miss is compared warm-against-warm. */
static void df_case(int op, uae_u32 addr, uae_u32 val, uae_u32 pobs, int engage,
                    const char *what)
{
	struct dfast_obs ref, fast;
	char lbl[192];
	int prev;

	snprintf(lbl, sizeof lbl, "%s %s @%08x", what, df_opname(op), (unsigned)addr);

	prev = z3660_dfast_arm(0);
	df_run(&ref, op, addr, val, pobs);      /* warm */
	df_run(&ref, op, addr, val, pobs);      /* observed */
	CHECK_EQ32((uae_u32)ref.serves, 0,
	           "the reference arm serves nothing from the fast path");

	z3660_dfast_arm(1);
	df_run(&fast, op, addr, val, pobs);     /* warm */
	df_run(&fast, op, addr, val, pobs);     /* observed */

	df_equal(&ref, &fast, lbl);

	if (engage == DF_MUST)
		CHECK(fast.serves == 1,
		      "%s: the fast path SERVED it (serves %lu, expected 1)\n", lbl, fast.serves);
	else
		CHECK(fast.serves == 0,
		      "%s: the fast path DECLINED it (serves %lu, expected 0)\n", lbl, fast.serves);
	/* In a profiling build the instrument's own counter must agree with the harness's.
	 * They are incremented on the same line for exactly this cross-check: a DFAST_HIT
	 * that disagreed would make every coverage figure in a metal dump a figure about
	 * something else. */
	if (z3660_prof_present())
		CHECK_EQ32((uae_u32)fast.dfast_hit, (uae_u32)fast.serves,
		           "DFAST_HIT agrees with the harness's own serve count");

	z3660_dfast_arm(prev);
}

static void test_dfast_state_equivalence(void)
{
	printf("[test] C2 accessor fast path: state equivalence against the un-flattened path\n");

	CHECK(harness_direct_ok(),
	      "the identity window is mapped -- without it every case below would DECLINE "
	      "and this corpus would test nothing\n");
	if (!harness_direct_ok()) return;

	/* Physical layout inside the identity window. Two adjacent pages so a page-crossing
	 * case has somewhere to cross INTO, and one page left unmapped. */
	const uae_u32 P0 = HDIRECT_BASE + 0x10000u;   /* data page 0 */
	const uae_u32 P1 = HDIRECT_BASE + 0x11000u;   /* data page 1 */
	const uae_u32 V0 = 0x00300000u;               /* kernel-ish VA for P0 */
	const uae_u32 V1 = 0x00301000u;               /* ... P1, contiguous     */
	const uae_u32 VU = 0x80800000u;               /* a USER VA              */
	const uae_u32 VNONE = 0x00310000u;            /* mapped to nothing      */

	/* ---- stage A: the ordinary hit, every size, supervisor ------------------------- */
	mmu040_gen_init();
	harness_direct_reset();
	harness_direct_map(1);
	mmu040_gen_map(V0, P0, 0);
	mmu040_gen_map(V1, P1, 0);
	hdirect_poke32(P0 + 0, 0xDEADBEEFu);
	hdirect_poke32(P0 + 4, 0x01020304u);
	regs.s = 1;

	df_case(DF_RD_L, V0 + 0, 0, P0, DF_MUST, "supervisor page-cache hit");
	df_case(DF_RD_W, V0 + 4, 0, P0, DF_MUST, "supervisor page-cache hit");
	df_case(DF_RD_B, V0 + 6, 0, P0, DF_MUST, "supervisor page-cache hit");
	df_case(DF_WR_L, V0 + 8, 0xA5A5F00Du, P0, DF_MUST, "supervisor page-cache hit");
	df_case(DF_WR_W, V0 + 12, 0x1234u,    P0, DF_MUST, "supervisor page-cache hit");
	df_case(DF_WR_B, V0 + 14, 0x77u,      P0, DF_MUST, "supervisor page-cache hit");

	/* THE LAST BYTE AND THE FIRST BYTE OF A PAGE. An aligned access cannot cross a page,
	 * which is the whole basis of "never fast-path across a page boundary" -- so the ends
	 * of a page must serve exactly like its middle rather than being special-cased. */
	df_case(DF_RD_L, V0 + 0xFFC, 0, P0, DF_MUST, "last aligned longword in the page");
	df_case(DF_RD_B, V0 + 0xFFF, 0, P0, DF_MUST, "last byte in the page");
	df_case(DF_RD_L, V1 + 0,     0, P1, DF_MUST, "first longword of the next page");

	/* ---- stage B: user space, and the supervisor/user tag split -------------------- */
	mmu040_gen_map(VU, P1, 0);
	regs.s = 0;
	df_case(DF_RD_L, VU + 0, 0, P1, DF_MUST, "user page-cache hit");
	df_case(DF_WR_L, VU + 4, 0xFEEDFACEu, P1, DF_MUST, "user page-cache hit");
	/* The same VA in the other privilege state is a DIFFERENT tag. V0 is mapped for
	 * both, but the entry cached under one `regs.s` must not answer for the other -- the
	 * fast path carries `| regs.s` in its tag exactly as the original does. */
	regs.s = 1;
	df_case(DF_RD_L, V0 + 0, 0, P0, DF_MUST, "supervisor after user (tag carries regs.s)");

	/* ---- stage C: misalignment -- the split path must not be fast-pathed ----------- */
	regs.s = 1;
	df_case(DF_RD_L, V0 + 1, 0, P0, DF_NEVER, "misaligned longword read (odd)");
	df_case(DF_RD_L, V0 + 2, 0, P0, DF_NEVER, "misaligned longword read (odd word)");
	df_case(DF_RD_W, V0 + 1, 0, P0, DF_NEVER, "misaligned word read");
	df_case(DF_WR_L, V0 + 2, 0x11223344u, P0, DF_NEVER, "misaligned longword write");
	df_case(DF_WR_W, V0 + 1, 0x5566u,     P0, DF_NEVER, "misaligned word write");

	/* THE MAP'S MANDATORY CASE: a misaligned access CROSSING a page boundary where the
	 * SECOND page is unmapped. The fault must arrive with the same fault address and the
	 * same guest PC, because the 68040 restarts the instruction. V1 is mapped, so unmap
	 * the page after it and straddle that edge. */
	df_case(DF_RD_L, V1 + 0xFFE, 0, P1, DF_NEVER,
	        "misaligned read crossing into an UNMAPPED page");
	df_case(DF_WR_L, V1 + 0xFFE, 0x99887766u, P1, DF_NEVER,
	        "misaligned write crossing into an UNMAPPED page");

	/* ---- stage D: faults ----------------------------------------------------------- */
	df_case(DF_RD_L, VNONE, 0, 0, DF_NEVER, "read of an unmapped page");
	df_case(DF_WR_L, VNONE, 0xCAFEBABEu, 0, DF_NEVER, "write to an unmapped page");

	/* Write-protected page: the read serves, the write must fault through the original
	 * path. Both arms of one page, which is the case a fast path that forgot to use the
	 * WRITE cache would pass anyway -- so the read is asserted MUST and the write NEVER. */
	{
		const uae_u32 VWP = 0x00320000u, PWP = HDIRECT_BASE + 0x12000u;
		mmu040_gen_map(VWP, PWP, MMU_DES_WP);
		hdirect_poke32(PWP, 0x0BADF00Du);
		df_case(DF_RD_L, VWP, 0, PWP, DF_MUST,  "write-protected page: the READ serves");
		df_case(DF_WR_L, VWP, 0xFFFFFFFFu, PWP, DF_NEVER,
		        "write-protected page: the WRITE faults through the original path");
	}

	/* Supervisor-only page accessed from user state. */
	{
		const uae_u32 VSO = 0x00330000u, PSO = HDIRECT_BASE + 0x13000u;
		mmu040_gen_map(VSO, PSO, MMU_DES_SUPER);
		hdirect_poke32(PSO, 0x5150F00Du);
		regs.s = 1;
		df_case(DF_RD_L, VSO, 0, PSO, DF_MUST,  "supervisor-only page, from supervisor");
		regs.s = 0;
		df_case(DF_RD_L, VSO, 0, PSO, DF_NEVER, "supervisor-only page, from USER: faults");
		regs.s = 1;
	}

	/* ---- stage E: not plain RAM ---------------------------------------------------- */
	/* The map: "an access to a destination that is NOT plain RAM must fall back rather
	 * than fast-path a device write." Same window, same page tables, same page-cache
	 * entries -- only the bank changes, so this isolates the bank test exactly. */
	harness_direct_map(0);
	{
		struct dfast_obs o;
		df_case(DF_RD_L, V0, 0, 0, DF_NEVER, "device bank: read declines");
		df_case(DF_WR_L, V0, 0x0D0E0F10u, 0, DF_NEVER, "device bank: write declines");
		/* ...and it really did reach the device's own accessor rather than being
		 * optimised away. Without this the DF_NEVER above is satisfied by an access that
		 * did not happen at all. */
		z3660_dfast_arm(1);
		df_run(&o, DF_WR_L, V0, 0x21222324u, 0);
		CHECK(o.io_hits > 0,
		      "the declined write reached the device bank's own put function (%lu calls)\n",
		      o.io_hits);
	}
	harness_direct_map(1);

	/* ---- stage F: transparent translation ------------------------------------------ */
	/* A DTT window over the identity window: the whole point of the rung's redesign, and
	 * the arm the AMIX-040 kernel puts ~30 % of its data accesses through. No page tables
	 * are consulted at all here, so this also proves the XPAR arm does not silently fall
	 * into the page cache. */
	regs.s = 1;
	regs.dtt0 = (HDIRECT_BASE & MMU_TTR_LOGICAL_BASE) | MMU_TTR_BIT_ENABLED
	          | MMU_TTR_BIT_SFIELD_ENABLED;
	regs.dtt1 = 0;
	mmu_tt_modified();
	mmu_flush_atc_all(true);
	hdirect_poke32(P0, 0x7E571460u);
	df_case(DF_RD_L, P0,     0,            P0, DF_MUST, "DTT0 transparent read");
	df_case(DF_WR_L, P0 + 4, 0x33445566u,  P0, DF_MUST, "DTT0 transparent write");
	df_case(DF_RD_B, P0 + 9, 0,            P0, DF_MUST, "DTT0 transparent byte read");
	df_case(DF_RD_L, P0 + 1, 0,            P0, DF_NEVER, "DTT0 transparent, misaligned");

	/* Write-protected TTR: reads transparent, writes must take the original path, which
	 * is where mmu_match_ttr_write() raises the fault. */
	regs.dtt0 |= MMU_TTR_BIT_WRITE_PROTECT;
	mmu_tt_modified();
	df_case(DF_RD_L, P0, 0, P0, DF_MUST, "write-protected DTT0: the READ serves");
	df_case(DF_WR_L, P0, 0x0u, P0, DF_NEVER,
	        "write-protected DTT0: the WRITE takes the original path");

	/* A supervisor-only DTT accessed from user state: the table's S-field replay has to
	 * agree with mmu_do_match_ttr()'s, per privilege level, per top byte. */
	regs.dtt0 = (HDIRECT_BASE & MMU_TTR_LOGICAL_BASE) | MMU_TTR_BIT_ENABLED
	          | MMU_TTR_BIT_SFIELD_SUPER;     /* S-field ENABLE bit clear => S is checked */
	mmu_tt_modified();
	mmu_flush_atc_all(true);
	regs.s = 1;
	df_case(DF_RD_L, P0, 0, P0, DF_MUST, "supervisor-only DTT0, from supervisor");
	regs.s = 0;
	/* From user the TTR does not match; there is no page mapping for this VA either, so
	 * the original path faults and the fast path must not have served anything. */
	df_case(DF_RD_L, P0, 0, P0, DF_NEVER, "supervisor-only DTT0, from USER");
	regs.s = 1;

	regs.dtt0 = regs.dtt1 = 0;
	mmu_tt_modified();

	/* ---- stage G: the invalidation ordering ---------------------------------------- */
	/* The map: "an access whose page is invalidated between the cache lookup and the
	 * use." There is no such window -- the lookup and the use are adjacent and nothing
	 * can run between them -- so what is testable, and what actually matters, is that an
	 * invalidation BEFORE the access is honoured: the fast path must not answer out of a
	 * page-cache entry that PFLUSH has retired. */
	{
		struct dfast_obs o;
		mmu040_gen_map(V0, P0, 0);
		hdirect_poke32(P0, 0x11111111u);
		z3660_dfast_arm(1);
		df_run(&o, DF_RD_L, V0, 0, P0);                    /* warm the entry */
		CHECK_EQ32(o.val, 0x11111111u, "warm read before the remap");

		/* Re-point the VA at the OTHER frame and flush. If the fast path answered from a
		 * stale entry it would return the old frame's contents. */
		hdirect_poke32(P1, 0x22222222u);
		mmu040_gen_map(V0, P1, 0);                          /* also flushes the ATC */
		df_run(&o, DF_RD_L, V0, 0, P1);
		CHECK_EQ32(o.val, 0x22222222u,
		           "after a remap+flush the fast path reads the NEW frame, not a stale entry");

		/* And a single-page PFLUSH, which is the narrower invalidation. */
		mmu040_gen_map(V0, P0, 0);
		mmu_flush_atc(V0, true, false);
		df_run(&o, DF_RD_L, V0, 0, P0);
		CHECK_EQ32(o.val, 0x11111111u, "and after a single-page PFLUSH");
	}

	/* ---- stage H: the MMU off ------------------------------------------------------ */
	/* Pre-MMU boot. The fast path declines outright on this arm (z3660_dfast.h says why:
	 * cache_default_data is not constant there and the write side can still raise a
	 * TCR_DWO bus error), so what is asserted is that it declines and that the machine
	 * still works. */
	{
		regs.dtt0 = regs.dtt1 = 0;
		mmu_tt_modified();
		mmu_set_tc(0);
		harness_direct_map(1);
		hdirect_poke32(P0, 0x0FF0FF00u);
		df_case(DF_RD_L, P0, 0, P0, DF_NEVER, "MMU off: the fast path declines");
		df_case(DF_WR_L, P0 + 4, 0x0BEEF00Du, P0, DF_NEVER, "MMU off: and on writes too");
	}

	/* ---- stage I: the disarm is a real reference ----------------------------------- */
	/* Every case above compares against the disarmed arm, so the disarm being subtly
	 * different from "rung 2 was never compiled in" would make the whole corpus agree
	 * about the wrong thing. Pin the two properties that make it the original machine:
	 * it serves nothing, and it leaves the verdict tables saying SLOW everywhere. */
	{
		int i, s, allslow = 1;
		z3660_dfast_arm(0);
		for (s = 0; s < 2; s++)
			for (i = 0; i < 256; i++)
				if (z3660_dfv_r[s][i] != Z3660_DFAST_SLOW ||
				    z3660_dfv_w[s][i] != Z3660_DFAST_SLOW) allslow = 0;
		CHECK(allslow, "disarmed: every verdict in both tables is SLOW\n");
		CHECK_EQ32((uae_u32)z3660_dfast_armed(), 0, "...and the switch reads back off");
		z3660_dfast_arm(1);
		CHECK_EQ32((uae_u32)z3660_dfast_armed(), 1, "re-arming restores it");
	}

	/* ---- stage J: the whole machine, not just the accessor -------------------------
	 *
	 * Everything above drives get_long_mmu040() and friends DIRECTLY, which is the right
	 * unit for an equivalence corpus but leaves one thing uncovered: the accessors are
	 * now considerably larger and inline a resolver that allocates registers, and they
	 * are called from ~3000 generated handler sites. A clobber that the direct calls
	 * survive -- because the harness holds nothing live across them -- could still eat a
	 * handler's operand.
	 *
	 * So: run real guest code through the run loop's own dispatch, with its DATA in the
	 * identity window so the fast path actually serves, once disarmed and once armed,
	 * and compare the whole visible machine. */
	{
		const uae_u32 VCODE = 0x00340000u, PCODE = HDIRECT_BASE + 0x14000u;
		const uae_u32 VDATA = 0x00341000u, PDATA = HDIRECT_BASE + 0x15000u;
		struct { uae_u32 d[8], a[8], pc, sr, mem[4]; int faulted; unsigned long serves; } arm[2];
		int pass, rep;

		for (pass = 0; pass < 2; pass++) {
			int i;
			mmu040_gen_init();
			harness_direct_reset();
			harness_direct_map(1);
			mmu040_gen_map(VCODE, PCODE, 0);
			mmu040_gen_map(VDATA, PDATA, 0);
			regs.s = 1;
			z3660_dfast_arm(pass);          /* 0 = reference, 1 = armed */

			/* moveq #$42,d0 / moveq #7,d1 / add.l d0,d1 / move.l d1,(a0) /
			 * move.l (a0),d2 / add.l d1,d2 / move.l d2,4(a0)
			 * -- a write, a read-back of what was written, and a second write at a
			 * different offset, all through a page the fast path serves. */
			hdirect_poke32(PCODE + 0,  0x70427207u);   /* moveq #$42,d0 ; moveq #7,d1    */
			hdirect_poke32(PCODE + 4,  0xd2802081u);   /* add.l d0,d1   ; move.l d1,(a0) */
			hdirect_poke32(PCODE + 8,  0x2410d481u);   /* move.l (a0),d2; add.l d1,d2    */
			hdirect_poke32(PCODE + 12, 0x21420004u);   /* move.l d2,4(a0)                */

			/* TWICE, and the SECOND pass is the one measured. The first is a cold
			 * cache: a write to a page whose write-cache entry does not exist yet is a
			 * miss, the fast path declines, and the original path fills it -- correct,
			 * and exactly one serve out of three accesses. A running guest is warm, so
			 * measuring the cold pass would understate engagement and hide a regression
			 * in the warm path behind it. Both passes run on both arms, so the
			 * state comparison is still like-for-like. */
			arm[pass].faulted = 0;
			for (rep = 0; rep < 2; rep++) {
				cpu_set_context(VCODE);
				for (i = 0; i < 8; i++) { m68k_dreg(regs, i) = 0; m68k_areg(regs, i) = 0; }
				m68k_areg(regs, 0) = VDATA;
				z3660_dfast_serves = 0;     /* so the count is the WARM pass's */

				SAVE_EXCEPTION;
				TRY(p) {
					for (i = 0; i < 7; i++) {
						regs.instruction_pc = m68k_getpc();
						regs.opcode = x_prefetch(0);
						(*cpufunctbl[regs.opcode])(regs.opcode);
					}
				} CATCH(p) { arm[pass].faulted = 1; (void)p; } ENDTRY
				RESTORE_EXCEPTION;
			}

			for (i = 0; i < 8; i++) { arm[pass].d[i] = m68k_dreg(regs, i);
			                          arm[pass].a[i] = m68k_areg(regs, i); }
			arm[pass].pc = m68k_getpc();
			arm[pass].sr = regs.sr;
			for (i = 0; i < 4; i++) arm[pass].mem[i] = hdirect_peek32(PDATA + (uae_u32)i * 4);
			arm[pass].serves = z3660_dfast_serves;
		}

		CHECK_EQ32((uae_u32)arm[0].serves, 0, "interpreted run, disarmed: nothing served");
		CHECK_EQ32((uae_u32)arm[1].serves, 3u,
		           "interpreted run, armed and warm: all THREE data accesses served");
		CHECK_EQ32((uae_u32)arm[1].faulted, (uae_u32)arm[0].faulted,
		           "interpreted run: same fault outcome");
		CHECK_EQ32(arm[1].pc, arm[0].pc, "interpreted run: same PC");
		CHECK_EQ32(arm[1].sr, arm[0].sr, "interpreted run: same SR");
		{
			int i;
			for (i = 0; i < 8; i++) {
				CHECK_EQ32(arm[1].d[i], arm[0].d[i], "interpreted run: same D register");
				CHECK_EQ32(arm[1].a[i], arm[0].a[i], "interpreted run: same A register");
			}
			for (i = 0; i < 4; i++)
				CHECK_EQ32(arm[1].mem[i], arm[0].mem[i], "interpreted run: same memory");
		}
		/* ...and the run actually computed something, so "identical" is not two
		 * identically-failed runs agreeing. */
		CHECK_EQ32(arm[1].d[1], 0x49u, "interpreted run: d1 = 0x42 + 7");
		CHECK_EQ32(arm[1].mem[0], 0x49u, "interpreted run: the store landed");
		CHECK_EQ32(arm[1].d[2], 0x92u,
		           "interpreted run: d2 = the READ-BACK of that store, plus d1");
		CHECK_EQ32(arm[1].mem[1], 0x92u, "interpreted run: and the second store landed");
	}

	mmu040_off();
	harness_direct_map(1);
	z3660_dfast_arm(1);
}

static void test_blk_state_equivalence(void)
{
	printf("[test] C2 block-idiom fast path: state equivalence against the interpreted path\n");

	struct blk_desc c;
	const unsigned CAPS[3] = { 1u, Z3660_BLK_DEFAULT, Z3660_BLK_MAX };

	/* ---- stage A: bzero ------------------------------------------------------- */
	for (int ci = 0; ci < 3; ci++) {
		unsigned cap = CAPS[ci];

		memset(&c, 0, sizeof c);
		c.stage = 1; c.map_dst2 = 1; c.fill = 0;

		c.n = 0;    blk_case(&c, cap, BLK_MAY,  "bzero n=0 (the DBcc boundary: ONE store, not zero)");
		c.n = 1;    blk_case(&c, cap, BLK_MUST, "bzero n=1");
		c.n = 7;    blk_case(&c, cap, BLK_MUST, "bzero n=7");
		c.n = 255;  blk_case(&c, cap, BLK_MUST, "bzero n=255 (exactly the default cap)");
		c.n = 1023; blk_case(&c, cap, BLK_MUST, "bzero n=1023 (a whole 4K page)");

		/* PRE-WARMED, so the recognizer engages on the FIRST pass with no interpreted
		 * iteration ahead of it to establish the loop invariants for it. Every case that
		 * follows is run this way as well as cold, because cold hides a whole class of
		 * defect: see blk_build()'s prewarm comment. */
		c.prewarm = 1;
		c.n = 0;    blk_case(&c, cap, BLK_MUST, "bzero n=0, page already WARM");
		c.n = 1;    blk_case(&c, cap, BLK_MUST, "bzero n=1, page already WARM");
		c.n = 1023; blk_case(&c, cap, BLK_MUST, "bzero n=1023, page already WARM");

		/* A non-zero fill: MOVE.L sets N and Z from the SOURCE. Warm, because cold the
		 * interpreter's own first iteration sets the flags and a fast path that forgets
		 * them passes every zero-fill test there is. */
		c.n = 5; c.fill = 0x80000000u;
		blk_case(&c, cap, BLK_MUST, "bzero with a NEGATIVE fill value (N flag from the source)");
		c.fill = 0x00000001u;
		blk_case(&c, cap, BLK_MUST, "bzero with a non-zero positive fill (Z clear)");
		c.fill = 0;
		c.prewarm = 0;

		c.n = 5; c.fill = 0x80000000u;
		blk_case(&c, cap, BLK_MUST, "bzero with a negative fill, page cold");
		c.fill = 0;

		/* Page crossing, both pages present: must stop at the boundary, re-validate
		 * and carry on. dst_off puts the last four longwords in page 0. */
		c.n = 31; c.dst_off = 0x1000u - 16u;
		blk_case(&c, cap, BLK_MUST, "bzero CROSSING a page boundary, second page mapped");

		/* Page crossing with the second page UNMAPPED: the 68040 write-back fault. */
		c.map_dst2 = 0;
		blk_case(&c, cap, BLK_MUST, "bzero FAULTING on an unmapped second page "
		                     "(write-back contract: same pc, same A0, same D1, same frame)");
		CHECK_EQ32((uae_u32)g_blk_ref.reached, 2u,
		           "...and it really did fault: the reference run reached vector 2");
		CHECK_EQ32((uae_u32)g_blk_fast.reached, 2u,
		           "...and so did the fast-path run");
		/* Where the hand-off happened, spelled out. The device gate only fast-paths a
		 * page whose data-page-cache entry a successful access already established, and
		 * a chunk never leaves that page -- so the fast path runs the four longwords of
		 * page 0 and then DECLINES on the cold, unmapped page 1, and the interpreter
		 * takes the fault. That is why a fault inside a chunk is not reachable in this
		 * design, and why the state the fault is reported against is the state the fast
		 * path left: A0 one longword past the last successful store, D1 un-decremented
		 * for the instruction that faulted. Compared against the interpreter above; named
		 * here so the property is not something a reader has to re-derive. */
		CHECK_EQ32(g_blk_fast.a[0], BLK_VDST + 0x1000u + 4u,
		           "A0 stands one longword past the faulting store, as the 68040 "
		           "write-back contract requires");
		c.map_dst2 = 1; c.dst_off = 0;

		/* Both again, warm. */
		c.prewarm = 1; c.n = 31; c.dst_off = 0x1000u - 16u;
		blk_case(&c, cap, BLK_MUST, "bzero CROSSING a page boundary, warm, second page mapped");
		c.map_dst2 = 0;
		blk_case(&c, cap, BLK_MUST, "bzero FAULTING on an unmapped second page, warm");
		CHECK_EQ32((uae_u32)g_blk_ref.reached, 2u, "...and it really did fault");
		c.map_dst2 = 1; c.dst_off = 0; c.prewarm = 0;

		/* User mode: the data page cache is indexed by regs.s, so the gate has to be
		 * asked the same question the accessor asks. */
		c.n = 63; c.user = 1;
		blk_case(&c, cap, BLK_MUST, "bzero in USER mode");
		c.prewarm = 1;
		blk_case(&c, cap, BLK_MUST, "bzero in USER mode, warm (the cache index carries S)");
		c.prewarm = 0; c.user = 0;

		/* Destination is not plain RAM: decline, and the guest cannot tell. */
		c.n = 15; c.dst_io = 1;
		blk_case(&c, cap, BLK_NEVER, "bzero to a NON-RAM destination falls back to the interpreter");
		c.dst_io = 0;

		/* Misaligned destination: decline. */
		c.n = 15; c.dst_off = 2;
		blk_case(&c, cap, BLK_NEVER, "bzero from a MISALIGNED destination declines");
		c.dst_off = 0;

		/* ---- THE TRIGGER IS EXACT: what must NOT be recognised ------------------
		 * Two opcodes next to each other are not a loop. The recognizer's whole claim
		 * to fire on any kernel's bzero -- rather than on a PC it was told about -- is
		 * that it checks the SHAPE, so the shape has to be checked against things that
		 * are nearly it. */
		c.n = 5; c.prewarm = 1;
		c.badloop = 1;
		blk_case(&c, cap, BLK_NEVER,
		         "MOVE.L An,(Ay)+ then a DBcc that branches to ITSELF is not a loop "
		         "over the MOVE, and must not be collapsed as one");
		c.badloop = 0;
		c.notdbf = 1;
		blk_case(&c, cap, BLK_NEVER,
		         "MOVE.L An,(Ay)+ followed by a NOP is not an idiom at all");
		c.notdbf = 0;

		/* A CONDITIONAL DBcc is not a DBF. DBF's condition is false forever, so the loop
		 * runs exactly Dn.w+1 times and the collapse is arithmetic. DBEQ terminates the
		 * moment Z is set -- and the MOVE ahead of it sets Z from its source, so with a
		 * zero fill this loop runs its body exactly ONCE. A recognizer that keyed on the
		 * DBcc family rather than on DBF would fill the whole buffer. */
		c.dbcc = 0x57C9; c.fill = 0; c.n = 40;
		blk_case(&c, cap, BLK_NEVER,
		         "DBEQ is not DBF: a CONDITIONAL DBcc must not be collapsed (its loop "
		         "ends on a flag the body sets)");
		c.fill = 0x00000001u;
		blk_case(&c, cap, BLK_NEVER,
		         "...and not when the condition happens to stay false either -- the "
		         "trigger is the opcode, not the outcome");
		c.dbcc = 0; c.fill = 0; c.n = 5;

		/* TRACING must see every instruction. A collapsed chunk is one trace exception
		 * where the guest asked for two per iteration, and a single-stepping debugger
		 * that skips 512 instructions is worse than no debugger. */
		c.trace = 1; c.n = 40;
		blk_case(&c, cap, BLK_NEVER, "with T1 set the recognizer declines: a traced "
		                             "instruction stream may not be collapsed");
		c.trace = 0; c.n = 5;

		/* The idiom STRADDLING an instruction page boundary. Reading past the opcode is
		 * the recognizer's only speculative act; if it does that across a page it can
		 * fault where the interpreted path would not have, at a completely different
		 * point in the instruction. Both arms are checked: with the next code page
		 * absent (where an unguarded peek faults early, before the MOVE has run) and
		 * with it present (where an unguarded peek would merely be reading the right
		 * bytes -- but the state must still match, and the recognizer must still
		 * decline, because "it happens to work" is not the contract). */
		c.code_off = 0x1000u - 2u;             /* MOVE at the last word of the page */
		c.map_code2 = 0;
		blk_case(&c, cap, BLK_NEVER,
		         "an idiom straddling an instruction page with the NEXT page absent: the "
		         "recognizer must not peek across and fault early");
		CHECK_EQ32((uae_u32)g_blk_ref.reached, 2u,
		           "...and the reference run really does fault, fetching the DBcc");
		c.map_code2 = 1;
		blk_case(&c, cap, BLK_NEVER,
		         "...and with the next page present it still declines rather than "
		         "relying on the peek having been safe");
		c.map_code2 = 0; c.code_off = 0; c.prewarm = 0;

		/* ---- stage B: mcpy --------------------------------------------------- */
		memset(&c, 0, sizeof c);
		c.stage = 2; c.map_dst2 = 1; c.map_src2 = 1;

		c.n = 0;    blk_case(&c, cap, BLK_MAY,  "mcpy n=0");
		c.n = 1;    blk_case(&c, cap, BLK_MUST, "mcpy n=1");
		c.n = 1023; blk_case(&c, cap, BLK_MUST, "mcpy n=1023 (a whole 4K page)");
		c.prewarm = 1;
		c.n = 0;    blk_case(&c, cap, BLK_MUST, "mcpy n=0, both pages WARM");
		c.n = 1;    blk_case(&c, cap, BLK_MUST, "mcpy n=1, both pages WARM");
		c.n = 1023; blk_case(&c, cap, BLK_MUST, "mcpy n=1023, both pages WARM (the flags "
		                                        "come from the LAST longword copied)");
		c.prewarm = 0;

		/* Source and destination crossing their pages at DIFFERENT offsets: the chunk
		 * has to be bounded by whichever runs out first, and a fast path that only
		 * bounds one of them writes past a page it never validated. */
		c.n = 63; c.src_off = 0x1000u - 32u; c.dst_off = 0x1000u - 16u;
		blk_case(&c, cap, BLK_MUST, "mcpy with source and destination crossing at DIFFERENT offsets");

		/* Read fault: the source's second page is missing. */
		c.map_src2 = 0;
		blk_case(&c, cap, BLK_MUST, "mcpy FAULTING on an unmapped source page (a READ fault, "
		                     "which leaves both pointers un-advanced)");
		CHECK_EQ32((uae_u32)g_blk_ref.reached, 2u,
		           "...and it really did fault: the reference run reached vector 2");
		CHECK_EQ32((uae_u32)g_blk_fast.reached, 2u,
		           "...and so did the fast-path run");
		c.map_src2 = 1; c.src_off = 0; c.dst_off = 0;

		/* ---- stage C: the CPUSHL cache-flush loop ---------------------------- */
		memset(&c, 0, sizeof c);
		c.stage = 3; c.step = 16;

		c.n = 0;    blk_case(&c, cap, BLK_MUST, "CPUSHL loop n=0");
		c.n = 1;    blk_case(&c, cap, BLK_MUST, "CPUSHL loop n=1");
		c.n = 255;  blk_case(&c, cap, BLK_MUST, "CPUSHL loop n=255 (a 4K page at 16 bytes a line)");
		c.step = -16;
		blk_case(&c, cap, BLK_MUST, "CPUSHL loop walking BACKWARDS (negative ADDA immediate)");
		c.step = 16;

		c.user = 1;
		blk_case(&c, cap, BLK_NEVER, "CPUSHL in USER mode declines (it must trap, not collapse)");
		c.user = 0;

		/* The ADDA must advance the register the CPUSHL walks. If it advances a
		 * different one, this is not the cache-flush loop -- the flush pointer never
		 * moves -- and collapsing it would step the wrong register. */
		c.n = 20; c.adda_reg = 2;
		blk_case(&c, cap, BLK_NEVER,
		         "CPUSHL (A0) with an ADDA that advances A2 is not the idiom: the "
		         "recognizer must decline rather than step A0");
		c.adda_reg = 0; c.n = 5;
	}

	/* ---- yield granularity ---------------------------------------------------
	 * The map's risk 2 is interrupt latency, and "the poll still runs at its normal
	 * cadence" is unfalsifiable as prose. The board stub counts every
	 * check_uae_int_request(), so it is a number: a 1024-longword fill at cap N must
	 * poll about 1024/N times, and the interpreted path polls once per instruction.
	 *
	 * The ">= 1024/cap" side is the one that matters -- it is the assertion that the
	 * fast path does NOT run the whole fill uninterruptibly. The extra polls come from
	 * the interpreted instruction at each page entry (the gate's cold decline) and from
	 * the loop's final fall-through. */
	{
		struct blk_snap *s = &g_blk_fast;
		memset(&c, 0, sizeof c);
		c.stage = 1; c.map_dst2 = 1; c.n = 1023;

		blk_run(&g_blk_ref, &c, Z3660_BLK_OFF, 4200);
		CHECK(g_blk_ref.polls >= 2048,
		      "interpreted: 1024 iterations poll at least 2048 times (once per instruction, "
		      "cadence 1) -- got %lu\n", g_blk_ref.polls);

		blk_run(s, &c, 256u, 4200);
		CHECK(s->polls >= 1024u / 256u,
		      "cap 256: the fill yields to the poll at least 4 times, not once at the end "
		      "-- got %lu\n", s->polls);
		CHECK(s->polls <= 32,
		      "cap 256: ...and not once per instruction either -- got %lu\n", s->polls);

		blk_run(s, &c, 64u, 4200);
		CHECK(s->polls >= 1024u / 64u,
		      "cap 64: a smaller cap yields MORE often (>= 16) -- got %lu\n", s->polls);

		blk_run(s, &c, Z3660_BLK_MAX, 4200);
		CHECK(s->polls >= 2,
		      "cap 1024: even at the ceiling the page bound forces a yield -- got %lu\n",
		      s->polls);
		CHECK(s->polls <= 12,
		      "cap 1024: ...but only a handful -- got %lu\n", s->polls);

		/* THE PAGE BOUND ITSELF. A fill that starts mid-page and runs 1024 longwords
		 * spans two pages, so at a cap of 1024 the ONLY thing that can split it is the
		 * page bound. Warm both pages first, so the gate's cold decline cannot be what
		 * splits it instead and the assertion is about the bound and nothing else.
		 *
		 * This is the yield-granularity test doing double duty as the device gate's
		 * REACH test: the gate's verdict is a property of one page, and a chunk that ran
		 * past the page end would be applying a RAM verdict to memory it never asked
		 * about. State equivalence cannot see that -- the same stores happen in the same
		 * order either way -- so the poll count is where it has to be caught. */
		c.dst_off = 0x800u;
		c.prewarm = 1;
		blk_run(s, &c, Z3660_BLK_MAX, 4200);
		CHECK(s->polls >= 2,
		      "a warm 1024-longword fill starting MID-PAGE splits at the page boundary "
		      "even at cap 1024 -- got %lu poll(s), which would be 1 if the chunk ran "
		      "past the end of the page its device gate actually validated\n", s->polls);
		c.dst_off = 0; c.prewarm = 0;
	}

	/* ---- the OFF state is a PURE passthrough ---------------------------------
	 * The whole corpus above compares against OFF, so OFF being subtly different from
	 * "the firmware before this change" would make every comparison agree about the
	 * wrong thing. It cannot be checked from inside, so check the two things that can
	 * be: OFF costs exactly one run-loop pass per guest instruction (no chunking, no
	 * declined recognizer eating a pass) and it polls exactly once per instruction. */
	{
		memset(&c, 0, sizeof c);
		c.stage = 1; c.map_dst2 = 1; c.n = 9;
		blk_run(&g_blk_ref, &c, Z3660_BLK_OFF, 200);
		CHECK_EQ32((uae_u32)g_blk_ref.steps, 20u,
		           "OFF: 10 iterations == 20 guest instructions == 20 run-loop passes");
		CHECK_EQ32((uae_u32)g_blk_ref.polls, 20u,
		           "OFF: ...and 20 interrupt polls, one per instruction at cadence 1");
	}

	/* ---- the resolution rule, every branch ----------------------------------- */
	CHECK_EQ32(z3660_blk_resolve_chunk(Z3660_BLK_UNSET), Z3660_BLK_DEFAULT,
	           "an unwritten mailbox word means the DEFAULT, never off");
	CHECK_EQ32(z3660_blk_resolve_chunk(Z3660_BLK_OFF), 0u,
	           "the off sentinel resolves to 0 == never fire");
	CHECK_EQ32(z3660_blk_resolve_chunk(1u), 1u, "a cap of 1 is honoured");
	CHECK_EQ32(z3660_blk_resolve_chunk(Z3660_BLK_MAX), Z3660_BLK_MAX, "the ceiling is honoured");
	CHECK_EQ32(z3660_blk_resolve_chunk(Z3660_BLK_MAX + 1u), Z3660_BLK_MAX,
	           "above the ceiling is clamped, not rejected and not wrapped");
	CHECK_EQ32(z3660_blk_resolve_chunk(0xFFFFFFFEu), Z3660_BLK_MAX,
	           "...including a value one below the off sentinel");

	/* The knob reaches the loop, and the read-back tells the truth about it. */
	CHECK_EQ32(z3660_blk_set(Z3660_BLK_OFF), 0u, "BLK OFF through the setter");
	CHECK_EQ32(z3660_blk_get(), 0u, "...and the read-back agrees");
	CHECK_EQ32(z3660_blk_set(Z3660_BLK_UNSET), Z3660_BLK_DEFAULT, "BLK default through the setter");
	shared->blk_chunk = Z3660_BLK_UNSET;
	z3660_knobs_sync();
	CHECK_EQ32(z3660_blk_get(), Z3660_BLK_DEFAULT,
	           "the mailbox path resolves the same way the setter does");

	harness_shared_defaults();
	z3660_knobs_sync();
	cpu_bringup(68030);
}


/* ==================================================================
 * CYBERSTORM C2 RUNG 1 -- THE DECODED-OP CACHE
 *
 * Two claims have to be established and they are not the same claim, so they get two
 * suites:
 *
 *   STATE EQUIVALENCE  the cache changes nothing a guest can observe. Same corpus
 *                      discipline as rung 0's -- every case runs twice from an identical
 *                      fixture, once with SHARED.dopc_on = OFF (a pure passthrough: the
 *                      lookup returns before it has read the table) and once with the
 *                      cache live -- and the two outcomes are compared as whole objects,
 *                      registers and SR and USP/ISP and a ONE MEGABYTE memcmp of guest
 *                      physical memory covering the tables, the code, the vectors, the
 *                      stack and the data. Plus one extra assertion rung 0 could not
 *                      make: the RUN-LOOP PASS COUNTS must be EXACTLY equal. This cache
 *                      changes what a pass costs and never how many there are, so an
 *                      inequality here is a bug even when the final state agrees.
 *
 *   COHERENCE          the cache is invalidated by everything that could make an entry
 *                      wrong -- and, just as importantly, is NOT invalidated by anything
 *                      else, which is what makes it worth having. The map names three
 *                      cases as mandatory and none of them are ordinary: self-modifying
 *                      code, CPUSHL/CINV/PFLUSH and MMU-root changes, and DMA into an
 *                      instruction page.
 *
 * The coherence suite asks the cache DIRECTLY (z3660_dopc_peek) as well as through
 * execution. A test that can only infer the mechanism from its consequences passes for
 * the wrong reason as soon as the consequence has a second cause.
 * ================================================================== */
#include "z3660_dopc.h"

/* Physical layout, inside the same one-megabyte window blk_equal() memcmps. */
#define DOPC_PCODE   0x001C0000u
#define DOPC_PCODE2  0x001C1000u
#define DOPC_PVEC    0x001C2000u
#define DOPC_PSTK    0x001C3000u
#define DOPC_PDATA   0x001C4000u

#define DOPC_VCODE   0x00800000u
#define DOPC_VVEC    0x00820000u   /* not adjacent to the code page on purpose */
#define DOPC_VSTK    0x40003000u
#define DOPC_VDATA   0x00900000u
#define DOPC_VNOMAP  0x00A00000u   /* deliberately never mapped */
#define DOPC_FAULTPC (DOPC_VCODE + 0x100u)

/* The mixed loop. Chosen so the corpus covers the shapes the cache treats differently:
 * an instruction with ONE extension word (the slot the entry can hold), one with THREE
 * (only the first of which the slot can hold), one with none, a guest read, a guest
 * write, and a DBcc that brings the pc back to the head so the same PCs repeat and the
 * cache is actually exercised rather than merely present.
 *
 *   +0x00  2028 0000        MOVE.L (0,A0),D0
 *   +0x04  0680 1234 5678   ADDI.L #$12345678,D0
 *   +0x0A  2140 0004        MOVE.L D0,(4,A0)
 *   +0x0E  5289             ADDQ.L #1,A1
 *   +0x10  51CA FFEE        DBF    D2,+0x00
 *   +0x14  4E71             NOP                 <- DONE
 */
#define DOPC_MIXED_DONE 0x14u
/* The rung-0 idiom, so the cached idiom verdict is exercised against the fast path it
 * gates rather than only against the recognizer's decline. */
#define DOPC_IDIOM_DONE 0x06u

struct dopc_desc {
	int      idiom;      /* plant the bzero idiom instead of the mixed loop     */
	uae_u32  n;          /* loop trip count                                     */
	uae_u32  code_off;   /* where in the code page the program starts           */
	int      map_code2;  /* map the SECOND code page (for a straddling program) */
	int      user;       /* run in user mode                                    */
	int      fault;      /* point A0 at an UNMAPPED page: the loop must fault   */
	unsigned blk;        /* rung 0's knob for this case (Z3660_BLK_OFF or a cap)*/
};

/* Set by dopc_run() just before it tears the fixture down: the cached opcode at the loop
 * head, or -1. The ANTI-VACUITY evidence -- "the two runs agree" proves nothing if the
 * cache never held anything, and this is how the corpus knows it did. */
static int g_dopc_peek_head;

static void dopc_build(const struct dopc_desc *c)
{
	mmu040_gen_init();                       /* resets RAM, brings up 68040 + MMU, S=1 */

	mmu040_gen_map(DOPC_VCODE, DOPC_PCODE, 0);
	if (c->map_code2) mmu040_gen_map(DOPC_VCODE + 0x1000u, DOPC_PCODE2, 0);
	mmu040_gen_map(DOPC_VVEC,  DOPC_PVEC,  0);
	mmu040_gen_map(DOPC_VSTK,  DOPC_PSTK,  0);
	mmu040_gen_map(DOPC_VDATA, DOPC_PDATA, 0);

	for (uae_u32 i = 0; i < 0x1000u; i += 4)
		hram_poke32(DOPC_PDATA + i, 0xDA7A0000u | i);

	{
		/* Plant by VIRTUAL offset from the head, so a program that straddles the code
		 * page lands in the second frame instead of wrapping inside the first. */
		#define DOPC_POKE(off, w)                                                       \
			do {                                                                        \
				uae_u32 _a = c->code_off + (uae_u32)(off);                              \
				if (_a < 0x1000u) hram_poke16(DOPC_PCODE  + _a, (uae_u16)(w));           \
				else              hram_poke16(DOPC_PCODE2 + _a - 0x1000u, (uae_u16)(w)); \
			} while (0)
		if (c->idiom) {
			DOPC_POKE(0x00, 0x20C9);                  /* MOVE.L A1,(A0)+ */
			DOPC_POKE(0x02, 0x51C9);                  /* DBF D1,-4       */
			DOPC_POKE(0x04, 0xFFFC);
			DOPC_POKE(0x06, 0x4E71);                  /* DONE: nop       */
		} else {
			DOPC_POKE(0x00, 0x2028); DOPC_POKE(0x02, 0x0000);
			DOPC_POKE(0x04, 0x0680); DOPC_POKE(0x06, 0x1234); DOPC_POKE(0x08, 0x5678);
			DOPC_POKE(0x0A, 0x2140); DOPC_POKE(0x0C, 0x0004);
			DOPC_POKE(0x0E, 0x5289);
			DOPC_POKE(0x10, 0x51CA); DOPC_POKE(0x12, 0xFFEE);
			DOPC_POKE(0x14, 0x4E71);                  /* DONE: nop       */
		}
		#undef DOPC_POKE
	}
	hram_poke16(DOPC_PCODE + 0x100, 0x60FE);              /* FAULT: bra.s .          */
	hram_poke32(DOPC_PVEC | 0x08, DOPC_FAULTPC);          /* vector 2: access fault  */
	hram_poke32(DOPC_PVEC | 0x20, DOPC_FAULTPC);          /* vector 8: privilege     */
	hram_poke32(DOPC_PVEC | 0x24, DOPC_FAULTPC);          /* vector 9: trace         */

	cpu_set_context(DOPC_VCODE + c->code_off);
	regs.vbr = DOPC_VVEC;
	regs.s = 1;
	m68k_areg(regs, 7) = DOPC_VSTK | 0xF00;

	/* Poison the register file and the CCR for the same reason rung 0's fixture does:
	 * the two runs of a case are sequential on one machine, so whatever the reference
	 * run leaves behind is exactly the state the second run is being checked for
	 * producing, and an omission would agree with it perfectly. */
	for (int i = 0; i < 8; i++) {
		m68k_dreg(regs, i) = 0xD0D00000u | (uae_u32)i;
		if (i != 7) m68k_areg(regs, i) = 0xA0A00000u | (uae_u32)i;
	}
	CLEAR_CZNV();
	SET_CFLG(1); SET_VFLG(1); SET_ZFLG(1); SET_NFLG(1); SET_XFLG(1);
	MakeSR();
	MakeFromSR();

	m68k_areg(regs, 0) = c->fault ? DOPC_VNOMAP : DOPC_VDATA;
	m68k_areg(regs, 1) = c->idiom ? 0x00000000u : 0x0000A000u;
	m68k_dreg(regs, 1) = 0xABCD0000u | c->n;   /* the idiom's counter        */
	m68k_dreg(regs, 2) = 0x12340000u | c->n;   /* the mixed loop's counter   */
	MakeSR();

	if (c->user) {
		regs.usp = DOPC_VSTK | 0xE00;
		regs.s = 0;
		MakeSR();
		m68k_areg(regs, 7) = regs.usp;
	}
}

/* Build, set BOTH knobs through the mailbox (do_specialties resyncs every 64th call, so
 * a directly-poked variable would be clobbered mid-run), drive to completion or to the
 * fault handler, snapshot. */
static void dopc_run(struct blk_snap *s, const struct dopc_desc *c, unsigned knob, int budget)
{
	const uae_u32 done = DOPC_VCODE + c->code_off
	                   + (c->idiom ? DOPC_IDIOM_DONE : DOPC_MIXED_DONE);
	unsigned long polls0;
	int steps = 0, reached = 0;
	int super_at_head;

	dopc_build(c);
	shared->dopc_on   = knob;
	shared->blk_chunk = c->blk;
	z3660_knobs_sync();
	super_at_head = c->user ? 0 : 1;

	polls0 = harness_ipl_polls;
	regs.spcflags |= Z_SPCFLAG_MODE_CHANGE;
	while (steps < budget) {
		uae_u32 pc = m68k_getpc();
		if (pc == done)         { reached = 1; break; }
		if (pc == DOPC_FAULTPC) { reached = 2; break; }
		m68k_run_mmu040();
		steps++;
	}
	regs.spcflags = 0;

	g_dopc_peek_head = z3660_dopc_peek(DOPC_VCODE + c->code_off, super_at_head);
	blk_take(s, polls0, steps, reached);

	shared->dopc_on   = Z3660_DOPC_UNSET;
	shared->blk_chunk = Z3660_BLK_UNSET;
	z3660_knobs_sync();
	mmu040_off();
}

/* One corpus entry: run it with the cache OFF and with the cache ON, demand the two
 * outcomes are identical, demand the pass counts are identical, and demand the cache
 * actually held the loop head in the ON run. */
static void dopc_case(const struct dopc_desc *c, const char *what)
{
	int budget = (int)(c->n + 2u) * 8 + 64;
	int ref_peek, fast_peek;
	uae_u16 head = c->idiom ? 0x20C9 : 0x2028;

	dopc_run(&g_blk_ref, c, Z3660_DOPC_OFF, budget);
	ref_peek = g_dopc_peek_head;
	dopc_run(&g_blk_fast, c, Z3660_DOPC_DEFAULT, budget);
	fast_peek = g_dopc_peek_head;

	blk_equal(&g_blk_ref, &g_blk_fast, what);

	CHECK_EQ32((uae_u32)g_blk_fast.steps, (uae_u32)g_blk_ref.steps, what);
	CHECK_EQ32((uae_u32)ref_peek, 0xFFFFFFFFu,
	           "OFF is a pure passthrough: nothing was cached at all");
	if (c->fault) {
		/* A run that faults on its first instruction may never complete one, so the
		 * head need not be cached -- but the OFF arm must still have cached nothing. */
		g_pass++;
	} else {
		CHECK_EQ32((uae_u32)fast_peek, (uae_u32)head,
		           "the cache actually held the loop head -- the comparison is not "
		           "two runs of the same uncached interpreter");
	}
}

static void test_dopc_state_equivalence(void)
{
	printf("[test] C2 decoded-op cache: state equivalence against the uncached path\n");

	struct dopc_desc c;

	/* Geometry first, against the compiler's own sizeof and not against the constants
	 * the struct was supposed to be built from. A set that grows past 64 bytes doubles
	 * the hit path's line traffic and nothing else in this suite would notice. */
	/* The set's size is a claim about the TARGET, where a handler pointer is four bytes.
	 * This host builds the same struct with eight-byte pointers, so it is legitimately 80
	 * bytes here and asserting 64 would be asserting the wrong machine. What transfers is
	 * the field arithmetic -- assert that, and let newcpu.cpp's compile-time tripwire
	 * refuse the 32-bit build if the struct ever grows a field or a hole. */
	{
		unsigned fields = 16u                              /* tag[4]     */
		                + 4u + 4u                          /* stamp, victim */
		                + 4u * (unsigned)sizeof(void *)    /* handler[4] */
		                + 8u + 8u + 4u + 4u;               /* opcode[4], ext0[4], meta[4], pad */
		CHECK_EQ32(z3660_dopc_set_bytes(), fields,
		           "the set is exactly its fields: no padding, no hole");
		CHECK_EQ32(16u + 4u + 4u + 4u * 4u + 8u + 8u + 4u + 4u, Z3660_DOPC_SET_BYTES,
		           "...and with 32-bit handler pointers that is 64 bytes -- two Cortex-A9 "
		           "lines, with all four tags and the stamp inside the first");
		CHECK_EQ32(z3660_dopc_table_bytes(), Z3660_DOPC_SETS * fields,
		           "the table is SETS x one set, contiguous");
	}
	CHECK_EQ32(Z3660_DOPC_ENTRIES, 4096u, "4096 entries, from the map's §9.1 measurement");
	CHECK_EQ32(Z3660_DOPC_WAYS,    4u,    "4-way");
	CHECK_EQ32(Z3660_DOPC_BYTES,   Z3660_DOPC_ENTRIES * 16u,
	           "64 KB at 16 bytes per entry -- the map's sizing budget");

	/* The switch rule. Every branch, including the ones no metal session will reach. */
	CHECK_EQ32(z3660_dopc_resolve(Z3660_DOPC_UNSET), 1u,
	           "UNSET resolves to the DEFAULT and not to off -- a core0 built before the "
	           "field existed must still run the shipped behaviour");
	CHECK_EQ32(z3660_dopc_resolve(Z3660_DOPC_OFF),   0u, "the off sentinel is off");
	CHECK_EQ32(z3660_dopc_resolve(1u),               1u, "1 is on");
	CHECK_EQ32(z3660_dopc_resolve(0xFFFFFFFEu),      1u,
	           "...and so is anything else, including one below the off sentinel");
	CHECK_EQ32(z3660_dopc_set(Z3660_DOPC_OFF), 0u, "OFF through the setter");
	CHECK_EQ32(z3660_dopc_get(), 0u, "...and the read-back agrees");
	CHECK_EQ32(z3660_dopc_set(Z3660_DOPC_UNSET), Z3660_DOPC_DEFAULT, "default through the setter");

	/* THE SENTINEL SWEEP. The empty-way marker must not be a legal key of the set it
	 * sits in, for ANY pc and either address space -- and this is not a hypothetical:
	 * a fixed sentinel of 1 is exactly "pc 0 in supervisor mode", which a guest reaches
	 * by taking an exception through an unplanted vector, and it dispatched a NULL
	 * handler out of a way nothing had ever filled the first time this suite ran. */
	{
		int bad = 0;
		for (unsigned i = 0; i < Z3660_DOPC_SETS; i++) {
			unsigned e = Z3660_DOPC_TAG_EMPTY(i);
			if (((e >> 1) & (Z3660_DOPC_SETS - 1u)) == i) bad++;
		}
		CHECK_EQ32((uae_u32)bad, 0u,
		           "no set's empty marker is a legal key of that set (all 1024 swept)");
	}
	CHECK_EQ32(Z3660_DOPC_KEY(0u, 1), 1u,
	           "pc 0 in supervisor mode is key 1 -- the value a fixed sentinel would have been");
	CHECK(Z3660_DOPC_KEY(0u, 1) != Z3660_DOPC_TAG_EMPTY(Z3660_DOPC_INDEX(0u)),
	      "...and it does not match set 0's empty marker\n");
	CHECK_EQ32(Z3660_DOPC_INDEX(0x00800000u), Z3660_DOPC_INDEX(0x00800000u + (Z3660_DOPC_SETS * 2u)),
	           "the index wraps every 2 KB of guest pc, as (pc >> 1) & 1023 says");
	CHECK(Z3660_DOPC_KEY(0x00800000u, 0) != Z3660_DOPC_KEY(0x00800000u, 1),
	      "supervisor and user tags for the same VA differ: a context switch cannot alias\n");

	memset(&c, 0, sizeof c);
	c.blk = Z3660_BLK_OFF;

	c.n = 40; dopc_case(&c, "mixed loop, supervisor, 40 iterations");
	c.n = 1;  dopc_case(&c, "mixed loop, one iteration (nothing is warm yet)");
	c.n = 0;  dopc_case(&c, "mixed loop, DBcc boundary: n=0 still runs the body once");

	c.n = 40; c.user = 1; dopc_case(&c, "mixed loop in USER mode (the tag's address-space bit)");
	c.user = 0;

	/* The straddle. The head sits two bytes from the end of the code page, so the
	 * extension-word lookahead must REFUSE -- its granule test is the only thing keeping
	 * it from issuing a fetch the opcode did not prove safe -- while the instructions
	 * themselves still execute across the boundary. */
	c.n = 8; c.code_off = 0x0FFEu; c.map_code2 = 1;
	dopc_case(&c, "program straddling the code page (the lookahead must decline)");
	c.code_off = 0; c.map_code2 = 0;

	/* Faulting: the run loop's CATCH disarms the extension-word window, and the whole
	 * exception frame is compared byte for byte by blk_equal's memcmp of the stack. */
	c.n = 8; c.fault = 1;
	dopc_case(&c, "the loop faults on an unmapped operand (frame compared as memory)");
	c.fault = 0;

	/* Rung 0 and rung 1 together, which is the combination that ships. The cached idiom
	 * verdict gates the recognizer, so a wrong verdict shows up here as a different
	 * final state or a different pass count -- and the pass-count check is the sharp one,
	 * because a lost fast path costs passes without changing any register. */
	c.idiom = 1; c.n = 300; c.blk = 256u;
	dopc_case(&c, "the rung-0 idiom with BLK 256 and the cache live");
	c.n = 300; c.blk = Z3660_BLK_OFF;
	dopc_case(&c, "the same idiom with BLK OFF: the verdict is cached either way");
	c.n = 0; c.blk = 256u;
	dopc_case(&c, "idiom, n=0 (one iteration) with BLK 256");
	c.idiom = 0; c.blk = Z3660_BLK_OFF;

	harness_shared_defaults();
	shared->dopc_on = Z3660_DOPC_UNSET;
	z3660_knobs_sync();
	cpu_bringup(68030);
}

/* ==================================================================
 * COHERENCE
 *
 * The fixture is two instructions:
 *
 *   +0x00  5289   ADDQ.L #1,A1
 *   +0x02  60FC   BRA.S  -4     -> +0x00
 *
 * Run it twice round and the head is cached. Then poke 0x5489 (ADDQ.L #2,A1) into the
 * code page BEHIND the interpreter's back -- hram_poke16 writes the host buffer directly,
 * which is exactly what core0's DMA does and what a guest store the cache does not watch
 * looks like from in here -- and step the head once more. A1 advances by 1 if the stale
 * decode was used and by 2 if it was not. One bit of evidence, unambiguous, and it does
 * not depend on any counter being compiled in.
 * ================================================================== */
#define DOPC_C_ADD1  0x5289u
#define DOPC_C_ADD2  0x5489u

static void dopc_coh_build(void)
{
	mmu040_gen_init();
	mmu040_gen_map(DOPC_VCODE, DOPC_PCODE, 0);
	mmu040_gen_map(DOPC_VVEC,  DOPC_PVEC,  0);
	mmu040_gen_map(DOPC_VSTK,  DOPC_PSTK,  0);
	mmu040_gen_map(DOPC_VDATA, DOPC_PDATA, 0);

	hram_poke16(DOPC_PCODE + 0x00, DOPC_C_ADD1);   /* ADDQ.L #1,A1 */
	hram_poke16(DOPC_PCODE + 0x02, 0x60FC);        /* BRA.S -4     */
	hram_poke16(DOPC_PCODE + 0x100, 0x60FE);
	hram_poke32(DOPC_PVEC | 0x08, DOPC_FAULTPC);
	hram_poke32(DOPC_PVEC | 0x20, DOPC_FAULTPC);

	cpu_set_context(DOPC_VCODE);
	regs.vbr = DOPC_VVEC;
	regs.s = 1;
	m68k_areg(regs, 7) = DOPC_VSTK | 0xF00;
	m68k_areg(regs, 0) = DOPC_VDATA;
	m68k_areg(regs, 1) = 0;
	m68k_dreg(regs, 1) = DOPC_C_ADD2;
	MakeSR();

	shared->dopc_on   = Z3660_DOPC_DEFAULT;
	shared->blk_chunk = Z3660_BLK_OFF;
	z3660_knobs_sync();
	regs.spcflags |= Z_SPCFLAG_MODE_CHANGE;
}

static void dopc_coh_step(int n)
{
	for (int i = 0; i < n; i++) {
		regs.spcflags |= Z_SPCFLAG_MODE_CHANGE;
		m68k_run_mmu040();
	}
}

/* Warm the cache on the head, rewrite the head behind the interpreter's back, apply
 * `maintain`, and report how far A1 moved on the next execution of the head:
 * 1 == the stale decode ran, 2 == the fresh one did. */
typedef void (*dopc_maint_fn)(void);
static uae_u32 dopc_coh_probe(dopc_maint_fn maintain, int *cached_before, int *cached_after)
{
	uae_u32 a1_before, a1_after;

	dopc_coh_build();
	dopc_coh_step(4);                       /* two round trips: the head is cached now */
	*cached_before = z3660_dopc_peek(DOPC_VCODE, 1);

	hram_poke16(DOPC_PCODE + 0x00, DOPC_C_ADD2);   /* the interpreter never sees this */
	if (maintain) maintain();
	*cached_after = z3660_dopc_peek(DOPC_VCODE, 1);

	a1_before = m68k_areg(regs, 1);
	dopc_coh_step(1);                       /* execute the head once */
	a1_after = m68k_areg(regs, 1);

	regs.spcflags = 0;
	shared->dopc_on   = Z3660_DOPC_UNSET;
	shared->blk_chunk = Z3660_BLK_UNSET;
	z3660_knobs_sync();
	return a1_after - a1_before;
}

static void dopc_m_cpushl(void)   { flush_mmu040(0, 0); }
static void dopc_m_pflush_pg(void){ mmu_flush_atc(DOPC_VCODE, true, true); }
/* C2 rung 1c: PFLUSH of a page that holds NO cached decode, and it must invalidate anyway.
 *
 * Rung 1b narrowed exactly this to a no-op through a page-presence filter, and the case
 * that asserted the skip lived here. The metal verdict reverted the filter, so the case
 * is inverted rather than deleted: whole-cache-on-every-PFLUSH is the shipping behaviour
 * again and something has to hold it. Deleting the case would have left the revert
 * unpinned -- a re-introduced filter would pass the remaining corpus unchanged, because
 * every other case flushes the page it cached in. */
static void dopc_m_pflush_other(void){ mmu_flush_atc(DOPC_VDATA, true, true); }
static void dopc_m_pflush_all(void){ mmu_flush_atc_all(true); }
static void dopc_m_settc(void)    { mmu_set_tc(0x8000); }
static void dopc_m_urp(void)      { uae_u32 v = regs.urp; m68k_move2c(0x806, &v); }
static void dopc_m_srp(void)      { uae_u32 v = regs.srp; m68k_move2c(0x807, &v); }
/* CHANGES CACR, which rung 1b made the condition. Bit 15 is the 68040's instruction-cache
 * enable -- the bit uae_mmu040_getc_iword() consults and the one the decoded-op cache is
 * standing in for -- so this is the write whose semantics actually reach this cache, not
 * merely a write to the register's address. The harness brings the CPU up with CACR 0, so
 * the pre-1b `v = 0` version of this asked whether an INERT write invalidates; that it did
 * was an accident of the policy rather than a requirement of the architecture, and the
 * negative case below now pins the opposite. */
static void dopc_m_cacr(void)     { uae_u32 v = regs.cacr ^ 0x8000u; m68k_move2c(2, &v); }
static void dopc_m_cacr_same(void){ uae_u32 v = regs.cacr; m68k_move2c(2, &v); }
/* NO FIXTURE FOR THE cacr_mask == 0 ARM, and the reason is a finding rather than an
 * omission: m68k_move2c()'s own movec_illg() returns "illegal" for every CPU model the
 * mask ladder does not name -- 68060 included -- so `case 2:` is reachable only on the
 * 68020/030/040, and all three of those HAVE a mask. The guard is therefore unreachable
 * today. It stays as a tripwire for the change that would otherwise break the narrowing
 * silently (teaching movec_illg() about the 68060 without teaching the mask ladder), and
 * it is documented as unreachable in newcpu_common.cpp rather than given a test that
 * would have to lie about what it exercises.
 */
static void dopc_m_dma(void)      { z3660_dopc_invalidate_dma(DOPC_PCODE, 0x1000u); }
static void dopc_m_tbl(void)      { build_cpufunctbl(); }
static void dopc_m_knob(void)     { shared->dopc_on = Z3660_DOPC_OFF; z3660_knobs_sync();
                                    shared->dopc_on = Z3660_DOPC_DEFAULT; z3660_knobs_sync(); }
/* THE WRAP IS NOT A CAUSE A CALL SITE RAISES, so this fixture no longer pretends it is.
 * It is raised BY z3660_dopc_do_invalidate() when the generation it just bumped lands on
 * 0 -- so the only honest way to drive it is to arrive at the boundary and then perform
 * one ORDINARY invalidation. `dopc_m_dma()` is the vehicle because it is a real firmware
 * call site. Typing INV_WRAP out here, as this did before, drove a counter increment and
 * NOT the wrap arm's own attribution -- which is exactly the gap being closed. */
static void dopc_m_wrap(void)     { z3660_dopc_force_generation(0xFFFFFFFFu);
                                    dopc_m_dma(); }

static void dopc_coh_one(dopc_maint_fn m, uae_u32 want_delta, const char *what)
{
	int before = -1, after = -1;
	uae_u32 d = dopc_coh_probe(m, &before, &after);
	CHECK_EQ32(d, want_delta, what);
	CHECK_EQ32((uae_u32)before, DOPC_C_ADD1,
	           "...the head really was cached before the rewrite (anti-vacuity)");
	if (want_delta == 2)
		CHECK_EQ32((uae_u32)after, 0xFFFFFFFFu, "...and the entry is gone afterwards");
	else
		CHECK_EQ32((uae_u32)after, DOPC_C_ADD1, "...and the entry is still there");
}

static void test_dopc_coherence(void)
{
	printf("[test] C2 decoded-op cache: coherence -- what invalidates it, and what does not\n");

	/* THE NEGATIVE CASE FIRST, because it is what makes every positive one meaningful.
	 * A guest store into a code page with no cache maintenance is NOT required to be
	 * visible to the instruction stream on a 68040 -- the architecture puts that duty on
	 * the guest, and this cache takes the architecture at its word rather than checking
	 * every write, which is the only way it can be cheap enough to be worth having. So
	 * the stale decode runs, and this test pins that as a documented contract rather
	 * than leaving it to be discovered. Every case below it changes the answer. */
	dopc_coh_one(NULL, 1u,
	             "no maintenance: the stale decode runs (the 68040 contract, pinned)");

	dopc_coh_one(dopc_m_cpushl,    2u, "CPUSHL / CINV (flush_mmu040) invalidates");
	dopc_coh_one(dopc_m_pflush_pg, 2u, "PFLUSH of the page holding the cached decode "
	                                   "invalidates");
	dopc_coh_one(dopc_m_pflush_all,2u, "PFLUSHA invalidates");

	/* C2 rung 1c -- THE PAGE FILTER'S REVERT, PINNED. Rung 1b made this exact call a
	 * no-op: PFLUSH of a page the cache holds nothing from was skipped, and the case here
	 * asserted delta 1. Metal reverted it (the cause is 6-9 % of requests and the marks
	 * were paid on every fill), so the assertion is INVERTED rather than removed. It is the
	 * only case in the corpus that can see the difference -- every other one flushes the
	 * page it just cached in, where a filter and no filter agree. */
	dopc_coh_one(dopc_m_pflush_other, 2u,
	             "PFLUSH of a DIFFERENT page invalidates too: the flush is whole-cache and "
	             "does not consult the address (rung 1c reverted the page filter)");
	dopc_coh_one(dopc_m_settc,     2u, "a TC write (MOVEC to TC -> mmu_set_tc) invalidates");
	dopc_coh_one(dopc_m_urp,       2u, "MOVEC to URP -- an MMU ROOT change -- invalidates");
	dopc_coh_one(dopc_m_srp,       2u, "MOVEC to SRP invalidates");
	dopc_coh_one(dopc_m_cacr,      2u, "MOVEC to CACR that CHANGES it invalidates");
	/* C2 rung 1b's CACR narrowing, from the other side -- and the one arm rung 1c KEPT, so
	 * this pair is now the whole of the firmware's narrowing. A write that leaves every
	 * retained bit where it was has changed no cache-enable, so the machine's cache
	 * configuration is bit-identical and the cached decode is still the right one. Delta 1
	 * == the cached decode ran, which is the ASSERTION here and not a tolerated outcome. */
	dopc_coh_one(dopc_m_cacr_same, 1u,
	             "...but a CACR write that changes nothing does not (rung 1b, kept by 1c)");
	dopc_coh_one(dopc_m_dma,       2u,
	             "DMA into an instruction page invalidates -- core1's own side of the "
	             "piscsi mailbox calls exactly this, after core0 has written guest DRAM");
	dopc_coh_one(dopc_m_tbl,       2u,
	             "build_cpufunctbl() invalidates: the entries hold RESOLVED handler "
	             "pointers, and a mode change makes every one of them wrong");
	dopc_coh_one(dopc_m_knob,      2u, "toggling the switch off and on invalidates");

	/* THE GENERATION-STAMP PATH. Validity is a stamp rather than a per-entry flag, which
	 * is what makes invalidation O(1) -- and it has exactly one boundary, 2^32
	 * invalidations away, where the counter wraps back onto stamps that are still
	 * sitting in the table. Driven directly here rather than left as an argument. */
	dopc_coh_one(dopc_m_wrap, 2u, "the generation wrap clears the table rather than "
	                              "revalidating it");
	CHECK_EQ32(z3660_dopc_generation(), 1u,
	           "...and it resumes at 1, never at 0: generation 0 is reserved so that a "
	           "zeroed table reads as entirely stale");

	/* SELF-MODIFYING CODE, EXECUTED BY THE GUEST. Everything above pokes memory from the
	 * host side. This one makes the guest do the whole sequence itself -- write the new
	 * opcode, CPUSHL, branch back -- which is the shape the AMIX kernel actually
	 * generates, and it exercises the store path, the flush handler and the cache in the
	 * order a real guest puts them in.
	 *
	 *   +0x00  5289               ADDQ.L #1,A1
	 *   +0x02  33C1 0080 0000     MOVE.W D1,$00800000     ; rewrite the head
	 *   +0x08  F468               CPUSHL (A0)             ; the architectural maintenance
	 *   +0x0A  60F4               BRA.S  -12  -> +0x00
	 */
	{
		uae_u32 a1_before, a1_after;
		int cached;

		mmu040_gen_init();
		mmu040_gen_map(DOPC_VCODE, DOPC_PCODE, 0);
		mmu040_gen_map(DOPC_VVEC,  DOPC_PVEC,  0);
		mmu040_gen_map(DOPC_VSTK,  DOPC_PSTK,  0);
		hram_poke16(DOPC_PCODE + 0x00, DOPC_C_ADD1);
		hram_poke16(DOPC_PCODE + 0x02, 0x33C1);
		hram_poke32(DOPC_PCODE + 0x04, DOPC_VCODE);
		hram_poke16(DOPC_PCODE + 0x08, 0xF468);
		hram_poke16(DOPC_PCODE + 0x0A, 0x60F4);
		hram_poke16(DOPC_PCODE + 0x100, 0x60FE);
		hram_poke32(DOPC_PVEC | 0x08, DOPC_FAULTPC);

		cpu_set_context(DOPC_VCODE);
		regs.vbr = DOPC_VVEC;
		regs.s = 1;
		m68k_areg(regs, 7) = DOPC_VSTK | 0xF00;
		m68k_areg(regs, 0) = DOPC_VCODE;
		m68k_areg(regs, 1) = 0;
		m68k_dreg(regs, 1) = DOPC_C_ADD2;
		MakeSR();

		shared->dopc_on   = Z3660_DOPC_DEFAULT;
		shared->blk_chunk = Z3660_BLK_OFF;
		z3660_knobs_sync();

		dopc_coh_step(4);        /* ADDQ, MOVE.W, CPUSHL, BRA -- one whole round trip */
		cached = z3660_dopc_peek(DOPC_VCODE, 1);
		CHECK_EQ32(hram_peek16(DOPC_PCODE + 0x00), DOPC_C_ADD2,
		           "the guest's own store landed in the code page");
		CHECK_EQ32((uae_u32)cached, 0xFFFFFFFFu,
		           "the guest's own CPUSHL invalidated the head it had just rewritten");

		a1_before = m68k_areg(regs, 1);
		dopc_coh_step(1);
		a1_after = m68k_areg(regs, 1);
		CHECK_EQ32(a1_after - a1_before, 2u,
		           "self-modifying code: the guest executes the NEW instruction");

		regs.spcflags = 0;
	}

	harness_shared_defaults();
	shared->dopc_on = Z3660_DOPC_UNSET;
	shared->blk_chunk = Z3660_BLK_UNSET;
	z3660_knobs_sync();
	cpu_bringup(68030);
}

/* ==================================================================
 * The BLK_ counters: the utilisation verdict a metal capture will be read through.
 *
 * A counter that is wired to the wrong quantity turns a working change into an
 * unreadable one, and the map's whole judgement of this rung is `BLK_INSNS / INSNS`
 * against a pre-registered 22.07 %. So the arithmetic is pinned here against a fill of
 * known length rather than sanity-checked on metal.
 * ================================================================== */
#if Z3660_EMU_PROF
static void test_blk_counters(void)
{
	printf("[test] C2 block-idiom counters: BLK_HIT / BLK_INSNS / BLK_BYTES mean what they say\n");

	struct blk_desc c;
	memset(&c, 0, sizeof c);
	c.stage = 1; c.map_dst2 = 1; c.n = 1023;      /* 1024 iterations, 4096 bytes */

	z3660_prof_set_buckets(0);
	z3660_prof_zero();
	blk_run(&g_blk_fast, &c, 256u, 4200);

	/* 4092, not 4096, and the missing longword is the point: the device gate declines on
	 * a cold page, so the FIRST iteration of each destination page is run by the
	 * interpreter and is not the fast path's to claim. One page here, one longword. A
	 * counter that read 4096 would be counting the loop rather than the fast path, and
	 * BLK_INSNS/INSNS -- the map's utilisation verdict -- would be overstated by the ramp
	 * -in on every page of every fill. */
	CHECK_EQ32((uae_u32)z3660_prof_cnt[Z3660_PROF_C_BLK_BYTES], 4096u - 4u,
	           "BLK_BYTES counts what the FAST PATH stored: 4096 less the one longword "
	           "the interpreter ran to warm the page");
	CHECK((uae_u32)z3660_prof_cnt[Z3660_PROF_C_BLK_INSNS]
	      == 2u * (uae_u32)(z3660_prof_cnt[Z3660_PROF_C_BLK_BYTES] / 4u),
	      "BLK_INSNS is two guest instructions per longword: the MOVE and the DBcc\n");
	CHECK(z3660_prof_cnt[Z3660_PROF_C_BLK_HIT] >= 4
	      && z3660_prof_cnt[Z3660_PROF_C_BLK_HIT] <= 16,
	      "BLK_HIT counts CHUNKS: 1024 iterations at a cap of 256 is a handful, not 1024 "
	      "and not 1 -- got %llu\n",
	      (unsigned long long)z3660_prof_cnt[Z3660_PROF_C_BLK_HIT]);
	CHECK(z3660_prof_cnt[Z3660_PROF_C_BLK_INSNS] / z3660_prof_cnt[Z3660_PROF_C_BLK_HIT] >= 16,
	      "mean chunk length is high: a mean near 2 means the recognizer's test is being "
	      "paid per iteration and the whole saving is gone\n");
	/* The counters are of the FAST PATH only. INSNS counts what the run-loop tail
	 * retired, and the tail runs once per chunk, so BLK_INSNS must EXCEED it here --
	 * which is the arithmetic that makes BLK_INSNS/INSNS a share of the guest stream
	 * rather than of the run loop's passes. */
	CHECK(z3660_prof_cnt[Z3660_PROF_C_BLK_INSNS] > z3660_prof_cnt[Z3660_PROF_C_INSNS],
	      "BLK_INSNS counts GUEST instructions, INSNS counts run-loop passes\n");

	/* With the recognizer off, none of them move at all. */
	z3660_prof_zero();
	blk_run(&g_blk_ref, &c, Z3660_BLK_OFF, 4200);
	CHECK_EQ32((uae_u32)z3660_prof_cnt[Z3660_PROF_C_BLK_HIT],   0, "OFF: no chunks");
	CHECK_EQ32((uae_u32)z3660_prof_cnt[Z3660_PROF_C_BLK_INSNS], 0, "OFF: no instructions");
	CHECK_EQ32((uae_u32)z3660_prof_cnt[Z3660_PROF_C_BLK_BYTES], 0, "OFF: no bytes");

	z3660_prof_zero();
	harness_shared_defaults();
	z3660_knobs_sync();
	cpu_bringup(68030);
}

/* ==================================================================
 * THE DOPC_ COUNTERS -- and, through them, THE HIT PATH'S COST
 *
 * The map pre-registers this rung's failure mode by name: **"a high hit rate with a low
 * speedup"**, which is what happens when the entry's tag check costs as much as the fetch
 * it replaces. The counters are what turn that from a worry into a measurement, so their
 * arithmetic is pinned here rather than sanity-checked on metal, against a workload of
 * known shape.
 *
 * It is also the only place the hit path's cost can be attacked from the host at all. An
 * ARM instruction count comes out of objdump, not out of x86-64; what IS measurable here
 * is the WORK REMOVED -- how many instruction-stream fetches the same guest workload
 * still performs with the cache live. That is the numerator of the whole rung, and a
 * capture where the fetches did not fall is a cache that is hitting and not saving.
 * ================================================================== */
struct dopc_cnt {
	uae_u64 c[Z3660_PROF_C_COUNT];
	/* C2 rung 1d: the per-bucket SPANS as well, so the LOOP split's identities can be
	 * asserted against the same run the counters come from rather than against a second
	 * one. Zero unless the caller armed the buckets first. */
	uae_u64 t[Z3660_PROF_B_COUNT];
	int steps;
};

static void dopc_count_run(struct dopc_cnt *out, unsigned knob, uae_u32 n)
{
	struct dopc_desc c;
	const uae_u32 done = DOPC_VCODE + DOPC_MIXED_DONE;
	int steps = 0, budget = (int)(n + 2u) * 8 + 64;

	memset(&c, 0, sizeof c);
	c.n = n; c.blk = Z3660_BLK_OFF;

	dopc_build(&c);
	shared->dopc_on   = knob;
	shared->blk_chunk = Z3660_BLK_OFF;
	z3660_knobs_sync();

	/* Zeroed AFTER the fixture is built: the page-table pokes and the bring-up are not
	 * the workload, and counting them would put the same constant in both arms and
	 * flatten the ratio this test exists to measure. */
	z3660_prof_zero();
	regs.spcflags |= Z_SPCFLAG_MODE_CHANGE;
	while (steps < budget) {
		if (m68k_getpc() == done) break;
		m68k_run_mmu040();
		steps++;
	}
	regs.spcflags = 0;

	memcpy(out->c, (const void *)z3660_prof_cnt, sizeof out->c);
	memcpy(out->t, (const void *)z3660_prof_tcnt, sizeof out->t);
	out->steps = steps;

	shared->dopc_on   = Z3660_DOPC_UNSET;
	shared->blk_chunk = Z3660_BLK_UNSET;
	z3660_knobs_sync();
	mmu040_off();
}

static void test_dopc_counters(void)
{
	printf("[test] C2 decoded-op cache counters: hit rate, and the fetches that stopped happening\n");

	struct dopc_cnt off, on;
	const uae_u32 N = 40;            /* 41 iterations of a five-instruction loop */

	z3660_prof_set_buckets(0);
	dopc_count_run(&off, Z3660_DOPC_OFF,     N);
	dopc_count_run(&on,  Z3660_DOPC_DEFAULT, N);

	/* Printed, not just asserted. The assertions below are floors chosen against the
	 * metal pre-registration; the actual figures are what a reader wants when the metal
	 * capture disagrees with them, and a test that only says PASS cannot supply one. */
	printf("       insns %llu | DOPC hit %llu miss %llu ext %llu | FETCH %llu -> %llu\n",
	       (unsigned long long)on.c[Z3660_PROF_C_INSNS],
	       (unsigned long long)on.c[Z3660_PROF_C_DOPC_HIT],
	       (unsigned long long)on.c[Z3660_PROF_C_DOPC_MISS],
	       (unsigned long long)on.c[Z3660_PROF_C_DOPC_EXT],
	       (unsigned long long)off.c[Z3660_PROF_C_FETCH],
	       (unsigned long long)on.c[Z3660_PROF_C_FETCH]);

	/* OFF is a pure passthrough all the way down to the instrument: the lookup returns
	 * before it has read the table, so it cannot even count a miss. A counter that moved
	 * here would mean the off state is a second implementation rather than the reference
	 * one, which is the property the whole equivalence corpus rests on. */
	CHECK_EQ32((uae_u32)off.c[Z3660_PROF_C_DOPC_HIT],  0, "OFF: no hits");
	CHECK_EQ32((uae_u32)off.c[Z3660_PROF_C_DOPC_MISS], 0, "OFF: not even a miss is counted");
	CHECK_EQ32((uae_u32)off.c[Z3660_PROF_C_DOPC_EXT],  0, "OFF: no extension words served");

	/* THE INVARIANT THAT MAKES THE HIT RATE A HIT RATE. Every run-loop pass consults the
	 * cache exactly once, so HIT + MISS is the number of dispatches -- which is what
	 * DOPC_HIT / (DOPC_HIT + DOPC_MISS) has to be a fraction of for the pre-registered
	 * 88 % to mean anything on metal. */
	CHECK_EQ32((uae_u32)(on.c[Z3660_PROF_C_DOPC_HIT] + on.c[Z3660_PROF_C_DOPC_MISS]),
	           (uae_u32)on.c[Z3660_PROF_C_INSNS],
	           "HIT + MISS == INSNS: every dispatch consults the cache exactly once");

	/* The cache changes what a dispatch COSTS, never how many there are. */
	CHECK_EQ32((uae_u32)on.c[Z3660_PROF_C_INSNS], (uae_u32)off.c[Z3660_PROF_C_INSNS],
	           "the same guest instructions retire either way");
	CHECK_EQ32((uae_u32)on.steps, (uae_u32)off.steps, "...in the same number of run-loop passes");

	/* Hit rate. A five-instruction loop run 41 times misses only on its first pass, so
	 * this workload is friendlier than dhrystone by construction -- the assertion is
	 * against the pre-registered metal bar (>= 88 %) precisely so that it is a floor and
	 * not a fit to this fixture. */
	{
		uae_u64 tot = on.c[Z3660_PROF_C_DOPC_HIT] + on.c[Z3660_PROF_C_DOPC_MISS];
		unsigned pct = tot ? (unsigned)((on.c[Z3660_PROF_C_DOPC_HIT] * 100u) / tot) : 0u;
		CHECK(pct >= 88u,
		      "hit rate %u%% clears the pre-registered >= 88 %% bar on a repeating loop\n", pct);
		CHECK(on.c[Z3660_PROF_C_DOPC_MISS] >= 5,
		      "...and the loop's five distinct PCs each missed once on the way in\n");
	}

	/* THE WORK REMOVED. Uncached, one iteration of this loop performs nine
	 * instruction-stream fetches: five opcodes plus a displacement, a 32-bit immediate,
	 * another displacement and the DBcc's. Cached, the five opcodes come out of the
	 * entries and three of the four extension words come out of the entries' single
	 * slots -- the ADDI's 32-bit immediate goes through get_ilong, which the one-word
	 * slot cannot answer, and it is the honest remainder this design does not reach. */
	CHECK(on.c[Z3660_PROF_C_FETCH] * 3u < off.c[Z3660_PROF_C_FETCH],
	      "instruction-stream fetches fall by more than two thirds (%llu -> %llu)\n",
	      (unsigned long long)off.c[Z3660_PROF_C_FETCH],
	      (unsigned long long)on.c[Z3660_PROF_C_FETCH]);
	CHECK(on.c[Z3660_PROF_C_IFETCH_CALLS] < off.c[Z3660_PROF_C_IFETCH_CALLS],
	      "...and so do the entries into the shared FETCHOP/FETCHEX bracket, which is "
	      "what the stage profile will report as those two buckets shrinking\n");
	CHECK(on.c[Z3660_PROF_C_DOPC_EXT] >= 3u * (uae_u64)N,
	      "three extension words per iteration are served from the entries (%llu)\n",
	      (unsigned long long)on.c[Z3660_PROF_C_DOPC_EXT]);
	CHECK(on.c[Z3660_PROF_C_FETCH] + on.c[Z3660_PROF_C_DOPC_EXT]
	      + on.c[Z3660_PROF_C_DOPC_HIT] >= off.c[Z3660_PROF_C_FETCH],
	      "every fetch the uncached run made is accounted for: still fetched, served "
	      "from a slot, or supplied by a hit\n");

	/* ---- THE WAY HISTOGRAM (C2 v3.1 SS25.2) -----------------------------------------
	 *
	 * Four counters, and the only thing the HOST can pin about them is the wiring --
	 * where the hits land is a property of the guest's PC stream, and this fixture's is
	 * a five-instruction loop, not dhrystone. So the assertions are the two that would
	 * make a metal verdict meaningless if they failed: the histogram partitions the hits
	 * exactly, and it is silent when nothing hit.
	 *
	 * The distribution is PRINTED rather than asserted, for the same reason the hit rate
	 * above is: when the metal capture disagrees with expectations, a reader wants the
	 * fixture's own numbers to compare against, and a test that only says PASS has none. */
	{
		uae_u64 wsum = 0;
		int k;
		for (k = 0; k < (int)Z3660_DOPC_WAYS; k++)
			wsum += on.c[Z3660_PROF_C_DOPC_WAY_BASE + k];
		printf("       dopc ways %llu / %llu / %llu / %llu\n",
		       (unsigned long long)on.c[Z3660_PROF_C_DOPC_WAY0],
		       (unsigned long long)on.c[Z3660_PROF_C_DOPC_WAY1],
		       (unsigned long long)on.c[Z3660_PROF_C_DOPC_WAY2],
		       (unsigned long long)on.c[Z3660_PROF_C_DOPC_WAY3]);
		CHECK_EQ32((uae_u32)wsum, (uae_u32)on.c[Z3660_PROF_C_DOPC_HIT],
		           "the four way counters partition DOPC_HIT exactly -- the identity the "
		           "metal verdict's way-0 share is computed against");
		/* OFF must move none of them: the lookup returns before it reads the table, so
		 * there is no way to attribute. Same property as the three counters above, and
		 * it is what makes the off arm a reference rather than a second implementation. */
		{
			uae_u64 offsum = 0;
			for (k = 0; k < (int)Z3660_DOPC_WAYS; k++)
				offsum += off.c[Z3660_PROF_C_DOPC_WAY_BASE + k];
			CHECK_EQ32((uae_u32)offsum, 0, "OFF: no way is attributed");
		}
		/* A five-instruction loop with distinct PCs cannot collide in a 1024-set index,
		 * so every hit here is way 0 -- which is the fixture saying what it is, not a
		 * prediction about dhrystone. It also pins the counters to the RIGHT ways: an
		 * off-by-one in WAY_BASE would show up as way 1 carrying the traffic. */
		CHECK_EQ32((uae_u32)on.c[Z3660_PROF_C_DOPC_WAY0], (uae_u32)on.c[Z3660_PROF_C_DOPC_HIT],
		           "this fixture's PCs never collide, so every hit is way 0 -- and the "
		           "base of the counter block is therefore pinned");
	}

	/* ---- INVALIDATION ATTRIBUTION (C2 rung 1b, kept by 1c) --------------------------
	 *
	 * The counters that decided rung 1c. Before them the firmware reported one aggregate
	 * DOPC_INVAL and discarded the `cause` it was already being passed, so the 2026-08-23
	 * verdict could establish that the misses were invalidation re-warm but not WHICH
	 * flush caused them -- and a narrowing aimed at the wrong cause is a correctness risk
	 * taken for nothing. Their first metal use said the page filter was aimed at 6-9 % of
	 * requests, before any timing was consulted.
	 *
	 * Driven from a cache in a KNOWN state rather than from a workload: what is pinned is
	 * that each cause lands in ITS OWN counter and that the block totals the aggregate. A
	 * cause filed under its neighbour's name would fail nothing otherwise.
	 *
	 * THROUGH THE REAL CALL SITES, not through z3660_dopc_invalidate() with a cause id
	 * typed out here -- and that distinction was found by mutation rather than by reading.
	 * With the causes named directly, making the DMA arm call the invalidator's internals
	 * and skip the attribution entirely passed the whole corpus: the sum identity claimed
	 * to catch "a call site bypassing the attribution" while never exercising one. Driving
	 * flush_mmu040(), mmu_flush_atc() and z3660_dopc_invalidate_dma() makes the claim true
	 * of the three sites the guest reaches most. */
	{
		uae_u64 v0 = z3660_prof_cnt[Z3660_PROF_C_DOPC_INVAL];
		uae_u64 f0 = z3660_prof_cnt[Z3660_PROF_C_DOPC_IV_FLUSH];
		uae_u64 d0 = z3660_prof_cnt[Z3660_PROF_C_DOPC_IV_DMA];
		uae_u64 p0 = z3660_prof_cnt[Z3660_PROF_C_DOPC_IV_PFLUSH];
		uae_u64 req = 0, iv, wr;
		int c;

		flush_mmu040(0, 0);                                 /* CPUSHL / CINV */
		z3660_dopc_invalidate_dma(DOPC_PCODE, 0x1000u);     /* core0's mailbox arm */
		mmu_flush_atc(DOPC_VCODE, true, true);              /* PFLUSH of one page */

		CHECK_EQ32((uae_u32)(z3660_prof_cnt[Z3660_PROF_C_DOPC_IV_FLUSH] - f0), 1u,
		           "IV_FLUSH counts the CPUSHL arm and only that");
		CHECK_EQ32((uae_u32)(z3660_prof_cnt[Z3660_PROF_C_DOPC_IV_DMA] - d0), 1u,
		           "IV_DMA counts the DMA arm and only that");
		CHECK_EQ32((uae_u32)(z3660_prof_cnt[Z3660_PROF_C_DOPC_IV_PFLUSH] - p0), 1u,
		           "IV_PFLUSH counts the per-page flush -- the cause that was summed into "
		           "the aggregate FLUSH before and could not be seen");
		CHECK_EQ32((uae_u32)(z3660_prof_cnt[Z3660_PROF_C_DOPC_INVAL] - v0), 3u,
		           "DOPC_INVAL counts invalidations, whatever the cause -- rung 1c put "
		           "every request back to performing one");

		/* THE SUM IDENTITY, restored by the revert. Rung 1b's short-circuit made this an
		 * inequality; with it gone, every request that reaches the invalidator performs
		 * one, so the block sums to the aggregate exactly. It is the check that would
		 * catch a call site bypassing the attribution, or a cause id out of range landing
		 * outside the block and silently incrementing a neighbour.
		 *
		 * IV_WRAP COMES OUT, for the same reason z3660_prof.cpp takes it out: it is in
		 * the block but is not a request -- the wrap arm raises it from INSIDE the
		 * invalidation another cause asked for, so it adds to the sum without adding to
		 * DOPC_INVAL.
		 *
		 * ONE WRAP IS DRIVEN INTO THE WINDOW ON PURPOSE, through a real call site, so
		 * the subtraction is LOAD-BEARING. A window that happened to contain no wrap
		 * would leave `- wr` untested and the identity accidentally true -- and an
		 * identity nobody can make fail is not evidence. Dropping the term must break
		 * this check, and with the wrap here it does. */
		z3660_dopc_force_generation(0xFFFFFFFFu);
		dopc_m_dma();

		for (c = 0; c < Z3660_DOPC_INV_COUNT; c++)
			req += z3660_prof_cnt[Z3660_PROF_C_DOPC_IV_BASE + c];
		iv = z3660_prof_cnt[Z3660_PROF_C_DOPC_INVAL];
		wr = z3660_prof_cnt[Z3660_PROF_C_DOPC_IV_WRAP];
		CHECK(req - wr == iv,
		      "sum(IV_*) - IV_WRAP == DOPC_INVAL: every invalidation is attributed to "
		      "exactly one cause (%llu - %llu vs %llu)\n",
		      (unsigned long long)req, (unsigned long long)wr, (unsigned long long)iv);
	}

	/* ---- THE GENERATION WRAP RAISES ITS OWN CAUSE ------------------------------------
	 *
	 * INV_WRAP was defined with the cause block in rung 1b and had no firmware call site
	 * until now: z3660_dopc_do_invalidate() did its memset silently, so the only thing
	 * that ever incremented IV_WRAP was a caller typing the id out -- which no board code
	 * does and which this harness's own fixture used to do. On metal the counter read zero
	 * however many times a boot wrapped, i.e. it could not mean what its name says.
	 *
	 * THE ANTI-VACUITY ARM IS THE FIRST HALF: an ordinary invalidation that does NOT wrap
	 * must leave IV_WRAP alone. Without it, a wrap arm that raised the cause on every
	 * invalidation would pass the second half unchanged.
	 *
	 * The wrap itself -- the memset and the resume at generation 1 -- is pinned by
	 * dopc_coh_one(dopc_m_wrap, ...) in test_dopc_coherence(), which runs in all three
	 * variants and asserts the guest-visible effect rather than a counter. What is added
	 * here is only the attribution, which needs counters and is therefore prof-only. */
	{
		uae_u64 w0, d0, v0;

		z3660_dopc_force_generation(0x1000u);
		w0 = z3660_prof_cnt[Z3660_PROF_C_DOPC_IV_WRAP];
		d0 = z3660_prof_cnt[Z3660_PROF_C_DOPC_IV_DMA];
		v0 = z3660_prof_cnt[Z3660_PROF_C_DOPC_INVAL];

		dopc_m_dma();
		CHECK_EQ32(z3660_dopc_generation(), 0x1001u,
		           "an ordinary invalidation bumps the generation and does not wrap");
		CHECK_EQ32((uae_u32)(z3660_prof_cnt[Z3660_PROF_C_DOPC_IV_WRAP] - w0), 0u,
		           "...and IV_WRAP does NOT move for it (anti-vacuity: the arm being "
		           "tested is the wrap, not the invalidation carrying it)");

		z3660_dopc_force_generation(0xFFFFFFFFu);
		dopc_m_dma();

		CHECK_EQ32((uae_u32)(z3660_prof_cnt[Z3660_PROF_C_DOPC_IV_WRAP] - w0), 1u,
		           "the generation wrap raises IV_WRAP ITSELF -- the cause the firmware "
		           "defined in rung 1b and never gave a call site");
		CHECK_EQ32((uae_u32)(z3660_prof_cnt[Z3660_PROF_C_DOPC_IV_DMA] - d0), 2u,
		           "...the requesting cause is still attributed to ITS own counter: the "
		           "wrap adds a cause, it does not steal one");
		CHECK_EQ32((uae_u32)(z3660_prof_cnt[Z3660_PROF_C_DOPC_INVAL] - v0), 2u,
		           "...and a wrap is ONE performed invalidation, not two: the memset IS "
		           "that invalidation at the boundary, not a second one");
		CHECK_EQ32(z3660_dopc_generation(), 1u,
		           "...and the generation resumes at 1, never at the reserved 0");
	}

	/* ---- THE CALL-SITE NARROWING'S OWN COUNTER (C2 rung 1c) --------------------------
	 *
	 * IV_CACR_SKIP exists because of a defect the 2026-08-23 verdict found in its own
	 * pre-registered instrument. The CACR narrowing decides in newcpu_common.cpp, BEFORE
	 * z3660_dopc_invalidate() -- so an inert write is not a request that was skipped, it
	 * is a request that is never counted, and `sum(IV_*) - DOPC_INVAL` could account for
	 * only 7.3 % of a measured 54.8 % fall in invalidations. The rest had to be
	 * reconstructed by hand across two sessions' captures.
	 *
	 * So both sides are pinned here: the write that CHANGES CACR lands in IV_CACR and
	 * performs, the write that changes nothing lands in IV_CACR_SKIP and does not. Without
	 * the second assertion a narrowing that quietly stopped narrowing would look identical
	 * to one that worked. */
	{
		uae_u64 v0, c0, s0;
		uae_u32 v;
		int   was_model = currprefs.cpu_model;
		int   was_s     = regs.s;
		uae_u32 was_cacr = regs.cacr;

		/* The model is set rather than brought up, because the code under test is the
		 * mask ladder in m68k_move2c() and nothing else: 68040 is what makes 0x80008000
		 * the retained mask and bit 15 a real cache enable. A cpu_bringup() here would
		 * re-initialise state the surrounding test owns. Restored below. */
		currprefs.cpu_model = 68040;
		regs.s = 1;
		v0 = z3660_prof_cnt[Z3660_PROF_C_DOPC_INVAL];
		c0 = z3660_prof_cnt[Z3660_PROF_C_DOPC_IV_CACR];
		s0 = z3660_prof_cnt[Z3660_PROF_C_DOPC_IV_CACR_SKIP];

		v = regs.cacr ^ 0x8000u;      /* the 68040's instruction-cache enable */
		m68k_move2c(2, &v);
		CHECK_EQ32((uae_u32)(z3660_prof_cnt[Z3660_PROF_C_DOPC_IV_CACR] - c0), 1u,
		           "a CACR write that CHANGES a retained bit reaches the invalidator and "
		           "counts as IV_CACR");
		CHECK_EQ32((uae_u32)(z3660_prof_cnt[Z3660_PROF_C_DOPC_IV_CACR_SKIP] - s0), 0u,
		           "...and is not counted as a skip");
		CHECK_EQ32((uae_u32)(z3660_prof_cnt[Z3660_PROF_C_DOPC_INVAL] - v0), 1u,
		           "...and an invalidation is performed");

		v = regs.cacr;                /* write it back unchanged */
		m68k_move2c(2, &v);
		CHECK_EQ32((uae_u32)(z3660_prof_cnt[Z3660_PROF_C_DOPC_IV_CACR_SKIP] - s0), 1u,
		           "an INERT CACR write is counted -- as a skip, at the call site, which "
		           "is the only place it is visible at all");
		CHECK_EQ32((uae_u32)(z3660_prof_cnt[Z3660_PROF_C_DOPC_IV_CACR] - c0), 1u,
		           "...and NOT as a cause: it never reached z3660_dopc_invalidate()");
		CHECK_EQ32((uae_u32)(z3660_prof_cnt[Z3660_PROF_C_DOPC_INVAL] - v0), 1u,
		           "...and no second invalidation was performed");

		regs.cacr = was_cacr;
		regs.s = was_s;
		currprefs.cpu_model = was_model;
		set_cpu_caches(false);
		regs.spcflags = 0;
	}

	/* ---- THE OFF ARM'S COUNTER CONTRACT, CORRECTED ----------------------------------
	 *
	 * docs/decoded-op-cache.md claimed "with the cache off, none of them move at all",
	 * and the 2026-08-23 verdict caught that being false: DOPC_INVAL read 164,311 and
	 * 2,951,874 in the two cache-off arms, because z3660_dopc_invalidate() never had an
	 * on/off test -- only the LOOKUP consults the switch. Rung 1b changed the fact as well
	 * as the sentence, and rung 1c KEPT that half while reverting the narrowing it
	 * originally rode in on: the switch is now tested in z3660_dopc_invalidate() itself
	 * rather than reached through an empty-cache short-circuit, so the property survives
	 * on its own merits. The two kinds of counter move in opposite directions and the
	 * corrected contract is pinned here rather than left to prose:
	 *
	 *   HIT / MISS / EXT   exactly 0 -- the lookup returns before it can count.
	 *   DOPC_INVAL         exactly 0 -- the invalidation path consults the switch too.
	 *   IV_*               DO MOVE. They count what the guest asked for, and the guest
	 *                      goes on issuing CPUSHL and PFLUSH whatever this switch says.
	 *
	 * That last line is the useful one: it makes the OFF arm a free measurement of the
	 * guest's own invalidation rate, which is the denominator every narrowing decision
	 * needs and which the ON arm cannot supply uncontaminated.
	 *
	 * AND THE ON TRANSITION IS THE SAFETY ARGUMENT, so it is driven rather than asserted:
	 * skipping invalidations while off is only sound because coming back on invalidates
	 * unconditionally. A cached decode that survived an off window would be a stale decode
	 * the moment the cache answered again, and nothing else in the corpus looks. */
	{
		struct dopc_cnt offc;

		z3660_prof_zero();
		dopc_count_run(&offc, Z3660_DOPC_OFF, N);

		/* dopc_count_run() restores the knob to UNSET, which resolves to ON. Put the
		 * switch back OFF explicitly and zero AFTER the resync, so what is measured below
		 * is one hand-driven request against a switch that is genuinely off -- not the
		 * residue of the run above. */
		shared->dopc_on = Z3660_DOPC_OFF;
		z3660_knobs_sync();
		z3660_prof_zero();

		z3660_dopc_invalidate(Z3660_DOPC_INV_FLUSH);
		CHECK_EQ32((uae_u32)z3660_prof_cnt[Z3660_PROF_C_DOPC_IV_FLUSH], 1u,
		           "OFF: the CAUSE still counts -- the guest's flush rate is measurable "
		           "with the cache switched off");
		CHECK_EQ32((uae_u32)z3660_prof_cnt[Z3660_PROF_C_DOPC_INVAL], 0u,
		           "OFF: but no invalidation is PERFORMED -- the switch reaches the "
		           "invalidation path, not only the lookup");

		/* The transition back on, which is what licenses the skips above. */
		{
			uae_u32 g = z3660_dopc_generation();
			shared->dopc_on = Z3660_DOPC_DEFAULT;
			z3660_knobs_sync();
			CHECK(z3660_dopc_generation() != g,
			      "...and switching back ON invalidates unconditionally, so nothing that "
			      "went stale during the off window can be answered from (%u -> %u)\n",
			      (unsigned)g, (unsigned)z3660_dopc_generation());
		}
	}

	z3660_prof_zero();
	harness_shared_defaults();
	shared->dopc_on = Z3660_DOPC_UNSET;
	z3660_knobs_sync();
	cpu_bringup(68030);
}

/* ==================================================================
 * CYBERSTORM C2 RUNG 1d -- THE `LOOP` SPLIT
 *
 * The C2 map's v3 ranked `LOOP` FIRST at 26.38-26.54 % of the interpreter and then
 * REFUSED to give it a reclaim estimate, because nothing had ever enumerated what is in
 * it: "a reclaim fraction for a 26 % bucket whose contents have never been enumerated
 * would be a guess dressed as a number". SS19 item 4 asks for one probe bracket around the
 * decoded-op lookup and calls it the highest-value instrument change available. Ids 14/15/
 * 16 are that bracket and two more beside it.
 *
 * This test has two halves and they fail differently on purpose.
 *
 *   THE PHASE-MACHINE HALF drives the buckets by hand against the injectable clock, so
 *   exclusivity and the residue are asserted as ARITHMETIC. It is the half that would
 *   catch an ENTER without its EXIT, or a sub-stage whose cycles also land in the parent.
 *
 *   THE EXECUTION HALF runs the real 040 loop and asserts the four IDENTITIES the split
 *   claims. They matter more than they look: the whole reason the map can consume a
 *   bucket's corrected share is that per-bucket spans self-check, and a bracket wired to
 *   the wrong quantity would produce a number that balances and means something else --
 *   which is the failure this profiler has already had twice (v1's ATC_HIT, and the
 *   2 x IFETCH_CALLS substitute the spans replaced).
 * ================================================================== */
static void test_prof_loop_split(void)
{
	printf("[test] C2 rung 1d LOOP split: the lookup, the fill and the recognizer leave the residue\n");

	/* ---- 1. the phase machine, by hand ------------------------------------------- */
	z3660_prof_set_buckets(1);
	z3660_prof_zero();
	prof_clock(2000);
	z3660_prof_loop_reset();

	/* One dispatch's worth of loop, in the order m68k_run_mmu040 runs it -- including the
	 * tail bracket at the end, because in the real loop THAT is what closes the residue.
	 * Leaving it off is not a shorter test, it is a different one: the last stretch of
	 * LOOPRES is charged by the next transition, so with no next transition it is charged
	 * to nothing and the totals quietly come up short. Spans are chosen distinct so a
	 * mis-attribution cannot alias onto another bucket's expectation. */
	prof_clock(2004);                            /* 4 cyc of LOOPRES: the loop head     */
	z3660_prof_enter(Z3660_PROF_B_DOPCFIND);
	prof_clock(2013); z3660_prof_exit();         /* 9 of DOPCFIND                       */
	prof_clock(2020);                            /* 7 more of LOOPRES                   */
	z3660_prof_enter(Z3660_PROF_B_BLKREC);
	prof_clock(2023); z3660_prof_exit();         /* 3 of BLKREC (a decline)             */
	prof_clock(2027);                            /* 4 more of LOOPRES                   */
	z3660_prof_enter(Z3660_PROF_B_DOPCFILL);
	prof_clock(2048); z3660_prof_exit();         /* 21 of DOPCFILL                      */
	prof_clock(2050);
	z3660_prof_enter(Z3660_PROF_B_TAIL);         /* 2 more of LOOPRES, charged here     */
	prof_clock(2060); z3660_prof_exit();         /* 10 of TAILADV                       */

	CHECK_EQ32((uae_u32)z3660_prof_acc[Z3660_PROF_B_DOPCFIND], 9,  "DOPCFIND gets its 9");
	CHECK_EQ32((uae_u32)z3660_prof_acc[Z3660_PROF_B_BLKREC],   3,  "BLKREC gets its 3");
	CHECK_EQ32((uae_u32)z3660_prof_acc[Z3660_PROF_B_DOPCFILL], 21, "DOPCFILL gets its 21");
	CHECK_EQ32((uae_u32)z3660_prof_acc[Z3660_PROF_B_TAIL],     10, "TAILADV is untouched by the split");
	CHECK_EQ32((uae_u32)z3660_prof_acc[Z3660_PROF_B_LOOP], 17,
	           "id 0 keeps only the RESIDUE (4 + 7 + 4 + 2) -- it is LOOPRES now, not LOOP");
	{
		uae_u64 loop = z3660_prof_acc[Z3660_PROF_B_LOOP]
		             + z3660_prof_acc[Z3660_PROF_B_DOPCFIND]
		             + z3660_prof_acc[Z3660_PROF_B_DOPCFILL]
		             + z3660_prof_acc[Z3660_PROF_B_BLKREC];
		uae_u64 total = 0; int i;
		CHECK_EQ32((uae_u32)loop, 50,
		           "the four ids re-sum to the whole dispatch loop -- which is what v2 "
		           "reported as LOOP, and what the [PROF] l line prints");
		for (i = 0; i < Z3660_PROF_B_COUNT; i++) total += z3660_prof_acc[i];
		CHECK_EQ32((uae_u32)total, 60,
		           "and the split changes no total: sum(buckets) is still the elapsed span");
	}
	/* The cost, and it has to be VISIBLE in TRANSITIONS or the probe subtraction
	 * under-corrects -- the same rule the tail split was held to. */
	CHECK_EQ32((uae_u32)z3660_prof_cnt[Z3660_PROF_C_TRANSITIONS], 8,
	           "four bracketed stages = eight transitions");
	CHECK_EQ32((uae_u32)(2u * (z3660_prof_tcnt[Z3660_PROF_B_DOPCFIND]
	                         + z3660_prof_tcnt[Z3660_PROF_B_DOPCFILL]
	                         + z3660_prof_tcnt[Z3660_PROF_B_BLKREC])), 6,
	           "...six of which are rung 1d's, which is exactly the arithmetic the "
	           "[PROF] l cost line prints so a capture can be re-priced without them");

	/* ---- 2. the identities, under real execution --------------------------------- */
	{
		struct dopc_cnt on, off;
		const uae_u32 N = 40;

		z3660_prof_set_buckets(1);
		dopc_count_run(&on, Z3660_DOPC_DEFAULT, N);

		printf("       spans: DOPCFIND %llu DOPCFILL %llu BLKREC %llu | dispatches %llu "
		       "miss %llu\n",
		       (unsigned long long)on.t[Z3660_PROF_B_DOPCFIND],
		       (unsigned long long)on.t[Z3660_PROF_B_DOPCFILL],
		       (unsigned long long)on.t[Z3660_PROF_B_BLKREC],
		       (unsigned long long)(on.c[Z3660_PROF_C_DOPC_HIT] + on.c[Z3660_PROF_C_DOPC_MISS]),
		       (unsigned long long)on.c[Z3660_PROF_C_DOPC_MISS]);

		/* Neither z3660_dopc_find() nor z3660_dopc_fill() touches guest memory, so neither
		 * can be abandoned by a fault unwind and both of these are EXACT -- which is what
		 * makes them assertions rather than tolerances. THIS FIXTURE TAKES NO FAULTS, which
		 * is why the fill row is an equality here: on a real capture the identity is the
		 * BOUND DOPC_MISS-FAULTS <= spans <= DOPC_MISS, because a miss whose opcode fetch
		 * faults never reaches the fill. The firmware notes that gap and warns only outside
		 * the bound; here there is no gap to note. */
		CHECK_EQ32((uae_u32)on.t[Z3660_PROF_B_DOPCFIND],
		           (uae_u32)(on.c[Z3660_PROF_C_DOPC_HIT] + on.c[Z3660_PROF_C_DOPC_MISS]),
		           "DOPCFIND spans == DOPC_HIT + DOPC_MISS: the bracket is entered exactly "
		           "once per dispatch, which is what makes its share the LOOKUP's share");
		CHECK_EQ32((uae_u32)on.t[Z3660_PROF_B_DOPCFIND], (uae_u32)on.c[Z3660_PROF_C_INSNS],
		           "...and equals INSNS on a run with no faults in it -- the dispatch "
		           "identity the map states as HIT + MISS == INSNS + FAULTS");
		CHECK_EQ32((uae_u32)on.t[Z3660_PROF_B_DOPCFILL], (uae_u32)on.c[Z3660_PROF_C_DOPC_MISS],
		           "DOPCFILL spans == DOPC_MISS: the fill bracket is on the MISS arm only, "
		           "so a hit pays two transitions for this split and not four");
		CHECK(on.t[Z3660_PROF_B_BLKREC] <= on.t[Z3660_PROF_B_DOPCFIND],
		      "BLKREC is entered at most once per dispatch (%llu <= %llu)\n",
		      (unsigned long long)on.t[Z3660_PROF_B_BLKREC],
		      (unsigned long long)on.t[Z3660_PROF_B_DOPCFIND]);

		/* AND HERE IT IS EQUAL, which is a real property and not a fixture artefact:
		 * dopc_count_run() pins BLK OFF, and a cap-0 recognizer returns before the head
		 * discrimination that would clear *shapep -- so every entry is filled with the
		 * conservative "may be an idiom" verdict and every later HIT calls the recognizer
		 * anyway. WITH RUNG 0 SWITCHED OFF, RUNG 1's IDIOM-VERDICT OPTIMISATION CANNOT
		 * ENGAGE. That is worth a line here because it is exactly the confound rung 1c's
		 * verdict was caught by one level up: a justification measured with another rung
		 * switched off is not a justification for the shipping configuration. */
		CHECK_EQ32((uae_u32)on.t[Z3660_PROF_B_BLKREC], (uae_u32)on.t[Z3660_PROF_B_DOPCFIND],
		           "...and with BLK OFF it is entered on every one of them: a cap-0 "
		           "recognizer never clears the shape bit, so no cached verdict can veto it");

		/* The sum identity the whole `s` block rests on has to survive three more buckets:
		 * every transition ends exactly one span and it is charged to exactly one id. */
		{
			uae_u64 spans = 0; int i;
			for (i = 0; i < Z3660_PROF_B_COUNT; i++) spans += on.t[i];
			CHECK(spans == on.c[Z3660_PROF_C_TRANSITIONS],
			      "sum(spans) == TRANSITIONS still holds across 17 buckets (%llu vs %llu)\n",
			      (unsigned long long)spans,
			      (unsigned long long)on.c[Z3660_PROF_C_TRANSITIONS]);
		}

		/* THE CACHE-OFF SHAPE, stated so it is not read as a defect on the day a session
		 * takes an off arm. The lookup returns before it counts, so the two dispatch
		 * counters stay at zero while DOPCFIND goes on being entered every pass -- and
		 * DOPCFILL, whose bracket is on the (now universal) miss arm, prices the fill's
		 * own switch test on every dispatch instead of on every miss. */
		z3660_prof_set_buckets(1);
		dopc_count_run(&off, Z3660_DOPC_OFF, N);
		CHECK_EQ32((uae_u32)(off.c[Z3660_PROF_C_DOPC_HIT] + off.c[Z3660_PROF_C_DOPC_MISS]), 0u,
		           "OFF: the lookup returns before it can count, as it always has");
		CHECK_EQ32((uae_u32)off.t[Z3660_PROF_B_DOPCFIND], (uae_u32)off.c[Z3660_PROF_C_INSNS],
		           "OFF: but DOPCFIND is still entered every dispatch -- with the cache off "
		           "these two buckets price the two switch tests and nothing else");
		CHECK_EQ32((uae_u32)off.t[Z3660_PROF_B_DOPCFILL], (uae_u32)off.c[Z3660_PROF_C_INSNS],
		           "OFF: ...and DOPCFILL with it, because every pass takes the miss arm");
	}

	z3660_prof_set_buckets(0);
	z3660_prof_zero();
	harness_shared_defaults();
	shared->dopc_on = Z3660_DOPC_UNSET;
	z3660_knobs_sync();
	cpu_bringup(68030);
}
#endif /* Z3660_EMU_PROF */

/* ==================================================================
 * WARP ENGINE -- M1 ROUND 1, THE PLUMBING SLICE
 *
 * docs/WARP-ENGINE.md §0.6 is the pre-registration and it was committed before any of the
 * code below its gates score. Read it first; this corpus is what holds the engine to it.
 *
 * WHAT IS BEING GUARDED, and none of it is self-evident from reading the diff:
 *
 *   1. DISARMED IS THE REFERENCE MACHINE, and disarmed is what every shipped image is until
 *      the M5 gate. If the engine could arm itself -- from a zeroed mailbox, from a
 *      neighbour's sentinel, from a config key, from the PREVIOUS BOOT -- an unproven engine
 *      would be running on a card nobody armed. Most of test_warp_switch() is about that and
 *      not about the engine at all.
 *   2. ARMED WITH NULL BLOCKS, THE TWO ARMS ARE IDENTICAL. Not equivalent -- identical. A
 *      null block changes nothing, so every register, every flag, every byte of guest memory
 *      and every instruction boundary must match the interpreter exactly. That is the
 *      strongest form this gate will ever take; every later round can only weaken it into
 *      "equivalent", so it is worth taking at full strength while it is available.
 *   3. THE COUNTERS MEAN WHAT §0.5.6 SAYS THEY MEAN. A counter incremented at the wrong site
 *      does not fail; it reports a plausible wrong number under the right name, which is the
 *      failure this campaign has paid for twice.
 *
 * AND EVERY CLAIM HAS A MUTATION ARM. "It catches X" is worth exactly as much as the test
 * that fails when it stops being true -- rung 1b's attribution test once passed a mutation
 * that bypassed the whole attribution, which is how this lane learned to write the arm
 * rather than assume it. The arms are M-5 (the key drops the supervisor bit), M-7 (the
 * cadence counter fed 1 per block), M-ICACHE (a block entered without the publish sequence),
 * M-ARM (a truthy word arms the switch) and M-REPUB (the per-boot republish deleted).
 *
 * IT RUNS IN ALL THREE VARIANTS, deliberately: the wall-clock arm of every verdict is a LEAN
 * boot, so the lean build is the one whose switch and attestation have to be right. Only the
 * profiler-counter assertions are guarded, because z3660_prof_cnt[] does not exist in a lean
 * binary -- everything else is written against the engine's own state instead.
 * ================================================================== */

/* The corpus: eight user-mode instructions in one page, looping forever. Chosen so that
 * every discovered block head is a distinct set in the block table (PCs 0x00400000..0x0E
 * index sets 0..7), and so that the loop touches the data MMU as well as the register file
 * -- a corpus that never reads or writes guest memory would leave the accessor path, and
 * therefore most of the reason the engine is hard, entirely unexercised.
 *
 * It cannot fault: every page it touches is mapped in BOTH roots, and no instruction in it
 * arms mmufixup. That is deliberate. Faults are M2's subject and there is nothing in round 1
 * that could take one -- a null block does nothing at all. */
#define WARP_VCODE  0x00400000u
#define WARP_PCODE  0x0C000000u
#define WARP_VDATA  0x00401000u
#define WARP_PDATA  0x0E000000u
#define WARP_VSTK   0x00402000u
#define WARP_PSTK   0x0F000000u
#define WARP_LOOP_INSNS 8u

static void warp_corpus_build(void)
{
	static const uae_u16 body[WARP_LOOP_INSNS] = {
		0x5280,   /* addq.l #1,d0                                     */
		0x7205,   /* moveq  #5,d1                                     */
		0xD081,   /* add.l  d1,d0                                     */
		0x2400,   /* move.l d0,d2                                     */
		0x5382,   /* subq.l #1,d2                                     */
		0x2082,   /* move.l d2,(a0)   -- a guest WRITE through the MMU */
		0x2610,   /* move.l (a0),d3   -- and a guest READ back         */
		0x60F0,   /* bra.s  -16       -- round the loop again          */
	};
	unsigned i;

	mmu040_pt_init();
	mmu040_map_page(WARP_VCODE, WARP_PCODE);
	mmu040_map_page(WARP_VDATA, WARP_PDATA);
	mmu040_map_page(WARP_VSTK,  WARP_PSTK);
	regs.urp = M4_UROOT;                       /* user gets its own root, as AMIX does */
	mmu040_map_user(WARP_VCODE, WARP_PCODE);
	mmu040_map_user(WARP_VDATA, WARP_PDATA);
	mmu040_map_user(WARP_VSTK,  WARP_PSTK);

	for (i = 0; i < WARP_LOOP_INSNS; i++)
		hram_poke16((WARP_PCODE | (WARP_VCODE & 0xFFF)) + i * 2, body[i]);
}

/* Put the machine in USER mode at the top of the loop. Generation one is user-mode only --
 * a block is never entered with regs.s set -- so a supervisor corpus would exercise the
 * decline gate and nothing else. */
static void warp_corpus_enter(void)
{
	int i;

	for (i = 0; i < 8; i++) { m68k_dreg(regs, i) = 0; m68k_areg(regs, i) = 0; }
	regs.isp = regs.msp = WARP_VSTK | 0xF00;
	regs.m = 0;
	m68k_areg(regs, 0) = WARP_VDATA;
	m68k_areg(regs, 7) = WARP_VSTK | 0xE00;
	m68k_setpc(WARP_VCODE);
	regs.t0 = regs.t1 = 0;
	regs.s = 0;                                /* <-- USER MODE */
	MakeSR();
	regs.spcflags = Z_SPCFLAG_MODE_CHANGE;     /* -> one instruction per run-loop call */
}

/* core0's per-boot publish plus core1's resync -- i.e. what a cold boot leaves behind. */
static void warp_boot(void)
{
	harness_shared_defaults();
	z3660_knobs_sync();
	z3660_warp_set_mutant(Z3660_WARP_MUT_NONE);
	z3660_warp_set_cadence_mutant(0);
	z3660_warp_stats_zero();
}

static void warp_write_knob(uae_u32 word)
{
	shared->warp_on = word;
	z3660_knobs_sync();
}

/* ------------------------------------------------------------------ the switch ------- */

static void test_warp_switch(void)
{
	unsigned i;
	int armed_any;

	printf("[test] WARP switch: DISARMED is the shipped machine, and an arm lasts one boot\n");

	warp_boot();

	/* ---- THE RESOLVE RULE, AND ITS POLARITY --------------------------------------
	 *
	 * The one rule that turns a mailbox word into a state, and the ONE place in this
	 * firmware where "unset" does not mean "the optimisation is on". blk_chunk, dopc_on
	 * and dfast_on all read unset -> DEFAULT -> ON, because for them the shipped
	 * behaviour IS the optimisation and a core0 built before the field existed must still
	 * run it. The reasoning here is identical and the value is inverted: unset means the
	 * shipped behaviour, and an engine that is not shipped until M5 has a shipped
	 * behaviour of OFF. If that inverts, an unproven engine becomes the default build and
	 * nothing else in this corpus would notice. */
	CHECK_EQ32(z3660_warp_resolve(Z3660_WARP_OFF), 0u,
	           "UNSET (0) resolves to DISARMED -- the opposite of every other C2 switch, "
	           "because the engine is not shipped behaviour until M5 (plan Q9)");
	CHECK_EQ32(z3660_warp_resolve(1u), 0u,
	           "1 does NOT arm: arming takes an exact magic, not a truthy word");
	CHECK_EQ32(z3660_warp_resolve(0xFFFFFFFFu), 0u,
	           "...nor does 0xFFFFFFFF, which is the OFF sentinel of the three switches "
	           "immediately before this field in SHARED");
	CHECK_EQ32(z3660_warp_resolve(Z3660_WARP_ARM), 1u, "the magic, and only the magic, arms");
	CHECK_EQ32(z3660_warp_resolve(Z3660_WARP_ARM - 1u), 0u, "...one below it does not");
	CHECK_EQ32(z3660_warp_resolve(Z3660_WARP_ARM + 1u), 0u, "...and one above it does not");

	/* MUTATION M-ARM, by the only route that does not require compiling a defect: the
	 * neighbours' whole vocabulary, by NAME rather than by value. A beta resync has already
	 * mis-merged SHARED once (docs/piscsi-service-path.md), so the failure being guarded is
	 * a neighbouring field's value landing in this one -- and a truthiness test would arm on
	 * eight of the values below. */
	CHECK(z3660_warp_resolve(Z3660_BLK_OFF) == 0u
	   && z3660_warp_resolve(Z3660_BLK_UNSET) == 0u
	   && z3660_warp_resolve(Z3660_BLK_DEFAULT) == 0u
	   && z3660_warp_resolve(64u) == 0u
	   && z3660_warp_resolve(256u) == 0u
	   && z3660_warp_resolve(1024u) == 0u
	   && z3660_warp_resolve(Z3660_DOPC_OFF) == 0u
	   && z3660_warp_resolve(Z3660_DOPC_UNSET) == 0u
	   && z3660_warp_resolve(Z3660_DOPC_DEFAULT) == 0u
	   && z3660_warp_resolve(Z3660_DFAST_OFF) == 0u
	   && z3660_warp_resolve(Z3660_DFAST_DEFAULT) == 0u,
	      "no value any neighbouring mailbox field can hold arms this one\n");

	/* A sweep, so the claim is about the FUNCTION and not about a list. 196 608 words
	 * across the three ranges a stray write is most likely to produce -- small integers,
	 * all-ones-high, and a value shifted into the top half. The magic is in none of them,
	 * so the expected count is exactly zero. */
	armed_any = 0;
	for (i = 0; i <= 0xFFFFu; i++) {
		if (z3660_warp_resolve(i))               armed_any++;
		if (z3660_warp_resolve(0xFFFF0000u | i)) armed_any++;
		if (z3660_warp_resolve(i << 16))         armed_any++;
	}
	CHECK_EQ32((uae_u32)armed_any, 0u,
	           "196608 swept mailbox words, none of them arms the engine");

	/* ---- THE PER-BOOT REPUBLISH, AND THE ANTI-VACUITY CHECK ----------------------
	 *
	 * MUTATION M-REPUB, and it is shown rather than argued -- a future reader who deletes
	 * core0's unconditional publish as redundant should fail here.
	 *
	 * The mailbox is DDR that survives a warm reset, so an arm written by the console in
	 * one boot is still sitting there in the next one and a resync would pick it straight
	 * back up. Step 3 is the whole point of the block: it demonstrates that the mailbox
	 * ALONE would carry the arm across a reboot, which is why the publish exists. */
	CHECK_EQ32((uae_u32)z3660_warp_armed(), 0u,
	           "at rest the engine is DISARMED, with no mailbox word ever written");

	warp_write_knob(Z3660_WARP_ARM);
	CHECK_EQ32((uae_u32)z3660_warp_armed(), 1u,
	           "the console's word arms it -- the only way in, and it needs a human");
	z3660_knobs_sync();
	CHECK_EQ32((uae_u32)z3660_warp_armed(), 1u,
	           "...and a resync that finds the SAME word leaves it armed: the mailbox alone "
	           "WOULD carry an arm across a reboot, which is why core0 republishes");

	harness_shared_defaults();       /* == core0's unconditional per-boot publish */
	z3660_knobs_sync();
	CHECK_EQ32((uae_u32)z3660_warp_armed(), 0u,
	           "and the boot publish disarms it: an arm lasts exactly one boot");
	CHECK_EQ32((uae_u32)shared->warp_on, Z3660_WARP_OFF,
	           "...because the publish writes the OFF SENTINEL, not merely nothing");

	/* ---- BOTH TRANSITIONS DISCARD THE CODE CACHE ---------------------------------
	 *
	 * The DISARM direction is the one that matters, and the argument is the decoded-op
	 * cache's own: an off period is an UNWATCHED period. Nothing is compiled while
	 * disarmed and no lookup consults the table, so the guest can rewrite a page with
	 * nothing watching -- and coming back on must not find the pre-rewrite blocks still
	 * stamped live. The generation is the instrument here rather than a counter, because
	 * the generation IS the mechanism by which a discard reaches the blocks. */
	{
		unsigned g;

		g = z3660_warp_generation();
		warp_write_knob(Z3660_WARP_ARM);
		CHECK(z3660_warp_generation() != g,
		      "ARMING discards the whole code cache (%u -> %u)\n",
		      g, z3660_warp_generation());

		g = z3660_warp_generation();
		warp_write_knob(Z3660_WARP_OFF);
		CHECK(z3660_warp_generation() != g,
		      "DISARMING discards it too -- nothing compiled before an off period survives "
		      "into the arm after it (%u -> %u)\n", g, z3660_warp_generation());
		CHECK_EQ32((uae_u32)z3660_warp_armed(), 0u, "...and it is disarmed after");
	}

	/* ---- DISARMED IS A PURE PASSTHROUGH -------------------------------------------
	 *
	 * Not "a second implementation that happens to agree": the run loop's hook returns
	 * before it has looked at anything, so nothing is discovered, nothing is compiled and
	 * no block is entered. That is what makes the state-equivalence arm below meaningful,
	 * and it is asserted against the engine's own counters rather than inferred. */
	warp_corpus_build();
	warp_boot();
	warp_corpus_enter();
	{
		int k;
		for (k = 0; k < 2000; k++) m68k_run_mmu040();
		regs.spcflags = 0;
	}
	CHECK(z3660_warp_stat(Z3660_WARP_STAT_BLK) == 0
	   && z3660_warp_stat(Z3660_WARP_STAT_COMPILE) == 0
	   && z3660_warp_stat(Z3660_WARP_STAT_DECLINE) == 0
	   && z3660_warp_entered() == 0,
	      "disarmed, 2000 guest instructions discover nothing, compile nothing and enter "
	      "nothing -- the hook is a pure passthrough\n");
}

/* ------------------------------------------------------------- the block cache ------- */

/* Run n guest instructions of the corpus from a fresh fixture, at a given arm state. */
static void warp_run(uae_u32 knob, unsigned n)
{
	unsigned k;

	warp_corpus_build();
	warp_boot();
	warp_write_knob(knob);
	z3660_warp_stats_zero();
	warp_corpus_enter();
	for (k = 0; k < n; k++) m68k_run_mmu040();
	regs.spcflags = 0;
}

static void test_warp_block_cache(void)
{
	printf("[test] WARP block cache: keyed on guest virtual PC plus the supervisor bit\n");

	/* ---- GEOMETRY AND KEYING, AS RULES ------------------------------------------- */
	CHECK_EQ32((uae_u32)Z3660_WARP_SETS, 1024u, "1024 sets");
	CHECK(sizeof(void *) != 4 || z3660_warp_block_bytes() == 16u,
	      "a block-table entry is 16 bytes on a 32-bit target -- half a Cortex-A9 line, so "
	      "a lookup touches one line and never two (host reads %u)\n",
	      z3660_warp_block_bytes());

	/* THE M0 CORRECTION, ASSERTED. The existing UAE block table keys on cacheline(pc_p) --
	 * a HOST pointer -- and under the MMU there is no pc_p. The replacement keys on the
	 * guest logical PC with the address space in bit 0, which is the encoding the dopc has
	 * already proven against a real guest. */
	CHECK(Z3660_WARP_KEY(0x00400000u, 0) != Z3660_WARP_KEY(0x00400000u, 1),
	      "a supervisor block and a user block at the same virtual address cannot alias\n");
	CHECK(Z3660_WARP_KEY(0x00400000u, 0) == 0x00400000u
	   && Z3660_WARP_KEY(0x00400000u, 1) == 0x00400001u,
	      "the address space rides in the pc's otherwise-dead bit 0\n");
	{
		/* The set-relative empty marker, inherited whole from the dopc because the dopc
		 * paid for it with a crash: a FIXED odd marker of 1 means "pc 0 in supervisor
		 * mode", which is exactly where a guest lands after an exception through an
		 * unplanted vector -- the single most likely wild pc there is. Built from the set
		 * index instead, the marker cannot equal any key that could legitimately live in
		 * this set, for any pc and either address space. */
		unsigned idx, bad = 0;
		for (idx = 0; idx < Z3660_WARP_SETS; idx++) {
			unsigned marker = Z3660_WARP_TAG_EMPTY(idx);
			if (Z3660_WARP_INDEX(marker) == idx) bad++;
		}
		CHECK_EQ32((uae_u32)bad, 0u,
		           "the empty marker of every one of the 1024 sets indexes a DIFFERENT set, "
		           "so no legitimate key can ever match it");
	}

	/* ---- DISCOVERY AND RESIDENCY -------------------------------------------------- */
	warp_run(Z3660_WARP_ARM, 400);
	{
		unsigned i, resident = 0;
		for (i = 0; i < WARP_LOOP_INSNS; i++)
			if (z3660_warp_peek(WARP_VCODE + i * 2, 0)) resident++;

		CHECK_EQ32((uae_u32)resident, WARP_LOOP_INSNS,
		           "all eight block heads of the corpus are discovered and cached");
		CHECK_EQ32((uae_u32)z3660_warp_stat(Z3660_WARP_STAT_COMPILE), WARP_LOOP_INSNS,
		           "...compiled exactly once each: 400 instructions, 8 compiles");
		CHECK(z3660_warp_stat(Z3660_WARP_STAT_BLK) == 400,
		      "...and executed on every one of the 400 dispatches (%llu)\n",
		      z3660_warp_stat(Z3660_WARP_STAT_BLK));
		CHECK_EQ32((uae_u32)z3660_warp_stat(Z3660_WARP_STAT_LIVE), WARP_LOOP_INSNS,
		           "eight blocks live in the table");
	}

	/* ---- MUTATION M-5: THE BLOCK KEY DROPS THE SUPERVISOR BIT --------------------
	 *
	 * A user block must not answer a supervisor lookup at the same virtual address. With
	 * the bit dropped it does, and that is the aliasing the dopc's own tag encoding exists
	 * to prevent -- on a real guest it means executing the other privilege level's code.
	 * Printed as well as asserted: a mutation arm that silently reports nothing looks
	 * identical in the log to one that was never compiled in. */
	{
		int super_hit_clean, super_hit_mutant;

		super_hit_clean = z3660_warp_peek(WARP_VCODE, 1);
		z3660_warp_set_mutant(Z3660_WARP_MUT_KEY_NOSUP);
		super_hit_mutant = z3660_warp_peek(WARP_VCODE, 1);
		z3660_warp_set_mutant(Z3660_WARP_MUT_NONE);

		printf("       M-5 (key drops the supervisor bit): supervisor lookup of a USER "
		       "block hits %d clean, %d mutated (want 0 then 1)\n",
		       super_hit_clean, super_hit_mutant);
		CHECK(!super_hit_clean,
		      "clean: a supervisor lookup does NOT find the user block at that address\n");
		CHECK(super_hit_mutant,
		      "M-5 CAUGHT: with the supervisor bit dropped the two alias, which is the "
		      "defect the encoding exists to prevent\n");
	}

	/* ---- INVALIDATION, FROM THE DOPC'S OWN CHOKE POINT ---------------------------
	 *
	 * The code cache does not invent call sites: mmu_flush_cache() and its nine siblings
	 * already funnel into z3660_dopc_invalidate(), and joining there is what makes a NEW
	 * upstream cause have to be opted OUT of invalidating the code cache rather than into
	 * it. Every cause is driven, because "all causes" is the claim. */
	{
		int cause, missed = 0;
		unsigned long long disc0;

		warp_run(Z3660_WARP_ARM, 400);
		disc0 = z3660_warp_stat(Z3660_WARP_STAT_DISC);

		for (cause = 0; cause < Z3660_DOPC_INV_COUNT; cause++) {
			unsigned g;
			/* re-warm, so each cause has live blocks to discard */
			warp_corpus_enter();
			{ int k; for (k = 0; k < 64; k++) m68k_run_mmu040(); regs.spcflags = 0; }
			g = z3660_warp_generation();
			z3660_dopc_invalidate(cause);
			if (z3660_warp_generation() == g) missed++;
			if (z3660_warp_stat(Z3660_WARP_STAT_LIVE) != 0) missed++;
		}
		CHECK_EQ32((uae_u32)missed, 0u,
		           "every one of the ten invalidation causes discards the whole code cache "
		           "-- the code cache is opted OUT of a new cause, never into it");
		CHECK(z3660_warp_stat(Z3660_WARP_STAT_DISC) > disc0,
		      "...and WARP_DISC charged the blocks each one discarded (%llu)\n",
		      z3660_warp_stat(Z3660_WARP_STAT_DISC));

		/* Q5's counter, and its whole purpose: WARP_DISC_ROOT against WARP_DISC is what
		 * decides whether a per-address-space generation is worth funding. It is live from
		 * day one so the question is settled by a ratio rather than by an argument. */
		warp_corpus_enter();
		{ int k; for (k = 0; k < 64; k++) m68k_run_mmu040(); regs.spcflags = 0; }
		{
			unsigned long long r0 = z3660_warp_stat(Z3660_WARP_STAT_DISCROOT);
			unsigned long long d0 = z3660_warp_stat(Z3660_WARP_STAT_DISC);
			z3660_dopc_invalidate(Z3660_DOPC_INV_ROOT);
			CHECK(z3660_warp_stat(Z3660_WARP_STAT_DISCROOT) > r0
			   && z3660_warp_stat(Z3660_WARP_STAT_DISC) > d0,
			      "IV_ROOT charges BOTH WARP_DISC and WARP_DISC_ROOT -- Q5's ratio needs "
			      "the same blocks in its numerator and its denominator\n");

			r0 = z3660_warp_stat(Z3660_WARP_STAT_DISCROOT);
			warp_corpus_enter();
			{ int k; for (k = 0; k < 64; k++) m68k_run_mmu040(); regs.spcflags = 0; }
			z3660_dopc_invalidate(Z3660_DOPC_INV_FLUSH);
			CHECK(z3660_warp_stat(Z3660_WARP_STAT_DISCROOT) == r0,
			      "...and no other cause touches WARP_DISC_ROOT, or the ratio would be a "
			      "measure of nothing\n");
		}
	}

	/* ---- DISARMED, AN INVALIDATION IS A NO-OP ------------------------------------ */
	{
		unsigned g;
		warp_run(Z3660_WARP_OFF, 400);
		g = z3660_warp_generation();
		z3660_dopc_invalidate(Z3660_DOPC_INV_FLUSH);
		CHECK(z3660_warp_generation() == g,
		      "disarmed, an invalidation does not move the code cache's generation -- "
		      "nothing was compiled, so there is nothing to discard\n");
	}

	/* ---- EVICTION: THE BUMP ALLOCATOR RUNS OUT ------------------------------------
	 *
	 * §2b's decision for generation one, unchanged: bump-allocate, then throw everything
	 * away and bump the generation. Simple, provably correct, and it composes with the
	 * generation counter. The flush RATE per boot is a counter before it is a design input,
	 * which is why WARP_FLUSH exists at all -- and why the cache limit is lowerable here
	 * rather than the path being left untested until a boot happens to reach it. */
	{
		warp_corpus_build();
		warp_boot();
		warp_write_knob(Z3660_WARP_ARM);
		z3660_warp_set_cache_limit(3u * Z3660_WARP_NULL_BYTES);   /* room for three blocks */
		z3660_warp_stats_zero();
		warp_corpus_enter();
		{ int k; for (k = 0; k < 400; k++) m68k_run_mmu040(); regs.spcflags = 0; }

		CHECK(z3660_warp_stat(Z3660_WARP_STAT_FLUSH) > 0,
		      "a code cache too small for the working set flushes (%llu times) rather than "
		      "running off the end of its arena\n",
		      z3660_warp_stat(Z3660_WARP_STAT_FLUSH));
		CHECK(z3660_warp_stat(Z3660_WARP_STAT_LIVE) <= 3,
		      "...and never holds more blocks than the arena has room for (%llu)\n",
		      z3660_warp_stat(Z3660_WARP_STAT_LIVE));
		CHECK(z3660_warp_bad(Z3660_WARP_BAD_UNPUBLISHED) == 0
		   && z3660_warp_bad(Z3660_WARP_BAD_RETIRED) == 0,
		      "...and thrashing the allocator breaks neither invariant\n");
		z3660_warp_set_cache_limit(0);
	}
}

/* -------------------------------------------------------------- the null block ------- */

static void test_warp_null_block(void)
{
	printf("[test] WARP null block: entry, exit, the bail contract and the publish discipline\n");

	/* ---- THE EMITTED WORDS, BYTE-EXACT -------------------------------------------
	 *
	 * This is the strongest claim the HOST can make about code it cannot execute, and it is
	 * worth making precisely because of that: §3.2's honest limit says the emitted sequence
	 * is proven on target and only on target, so everything provable here is proven before
	 * a metal session is booked. The emitter is portable C, so these are the same two words
	 * the ARM build writes into the code cache. */
	{
		uae_u32 words[4];
		unsigned n;

		memset(words, 0xAA, sizeof words);
		n = z3660_warp_emit_null(words);
		CHECK_EQ32((uae_u32)n, Z3660_WARP_NULL_BYTES, "a null block is 8 bytes");
		CHECK_EQ32(words[0], 0xE3A00000u,
		           "word 0 is `mov r0, #0` -- a null block retires no guest instruction");
		CHECK_EQ32(words[1], 0xE12FFF1Eu,
		           "word 1 is `bx lr` -- back to the interpreter, at the PC it was entered at");
		CHECK_EQ32(words[2], 0xAAAAAAAAu, "and it writes nothing past its own two words");
	}

	/* ---- ENTRY, EXIT AND THE BAIL ACCOUNTING ------------------------------------- */
	warp_run(Z3660_WARP_ARM, 400);
	{
		unsigned long long blk = z3660_warp_stat(Z3660_WARP_STAT_BLK);

		CHECK(blk == 400,
		      "400 dispatches, 400 block executions -- discovery fires at every dispatched "
		      "PC while the engine is armed (%llu)\n", blk);
		CHECK(z3660_warp_stat(Z3660_WARP_STAT_BAIL) == blk,
		      "...every one of them BAILS: a null block retires nothing, so the bail arm is "
		      "the only arm there is\n");
		CHECK_EQ32((uae_u32)z3660_warp_stat(Z3660_WARP_STAT_ENTER), 0u,
		           "WARP_ENTER is 0, BY CONSTRUCTION: it counts entries that consume a "
		           "run-loop pass, and no null block does");
		CHECK(z3660_warp_bad(Z3660_WARP_BAD_RETIRED) == 0,
		      "THE NO-EMPTY-PASS INVARIANT HOLDS: no block retired anything in a round with "
		      "no pass-consuming arm\n");

		/* THE ARM ATTESTATION, and the reason it is a counter and not a banner: two cores
		 * share one unlocked UART, so an arm banner arrives spliced. It is the SAME
		 * increment site as WARP_BLK, so the lean confession and the profiled counter
		 * cannot disagree -- which is what makes a lean verdict boot attestable at all. */
		CHECK(z3660_warp_entered() == blk,
		      "the always-compiled attestation is the same quantity as WARP_BLK, from the "
		      "same increment site: %llu\n", z3660_warp_entered());
	}

	/* ---- CODE BYTES, AND THE COMPILE ACCOUNTING ---------------------------------- */
	CHECK(z3660_warp_stat(Z3660_WARP_STAT_CBYTES)
	          == z3660_warp_stat(Z3660_WARP_STAT_COMPILE) * Z3660_WARP_NULL_BYTES,
	      "code bytes emitted == compiles x 8. Code bytes per guest instruction is a "
	      "first-class metric for this project: DDR is not the constraint, L1I is\n");

	/* ---- THE PUBLISH DISCIPLINE, AND MUTATION M-ICACHE ----------------------------
	 *
	 * A block is published through the I-cache sequence BEFORE it is written into the
	 * table, so there is no window in which a lookup could hand the run loop a pointer to
	 * bytes the I-cache has not seen. `8be8949` is this repository's own receipt for what
	 * the other ordering costs: the first pushall_call_handler() fetched stale I-cache
	 * lines and the board rebooted with an ARM Undefined Instruction -- caused by a
	 * compiler-flag change, which is to say by nothing anyone would look at.
	 *
	 * On the host the sequence is a no-op, so what is proven here is the DISCIPLINE and not
	 * the ARM semantics -- the barriers themselves are Q3's separate metal session, and the
	 * standing caveat rides unchanged: adding barriers is not evidence of a fix. */
	CHECK(z3660_warp_stat(Z3660_WARP_STAT_PUBLISH)
	          == z3660_warp_stat(Z3660_WARP_STAT_COMPILE),
	      "every compiled block was published: publishes %llu == compiles %llu\n",
	      z3660_warp_stat(Z3660_WARP_STAT_PUBLISH),
	      z3660_warp_stat(Z3660_WARP_STAT_COMPILE));
	CHECK(z3660_warp_bad(Z3660_WARP_BAD_UNPUBLISHED) == 0,
	      "...and no block was ever entered before its publish ran\n");
	{
		unsigned long long bad_clean, bad_mutant;

		bad_clean = z3660_warp_bad(Z3660_WARP_BAD_UNPUBLISHED);
		warp_corpus_build();
		warp_boot();
		warp_write_knob(Z3660_WARP_ARM);
		z3660_warp_set_mutant(Z3660_WARP_MUT_NO_ICACHE);
		warp_corpus_enter();
		{ int k; for (k = 0; k < 200; k++) m68k_run_mmu040(); regs.spcflags = 0; }
		z3660_warp_set_mutant(Z3660_WARP_MUT_NONE);
		bad_mutant = z3660_warp_bad(Z3660_WARP_BAD_UNPUBLISHED);

		printf("       M-ICACHE (block published without the sequence): %llu unpublished "
		       "entries clean, %llu mutated (want 0 then >0)\n", bad_clean, bad_mutant);
		CHECK(bad_mutant > bad_clean,
		      "M-ICACHE CAUGHT: a block that reached the run loop without the I-cache "
		      "sequence is detected at the point of use, not afterwards\n");
	}
	warp_boot();
}

/* ---------------------------------------------------------------- the cadence -------- */

static void test_warp_cadence(void)
{
	printf("[test] WARP cadence contract (Q10, frozen): a block feeds RETIRED instructions\n");

	/* WHY THIS IS A CORRECTNESS TEST AND NOT A PERFORMANCE ONE. One pump tick is
	 * 256 x service_cadence GUEST instructions, and the A3000-SCSI interrupt-delay pump
	 * rides on this poll: at cadence 8 its bootstrap already hangs, and a network flood at
	 * cadence 16+ has panicked AMIX. If a block feeds the counter with anything other than
	 * what it retired, every latency statement in docs/AMIX.md silently changes meaning --
	 * and the symptom is a MISSED INTERRUPT, not a slow number. It is also the one item in
	 * the plan with cross-repo reach.
	 *
	 * The feed advances the counter by n-1 and not by n, and that is not an off-by-one: the
	 * pass's own tail supplies the last tick, so an interpreted pass and a block that
	 * retired exactly one instruction advance the counter identically. */
	warp_boot();
	shared->service_cadence = 4;
	z3660_knobs_sync();

	{
		unsigned long polls0;

		/* n = 0 and n = 1 must cost nothing: the tail is about to supply the only tick. */
		polls0 = harness_ipl_polls;
		z3660_warp_cadence_feed(0);
		z3660_warp_cadence_feed(1);
		CHECK(harness_ipl_polls == polls0,
		      "a block that retired 0 or 1 instructions feeds nothing -- the pass's own "
		      "tail supplies that tick\n");

		/* n = 17 at cadence 4 means 16 further instructions, i.e. exactly four crossings.
		 * Asserted as a NUMBER, because "it polls" is satisfied by polling once. */
		polls0 = harness_ipl_polls;
		z3660_warp_cadence_feed(17);
		CHECK(harness_ipl_polls - polls0 == 4,
		      "a block retiring 17 instructions at cadence 4 polls exactly 4 times -- not "
		      "once. Firing one poll where interpretation would have fired four stretches "
		      "the pump tick by the block length, silently (%lu)\n",
		      harness_ipl_polls - polls0);

		/* And the counter's PHASE survives, which is what makes a block and a run of
		 * interpreted instructions interchangeable rather than merely similar. */
		polls0 = harness_ipl_polls;
		z3660_warp_cadence_feed(3);       /* +2 -> counter at 2 of 4 */
		z3660_warp_cadence_feed(3);       /* +2 -> crosses once      */
		CHECK(harness_ipl_polls - polls0 == 1,
		      "two blocks of 3 cross the cadence once between them: the counter carries its "
		      "phase across a block boundary exactly as it does across an instruction "
		      "boundary (%lu)\n", harness_ipl_polls - polls0);
	}

	/* ---- MUTATION M-7: THE COUNTER FED 1 PER BLOCK ------------------------------- */
	{
		unsigned long polls0, clean, mutated;

		polls0 = harness_ipl_polls;
		z3660_warp_cadence_feed(41);
		clean = harness_ipl_polls - polls0;

		z3660_warp_set_cadence_mutant(1);
		polls0 = harness_ipl_polls;
		z3660_warp_cadence_feed(41);
		mutated = harness_ipl_polls - polls0;
		z3660_warp_set_cadence_mutant(0);

		printf("       M-7 (cadence fed 1 per block): 41 retired instructions poll %lu "
		       "times clean, %lu times mutated (want 10 then 0)\n", clean, mutated);
		CHECK(clean == 10,
		      "clean: 41 retired instructions at cadence 4 poll ten times\n");
		CHECK(mutated == 0,
		      "M-7 CAUGHT: fed 1 per block the counter never crosses, so the pump would "
		      "tick once per block instead of once per 256 x cadence guest instructions\n");
	}

	warp_boot();
}

/* --------------------------------------------------------------- the lockstep -------- */

/* THE COMPARISON, taken at the end as well as at every boundary: registers and the guest
 * data page, byte for byte. The per-instruction digest is what proves the two arms never
 * diverged; this is what proves the digest was over something. */
struct warp_ls_snap {
	uae_u32 d[8], a[8], pc, ipc;
	uae_u32 mem[16];
};

static void warp_ls_take(struct warp_ls_snap *s)
{
	int i;
	for (i = 0; i < 8; i++) { s->d[i] = m68k_dreg(regs, i); s->a[i] = m68k_areg(regs, i); }
	s->pc  = m68k_getpc();
	s->ipc = (uae_u32)regs.instruction_pc;
	for (i = 0; i < 16; i++) s->mem[i] = hram_peek32(WARP_PDATA + i * 4);
}

static uae_u64 warp_ls_arm(uae_u32 knob, uae_u64 n, struct warp_ls_snap *out)
{
	uae_u64 k;

	warp_corpus_build();
	warp_boot();
	warp_write_knob(knob);
	z3660_warp_stats_zero();
	warp_corpus_enter();
	for (k = 0; k < n; k++) {
		m68k_run_mmu040();
		z3660_warp_ls_step();
	}
	regs.spcflags = 0;
	warp_ls_take(out);
	return n;
}

static void test_warp_lockstep(void)
{
	/* THE ROUND'S CORRECTNESS GATE, and it is the strongest form this gate will ever take.
	 *
	 * Armed against disarmed, over the same guest span, compared after EVERY guest
	 * instruction across the set §3.1 defines: the sixteen data and address registers, the
	 * guest pc, the architectural CCR and X, regs.instruction_pc, mmu_restart and
	 * mmufixup[0..1]. With null blocks the two arms must be IDENTICAL rather than merely
	 * equivalent, because a null block changes nothing at all -- so any divergence at all
	 * is a defect in the plumbing and not a semantic difference to be reasoned about.
	 *
	 * A LENGTH MISMATCH IS ALSO A DIVERGENCE, and it is the failure this round is most
	 * likely to produce: an engine that burned a run-loop pass without retiring anything
	 * would make the armed arm shorter, which is precisely the no-empty-pass invariant
	 * failing. The oracle counts it rather than comparing only where both arms have data.
	 *
	 * A single divergence is a stop, not a statistic. */
	const uae_u64 N = 10000000ull;
	struct warp_ls_snap ref, run;
	uae_u64 diverge;

	printf("[test] WARP lockstep oracle: armed and disarmed must be IDENTICAL over 10^7 "
	       "guest instructions\n");

	z3660_warp_ls_zero();

	warp_ls_arm(Z3660_WARP_OFF, N, &ref);
	z3660_warp_ls_commit(0);                      /* the reference arm */

	warp_ls_arm(Z3660_WARP_ARM, N, &run);
	diverge = z3660_warp_ls_commit(1);

	/* THE ANTI-VACUITY CHECK COMES FIRST. "The two paths agree" is trivially true when the
	 * engine silently never engaged -- the failure a state-equivalence corpus is blindest
	 * to, and a JIT's decline surface is larger than any rung's. */
	CHECK(z3660_warp_stat(Z3660_WARP_STAT_BLK) == N,
	      "the armed arm ENGAGED on every one of the %llu instructions (%llu block "
	      "executions) -- without this the comparison below proves nothing\n",
	      (unsigned long long)N, z3660_warp_stat(Z3660_WARP_STAT_BLK));

	CHECK_EQ32((uae_u32)diverge, 0u,
	           "WARP_DIVERGE == 0: not one instruction boundary differs between the arms");
	CHECK(z3660_warp_ls_insns() >= N,
	      "...over %llu compared boundaries, which is the pre-registered span\n",
	      z3660_warp_ls_insns());
	CHECK(z3660_warp_diverge() == 0,
	      "...and the oracle's own running total agrees (%llu)\n", z3660_warp_diverge());

	CHECK(memcmp(&ref, &run, sizeof ref) == 0,
	      "the final register file, pc, restart pc and guest data page are byte-identical "
	      "between the arms\n");
	CHECK(z3660_warp_bad(Z3660_WARP_BAD_UNPUBLISHED) == 0
	   && z3660_warp_bad(Z3660_WARP_BAD_RETIRED) == 0,
	      "...and neither engine invariant was broken over the whole span\n");

	z3660_warp_ls_zero();
	warp_boot();
}

#if Z3660_EMU_PROF
/* ==================================================================
 * WARP ENGINE -- the counter identities, profiling build only.
 *
 * §0.5.6 pre-registered every one of these directions BEFORE the engine existed, and
 * checked each against the counter's position in the call topology -- the law being obeyed
 * is that a counter reached only AFTER the branch being deleted cannot fall when it is
 * deleted, which was once a declared direction that was arithmetically impossible.
 *
 * Round 1 is the one wave in which most of them can be asserted as EXACT EQUALITIES rather
 * than as bands, because a null block changes nothing: the two arms execute the same guest
 * instructions, take the same fetches, consult the decoded-op cache the same number of
 * times and raise the same invalidations. Every equality below stops being available the
 * moment the first block retires something, so it is worth taking now.
 * ================================================================== */
static void test_warp_counters(void)
{
	uae_u64 hit_off, miss_off, insns_off, faults_off, inval_off;
	uae_u64 blkhit_off, blkinsns_off, blkbytes_off;
	uae_u64 hit_on, miss_on, insns_on, faults_on, inval_on;
	uae_u64 blkhit_on, blkinsns_on, blkbytes_on;
	uae_u64 enter_on, blk_on, warpinsns_on, bail_on, bailst_on;
	uae_u64 compile_on, cinsns_on, cbytes_on, cfetch_on, decline_on;
	uae_u64 req, c;
	const unsigned N = 20000;

	printf("[test] WARP counters: the identities S0.5.6 pre-registered, as exact equalities\n");

	/* ---- the reference arm ---- */
	warp_corpus_build();
	warp_boot();
	warp_write_knob(Z3660_WARP_OFF);
	z3660_prof_zero();
	warp_corpus_enter();
	{ unsigned k; for (k = 0; k < N; k++) m68k_run_mmu040(); regs.spcflags = 0; }
	hit_off      = z3660_prof_cnt[Z3660_PROF_C_DOPC_HIT];
	miss_off     = z3660_prof_cnt[Z3660_PROF_C_DOPC_MISS];
	insns_off    = z3660_prof_cnt[Z3660_PROF_C_INSNS];
	faults_off   = z3660_prof_cnt[Z3660_PROF_C_FAULTS];
	inval_off    = z3660_prof_cnt[Z3660_PROF_C_DOPC_INVAL];
	blkhit_off   = z3660_prof_cnt[Z3660_PROF_C_BLK_HIT];
	blkinsns_off = z3660_prof_cnt[Z3660_PROF_C_BLK_INSNS];
	blkbytes_off = z3660_prof_cnt[Z3660_PROF_C_BLK_BYTES];

	/* ---- the armed arm, same span, same fixture ---- */
	warp_corpus_build();
	warp_boot();
	warp_write_knob(Z3660_WARP_ARM);
	z3660_prof_zero();
	warp_corpus_enter();
	{ unsigned k; for (k = 0; k < N; k++) m68k_run_mmu040(); regs.spcflags = 0; }
	hit_on      = z3660_prof_cnt[Z3660_PROF_C_DOPC_HIT];
	miss_on     = z3660_prof_cnt[Z3660_PROF_C_DOPC_MISS];
	insns_on    = z3660_prof_cnt[Z3660_PROF_C_INSNS];
	faults_on   = z3660_prof_cnt[Z3660_PROF_C_FAULTS];
	inval_on    = z3660_prof_cnt[Z3660_PROF_C_DOPC_INVAL];
	blkhit_on   = z3660_prof_cnt[Z3660_PROF_C_BLK_HIT];
	blkinsns_on = z3660_prof_cnt[Z3660_PROF_C_BLK_INSNS];
	blkbytes_on = z3660_prof_cnt[Z3660_PROF_C_BLK_BYTES];
	enter_on     = z3660_prof_cnt[Z3660_PROF_C_WARP_ENTER];
	blk_on       = z3660_prof_cnt[Z3660_PROF_C_WARP_BLK];
	warpinsns_on = z3660_prof_cnt[Z3660_PROF_C_WARP_INSNS];
	bail_on      = z3660_prof_cnt[Z3660_PROF_C_WARP_BAIL];
	bailst_on    = z3660_prof_cnt[Z3660_PROF_C_WARP_BAIL_ST];
	compile_on   = z3660_prof_cnt[Z3660_PROF_C_WARP_COMPILE];
	cinsns_on    = z3660_prof_cnt[Z3660_PROF_C_WARP_CINSNS];
	cbytes_on    = z3660_prof_cnt[Z3660_PROF_C_WARP_CBYTES];
	cfetch_on    = z3660_prof_cnt[Z3660_PROF_C_WARP_CFETCH];
	decline_on   = z3660_prof_cnt[Z3660_PROF_C_WARP_DECLINE];

	/* ---- G2's first identity, in the form §2e's claim 3 was CORRECTED into --------
	 *
	 * The literal claim ("INSNS identical between arms") is wrong and §0.5.2 says why:
	 * INSNS is incremented once per RUN-LOOP PASS, and a block that retires n instructions
	 * is one pass. The comparable quantity is the reconstructed guest stream, which is
	 * rung 0's own correction with WARP_ENTER in BLK_HIT's place. In round 1 WARP_ENTER is
	 * 0 by construction, so the identity reduces to the one the firmware has always had --
	 * in BOTH arms, which is what makes an armed capture readable by tools that predate the
	 * engine. */
	CHECK(hit_off + miss_off == (insns_off - 0) + faults_off,
	      "disarmed: DOPC_HIT + DOPC_MISS == INSNS + FAULTS (%llu == %llu)\n",
	      (unsigned long long)(hit_off + miss_off),
	      (unsigned long long)(insns_off + faults_off));
	CHECK(hit_on + miss_on == (insns_on - enter_on) + faults_on,
	      "armed: DOPC_HIT + DOPC_MISS == (INSNS - WARP_ENTER) + FAULTS (%llu == %llu)\n",
	      (unsigned long long)(hit_on + miss_on),
	      (unsigned long long)((insns_on - enter_on) + faults_on));
	CHECK_EQ32((uae_u32)enter_on, 0u,
	           "WARP_ENTER is 0 armed: it counts entries that COST one INSNS, and a null "
	           "block falls through inside the pass rather than consuming it");

	/* ---- the exact equalities a null block makes available ------------------------ */
	CHECK(insns_on == insns_off,
	      "INSNS is IDENTICAL between the arms (%llu), because every pass still retires "
	      "exactly one interpreted instruction\n", (unsigned long long)insns_on);
	CHECK(hit_on + miss_on == hit_off + miss_off,
	      "...and the decoded-op cache is consulted exactly as often (%llu)\n",
	      (unsigned long long)(hit_on + miss_on));
	CHECK(faults_on == faults_off,
	      "FAULTS unchanged (%llu == %llu) -- the bail contract means every fault is still "
	      "the interpreter's, and on a deterministic host corpus that is an equality rather "
	      "than the +/-1 a metal capture allows\n",
	      (unsigned long long)faults_on, (unsigned long long)faults_off);

	/* Q8, and it is a stronger check than the plan could pre-register: rung 0 is not merely
	 * unmodified, it still FIRES identically -- because a null block retires nothing, so the
	 * recognizer runs in the same pass exactly as it does today. A fall here would be the
	 * engine poaching rung-0 PCs, and this is how that is detected rather than argued. */
	CHECK(blkhit_on == blkhit_off && blkinsns_on == blkinsns_off
	   && blkbytes_on == blkbytes_off,
	      "rung 0's BLK_HIT / BLK_INSNS / BLK_BYTES are identical between the arms -- the "
	      "engine does not poach its PCs (%llu / %llu / %llu)\n",
	      (unsigned long long)blkhit_on, (unsigned long long)blkinsns_on,
	      (unsigned long long)blkbytes_on);

	/* The guest's invalidation demand is a GUEST property, so WARP adds a consumer and not
	 * a cause. If this moved, the code cache would have started raising invalidations of
	 * its own -- and every narrowing decision downstream reads this number as a
	 * denominator. */
	CHECK(inval_on == inval_off,
	      "DOPC_INVAL is unchanged (%llu): WARP is a CONSUMER of the invalidation stream, "
	      "never a cause of it\n", (unsigned long long)inval_on);
	req = 0;
	for (c = 0; c < Z3660_DOPC_INV_COUNT; c++)
		req += z3660_prof_cnt[Z3660_PROF_C_DOPC_IV_BASE + c];
	CHECK(req - z3660_prof_cnt[Z3660_PROF_C_DOPC_IV_WRAP] == inval_on,
	      "...and the standing identity sum(IV_*) - IV_WRAP == DOPC_INVAL still closes "
	      "with the engine armed (%llu)\n", (unsigned long long)inval_on);

	/* ---- WARP's own block, each against its topology ----------------------------- */
	CHECK(blk_on == (uae_u64)N,
	      "WARP_BLK counts every block execution: %llu on %u dispatches\n",
	      (unsigned long long)blk_on, N);
	CHECK(bail_on == blk_on,
	      "WARP_BAIL == WARP_BLK: every null block bails, so the two are the same event "
	      "counted at two sites, and a difference would be a leak in the bail contract\n");
	CHECK_EQ32((uae_u32)warpinsns_on, 0u,
	           "WARP_INSNS is 0: no guest instruction is retired inside a block");
	CHECK_EQ32((uae_u32)bailst_on, 0u,
	           "WARP_BAIL_ST is 0 -- MEASURED zero, not omitted. It is the number round 2's "
	           "state-sync discipline is measured against");
	CHECK(compile_on == 8,
	      "WARP_COMPILE counts the eight distinct block heads once each (%llu)\n",
	      (unsigned long long)compile_on);
	CHECK_EQ32((uae_u32)cinsns_on, 0u,
	           "WARP_CINSNS is 0: a null block compiles no guest instruction");
	CHECK(cbytes_on == compile_on * Z3660_WARP_NULL_BYTES,
	      "WARP_CBYTES == compiles x 8 (%llu)\n", (unsigned long long)cbytes_on);
	CHECK_EQ32((uae_u32)cfetch_on, 0u,
	           "WARP_CFETCH is 0: round 1's compiler reads no guest instruction word. THIS "
	           "IS Q6'S COUNTER and it is live and reading zero, which is the honest value");
	CHECK_EQ32((uae_u32)decline_on, 0u,
	           "WARP_DECLINE is 0 on a user-mode corpus with tracing off -- supervisor PCs "
	           "are gated before discovery and are read from INSNS_SUPER instead");

	/* THE COVERAGE ARITHMETIC ITSELF, on the wave where its answer is known in advance.
	 * §0.5.2 pre-registers it because the obvious ratio is wrong -- rung 0 walked into
	 * exactly this and its metal boot read 59.28 % where the correct arithmetic read
	 * 37.25 %, "not merely inflated -- outside the range in which the prediction could be
	 * judged at all". Round 1 must read 0, and reading 0 through the CORRECT formula is
	 * what says the formula is wired up before there is anything to measure with it. */
	CHECK(warpinsns_on == 0 && ((insns_on - enter_on) + warpinsns_on) == insns_on,
	      "coverage = WARP_INSNS / ((INSNS - WARP_ENTER) + WARP_INSNS) = 0/%llu = 0.00 %%, "
	      "which is what a round with no compiler must read\n",
	      (unsigned long long)insns_on);

	z3660_prof_zero();
	warp_boot();
	cpu_bringup(68030);
}
#endif /* Z3660_EMU_PROF */

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
	test_mmu030_pagewalk();
	test_mmu030_fault_restart();
	test_mmu030_lrmw();
	test_mmu030_pflush();
	test_mmu030_interp_exec();
	test_mmu040_interp_exec();
	test_mmu040_ttr();
	test_mmu040_translate_4k();
	test_mmu040_translate_8k();
	test_mmu040_protection();
	test_mmu040_modified_bit();
	test_fpu040_unimplemented_trap();
	test_mmu040_access_error_frame();
	test_mmu040_movec_tc_enables_mmu();
	test_mmu040_pflush_ptest();
	test_mmu040_reset_clears_mmu_state();
	test_mmu040_bitfield_translates();
	test_mmu040_user_mode();
	test_mmu040_rte_delivers_pc();
	test_mmu040_rte_syscall_return_usp();
	test_mmu040_lea_pcrel_fullformat();
	test_mmu040_user_icode_stream();
	test_mmu040_writeback_slot_consumed();
	test_mmu040_misaligned_split();
	test_mmu040_copyout_fault_keeps_source_reg();
	test_stop_advances_pc_all_tables();
	test_amix_mmu040_latch();
	test_mmu030_demand_page_read();
	test_mmu030_demand_page_write();
	test_mmu030_multifault_continuation();
	test_runloop_overhead_switches();
	test_perf_line_clock();
	test_runloop_build_variant();
	test_shared_mailbox_offsets();
	test_dfast_verdict_table();
	test_dfast_state_equivalence();
	test_blk_state_equivalence();
	test_dopc_state_equivalence();
	test_dopc_coherence();
	test_prof_wire_format();
	test_prof_u64_ring_depth();
	test_prof_build_variant();
	test_warp_switch();
	test_warp_block_cache();
	test_warp_null_block();
	test_warp_cadence();
	test_warp_lockstep();
#if Z3660_EMU_PROF
	test_prof_bucket_arithmetic();
	test_prof_ret_macro_ordering();
	test_prof_tail_split();
	test_prof_probe_calibration();
	test_prof_price_ceiling();
	test_prof_clock_source();
	test_prof_translation_counters();
	test_prof_ifetch_calls();
	test_blk_counters();
	test_dopc_counters();
	test_prof_loop_split();
	test_prof_ring();
	test_prof_under_execution();
	test_warp_counters();
#endif

	printf("\n=== %d passed, %d failed ===\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
