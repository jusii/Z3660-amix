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
	test_stop_advances_pc_all_tables();
	test_amix_mmu040_latch();
	test_mmu030_demand_page_read();
	test_mmu030_demand_page_write();
	test_mmu030_multifault_continuation();

	printf("\n=== %d passed, %d failed ===\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
