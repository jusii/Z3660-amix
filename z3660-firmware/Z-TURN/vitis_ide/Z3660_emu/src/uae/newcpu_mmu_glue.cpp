/* newcpu_mmu_glue.cpp — Z3660 glue for the imported WinUAE 4.4.0 68030 MMU engine.
 *
 * The Z3660 fork stripped the MMU support from newcpu.cpp; the imported
 * cpummu030.cpp / cpummu.cpp reference a set of accessor function pointers and
 * helper functions that live in WinUAE's newcpu.cpp. None of them exist anywhere
 * in this tree, so they are all (re)defined here, in ONE place shared by both the
 * firmware build (Makefile globs src/**) and the host MMU harness.
 *
 * Z3660 operating mode (per UAE_030_MMU_plan.md): 68030, direct interpreter,
 * NO JIT, NO cycle-exact, NO 030/040 hardware cache emulation. Therefore the
 * cycle-exact (*_ce020) and 040-cache (*_c040, *_cache_040, *_icache040) paths
 * are never on the hot path — they are provided as physical pass-throughs so the
 * engine links and any stray reference behaves sanely. The 030 data-access
 * function pointers default to PHYSICAL access here; set_x_funcs()' MMU arm
 * (Stage 3) repoints the logical ones at the translating mmu030 accessors. */

#include "sysconfig.h"
#include "sysdeps.h"
#include "options.h"
#include "memory.h"
#include "newcpu.h"
#include "cpummu030.h"

/* ce_cachable: hardware cache-attribute table. Upstream owner is memory.cpp (def +
 * memset/per-bank init) but that whole block is #if 0'd out in this fork, so this
 * glue is the SOLE definition — if a future reinauer sync re-enables memory.cpp's
 * block this becomes a multiple-definition link error. Left all-zero on purpose: the
 * Z3660 emulates NO 030 data/instruction cache, so mmu030_cache_state stores a .cs
 * hint in the ATC that never gates a real access. (custom.cpp does NOT own this.) */
uae_u8 ce_cachable[65536];

/* Branch-trace debugger flag (cpuemu_32). Always off on the Z3660. */
bool debugmem_trace = false;

/* (An)+/-(An) address-register fixup, called from the bus-fault CATCH READ path
 * (newcpu.cpp's `else` branch, ~4333) and the gencpu MULL sub-fault paths
 * (cpuemu_32.cpp). In WinUAE this restores An to its pre-(An)+ value and DISARMS
 * the mmufixup[] records after a fault, so the instruction restart re-arms them
 * cleanly. This fork previously stubbed it out (misread as a savestate hook),
 * which left a stale (An)+ fixup record ARMED on a READ-fault restart; the next
 * data fault's mmu030_page_fault then re-applied that stale record to an address
 * register the new instruction never incremented -> corrupted An (observed:
 * strcmp's `cmp.b (a1)+,d0` landing on a1=0 -> getty/cron User BUS ERROR -> no
 * login). Mirror the hand-patched WRITE branch (newcpu.cpp ~4327, commits
 * f0325ee/2fac97d) and upstream cpu_restore_fixup(): restore + disarm. */
void cpu_restore_fixup(void)
{
   if (mmufixup[0].reg >= 0) { m68k_areg(regs, mmufixup[0].reg & 7) = mmufixup[0].value; mmufixup[0].reg = -1; }
   if (mmufixup[1].reg >= 0) { m68k_areg(regs, mmufixup[1].reg & 7) = mmufixup[1].value; mmufixup[1].reg = -1; }
}

/* MMU-named bitfield aliases used by cpuemu_32 (BFEXTU/BFEXTS/BFINS/BFCLR/BFSET/
 * BFCHG/BFTST/BFFFO on a memory operand). These MUST translate through the 68030
 * PMMU and use the _mmu030_state accessors — same as every other operand access in
 * the MMU interpreter (e.g. op_2168_32's get_long_mmu030_state/put_long_mmu030_state)
 * so a mid-bitfield demand-page fault replays correctly on retry. The bare
 * get_bitfield/put_bitfield in newcpu_common.cpp use PHYSICAL get_byte/get_word/
 * get_long (= memory_get_*), which read/write the UNTRANSLATED logical address —
 * for a SCN1 kernel-stack VA that lands in slow_bank (the real bus) instead of the
 * MMU-mapped a3000mem page the move.l just wrote, corrupting the value (this was the
 * AMIX "vatosde() address not in SCN1" panic: bfextu read the high byte off the bus).
 * Bodies mirror newcpu_common.cpp get_bitfield/put_bitfield with each accessor swapped
 * for its _mmu030_state twin. */
uae_u32 REGPARAM2 x_get_bitfield(uae_u32 src, uae_u32 bdata[2], uae_s32 offset, int width)
{
	uae_u32 tmp, res, mask;
	offset &= 7;
	mask = 0xffffffffu << (32 - width);
	switch ((offset + width + 7) >> 3) {
	case 1:
		tmp = get_byte_mmu030_state(src);
		res = tmp << (24 + offset);
		bdata[0] = tmp & ~(mask >> (24 + offset));
		break;
	case 2:
		tmp = get_word_mmu030_state(src);
		res = tmp << (16 + offset);
		bdata[0] = tmp & ~(mask >> (16 + offset));
		break;
	case 3:
		tmp = get_word_mmu030_state(src);
		res = tmp << (16 + offset);
		bdata[0] = tmp & ~(mask >> (16 + offset));
		tmp = get_byte_mmu030_state(src + 2);
		res |= tmp << (8 + offset);
		bdata[1] = tmp & ~(mask >> (8 + offset));
		break;
	case 4:
		tmp = get_long_mmu030_state(src);
		res = tmp << offset;
		bdata[0] = tmp & ~(mask >> offset);
		break;
	case 5:
		tmp = get_long_mmu030_state(src);
		res = tmp << offset;
		bdata[0] = tmp & ~(mask >> offset);
		tmp = get_byte_mmu030_state(src + 4);
		res |= tmp >> (8 - offset);
		bdata[1] = tmp & ~(mask << (8 - offset));
		break;
	default:
		res = 0;
		break;
	}
	return res;
}
void REGPARAM2 x_put_bitfield(uae_u32 dst, uae_u32 bdata[2], uae_u32 val, uae_s32 offset, int width)
{
	offset = (offset & 7) + width;
	switch ((offset + 7) >> 3) {
	case 1:
		put_byte_mmu030_state(dst, bdata[0] | (val << (8 - offset)));
		break;
	case 2:
		put_word_mmu030_state(dst, bdata[0] | (val << (16 - offset)));
		break;
	case 3:
		put_word_mmu030_state(dst, bdata[0] | (val >> (offset - 16)));
		put_byte_mmu030_state(dst + 2, bdata[1] | (val << (24 - offset)));
		break;
	case 4:
		put_long_mmu030_state(dst, bdata[0] | (val << (32 - offset)));
		break;
	case 5:
		put_long_mmu030_state(dst, bdata[0] | (val >> (offset - 32)));
		put_byte_mmu030_state(dst + 4, bdata[1] | (val << (40 - offset)));
		break;
	default:
		break;
	}
}

/* ================= physical (post-MMU) access wrappers ================= */
static uae_u32 z_phys_get_byte(uaecptr a){ return memory_get_byte(a); }
static uae_u32 z_phys_get_word(uaecptr a){ return memory_get_word(a); }
static uae_u32 z_phys_get_long(uaecptr a){ return memory_get_long(a); }
static uae_u32 z_phys_get_iword(uaecptr a){ return memory_get_wordi(a); }
static uae_u32 z_phys_get_ilong(uaecptr a){ return memory_get_longi(a); }
static void z_phys_put_byte(uaecptr a, uae_u32 v){ memory_put_byte(a, v); }
static void z_phys_put_word(uaecptr a, uae_u32 v){ memory_put_word(a, v); }
static void z_phys_put_long(uaecptr a, uae_u32 v){ memory_put_long(a, v); }

/* These static initializers are LOAD-BEARING: the engine's physical accessors are
 * otherwise (re)assigned only inside mmu030_set_funcs(), which runs solely from
 * mmu030_reset(). m68k_reset_newcpu() now calls mmu030_reset() in MMU mode, so
 * set_funcs does run on the firmware — but it re-points iword/ilong to the non-i
 * phys_get_word/long (equivalent to these i-variant defaults for RAM execution,
 * which is all AMIX does). Do NOT drop these defaults to NULL "because set_funcs
 * sets them": the descriptor-walk path (cpummu030.cpp) would jump through null. */
uae_u32 (*x_phys_get_iword)(uaecptr) = z_phys_get_iword;
uae_u32 (*x_phys_get_ilong)(uaecptr) = z_phys_get_ilong;
uae_u32 (*x_phys_get_byte)(uaecptr)  = z_phys_get_byte;
uae_u32 (*x_phys_get_word)(uaecptr)  = z_phys_get_word;
uae_u32 (*x_phys_get_long)(uaecptr)  = z_phys_get_long;
void (*x_phys_put_byte)(uaecptr, uae_u32) = z_phys_put_byte;
void (*x_phys_put_word)(uaecptr, uae_u32) = z_phys_put_word;
void (*x_phys_put_long)(uaecptr, uae_u32) = z_phys_put_long;

static uae_u32 z_prefetch(int o){ return memory_get_wordi(m68k_getpc() + o); }
uae_u32 (*x_prefetch)(int) = z_prefetch;

/* ===== 030 data-access function pointers (logical; default to physical) =====
 * Stage 3 (set_x_funcs MMU arm) repoints the non-fc ones at the translating
 * mmu030 accessors; the _fc variants take an explicit function code. */
static uae_u32 z_d030_bget(uaecptr a){ return memory_get_byte(a); }
static uae_u32 z_d030_wget(uaecptr a){ return memory_get_word(a); }
static uae_u32 z_d030_lget(uaecptr a){ return memory_get_long(a); }
static void z_d030_bput(uaecptr a, uae_u32 v){ memory_put_byte(a, v); }
static void z_d030_wput(uaecptr a, uae_u32 v){ memory_put_word(a, v); }
static void z_d030_lput(uaecptr a, uae_u32 v){ memory_put_long(a, v); }
static uae_u32 z_d030_fc_bget(uaecptr a, uae_u32 fc){ (void)fc; return memory_get_byte(a); }
static uae_u32 z_d030_fc_wget(uaecptr a, uae_u32 fc){ (void)fc; return memory_get_word(a); }
static uae_u32 z_d030_fc_lget(uaecptr a, uae_u32 fc){ (void)fc; return memory_get_long(a); }
static void z_d030_fc_bput(uaecptr a, uae_u32 v, uae_u32 fc){ (void)fc; memory_put_byte(a, v); }
static void z_d030_fc_wput(uaecptr a, uae_u32 v, uae_u32 fc){ (void)fc; memory_put_word(a, v); }
static void z_d030_fc_lput(uaecptr a, uae_u32 v, uae_u32 fc){ (void)fc; memory_put_long(a, v); }

uae_u32 (*read_data_030_bget)(uaecptr) = z_d030_bget;
uae_u32 (*read_data_030_wget)(uaecptr) = z_d030_wget;
uae_u32 (*read_data_030_lget)(uaecptr) = z_d030_lget;
void (*write_data_030_bput)(uaecptr,uae_u32) = z_d030_bput;
void (*write_data_030_wput)(uaecptr,uae_u32) = z_d030_wput;
void (*write_data_030_lput)(uaecptr,uae_u32) = z_d030_lput;
uae_u32 (*read_data_030_fc_bget)(uaecptr, uae_u32) = z_d030_fc_bget;
uae_u32 (*read_data_030_fc_wget)(uaecptr, uae_u32) = z_d030_fc_wget;
uae_u32 (*read_data_030_fc_lget)(uaecptr, uae_u32) = z_d030_fc_lget;
void (*write_data_030_fc_bput)(uaecptr, uae_u32, uae_u32) = z_d030_fc_bput;
void (*write_data_030_fc_wput)(uaecptr, uae_u32, uae_u32) = z_d030_fc_wput;
void (*write_data_030_fc_lput)(uaecptr, uae_u32, uae_u32) = z_d030_fc_lput;

/* ===== dcache030 retry path (RMW/fault restart). Z3660 has no 030 dcache, so
 * these reduce to a plain physical access. ===== */
void write_dcache030_retry(uaecptr addr, uae_u32 v, uae_u32 fc, int size, int flags)
{
	(void)fc; (void)flags;
	if (size == sz_byte) memory_put_byte(addr, v);
	else if (size == sz_word) memory_put_word(addr, v);
	else memory_put_long(addr, v);
}
uae_u32 read_dcache030_retry(uaecptr addr, uae_u32 fc, int size, int flags)
{
	(void)fc; (void)flags;
	if (size == sz_byte) return memory_get_byte(addr);
	if (size == sz_word) return memory_get_word(addr);
	return memory_get_long(addr);
}

/* ===== 030 prefetch refill. Direct mode does not use the prefetch pipeline for
 * correctness; provide minimal refills so the engine's prefetch bookkeeping is
 * consistent. ===== */
void fill_prefetch_030_ntx(void) { }
void fill_prefetch_030_ntx_continue(void) { }
uae_u32 get_word_030_prefetch(int o) { return memory_get_wordi(m68k_getpc() + o); }

/* ===== cycle-exact (ce020) memory-with-delay: Z3660 is not cycle-exact, so
 * drop the delay and do a plain physical access. ===== */
uae_u32 mem_access_delay_byte_read_ce020(uaecptr a){ return memory_get_byte(a); }
uae_u32 mem_access_delay_word_read_ce020(uaecptr a){ return memory_get_word(a); }
uae_u32 mem_access_delay_long_read_ce020(uaecptr a){ return memory_get_long(a); }
uae_u32 mem_access_delay_wordi_read_ce020(uaecptr a){ return memory_get_wordi(a); }
uae_u32 mem_access_delay_longi_read_ce020(uaecptr a){ return memory_get_longi(a); }
void mem_access_delay_byte_write_ce020(uaecptr a, uae_u32 v){ memory_put_byte(a, v); }
void mem_access_delay_word_write_ce020(uaecptr a, uae_u32 v){ memory_put_word(a, v); }
void mem_access_delay_long_write_ce020(uaecptr a, uae_u32 v){ memory_put_long(a, v); }

/* ===== 040 cache-with-delay variants (cpummu.cpp, 68040 path) ===== */
uae_u32 mem_access_delay_byte_read_c040(uaecptr a){ return memory_get_byte(a); }
uae_u32 mem_access_delay_word_read_c040(uaecptr a){ return memory_get_word(a); }
uae_u32 mem_access_delay_long_read_c040(uaecptr a){ return memory_get_long(a); }
uae_u32 mem_access_delay_longi_read_c040(uaecptr a){ return memory_get_longi(a); }
void mem_access_delay_byte_write_c040(uaecptr a, uae_u32 v){ memory_put_byte(a, v); }
void mem_access_delay_word_write_c040(uaecptr a, uae_u32 v){ memory_put_word(a, v); }
void mem_access_delay_long_write_c040(uaecptr a, uae_u32 v){ memory_put_long(a, v); }

/* ===== 040 instruction/data cache accessors (no 040 cache emulation) ===== */
void put_byte_cache_040(uaecptr a, uae_u32 v){ memory_put_byte(a, v); }
void put_word_cache_040(uaecptr a, uae_u32 v){ memory_put_word(a, v); }
void put_long_cache_040(uaecptr a, uae_u32 v){ memory_put_long(a, v); }
uae_u32 get_byte_cache_040(uaecptr a){ return memory_get_byte(a); }
uae_u32 get_word_cache_040(uaecptr a){ return memory_get_word(a); }
uae_u32 get_long_cache_040(uaecptr a){ return memory_get_long(a); }
uae_u32 get_word_icache040(uaecptr a){ return memory_get_wordi(a); }
uae_u32 get_long_icache040(uaecptr a){ return memory_get_longi(a); }
