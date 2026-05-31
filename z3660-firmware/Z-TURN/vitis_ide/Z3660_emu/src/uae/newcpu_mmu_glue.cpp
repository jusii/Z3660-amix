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

/* ---- cache-attribute table normally defined in custom.cpp (stripped here) ---- */
uae_u8 ce_cachable[65536];

/* Branch-trace debugger flag (cpuemu_32). Always off on the Z3660. */
bool debugmem_trace = false;

/* Savestate MMU fixup hook referenced by cpuemu_32 RTE/restore paths. The Z3660
 * has no savestate, so nothing to fix up. */
void cpu_restore_fixup(void) { }

/* MMU-named bitfield aliases used by cpuemu_32 — forward to the real ones. */
uae_u32 REGPARAM2 x_get_bitfield(uae_u32 src, uae_u32 bdata[2], uae_s32 offset, int width)
{ return get_bitfield(src, bdata, offset, width); }
void REGPARAM2 x_put_bitfield(uae_u32 dst, uae_u32 bdata[2], uae_u32 val, uae_s32 offset, int width)
{ put_bitfield(dst, bdata, val, offset, width); }

/* ================= physical (post-MMU) access wrappers ================= */
static uae_u32 z_phys_get_byte(uaecptr a){ return memory_get_byte(a); }
static uae_u32 z_phys_get_word(uaecptr a){ return memory_get_word(a); }
static uae_u32 z_phys_get_long(uaecptr a){ return memory_get_long(a); }
static uae_u32 z_phys_get_iword(uaecptr a){ return memory_get_wordi(a); }
static uae_u32 z_phys_get_ilong(uaecptr a){ return memory_get_longi(a); }
static void z_phys_put_byte(uaecptr a, uae_u32 v){ memory_put_byte(a, v); }
static void z_phys_put_word(uaecptr a, uae_u32 v){ memory_put_word(a, v); }
static void z_phys_put_long(uaecptr a, uae_u32 v){ memory_put_long(a, v); }

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
